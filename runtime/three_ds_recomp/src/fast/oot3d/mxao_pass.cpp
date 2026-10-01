#include "fast/oot3d/mxao_pass.h"
#ifdef ENABLE_OOT3D_VULKAN
#include "fast/oot3d/nri_effect_graph_transient_image_arena.h"
#include "fast/oot3d/nri_interop_context.h"
#include "fast/oot3d/resource_state_tracker.h"
#include "fast/oot3d/temporal_history_manager.h"
#ifdef ENABLE_OOT3D_IMMERSE
#include "fast/immerse/mxao_shader.h"
#endif
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
#include "nri_interop_internal.h"
#endif
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace Fast::Oot3d {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
namespace {
// Descriptor sets, one per dispatch input/output pairing. The temporal sets
// come in pairs: the one that writes history image 0, then image 1.
enum : size_t {
    kDeinterleaveSet,
    kOcclusionSet,
    kFilterSet,
    kResolveRawSet,
    kResolveFilteredSet,
    kTemporalSet,
    kFilterAccumulatedSet = kTemporalSet + 2,
    kResolveAccumulatedSet = kFilterAccumulatedSet + 2,
    kSetCount = kResolveAccumulatedSet + 2
};
enum : size_t { kDeinterleaveProgram, kOcclusionProgram, kFilterProgram, kResolveProgram, kTemporalProgram, kProgramCount };
} // namespace
#endif

struct MxaoPass::Impl {
    VkDevice Device = VK_NULL_HANDLE;
    NriInteropContext* Interop = nullptr;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    nri::CoreInterface* Core = nullptr;
    nri::PipelineLayout* Layout = nullptr;
    std::array<nri::Pipeline*, kProgramCount> Pipelines{};
    nri::Descriptor* Sampler = nullptr;
    nri::DescriptorPool* Pool = nullptr;
    std::array<nri::DescriptorSet*, kSetCount> Sets{};
#endif
    NriOwnedTexture2D TiledDepth;
    NriOwnedTexture2D Raw;
    NriOwnedTexture2D Filtered;
    NriOwnedTexture2D Output;
    std::array<VkImage, 2> Inputs{};
    std::array<VkFormat, 2> Formats{};
    uint32_t Width = 0, Height = 0;
    // Temporal mode: the motion vectors and two accumulation images, written
    // alternately (the other one is the history).
    MxaoMotionInput Motion;
    std::array<NriOwnedTexture2D, 2> History;
    uint32_t NextHistory = 0;
    bool HistoryValid = false;
    uint64_t HistoryFrameId = 0;
    float HistoryFarPlane = 0.0F;
    std::array<float, 8> HistorySettings{};
    bool LastHistoryValid = false;
    bool Ready = false;
    ResourceStateTracker PrivateStates;
    ResourceStateTracker* OutputStates = nullptr;
    EffectPassBarrierExecution LastBarriers;
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    std::string Reason = "iMMERSE MXAO is not initialized";
#else
    std::string Reason = "iMMERSE MXAO support was not compiled into this build";
#endif
};

namespace {
bool SameMotion(const MxaoMotionInput& lhs, const MxaoMotionInput& rhs) {
    return lhs.Image == rhs.Image && lhs.Format == rhs.Format && lhs.Width == rhs.Width && lhs.Height == rhs.Height;
}
} // namespace

MxaoPass::MxaoPass() : mImpl(std::make_unique<Impl>()) {
}
MxaoPass::~MxaoPass() {
    Shutdown();
}

bool MxaoPass::Initialize(VkPhysicalDevice physical, VkDevice device, NriInteropContext& interop) {
    Shutdown();
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)physical;
    (void)device;
    (void)interop;
    return false;
#else
    mImpl->Device = device;
    mImpl->Interop = &interop;
    if (!interop.Available()) {
        mImpl->Reason = interop.UnavailableReason();
        return false;
    }
    try {
        constexpr VkFormatFeatureFlags required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        for (const VkFormat format : { VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT }) {
            VkFormatProperties properties{};
            vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
            if ((properties.optimalTilingFeatures & required) != required)
                throw std::runtime_error("MXAO requires sampled/storage RG16F, R32F and RGBA16F");
        }
        auto* nriDevice = NriInteropAccess::Device(interop);
        mImpl->Core = NriInteropAccess::Core(interop);
        if (nriDevice == nullptr || mImpl->Core == nullptr)
            throw std::runtime_error("MXAO NRI device unavailable");
        const std::array<nri::DescriptorRangeDesc, 7> ranges{
            { { 0, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 1, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 2, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 3, 1, nri::DescriptorType::STORAGE_TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 4, 1, nri::DescriptorType::SAMPLER, nri::StageBits::COMPUTE_SHADER },
              { 5, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER },
              { 6, 1, nri::DescriptorType::TEXTURE, nri::StageBits::COMPUTE_SHADER } }
        };
        nri::DescriptorSetDesc set{};
        set.ranges = ranges.data();
        set.rangeNum = uint32_t(ranges.size());
        nri::RootConstantDesc root{};
        root.size = sizeof(Immerse::MxaoConstants);
        root.shaderStages = nri::StageBits::COMPUTE_SHADER;
        nri::PipelineLayoutDesc layout{};
        layout.descriptorSets = &set;
        layout.descriptorSetNum = 1;
        layout.rootConstants = &root;
        layout.rootConstantNum = 1;
        layout.shaderStages = nri::StageBits::COMPUTE_SHADER;
        layout.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;
        if (mImpl->Core->CreatePipelineLayout(*nriDevice, layout, mImpl->Layout) != nri::Result::SUCCESS)
            throw std::runtime_error("MXAO pipeline layout failed");
        const std::array<std::string, kProgramCount> sources{
            Immerse::BuildMxaoDeinterleaveShader(), Immerse::BuildMxaoOcclusionShader(),
            Immerse::BuildMxaoFilterShader(false), Immerse::BuildMxaoFilterShader(true),
            Immerse::BuildMxaoTemporalShader() };
        constexpr std::array<const char*, kProgramCount> names{
            "immerse_mxao_deinterleave.comp", "immerse_mxao_occlusion.comp", "immerse_mxao_filter.comp",
            "immerse_mxao_resolve.comp", "immerse_mxao_temporal.comp" };
        for (size_t i = 0; i < sources.size(); ++i) {
            const auto spirv = interop.Shaders().Resolve(sources[i], Renderer::SpirvStage::Compute, names[i]);
            nri::ComputePipelineDesc pipeline{};
            pipeline.pipelineLayout = mImpl->Layout;
            pipeline.shader.stage = nri::StageBits::COMPUTE_SHADER;
            pipeline.shader.bytecode = spirv.data();
            pipeline.shader.size = spirv.size() * sizeof(uint32_t);
            if (mImpl->Core->CreateComputePipeline(*nriDevice, pipeline, mImpl->Pipelines[i]) != nri::Result::SUCCESS)
                throw std::runtime_error("MXAO pipeline failed");
        }
        nri::SamplerDesc sampler{};
        sampler.filters = { nri::Filter::NEAREST, nri::Filter::NEAREST, nri::Filter::NEAREST };
        sampler.addressModes = { nri::AddressMode::CLAMP_TO_EDGE, nri::AddressMode::CLAMP_TO_EDGE,
                                 nri::AddressMode::CLAMP_TO_EDGE };
        if (mImpl->Core->CreateSampler(*nriDevice, sampler, mImpl->Sampler) != nri::Result::SUCCESS)
            throw std::runtime_error("MXAO sampler failed");
        mImpl->Ready = true;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        return false;
    }
#endif
}

