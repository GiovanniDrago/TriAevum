#include "fast/renderer/diffuse_gi_pass_plan.h"

// CPU half of the M5 runtime pass: own code. Every constant block comes from
// the M2-M4 functions the FP32 twins read (MakeGatherShaderParams,
// MakeM4ShaderParams, MakeTemporalShaderParams, MakeFilterShaderParams), so the
// pass and the twins cannot drift apart.

#include <algorithm>
#include <cstring>

namespace Fast::Renderer::DiffuseGi {
namespace {

constexpr uint32_t kMaxPassSections = 64;     // one dispatch per frame must stay short
constexpr uint32_t kMaxPassExtent = 16384;    // per axis: indices stay far below 2^32

struct Fnv {
    uint64_t Hash = 14695981039346656037ULL;
    void Byte(uint8_t b) noexcept {
        Hash ^= b;
        Hash *= 1099511628211ULL;
    }
    void U64(uint64_t v) noexcept {
        for (int k = 0; k < 8; ++k) Byte(static_cast<uint8_t>(v >> (8 * k)));
    }
    void F64(double v) noexcept {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        U64(bits);
    }
};

uint32_t AlignUp(uint32_t value, uint32_t alignment) noexcept { return (value + alignment - 1U) / alignment * alignment; }

template <typename Block>
void Put(std::vector<uint8_t>& bytes, uint32_t offset, const Block& block) {
    std::memcpy(bytes.data() + offset, &block, sizeof(Block));
}

} // namespace

PassSettings DefaultPassSettings() {
    PassSettings s;
    GatherSettings& g = s.Gather;
    g.Radius = 1.0;
    g.Thickness = 0.5;
    g.Sections = 1;
    g.Sectors = 64;
    g.Rotation = 0.5;
    g.Schedule = CandidateSchedule::MetricRadial;
    g.CandidatesPerSide = 16;
    g.RadialExponent = 2.0;
    g.RadialJitter = 0.5;
    g.Mask = MaskMode::CentreRule;
    g.Rotations = {RotationMode::Stratified, 2, 7, 0};
    g.SequenceRadialJitter = true;
    g.HierarchyFootprint = true;
    g.Cache = CacheMode::Mean;
    g.HierarchyMaxLevel = 4;
    g.HierarchySpacingScale = 1.0;
    g.Guiding = false;
    s.Hierarchy.ShOrder = 0;
    return s;
}

bool PassUsesHierarchy(const PassSettings& s) noexcept {
    return (s.Gather.HierarchyFootprint || s.Gather.Cache != CacheMode::Off) &&
           s.Gather.Schedule != CandidateSchedule::EveryTexel;
}

const char* ValidatePassSettings(const PassSettings& s) noexcept {
    const GatherSettings& g = s.Gather;
    if (const char* e = ValidateGatherSettings(g); *e != '\0') return e;
    if (g.Mask != MaskMode::CentreRule) return "the pass runs the centre rule (continuous intervals are the reference)";
    if (g.Sections > kMaxPassSections) return "more than 64 sections per pixel and frame";
    if (g.Guiding) return "guiding is not part of the runtime pass (M4: off, open point 20)";
    if (g.Cache == CacheMode::SphericalHarmonics)
        return "the SH backend is not part of the runtime pass (M4: off, open point 20)";
    if (g.Rotations.Mode == RotationMode::BlueNoise)
        return "blue-noise rotations are not part of the runtime pass (M4: off, open point 20)";
    if (const char* e = ValidateHierarchySettings(s.Hierarchy); *e != '\0') return e;
    if (s.Hierarchy.ShOrder != 0U) return "the runtime pass builds no SH sums (SH order must be 0)";
    if (PassUsesHierarchy(s) && g.HierarchyMaxLevel > s.Hierarchy.Levels - 1U)
        return "footprint level beyond the hierarchy levels";
    if (const char* e = ValidateTemporalSettings(s.Temporal); *e != '\0') return e;
    if (g.Rotations.Mode == RotationMode::Stratified && s.Temporal.TileLog2 != g.Rotations.TileLog2)
        return "the reconstruction tile must equal the rotation tile";
    if (const char* e = ValidateFilterSettings(s.Filter); *e != '\0') return e;
    return "";
}

uint64_t PassTransportKey(const PassSettings& s, GeometricNormalSource normals) noexcept {
    const GatherSettings& g = s.Gather;
    Fnv h;
    h.F64(g.Radius);
    h.F64(g.Thickness);
    h.U64(g.Sections);
    h.U64(g.Sectors);
    h.F64(g.Rotation);
    h.U64(static_cast<uint64_t>(g.Schedule));
    h.U64(g.CandidatesPerSide);
    h.F64(g.RadialExponent);
    h.F64(g.RadialJitter);
    h.U64(static_cast<uint64_t>(g.Mask));
    h.U64(g.RejectSelfIntersection ? 1U : 0U);
    h.F64(g.SelfIntersectionSine);
    h.F64(g.SelfIntersectionDepth);
    h.U64(g.InterleaveSides ? 1U : 0U);
    h.F64(g.ChordEpsilon);
    h.U64(static_cast<uint64_t>(g.Rotations.Mode));
    h.U64(g.Rotations.TileLog2);
    h.U64(g.Rotations.Cycle);
    h.U64(g.SequenceRadialJitter ? 1U : 0U);
    h.U64(g.HierarchyFootprint ? 1U : 0U);
    h.U64(static_cast<uint64_t>(g.Cache));
    h.U64(g.HierarchyMaxLevel);
    h.F64(g.HierarchySpacingScale);
    h.U64(g.Guiding ? 1U : 0U);
    h.U64(static_cast<uint64_t>(g.GuidingAccessibility));
    h.U64(s.Hierarchy.Levels);
    h.F64(s.Hierarchy.PlaneTolerance);
    h.F64(s.Hierarchy.NormalCos);
    h.U64(s.Hierarchy.ShOrder);
    h.U64(static_cast<uint64_t>(normals));
    return h.Hash;
}

const char* ValidatePassExtent(const PassExtent& e) noexcept {
    if (e.BufferWidth == 0U || e.BufferHeight == 0U) return "empty buffers";
    if (e.BufferWidth > kMaxPassExtent || e.BufferHeight > kMaxPassExtent) return "buffers larger than 16384 per axis";
    if (e.Viewport.Width == 0U || e.Viewport.Height == 0U) return "empty viewport";
    if (static_cast<uint64_t>(e.Viewport.X) + e.Viewport.Width > e.BufferWidth ||
        static_cast<uint64_t>(e.Viewport.Y) + e.Viewport.Height > e.BufferHeight)
        return "viewport outside the buffers";
    return "";
}

uint64_t PassResourcePlan::Bytes(PassLifetime lifetime) const noexcept {
    uint64_t total = 0;
    for (const PassResource& r : Buffers)
        if (r.Allocated && r.Lifetime == lifetime) total += r.Bytes;
    return total;
}

uint64_t PassResourcePlan::PassBytes() const noexcept {
    uint64_t total = 0;
    for (const PassResource& r : Buffers)
        if (r.Allocated) total += r.Bytes;
    return total;
}

uint64_t PassResourcePlan::OutputBytes() const noexcept { return Outputs[0].Bytes + Outputs[1].Bytes; }

PassResourcePlan PlanPassResources(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                                   uint32_t uniformAlignment) {
    PassResourcePlan plan;
    plan.Extent = extent;
    const uint64_t texels = static_cast<uint64_t>(extent.BufferWidth) * extent.BufferHeight;
    const uint64_t pixels = static_cast<uint64_t>(extent.Viewport.Width) * extent.Viewport.Height;
    const bool hierarchy = PassUsesHierarchy(settings);
    const uint32_t levels = std::clamp(settings.Hierarchy.Levels, 1U, kMaxHierarchyLevels);
    plan.HierarchyCells = hierarchy ? HierarchyCellCount(extent.Viewport.Width, extent.Viewport.Height, levels) : 0U;
    const uint32_t filterLevels = settings.Filter.Levels;
    const auto set = [&](PassResourceId id, const char* name, const char* layout, uint32_t elementBytes,
                         uint64_t elements, PassLifetime lifetime, const char* access, bool allocated) {
        PassResource& r = plan.Buffers[static_cast<size_t>(id)];
        r = {id, name, layout, elementBytes, elements, allocated ? elementBytes * elements : 0U, lifetime, access,
             allocated};
    };
    using enum PassResourceId;
    using enum PassLifetime;
    set(Depth, "depth", "float", 4, texels, Input, "adapter -> normal, level 0, gather, pack", true);
    set(Class, "class", "uint", 4, texels, Input, "adapter -> normal, level 0, gather, pack", true);
    set(Radiance, "radiance", "vec4", 16, texels, Input, "adapter -> level 0, gather", true);
    set(Normal, "normal", "vec4", 16, texels, Input,
        "adapter (face normals) -> normal pass (in place) -> level 0, gather, pack", true);
    set(Motion, "motion", "vec2", 8, pixels, Input, "adapter -> temporal", true);
    set(PreviousPosition, "previous position", "vec4", 16, pixels, Input, "adapter -> temporal",
        inputs.PreviousPositions);
    set(SurfaceId, "surface id", "uint", 4, pixels, Input, "adapter -> temporal", inputs.SurfaceIds);
    set(Cells, "hierarchy cells", "3 x vec4 per cell", 48, plan.HierarchyCells, Frame,
        "level 0, reduce -> reduce, gather", hierarchy);
    set(Results, "raw J and A", "vec4", 16, pixels, Frame, "gather -> temporal", true);
    set(Aux, "gather aux", "vec4", 16, pixels, Frame, "gather -> pack", true);
    set(Geometry, "receiver geometry", "vec4", 16, pixels, Frame, "pack -> temporal, filter, output", true);
    set(Colour, "accumulated J and A", "vec4", 16, pixels, Frame, "temporal -> filter level 0 or output", true);
    set(Variance, "variance", "vec2", 8, pixels, Frame, "temporal -> filter level 0", true);
    set(Diag, "temporal diagnostics", "vec4", 16, pixels, Frame, "temporal -> diagnostics", true);
    set(Diag2, "temporal diagnostics 2", "vec4", 16, pixels, Frame, "temporal -> diagnostics", true);
    set(Filter0, "filter ping", "vec4", 16, pixels, Frame, "filter level -> next level or output",
        filterLevels >= 1U);
    // Both ping-pong buffers exist whenever a level runs, so changing the level
    // count within 1-8 never reallocates.
    set(Filter1, "filter pong", "vec4", 16, pixels, Frame, "filter level -> next level or output",
        filterLevels >= 1U);
    set(FilterVariance0, "filter ping variance", "vec2", 8, pixels, Frame, "filter level -> next level",
        filterLevels >= 1U);
    set(FilterVariance1, "filter pong variance", "vec2", 8, pixels, Frame, "filter level -> next level",
        filterLevels >= 1U);
    set(History0, "history A", "4 x vec4", 64, pixels, Persistent, "temporal (frame t) -> temporal (frame t + 1)",
        true);
    set(History1, "history B", "4 x vec4", 64, pixels, Persistent, "temporal (frame t) -> temporal (frame t + 1)",
        true);
    set(Parameters, "parameters", "uniform blocks", 1, MakePassParameterLayout(uniformAlignment).Size, Static,
        "command stream update -> every kernel", true);
    set(Placeholder, "placeholder", "16 bytes", 16, 1, Static, "never accessed (unused kernel bindings)", true);
    plan.Outputs[0] = {"DiffuseIllumination (J, receiver)", "R16G16B16A16_SFLOAT", 8, 8U * texels};
    plan.Outputs[1] = {"diffuse accessibility (A)", "R32_SFLOAT", 4, 4U * texels};
    return plan;
}

PassParameterLayout MakePassParameterLayout(uint32_t uniformAlignment) noexcept {
    PassParameterLayout layout;
    layout.Alignment = std::max(uniformAlignment, 16U);
    uint32_t offset = 0;
    const auto take = [&](uint32_t bytes) {
        const uint32_t at = offset;
        offset = AlignUp(offset + bytes, layout.Alignment);
        return at;
    };
    layout.Gather = take(sizeof(GatherShaderParams));
    for (uint32_t& m4 : layout.M4) m4 = take(sizeof(M4ShaderParams));
    layout.Temporal = take(sizeof(TemporalShaderParams));
    for (uint32_t& filter : layout.Filter) filter = take(sizeof(FilterShaderParams));
    layout.Output = take(sizeof(OutputShaderParams));
    layout.OutputNeutral = take(sizeof(OutputShaderParams));
    layout.Size = offset;
    return layout;
}

const char* ValidatePassFrame(const PassFrame& frame, const PassInputLayout& inputs) noexcept {
    if (const char* e = ValidateDepthCamera(frame.Camera); *e != '\0') return e;
    if (frame.Validation == HistoryValidation::PreviousPosition && !inputs.PreviousPositions)
        return "previous positions declared but not provided";
    if (frame.SurfaceIds && !inputs.SurfaceIds) return "surface ids declared but not provided";
    return "";
}

OutputShaderParams MakeOutputShaderParams(const PassExtent& extent, uint32_t mode) noexcept {
    OutputShaderParams p;
    p.Extent = {extent.BufferWidth, extent.BufferHeight, extent.Viewport.X, extent.Viewport.Y};
    p.Viewport = {extent.Viewport.Width, extent.Viewport.Height, mode, 0U};
    p.Limits = {kOutputHalfMax, 0.0F, 0.0F, 0.0F};
    return p;
}

const char* PlanPassFrame(const PassExtent& extent, const PassSettings& settings, const PassInputLayout& inputs,
                          const PassParameterLayout& layout, const TemporalHistoryState& history,
                          const PassFrame& frame, PassFramePlan& plan) {
    if (const char* e = ValidatePassFrame(frame, inputs); *e != '\0') return e;
    // Frame-level gather input: extents, camera and normal source only (the
    // per-texel data lives in the pass buffers).
    GatherInput dims;
    dims.BufferWidth = extent.BufferWidth;
    dims.BufferHeight = extent.BufferHeight;
    dims.Viewport = extent.Viewport;
    dims.Camera = frame.Camera;
    dims.NormalSource = frame.NormalSource;
    GatherSettings gather = settings.Gather;
    gather.Rotations.Frame = frame.FrameIndex;

    TemporalFrameState state;
    state.Width = extent.Viewport.Width;
    state.Height = extent.Viewport.Height;
    state.FrameIndex = frame.FrameIndex;
    state.ViewId = frame.ViewId;
    state.CameraCut = frame.CameraCut;
    state.ExplicitReset = frame.ExplicitReset;
    state.PreExposure = frame.PreExposure;
    state.TransportKey = PassTransportKey(settings, frame.NormalSource);
    state.ViewX = frame.Camera.ViewX;
    state.ViewY = frame.Camera.ViewY;
    state.JitterX = frame.JitterX;
    state.JitterY = frame.JitterY;
    state.CurrentToPrevious = frame.CurrentToPrevious;
    state.Motion = frame.Motion;
    state.Validation = frame.Validation;
    state.SurfaceIds = frame.SurfaceIds;
    if (const char* e = ValidateTemporalFrameState(state); *e != '\0') return e;

    plan = {};
    plan.Gather = MakeGatherShaderParams(dims, gather);
    plan.Hierarchy = PassUsesHierarchy(settings);
    const uint32_t levels = std::clamp(settings.Hierarchy.Levels, 1U, kMaxHierarchyLevels);
    const M4ShaderParams m4 = MakeM4ShaderParams(dims, gather, settings.Hierarchy, nullptr, nullptr, nullptr, false);
    for (uint32_t l = 0; l < kMaxHierarchyLevels; ++l) {
        plan.M4[l] = m4;
        plan.M4[l].Build[0] = l;
        plan.LevelExtent[l] = {m4.Levels[l][0], m4.Levels[l][1]};
    }
    plan.HierarchyLevels = plan.Hierarchy ? levels : 0U;

    const TemporalHistoryState::Plan decision = history.Prepare(state, settings.Temporal);
    plan.ResetReasons = decision.ResetReasons;
    plan.Temporal = state;
    plan.TemporalParams = MakeTemporalShaderParams(state, settings.Temporal, decision);

    FilterInput filter;
    filter.Width = extent.Viewport.Width;
    filter.Height = extent.Viewport.Height;
    filter.ViewX = frame.Camera.ViewX;
    filter.ViewY = frame.Camera.ViewY;
    plan.FilterLevels = settings.Filter.Levels;
    for (uint32_t l = 0; l < kMaxFilterLevels; ++l) plan.Filter[l] = MakeFilterShaderParams(filter, settings.Filter, l);

    plan.Output = MakeOutputShaderParams(extent, kOutputModeFinal);
    plan.OutputNeutral = MakeOutputShaderParams(extent, kOutputModeNeutral);

    plan.Parameters.assign(layout.Size, 0U);
    Put(plan.Parameters, layout.Gather, plan.Gather);
    for (uint32_t l = 0; l < kMaxHierarchyLevels; ++l) Put(plan.Parameters, layout.M4[l], plan.M4[l]);
    Put(plan.Parameters, layout.Temporal, plan.TemporalParams);
    for (uint32_t l = 0; l < kMaxFilterLevels; ++l) Put(plan.Parameters, layout.Filter[l], plan.Filter[l]);
    Put(plan.Parameters, layout.Output, plan.Output);
    Put(plan.Parameters, layout.OutputNeutral, plan.OutputNeutral);
    return "";
}

PassWorkload PassFrameWorkload(const PassSettings& settings) noexcept {
    PassWorkload w;
    const uint32_t levels = std::clamp(settings.Hierarchy.Levels, 1U, kMaxHierarchyLevels);
    // normal, gather, pack, temporal, output; hierarchy levels; filter levels
    w.Dispatches = 5U + (PassUsesHierarchy(settings) ? levels : 0U) + settings.Filter.Levels;
    // two leading barriers, then one between consecutive dispatches
    w.PrivateBarriers = 2U + (w.Dispatches - 1U);
    return w;
}

} // namespace Fast::Renderer::DiffuseGi
