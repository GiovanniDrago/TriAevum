// M5 (Stage A) tests of the CPU half of the diffuse GI runtime pass
// (diffuse_gi_pass_plan.h): settings and their validation, the transport key,
// the reset rules shared with the M3 accumulator, the resource plan, the
// uniform parameter layout, the frame plan built from the M2-M4 constant
// blocks, the workload and the FP32 twin of the output kernel. Lines starting
// with DGI_M5 are quoted in docs/TRIAEVUM_SCREEN_SPACE_GI.md ("M5").
#include "fast/renderer/diffuse_gi_pass_plan.h"
#include "fast/renderer/diffuse_gi_pass_shader.h"
#include "fast/renderer/diffuse_gi_scene.h"
#include "fast/renderer/diffuse_gi_sequence.h"
#include "fast/renderer/diffuse_gi_temporal.h"
#include "fast/renderer/diffuse_gi_temporal_shader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace Fast::Renderer::DiffuseGi;
using namespace Fast::Renderer::DiffuseGi::Synthetic;

namespace {

PassExtent Full(uint32_t width, uint32_t height) {
    PassExtent e;
    e.BufferWidth = width;
    e.BufferHeight = height;
    e.Viewport = {0, 0, width, height};
    return e;
}

template <typename T>
bool SameBytes(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

// A temporal frame without receivers: the reset rules and constant blocks do
// not depend on per-pixel data.
TemporalFrame EmptyFrame(const RenderedFrame& rendered, uint64_t index) {
    TemporalFrame f;
    f.Width = rendered.Input.Viewport.Width;
    f.Height = rendered.Input.Viewport.Height;
    const size_t count = static_cast<size_t>(f.Width) * f.Height;
    f.FrameIndex = index;
    f.Depth.assign(count, 0.0);
    f.Normal.assign(3 * count, 0.0);
    f.Raw.assign(4 * count, 0.0);
    f.MotionVectors = rendered.MotionIncluded;
    f.ViewX = rendered.Input.Camera.ViewX;
    f.ViewY = rendered.Input.Camera.ViewY;
    f.JitterX = rendered.JitterX;
    f.JitterY = rendered.JitterY;
    f.CurrentToPrevious = rendered.CurrentToPrevious;
    return f;
}

} // namespace

// The pass and TemporalAccumulator apply one set of reset rules
// (TemporalHistoryState) and build the same constant block, bit for bit, over
// every reset reason.
TEST(DiffuseGiPass, ResetRulesAndTemporalBlockMatchTheAccumulator) {
    const Scene base = CornerRoomScene(48, 32);
    SequenceScene scene{base.Name, base.Camera, base.Primitives, std::vector<uint32_t>(base.Primitives.size(), 0U)};
    const Scene small = CornerRoomScene(40, 28);
    SequenceScene smallScene{small.Name, small.Camera, small.Primitives,
                             std::vector<uint32_t>(small.Primitives.size(), 0U)};
    TemporalAccumulator accumulator(Precision::Float32);
    TemporalHistoryState state;
    uint32_t reasonsSeen = 0, frames = 0, mismatches = 0;
    SequenceFrame previous;
    for (uint64_t k = 0; k < 20; ++k) {
        SequenceFrame current;
        current.Camera.Position = {0.01 * static_cast<double>(k), 0.0, -0.02 * static_cast<double>(k)};
        current.Camera.Yaw = 0.004 * static_cast<double>(k);
        current.JitterX = (k % 2 == 0) ? 0.25 : -0.25;
        current.JitterY = (k % 2 == 0) ? -0.25 : 0.25;
        const bool resized = k == 14;
        const RenderedFrame rendered =
            RenderSequenceFrame(resized ? smallScene : scene, current, k == 0 ? current : previous);
        previous = current;
        TemporalFrame f = EmptyFrame(rendered, k);
        TemporalSettings settings;
        f.Motion.Jitter = (k % 3 == 2) ? JitterInMotion::Excluded : JitterInMotion::Included;
        f.PreExposure = k >= 6 ? 2.0 : 1.0;
        f.CameraCut = k == 9;
        f.ExplicitReset = k == 11;
        f.ViewId = k == 12 ? 1U : 0U;
        f.TransportKey = k >= 13 ? 7U : 3U;
        settings.Enabled = k != 4;
        if (k == 16) settings.Exposure = ExposureChange::Invalidate, f.PreExposure = 4.0;
        if (k == 17) accumulator.Invalidate(), state.Invalidate();
        const TemporalResult result = accumulator.Process(f, settings);
        const TemporalFrameState s = TemporalStateOf(f);
        ASSERT_STREQ(ValidateTemporalFrameState(s), "");
        const TemporalHistoryState::Plan plan = state.Prepare(s, settings);
        const TemporalShaderParams params = MakeTemporalShaderParams(s, settings, plan);
        state.Commit(s);
        mismatches += (plan.ResetReasons != result.ResetReasons) || !SameBytes(params, accumulator.LastShaderParams());
        reasonsSeen |= plan.ResetReasons;
        ++frames;
    }
    std::printf("DGI_M5 reset_rules frames=%u reasons_seen=0x%02x block_or_reason_mismatches=%u\n", frames, reasonsSeen,
                mismatches);
    EXPECT_EQ(mismatches, 0U);
    // Every reason occurred: first frame, cut, view, resize, explicit, transport, exposure, disabled.
    EXPECT_EQ(reasonsSeen, 0xFFU);
}

TEST(DiffuseGiPass, DefaultsAreTheM4RecommendationAndValidationNamesTheRule) {
    const PassSettings d = DefaultPassSettings();
    EXPECT_STREQ(ValidatePassSettings(d), "");
    EXPECT_EQ(d.Gather.Schedule, CandidateSchedule::MetricRadial);
    EXPECT_EQ(d.Gather.CandidatesPerSide, 16U);
    EXPECT_EQ(d.Gather.RadialExponent, 2.0);
    EXPECT_TRUE(d.Gather.HierarchyFootprint);
    EXPECT_EQ(d.Gather.Cache, CacheMode::Mean);
    EXPECT_FALSE(d.Gather.Guiding);
    EXPECT_EQ(d.Gather.Rotations.Mode, RotationMode::Stratified);
    EXPECT_EQ(d.Gather.Rotations.TileLog2, 2U);
    EXPECT_EQ(d.Gather.Rotations.Cycle, 7U);
    EXPECT_TRUE(d.Gather.SequenceRadialJitter);
    EXPECT_EQ(d.Gather.Sections, 1U);
    EXPECT_EQ(d.Gather.Sectors, 64U);
    EXPECT_EQ(d.Hierarchy.ShOrder, 0U);
    EXPECT_EQ(d.Temporal.AlphaMin, 1.0 / 32.0);
    EXPECT_EQ(d.Filter.Levels, 4U);
    EXPECT_TRUE(PassUsesHierarchy(d));

    const auto reject = [&](auto change, const char* expected) {
        PassSettings s = DefaultPassSettings();
        change(s);
        EXPECT_STREQ(ValidatePassSettings(s), expected);
    };
    reject([](PassSettings& s) { s.Gather.Guiding = true; },
           "guiding is not part of the runtime pass (M4: off, open point 20)");
    reject([](PassSettings& s) { s.Gather.Cache = CacheMode::SphericalHarmonics; },
           "the SH backend is not part of the runtime pass (M4: off, open point 20)");
    reject([](PassSettings& s) { s.Gather.Rotations.Mode = RotationMode::BlueNoise; },
           "blue-noise rotations are not part of the runtime pass (M4: off, open point 20)");
    reject([](PassSettings& s) { s.Gather.Mask = MaskMode::ContinuousIntervals; },
           "the pass runs the centre rule (continuous intervals are the reference)");
    reject([](PassSettings& s) { s.Hierarchy.ShOrder = 2; }, "the runtime pass builds no SH sums (SH order must be 0)");
    reject([](PassSettings& s) { s.Temporal.TileLog2 = 3; }, "the reconstruction tile must equal the rotation tile");
    reject([](PassSettings& s) { s.Gather.Radius = 0.0; }, "radius R must be finite and positive");
    reject([](PassSettings& s) { s.Gather.Sections = 65; }, "more than 64 sections per pixel and frame");
    reject([](PassSettings& s) { s.Gather.HierarchyMaxLevel = 5; }, "footprint level beyond the hierarchy levels");
    reject([](PassSettings& s) { s.Filter.Levels = 9; }, "more than 8 filter levels");
    PassSettings off = DefaultPassSettings();
    off.Gather.HierarchyFootprint = false;
    off.Gather.Cache = CacheMode::Off;
    EXPECT_FALSE(PassUsesHierarchy(off));
    EXPECT_STREQ(ValidatePassSettings(off), "");

    EXPECT_STREQ(ValidatePassExtent(Full(800, 240)), "");
    EXPECT_STREQ(ValidatePassExtent(Full(0, 240)), "empty buffers");
    PassExtent outside = Full(64, 64);
    outside.Viewport = {8, 8, 60, 10};
    EXPECT_STREQ(ValidatePassExtent(outside), "viewport outside the buffers");
}

TEST(DiffuseGiPass, TransportKeyCoversTheTransportOnly) {
    const PassSettings base = DefaultPassSettings();
    const uint64_t key = PassTransportKey(base, GeometricNormalSource::DepthReconstruction);
    std::set<uint64_t> keys{key};
    uint32_t changes = 0;
    const auto differs = [&](auto change) {
        PassSettings s = base;
        change(s);
        keys.insert(PassTransportKey(s, GeometricNormalSource::DepthReconstruction));
        ++changes;
    };
    differs([](PassSettings& s) { s.Gather.Radius = 2.0; });
    differs([](PassSettings& s) { s.Gather.Thickness = 0.25; });
    differs([](PassSettings& s) { s.Gather.Sections = 2; });
    differs([](PassSettings& s) { s.Gather.Sectors = 32; });
    differs([](PassSettings& s) { s.Gather.Schedule = CandidateSchedule::Radial; });
    differs([](PassSettings& s) { s.Gather.CandidatesPerSide = 8; });
    differs([](PassSettings& s) { s.Gather.RadialExponent = 1.0; });
    differs([](PassSettings& s) { s.Gather.HierarchyFootprint = false; });
    differs([](PassSettings& s) { s.Gather.Cache = CacheMode::Off; });
    differs([](PassSettings& s) { s.Gather.Rotations.Cycle = 5; });
    differs([](PassSettings& s) { s.Gather.SelfIntersectionSine = 8.0e-3; });
    differs([](PassSettings& s) { s.Hierarchy.PlaneTolerance = 1.0e-4; });
    keys.insert(PassTransportKey(base, GeometricNormalSource::RendererFaceNormal));
    ++changes;
    EXPECT_EQ(keys.size(), changes + 1U);
    PassSettings temporal = base;
    temporal.Temporal.AlphaMin = 1.0 / 16.0;
    temporal.Filter.Levels = 2;
    temporal.Temporal.Enabled = false;
    EXPECT_EQ(PassTransportKey(temporal, GeometricNormalSource::DepthReconstruction), key);
}

TEST(DiffuseGiPass, ResourcePlanAndVram) {
    const PassSettings s = DefaultPassSettings();
    for (const auto& [w, h] : std::vector<std::pair<uint32_t, uint32_t>>{{800, 240}, {960, 800}, {1920, 1600}}) {
        const PassResourcePlan plan = PlanPassResources(Full(w, h), s, {}, 256);
        const uint64_t p = static_cast<uint64_t>(w) * h;
        const uint64_t cells = HierarchyCellCount(w, h, 5);
        EXPECT_EQ(plan.HierarchyCells, cells);
        // Inputs: depth 4 + class 4 + radiance 16 + normal 16 per texel, motion 8 per pixel.
        EXPECT_EQ(plan.Bytes(PassLifetime::Input), 40U * p + 8U * p);
        // Frame: cells 48 per cell; raw, aux, geometry, colour, diag, diag2, two
        // filter buffers 16 each; variance and two filter variances 8 each.
        EXPECT_EQ(plan.Bytes(PassLifetime::Frame), 48U * cells + 8U * 16U * p + 3U * 8U * p);
        EXPECT_EQ(plan.Bytes(PassLifetime::Persistent), 128U * p);
        EXPECT_EQ(plan.Bytes(PassLifetime::Static), 5120U + 16U);
        EXPECT_EQ(plan.OutputBytes(), 12U * p);
        EXPECT_FALSE(plan[PassResourceId::PreviousPosition].Allocated);
        EXPECT_FALSE(plan[PassResourceId::SurfaceId].Allocated);
        std::printf("DGI_M5 vram extent=%ux%u inputs=%.2f MB frame=%.2f MB (cells %.2f) history=%.2f MB static=%.4f MB "
                    "pass_total=%.2f MB (%.1f B/px) outputs=%.2f MB (J RGBA16F + A R32F)\n",
                    w, h, plan.Bytes(PassLifetime::Input) / 1.0e6, plan.Bytes(PassLifetime::Frame) / 1.0e6,
                    plan[PassResourceId::Cells].Bytes / 1.0e6, plan.Bytes(PassLifetime::Persistent) / 1.0e6,
                    plan.Bytes(PassLifetime::Static) / 1.0e6, plan.PassBytes() / 1.0e6,
                    static_cast<double>(plan.PassBytes()) / static_cast<double>(p), plan.OutputBytes() / 1.0e6);
    }
    // Hierarchy and filter off, optional inputs on, padded buffers.
    PassSettings lean = s;
    lean.Gather.HierarchyFootprint = false;
    lean.Gather.Cache = CacheMode::Off;
    lean.Filter.Levels = 0;
    PassExtent padded = Full(110, 70);
    padded.Viewport = {5, 3, 96, 60};
    const PassResourcePlan plan = PlanPassResources(padded, lean, {true, true}, 64);
    EXPECT_FALSE(plan[PassResourceId::Cells].Allocated);
    EXPECT_FALSE(plan[PassResourceId::Filter0].Allocated);
    EXPECT_FALSE(plan[PassResourceId::FilterVariance1].Allocated);
    EXPECT_EQ(plan[PassResourceId::Depth].Bytes, 4U * 110U * 70U);
    EXPECT_EQ(plan[PassResourceId::Motion].Bytes, 8U * 96U * 60U);
    EXPECT_EQ(plan[PassResourceId::PreviousPosition].Bytes, 16U * 96U * 60U);
    EXPECT_EQ(plan[PassResourceId::SurfaceId].Bytes, 4U * 96U * 60U);
    EXPECT_EQ(plan.OutputBytes(), 12U * 110U * 70U);
    for (const PassResource& r : plan.Buffers) {
        EXPECT_NE(std::string(r.Name), "");
        EXPECT_EQ(r.Bytes, r.Allocated ? static_cast<uint64_t>(r.ElementBytes) * r.Elements : 0U);
    }
}

TEST(DiffuseGiPass, ParameterLayoutIsAlignedAndFitsOneUpdate) {
    for (const uint32_t alignment : {1U, 16U, 64U, 256U}) {
        const PassParameterLayout l = MakePassParameterLayout(alignment);
        const uint32_t a = std::max(alignment, 16U);
        std::vector<std::pair<uint32_t, uint32_t>> blocks{{l.Gather, sizeof(GatherShaderParams)},
                                                          {l.Temporal, sizeof(TemporalShaderParams)},
                                                          {l.Output, sizeof(OutputShaderParams)},
                                                          {l.OutputNeutral, sizeof(OutputShaderParams)}};
        for (const uint32_t m4 : l.M4) blocks.push_back({m4, sizeof(M4ShaderParams)});
        for (const uint32_t f : l.Filter) blocks.push_back({f, sizeof(FilterShaderParams)});
        for (size_t i = 0; i < blocks.size(); ++i) {
            EXPECT_EQ(blocks[i].first % a, 0U);
            EXPECT_LE(blocks[i].first + blocks[i].second, l.Size);
            for (size_t j = i + 1; j < blocks.size(); ++j)
                EXPECT_TRUE(blocks[i].first + blocks[i].second <= blocks[j].first ||
                            blocks[j].first + blocks[j].second <= blocks[i].first);
        }
        EXPECT_EQ(l.Size % 4U, 0U);
        EXPECT_LE(l.Size, 65536U);
        std::printf("DGI_M5 parameter_layout alignment=%u blocks=%zu size=%u\n", a, blocks.size(), l.Size);
    }
}

// Every constant block of a planned frame is the block the FP32 twins read.
TEST(DiffuseGiPass, FramePlanUsesTheModuleBlocks) {
    const Scene scene = CornerRoomScene(37, 23);
    const GatherInput input = Rasterize(scene);
    const PassSettings s = DefaultPassSettings();
    const PassParameterLayout layout = MakePassParameterLayout(256);
    PassFrame frame;
    frame.Camera = input.Camera;
    frame.FrameIndex = 11;
    TemporalHistoryState history;
    PassFramePlan plan;
    ASSERT_STREQ(PlanPassFrame(Full(37, 23), s, {}, layout, history, frame, plan), "");
    EXPECT_EQ(plan.ResetReasons, kResetFirstFrame);
    GatherInput dims = input;
    GatherSettings g = s.Gather;
    g.Rotations.Frame = 11;
    EXPECT_TRUE(SameBytes(plan.Gather, MakeGatherShaderParams(dims, g)));
    EXPECT_EQ(plan.Gather.Sequence[3], 11U % 7U);
    const M4ShaderParams m4 = MakeM4ShaderParams(dims, g, s.Hierarchy, nullptr, nullptr, nullptr, false);
    for (uint32_t l = 0; l < kMaxHierarchyLevels; ++l) {
        M4ShaderParams expected = m4;
        expected.Build[0] = l;
        EXPECT_TRUE(SameBytes(plan.M4[l], expected));
        EXPECT_EQ(std::memcmp(plan.Parameters.data() + layout.M4[l], &expected, sizeof(expected)), 0);
    }
    EXPECT_EQ(plan.HierarchyLevels, 5U);
    FilterInput fi;
    fi.Width = 37, fi.Height = 23, fi.ViewX = input.Camera.ViewX, fi.ViewY = input.Camera.ViewY;
    for (uint32_t l = 0; l < kMaxFilterLevels; ++l)
        EXPECT_TRUE(SameBytes(plan.Filter[l], MakeFilterShaderParams(fi, s.Filter, l)));
    EXPECT_EQ(std::memcmp(plan.Parameters.data() + layout.Gather, &plan.Gather, sizeof(plan.Gather)), 0);
    EXPECT_EQ(std::memcmp(plan.Parameters.data() + layout.Temporal, &plan.TemporalParams, sizeof(plan.TemporalParams)),
              0);
    EXPECT_EQ(plan.TemporalParams.Extent[2], kTemporalFlagReset);
    EXPECT_EQ(plan.Temporal.TransportKey, PassTransportKey(s, GeometricNormalSource::DepthReconstruction));
    EXPECT_EQ(plan.Output.Viewport[2], kOutputModeFinal);
    EXPECT_EQ(plan.OutputNeutral.Viewport[2], kOutputModeNeutral);
    // Declarations are checked before any constant is produced.
    PassFrame bad = frame;
    bad.Validation = HistoryValidation::PreviousPosition;
    EXPECT_STREQ(PlanPassFrame(Full(37, 23), s, {}, layout, history, bad, plan),
                 "previous positions declared but not provided");
    bad = frame;
    bad.SurfaceIds = true;
    EXPECT_STREQ(PlanPassFrame(Full(37, 23), s, {}, layout, history, bad, plan), "surface ids declared but not provided");
    bad = frame;
    bad.Camera.Encoding = DepthEncoding::Unknown;
    EXPECT_STREQ(PlanPassFrame(Full(37, 23), s, {}, layout, history, bad, plan), "depth encoding is undeclared");
    bad = frame;
    bad.Motion.Direction = MotionDirection::Unknown;
    EXPECT_STREQ(PlanPassFrame(Full(37, 23), s, {}, layout, history, bad, plan), "motion convention is undeclared");
}

TEST(DiffuseGiPass, WorkloadCountsDispatchesAndBarriers) {
    PassSettings s = DefaultPassSettings();
    EXPECT_EQ(PassFrameWorkload(s).Dispatches, 5U + 5U + 4U);
    EXPECT_EQ(PassFrameWorkload(s).PrivateBarriers, 15U);
    s.Gather.HierarchyFootprint = false;
    s.Gather.Cache = CacheMode::Off;
    EXPECT_EQ(PassFrameWorkload(s).Dispatches, 9U);
    s.Filter.Levels = 0;
    EXPECT_EQ(PassFrameWorkload(s).Dispatches, 5U);
    EXPECT_EQ(PassFrameWorkload(s).PrivateBarriers, 6U);
}

TEST(DiffuseGiPass, OutputTexelTwin) {
    PassExtent e = Full(6, 4);
    e.Viewport = {1, 1, 4, 2};
    const OutputShaderParams p = MakeOutputShaderParams(e, kOutputModeFinal);
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    // Viewport pixels: receiver, non-receiver, non-finite colour, huge J (row 0); negative J, A < 0, A > 1, NaN normal.
    const std::vector<float> geometry{0, 0, 1, 2, 0, 0, 0, 0, 0, 0, 1, 3, 0, 1, 0, 4,
                                      1, 0, 0, 5, 0, 0, 1, 6, 0, 0, 1, 7, nan, 0, 1, 8};
    const std::vector<float> colour{0.5F, 0.25F, 2.0F, 0.75F, 9, 9, 9, 0.1F, nan, 1.0F, inf, nan, 1.0e6F, -1.0e6F, 1.0F, 1.0F,
                                    -0.5F, 0.0F, 0.0F, 0.5F, 0, 0, 0, -0.25F, 0, 0, 0, 1.75F, 9, 9, 9, 0.3F};
    const auto at = [&](uint32_t x, uint32_t y) { return OutputTexelF32(p, x, y, colour.data(), geometry.data()); };
    OutputTexel t = at(1, 1);
    EXPECT_EQ(t.Illumination, (std::array<float, 4>{0.5F, 0.25F, 2.0F, 1.0F}));
    EXPECT_EQ(t.Accessibility, 0.75F);
    t = at(2, 1); // not a receiver
    EXPECT_EQ(t.Illumination, (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(t.Accessibility, 1.0F);
    t = at(3, 1); // non-finite colour: zero J, open A
    EXPECT_EQ(t.Illumination, (std::array<float, 4>{0.0F, 1.0F, 0.0F, 1.0F}));
    EXPECT_EQ(t.Accessibility, 1.0F);
    t = at(4, 1); // HDR beyond FP16: limited to the largest finite half
    EXPECT_EQ(t.Illumination, (std::array<float, 4>{kOutputHalfMax, -kOutputHalfMax, 1.0F, 1.0F}));
    t = at(1, 2);
    EXPECT_EQ(t.Illumination[0], -0.5F); // no artistic clamp: the sign is kept
    EXPECT_EQ(at(2, 2).Accessibility, 0.0F);
    EXPECT_EQ(at(3, 2).Accessibility, 1.0F);
    EXPECT_EQ(at(4, 2).Illumination[3], 0.0F); // NaN normal: not a receiver
    EXPECT_EQ(at(0, 0).Accessibility, 1.0F);   // outside the viewport
    EXPECT_EQ(at(5, 3).Illumination[3], 0.0F);
    const OutputShaderParams neutral = MakeOutputShaderParams(e, kOutputModeNeutral);
    EXPECT_EQ(OutputTexelF32(neutral, 1, 1, colour.data(), geometry.data()).Illumination[3], 0.0F);
    EXPECT_EQ(OutputTexelF32(neutral, 1, 1, colour.data(), geometry.data()).Accessibility, 1.0F);
}

TEST(DiffuseGiPass, KernelCatalogueIsTheModuleKernels) {
    const auto kernels = BuildDiffuseGiPassKernels();
    ASSERT_EQ(kernels.size(), kPassKernelCount);
    std::set<std::string> names, sources;
    for (uint32_t k = 0; k < kernels.size(); ++k) {
        EXPECT_EQ(static_cast<uint32_t>(kernels[k].Kernel), k);
        names.insert(kernels[k].Name);
        sources.insert(kernels[k].Source);
        EXPECT_EQ(kernels[k].Source.rfind("#version 450", 0), 0U);
    }
    EXPECT_EQ(names.size(), kPassKernelCount);
    EXPECT_EQ(sources.size(), kPassKernelCount);
    EXPECT_EQ(kernels[0].Source, BuildDiffuseGiNormalShader());
    EXPECT_EQ(kernels[1].Source, BuildDiffuseGiHierarchyLevel0Shader());
    EXPECT_EQ(kernels[2].Source, BuildDiffuseGiHierarchyReduceShader());
    EXPECT_EQ(kernels[3].Source, BuildDiffuseGiGatherM4Shader());
    EXPECT_EQ(kernels[4].Source, BuildDiffuseGiPackShader());
    EXPECT_EQ(kernels[5].Source, BuildDiffuseGiTemporalShader());
    EXPECT_EQ(kernels[6].Source, BuildDiffuseGiFilterShader());
    EXPECT_EQ(kernels[7].Source, BuildDiffuseGiOutputShader());
}