bool MxaoPass::Configure(const NriEffectGraphTransientImageBinding& output, VkImage depth, VkFormat depthFormat,
                         VkImage normal, VkFormat normalFormat, const MxaoMotionInput* motion) {
    const std::array inputs{ depth, normal };
    const std::array formats{ depthFormat, normalFormat };
    constexpr auto usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (!Available() || !output.Valid() || output.Resource != EffectResource::AmbientOcclusion ||
        output.Format != VK_FORMAT_R32_SFLOAT || output.MipLevels != 1 || output.Layers != 1 ||
        output.Samples != VK_SAMPLE_COUNT_1_BIT || (output.Usage & usage) != usage ||
        output.Texture.SampledView == VK_NULL_HANDLE || output.Texture.StorageView == VK_NULL_HANDLE ||
        depth == VK_NULL_HANDLE || normal == VK_NULL_HANDLE)
        return false;
    const MxaoMotionInput temporal = motion != nullptr && motion->Image != VK_NULL_HANDLE && motion->Width != 0U &&
                                             motion->Height != 0U
                                         ? *motion
                                         : MxaoMotionInput{};
    if (mImpl->Width == output.Width && mImpl->Height == output.Height && mImpl->Output.Image == output.Texture.Image &&
        mImpl->OutputStates == output.StateTracker && mImpl->Inputs == inputs && mImpl->Formats == formats &&
        SameMotion(mImpl->Motion, temporal))
        return true;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    return false;
#else
    if (mImpl->Width != 0 && !mImpl->Interop->CompleteRecordedWork())
        return false;
    InvalidateScreenResources();
    try {
        const uint32_t tiledWidth = Immerse::MxaoTiledExtent(output.Width);
        const uint32_t tiledHeight = Immerse::MxaoTiledExtent(output.Height);
        if (!mImpl->Interop->CreateOwnedTexture2D(tiledWidth, tiledHeight, VK_FORMAT_R32_SFLOAT, mImpl->TiledDepth) ||
            !mImpl->Interop->CreateOwnedTexture2D(output.Width, output.Height, VK_FORMAT_R16G16_SFLOAT, mImpl->Raw) ||
            !mImpl->Interop->CreateOwnedTexture2D(output.Width, output.Height, VK_FORMAT_R16G16_SFLOAT,
                                                  mImpl->Filtered))
            throw std::runtime_error("MXAO scratch allocation failed");
        const bool accumulates = temporal.Image != VK_NULL_HANDLE;
        if (accumulates)
            for (auto& history : mImpl->History)
                if (!mImpl->Interop->CreateOwnedTexture2D(output.Width, output.Height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                                          history))
                    throw std::runtime_error("MXAO history allocation failed");
        mImpl->Output = output.Texture;
        mImpl->OutputStates = output.StateTracker;
        std::array<nri::Descriptor*, 2> sources{};
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (!mImpl->Interop->WrapTexture(inputs[i], formats[i], VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT,
                                             output.Width, output.Height))
                throw std::runtime_error("MXAO input wrapping failed");
            sources[i] = NriInteropAccess::TextureView(*mImpl->Interop, inputs[i], false);
            if (!sources[i])
                throw std::runtime_error("MXAO input view unavailable");
        }
        // Programs that do not read the motion vectors or a history still
        // bind a valid input there; none of them writes the native depth.
        nri::Descriptor* motionView = sources[0];
        if (accumulates) {
            if (!mImpl->Interop->WrapTexture(temporal.Image, temporal.Format, VK_IMAGE_TYPE_2D,
                                             VK_IMAGE_USAGE_SAMPLED_BIT, temporal.Width, temporal.Height))
                throw std::runtime_error("MXAO motion wrapping failed");
            motionView = NriInteropAccess::TextureView(*mImpl->Interop, temporal.Image, false);
            if (!motionView)
                throw std::runtime_error("MXAO motion view unavailable");
        }
        const size_t setCount = accumulates ? size_t(kSetCount) : size_t(kTemporalSet);
        nri::DescriptorPoolDesc pool{};
        pool.descriptorSetMaxNum = uint32_t(setCount);
        pool.textureMaxNum = uint32_t(5 * setCount);
        pool.storageTextureMaxNum = uint32_t(setCount);
        pool.samplerMaxNum = uint32_t(setCount);
        auto* device = NriInteropAccess::Device(*mImpl->Interop);
        if (mImpl->Core->CreateDescriptorPool(*device, pool, mImpl->Pool) != nri::Result::SUCCESS ||
            mImpl->Core->AllocateDescriptorSets(*mImpl->Pool, *mImpl->Layout, 0, mImpl->Sets.data(),
                                                uint32_t(setCount), 0) != nri::Result::SUCCESS)
            throw std::runtime_error("MXAO descriptor allocation failed");
        const auto view = [&](VkImage image, bool storage) {
            auto* descriptor = NriInteropAccess::TextureView(*mImpl->Interop, image, storage);
            if (!descriptor)
                throw std::runtime_error("MXAO view unavailable");
            return descriptor;
        };
        struct SetBinding {
            nri::Descriptor* Intermediate = nullptr;
            nri::Descriptor* Destination = nullptr;
            nri::Descriptor* History = nullptr;
        };
        // The deinterleave program does not read binding 2; bind an input so
        // the unused descriptor still has a valid layout.
        std::array<SetBinding, kSetCount> bindings{ {
            { sources[0], view(mImpl->TiledDepth.Image, true), sources[0] },
            { view(mImpl->TiledDepth.Image, false), view(mImpl->Raw.Image, true), sources[0] },
            { view(mImpl->Raw.Image, false), view(mImpl->Filtered.Image, true), sources[0] },
            { view(mImpl->Raw.Image, false), view(mImpl->Output.Image, true), sources[0] },
            { view(mImpl->Filtered.Image, false), view(mImpl->Output.Image, true), sources[0] },
        } };
        if (accumulates) {
            for (size_t written = 0; written < 2; ++written) {
                const VkImage accumulation = mImpl->History[written].Image;
                bindings[kTemporalSet + written] = { view(mImpl->Raw.Image, false), view(accumulation, true),
                                                     view(mImpl->History[1 - written].Image, false) };
                bindings[kFilterAccumulatedSet + written] = { view(accumulation, false),
                                                              view(mImpl->Filtered.Image, true), sources[0] };
                bindings[kResolveAccumulatedSet + written] = { view(accumulation, false),
                                                               view(mImpl->Output.Image, true), sources[0] };
            }
        }
        for (size_t i = 0; i < setCount; ++i) {
            auto* set = mImpl->Sets[i];
            const auto& binding = bindings[i];
            const std::array<nri::UpdateDescriptorRangeDesc, 7> updates{ { { set, 0, 0, &sources[0], 1 },
                                                                           { set, 1, 0, &sources[1], 1 },
                                                                           { set, 2, 0, &binding.Intermediate, 1 },
                                                                           { set, 3, 0, &binding.Destination, 1 },
                                                                           { set, 4, 0, &mImpl->Sampler, 1 },
                                                                           { set, 5, 0, &motionView, 1 },
                                                                           { set, 6, 0, &binding.History, 1 } } };
            mImpl->Core->UpdateDescriptorRanges(updates.data(), uint32_t(updates.size()));
        }
        mImpl->Width = output.Width;
        mImpl->Height = output.Height;
        mImpl->Inputs = inputs;
        mImpl->Formats = formats;
        mImpl->Motion = temporal;
        mImpl->Reason.clear();
        return true;
    } catch (const std::exception& error) {
        mImpl->Reason = error.what();
        InvalidateScreenResources();
        return false;
    }
#endif
}

bool MxaoPass::Execute(uint32_t frameSlot, const PerspectiveViewState& view, bool wBuffer,
                       const EffectsSettings& settings, const EffectPassBarrierPlan& barriers,
                       PicaSurfaceCoordinates coordinates, const MxaoTemporalFrame* temporal) {
    mImpl->LastBarriers = {};
    mImpl->LastHistoryValid = false;
    const auto* output = barriers.Find(EffectResource::AmbientOcclusion);
    if (!barriers.Valid() || !output || !output->Managed() || !mImpl->OutputStates ||
        output->DispatchAccess != ResourceAccess::ComputeWrite ||
        output->CompletionAccess != ResourceAccess::ShaderRead || !output->DownstreamRead || view.NearPlane <= 0 ||
        view.FarPlane <= view.NearPlane)
        return false;
#if !defined(ENABLE_OOT3D_NRI) || !defined(ENABLE_OOT3D_IMMERSE)
    (void)frameSlot;
    (void)settings;
    (void)coordinates;
    (void)wBuffer;
    (void)temporal;
    return false;
#else
    auto* command = NriInteropAccess::CommandBuffer(*mImpl->Interop, frameSlot);
    const bool accumulates = mImpl->Motion.Image != VK_NULL_HANDLE;
    const size_t setCount = accumulates ? size_t(kSetCount) : size_t(kTemporalSet);
    if (!command || std::any_of(mImpl->Sets.begin(), mImpl->Sets.begin() + ptrdiff_t(setCount),
                                [](auto* set) { return set == nullptr; }))
        return false;
    Immerse::MxaoConstants push;
    push.Width = mImpl->Width;
    push.Height = mImpl->Height;
    push.Method = std::min<uint32_t>(uint32_t(settings.MxaoMethod), 3U);
    const uint32_t quality = std::min<uint32_t>(settings.MxaoQuality, Immerse::kMxaoQualityPresets - 1U);
    const auto zero = coordinates.StorageToViewNdc({ 0, 0 });
    const auto x = coordinates.StorageToViewNdc({ 1, 0 });
    const auto y = coordinates.StorageToViewNdc({ 0, 1 });
    push.ViewX = { (x[0] - zero[0]) / view.Projection[0], (y[0] - zero[0]) / view.Projection[0],
                   (zero[0] + view.Projection[8]) / view.Projection[0], 0 };
    push.ViewY = { (x[1] - zero[1]) / view.Projection[5], (y[1] - zero[1]) / view.Projection[5],
                   (zero[1] + view.Projection[9]) / view.Projection[5], 0 };
    push.Depth = { wBuffer ? view.NearPlane : view.NearPlane * view.FarPlane, view.FarPlane,
                   view.FarPlane - view.NearPlane, wBuffer ? 1.0F : 0.0F };
    // The original's thickness heuristic for the visibility bitmask.
    const float radius = std::max(settings.AoRadius, 1.0e-4F);
    push.RadiusFade = { radius, std::log(1.0F + radius) / 3.0F, settings.AoFadeOutFrom, settings.AoFadeOutTo };
    // Decoding and renormalizing RGB8 normals introduces at most
    // sqrt(3)/(255-sqrt(3)) angular error; use the GTAO kernels' 2/255 bound.
    const float normalError =
        mImpl->Formats[1] == VK_FORMAT_R8G8B8A8_UNORM || mImpl->Formats[1] == VK_FORMAT_B8G8R8A8_UNORM ? 2.0F / 255.0F
        : mImpl->Formats[1] == VK_FORMAT_R16G16B16A16_SFLOAT                                           ? 1.0F / 1024.0F
                                                                                                       : 0.0F;
    push.Response = { settings.AoStrength, 1.0F / view.FarPlane, normalError * normalError, 0 };
    // Temporal mode: the history continues only from the directly preceding
    // frame of the same view and settings; the jitter phase rotates per frame.
    const bool temporalFrame = accumulates && temporal != nullptr && temporal->View != nullptr;
    const std::array<float, 8> historySettings{ float(push.Method), float(quality), radius, settings.AoStrength,
                                                settings.AoFadeOutFrom, settings.AoFadeOutTo,
                                                wBuffer ? 1.0F : 0.0F, normalError };
    uint32_t phase = 0U;
    if (temporalFrame) {
        const auto& state = *temporal->View;
        const bool continuous = mImpl->HistoryValid && state.HistoryValid && !state.CameraCut &&
                                temporal->FrameId == mImpl->HistoryFrameId + 1U &&
                                mImpl->HistorySettings == historySettings;
        push.Temporal = { continuous ? 1.0F : 0.0F, Immerse::kMxaoTemporalHistoryLength,
                          Immerse::kMxaoTemporalDepthTolerance, Immerse::kMxaoTemporalMovingHistoryLength };
        if (continuous)
            push.PreviousDepth = Immerse::MxaoPreviousDepthRow(state.PreviousWorldToClip, state.InverseCurrentWorldToClip,
                                                      view.Projection, mImpl->HistoryFarPlane);
        phase = Immerse::MxaoTemporalPhase(temporal->FrameId);
        mImpl->LastHistoryValid = continuous;
    }
    mImpl->LastBarriers = barriers.BeginOutputExecution(EffectResource::AmbientOcclusion);
    const auto transition = [&](VkImage image, ResourceStateTracker& states, ResourceAccess access, bool graph) {
        const auto change = states.PlanTransition(reinterpret_cast<uintptr_t>(image), { access, 0 });
        if (!NriInteropAccess::CmdTextureBarrier(*mImpl->Interop, frameSlot, image, change))
            return false;
        states.Commit(change);
        if (graph)
            mImpl->LastBarriers.RecordGraphTransition(change);
        else
            mImpl->LastBarriers.RecordPrivateTransition(change);
        return true;
    };
    mImpl->Core->CmdSetDescriptorPool(*command, *mImpl->Pool);
    mImpl->Core->CmdSetPipelineLayout(*command, nri::BindPoint::COMPUTE, *mImpl->Layout);
    const auto dispatch = [&](size_t program, size_t set, uint32_t flags, uint32_t width, uint32_t height,
                              uint32_t group) {
        push.Flags = quality | (phase << Immerse::kMxaoJitterPhaseShift) | flags;
        mImpl->Core->CmdSetPipeline(*command, *mImpl->Pipelines[program]);
        const nri::SetDescriptorSetDesc descriptors{ 0, mImpl->Sets[set] };
        mImpl->Core->CmdSetDescriptorSet(*command, descriptors);
        const nri::SetRootConstantsDesc constants{ 0, &push, sizeof(push) };
        mImpl->Core->CmdSetRootConstants(*command, constants);
        mImpl->Core->CmdDispatch(*command, { (width + group - 1) / group, (height + group - 1) / group, 1 });
    };
    const uint32_t tiledWidth = Immerse::MxaoTiledExtent(mImpl->Width);
    const uint32_t tiledHeight = Immerse::MxaoTiledExtent(mImpl->Height);
    if (!transition(mImpl->TiledDepth.Image, mImpl->PrivateStates, ResourceAccess::ComputeWrite, false))
        return false;
    dispatch(kDeinterleaveProgram, kDeinterleaveSet, 0, tiledWidth, tiledHeight, 8);
    if (!transition(mImpl->TiledDepth.Image, mImpl->PrivateStates, ResourceAccess::ShaderRead, false) ||
        !transition(mImpl->Raw.Image, mImpl->PrivateStates, ResourceAccess::ComputeWrite, false))
        return false;
    dispatch(kOcclusionProgram, kOcclusionSet, 0, tiledWidth, tiledHeight, 16);
    if (!transition(mImpl->Raw.Image, mImpl->PrivateStates, ResourceAccess::ShaderRead, false))
        return false;
    // The filter reads this frame's raw estimate, or its accumulation.
    size_t filterSet = kFilterSet;
    size_t resolveSet = kResolveRawSet;
    if (temporalFrame) {
        const uint32_t written = mImpl->NextHistory;
        if (!transition(mImpl->History[written].Image, mImpl->PrivateStates, ResourceAccess::ComputeWrite, false) ||
            !transition(mImpl->History[1U - written].Image, mImpl->PrivateStates, ResourceAccess::ShaderRead, false))
            return false;
        dispatch(kTemporalProgram, kTemporalSet + written, 0, mImpl->Width, mImpl->Height, 8);
        if (!transition(mImpl->History[written].Image, mImpl->PrivateStates, ResourceAccess::ShaderRead, false))
            return false;
        filterSet = kFilterAccumulatedSet + written;
        resolveSet = kResolveAccumulatedSet + written;
        mImpl->NextHistory = 1U - written;
        mImpl->HistoryValid = true;
        mImpl->HistoryFrameId = temporal->FrameId;
        mImpl->HistoryFarPlane = view.FarPlane;
        mImpl->HistorySettings = historySettings;
    } else {
        mImpl->HistoryValid = false;
    }
    // Filter quality 2 runs both regression iterations, 1 only the mirrored
    // one, 0 publishes the raw estimate.
    const uint32_t filter = std::min<uint32_t>(settings.MxaoFilter, 2U);
    if (filter == 2U) {
        if (!transition(mImpl->Filtered.Image, mImpl->PrivateStates, ResourceAccess::ComputeWrite, false))
            return false;
        dispatch(kFilterProgram, filterSet, 0, mImpl->Width, mImpl->Height, 8);
        if (!transition(mImpl->Filtered.Image, mImpl->PrivateStates, ResourceAccess::ShaderRead, false))
            return false;
        resolveSet = kResolveFilteredSet;
    }
    if (!transition(mImpl->Output.Image, *mImpl->OutputStates, output->DispatchAccess, true))
        return false;
    dispatch(kResolveProgram, resolveSet,
             filter == 0U ? Immerse::kMxaoFilterCopy : Immerse::kMxaoFilterMirrored, mImpl->Width, mImpl->Height, 8);
    return transition(mImpl->Output.Image, *mImpl->OutputStates, output->CompletionAccess, true);
#endif
}

void MxaoPass::InvalidateScreenResources() {
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    if (mImpl->Core && mImpl->Pool)
        mImpl->Core->DestroyDescriptorPool(mImpl->Pool);
    mImpl->Pool = nullptr;
    mImpl->Sets = {};
#endif
    if (mImpl->Interop) {
        mImpl->Interop->DestroyOwnedTexture(mImpl->TiledDepth.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Raw.Image);
        mImpl->Interop->DestroyOwnedTexture(mImpl->Filtered.Image);
        for (const auto& history : mImpl->History)
            mImpl->Interop->DestroyOwnedTexture(history.Image);
    }
    mImpl->TiledDepth = {};
    mImpl->Raw = {};
    mImpl->Filtered = {};
    mImpl->History = {};
    mImpl->Output = {};
    mImpl->OutputStates = nullptr;
    mImpl->Width = mImpl->Height = 0;
    mImpl->Inputs = {};
    mImpl->Formats = {};
    mImpl->Motion = {};
    mImpl->NextHistory = 0;
    mImpl->HistoryValid = false;
    mImpl->LastHistoryValid = false;
    mImpl->PrivateStates.Clear();
    mImpl->LastBarriers = {};
}
void MxaoPass::Shutdown() {
    InvalidateScreenResources();
#if defined(ENABLE_OOT3D_NRI) && defined(ENABLE_OOT3D_IMMERSE)
    if (mImpl->Core) {
        if (mImpl->Sampler)
            mImpl->Core->DestroyDescriptor(mImpl->Sampler);
        for (auto* pipeline : mImpl->Pipelines)
            if (pipeline)
                mImpl->Core->DestroyPipeline(pipeline);
        if (mImpl->Layout)
            mImpl->Core->DestroyPipelineLayout(mImpl->Layout);
    }
#endif
    *mImpl = {};
}
bool MxaoPass::Available() const {
    return mImpl->Ready;
}
VkImage MxaoPass::OutputImage() const {
    return mImpl->Output.Image;
}
VkImageView MxaoPass::OutputView() const {
    return mImpl->Output.SampledView;
}
bool MxaoPass::OutputOwnedByNri() const {
    return mImpl->Interop && mImpl->Interop->OwnsTexture(mImpl->Output.Image);
}
bool MxaoPass::LastHistoryValid() const {
    return mImpl->LastHistoryValid;
}
const EffectPassBarrierExecution& MxaoPass::LastBarrierExecution() const {
    return mImpl->LastBarriers;
}
const std::string& MxaoPass::UnavailableReason() const {
    return mImpl->Reason;
}
} // namespace Fast::Oot3d
#endif
