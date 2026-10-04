#include "fast/oot3d/screen_space_gi_provider.h"

// Screen-space diffuse GI provider: own glue code around the title-neutral
// pass. Plumbing follows the repository's providers (per-surface instances,
// graph-declared output transitions through the arena's state tracker,
// recorded-work retirement before replacing resources, a lifecycle line on
// stderr); nothing here is a GI algorithm.

#include <algorithm>
#include <chrono>
#include <cstdio>

#ifdef ENABLE_OOT3D_VULKAN
#include "fast/oot3d/nri_effect_graph_transient_image_arena.h"
#include "fast/oot3d/nri_interop_context.h"
#ifdef ENABLE_OOT3D_NRI
#include "nri_interop_internal.h"
#endif
#endif

namespace Fast::Oot3d {

Renderer::DiffuseGi::PassSettings ScreenSpaceGiPassSettings(const GlobalIlluminationSettings& settings) {
    Renderer::DiffuseGi::PassSettings s = Renderer::DiffuseGi::DefaultPassSettings();
    const ScreenSpaceGiSettings& ss = settings.ScreenSpace;
    s.Gather.Radius = ss.Radius;
    s.Gather.Thickness = ss.Thickness;
    s.Gather.Sections = ss.Sections;
    s.Gather.Sectors = ss.Sectors;
    s.Gather.CandidatesPerSide = ss.RadialCandidates;
    s.Temporal.Enabled = ss.Temporal;
    s.Filter.Levels = ss.FilterLevels;
    return s;
}

uint32_t ScreenSpaceGiWorkFactor(GlobalIlluminationResolution resolution) noexcept {
    return resolution == GlobalIlluminationResolution::Quarter ? 4U : 2U;
}

uint64_t ScreenSpaceGiSurfaceBytes(uint32_t width, uint32_t height, const GlobalIlluminationSettings& settings) {
    const auto extent =
        Renderer::DiffuseGi::MakeWorkScaleExtent(width, height, ScreenSpaceGiWorkFactor(settings.Resolution));
    const auto plan = Renderer::DiffuseGi::PlanPassResources(extent, ScreenSpaceGiPassSettings(settings), {}, 256U);
    // The pass buffers (output guide included) and the RGBA16F output image.
    return plan.PassBytes() + 8ULL * width * height;
}

} // namespace Fast::Oot3d

#ifdef ENABLE_OOT3D_VULKAN
namespace Fast::Oot3d {

namespace DGI = Renderer::DiffuseGi;

struct ScreenSpaceGiProvider::Instance {
    ScreenSpaceGiSurface Surface;
    DGI::DiffuseGiPass Pass;
    VkDescriptorPool Pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> AdapterSets; // one per frame slot
    DGI::PassExtent Extent;
    uint64_t LastFrameId = 0;
    bool HasFrame = false;
    uint64_t LastUse = 0;
};

ScreenSpaceGiProvider::ScreenSpaceGiProvider() = default;
ScreenSpaceGiProvider::~ScreenSpaceGiProvider() { Shutdown(); }

bool ScreenSpaceGiProvider::Supported(VkPhysicalDevice physical, std::string& reason) {
    return DGI::DiffuseGiPassSupported(physical, reason);
}

void ScreenSpaceGiProvider::Initialize(VkPhysicalDevice physical, VkDevice device, NriInteropContext& interop,
                                       uint32_t framesInFlight) {
    Shutdown();
    mPhysical = physical;
    mDevice = device;
    mInterop = &interop;
    mFramesInFlight = std::max(framesInFlight, 1U);
    mReason = "pipelines not created";
}

bool ScreenSpaceGiProvider::Prepare() {
    if (mReady) return true;
    if (mFailed || mInterop == nullptr) return false;
    if (!mStarted) {
        mStarted = true;
        const auto start = std::chrono::steady_clock::now();
        std::string reason;
        if (!Supported(mPhysical, reason)) {
            mFailed = true;
            mReason = reason;
            return false;
        }
        // SPIR-V from the precompiled pack on this thread (the pass shader
        // compiler is not shared with the worker); pipelines on the worker.
        DGI::PassKernelBinaries binaries = DGI::DiffuseGiKernels::Resolve(mInterop->Shaders(), reason);
        std::vector<uint32_t> adapter;
        if (reason.empty()) {
            try {
                adapter = mInterop->Shaders().Resolve(BuildDiffuseGiAdapterShader(), Renderer::SpirvStage::Compute,
                                                      "oot3d_diffuse_gi_inputs.comp");
            } catch (const std::exception& error) {
                reason = error.what();
            }
        }
        if (!reason.empty()) {
            mFailed = true;
            mReason = "kernels unavailable: " + reason;
            return false;
        }
        mKernels = std::make_shared<DGI::DiffuseGiKernels>();
        mAdapter = std::make_shared<DiffuseGiAdapterKernel>();
        const DGI::PassDevice device{mPhysical, mDevice, VK_NULL_HANDLE};
        mCreation = std::async(std::launch::async, [kernels = mKernels, adapterKernel = mAdapter, device,
                                                    binaries = std::move(binaries), spirv = std::move(adapter)] {
            return kernels->Create(device, binaries) && adapterKernel->Create(device.Device, spirv);
        });
        mReason = "creating pipelines";
        // The frame's own share: resolving the pack modules and launching the worker.
        mStats.FrameMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
    if (mCreation.valid() && mCreation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool created = mCreation.get();
        if (!created) {
            mFailed = true;
            mReason = "pipeline creation failed: " +
                      (mKernels->Complete() ? std::string("adapter kernel") : mKernels->Reason());
            return false;
        }
        mReady = true;
        mReason.clear();
        mStats.PipelinesReady = true;
        mStats.PipelineMilliseconds = mKernels->CreationMilliseconds() + mAdapter->CreationMilliseconds();
        std::fprintf(stderr,
                     "TRIAEVUM_SCREEN_SPACE_GI pipelines_ready=1 worker_ms=%.1f gather_ms=%.1f upsample_ms=%.1f "
                     "adapter_ms=%.1f frame_ms=%.2f\n",
                     mStats.PipelineMilliseconds,
                     mKernels->KernelMilliseconds()[static_cast<size_t>(DGI::PassKernel::Gather)],
                     mKernels->KernelMilliseconds()[static_cast<size_t>(DGI::PassKernel::Upsample)],
                     mAdapter->CreationMilliseconds(), mStats.FrameMilliseconds);
    }
    return mReady;
}

ScreenSpaceGiProvider::Instance* ScreenSpaceGiProvider::InstanceFor(const ScreenSpaceGiFrame& frame) {
    for (auto& instance : mInstances)
        if (instance->Surface == frame.Surface) {
            instance->LastUse = ++mUseCounter;
            return instance.get();
        }
    if (mInstances.size() >= kMaxSurfaces) {
        // Retire the least recently used surface not composed in this frame.
        auto victim = mInstances.end();
        for (auto it = mInstances.begin(); it != mInstances.end(); ++it)
            if (!((*it)->HasFrame && (*it)->LastFrameId == frame.FrameId) &&
                (victim == mInstances.end() || (*it)->LastUse < (*victim)->LastUse))
                victim = it;
        if (victim == mInstances.end() || !mInterop->CompleteRecordedWork()) return nullptr;
        (*victim)->Pass.Shutdown();
        if ((*victim)->Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(mDevice, (*victim)->Pool, nullptr);
        mInstances.erase(victim);
        ++mStats.InstancesRetired;
    }
    auto instance = std::make_unique<Instance>();
    instance->Surface = frame.Surface;
    instance->LastUse = ++mUseCounter;
    if (!instance->Pass.Initialize(DGI::PassDevice{mPhysical, mDevice, VK_NULL_HANDLE}, mKernels)) {
        mReason = instance->Pass.UnavailableReason();
        return nullptr;
    }
    const std::array<VkDescriptorPoolSize, 2> sizes{{
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4U * mFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 6U * mFramesInFlight},
    }};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = mFramesInFlight;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(mDevice, &pool, nullptr, &instance->Pool) != VK_SUCCESS) {
        mReason = "adapter descriptor pool";
        return nullptr;
    }
    instance->AdapterSets.resize(mFramesInFlight, VK_NULL_HANDLE);
    const std::vector<VkDescriptorSetLayout> layouts(mFramesInFlight, mAdapter->SetLayout());
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool = instance->Pool;
    allocation.descriptorSetCount = mFramesInFlight;
    allocation.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(mDevice, &allocation, instance->AdapterSets.data()) != VK_SUCCESS) {
        vkDestroyDescriptorPool(mDevice, instance->Pool, nullptr);
        mReason = "adapter descriptor sets";
        return nullptr;
    }
    mInstances.push_back(std::move(instance));
    ++mStats.InstancesCreated;
    std::fprintf(stderr, "TRIAEVUM_SCREEN_SPACE_GI frame=%llu instances_created=%llu retired=%llu\n",
                 static_cast<unsigned long long>(frame.FrameId), static_cast<unsigned long long>(mStats.InstancesCreated),
                 static_cast<unsigned long long>(mStats.InstancesRetired));
    return mInstances.back().get();
}

bool ScreenSpaceGiProvider::ConfigureInstance(Instance& instance, const ScreenSpaceGiFrame& frame,
                                              const GlobalIlluminationSettings& settings,
                                              const NriEffectGraphTransientImageBinding& output) {
    const DGI::PassExtent extent =
        DGI::MakeWorkScaleExtent(frame.Width, frame.Height, ScreenSpaceGiWorkFactor(settings.Resolution));
    const DGI::PassSettings passSettings = ScreenSpaceGiPassSettings(settings);
    DGI::PassOutputs outputs;
    outputs.Illumination = {output.Texture.Image, output.Texture.StorageView, output.Format, output.Width,
                            output.Height};
    const auto start = std::chrono::steady_clock::now();
    const bool configured = instance.Pass.Available();
    const bool replaces = configured && instance.Pass.ReplacesResources(extent, passSettings, {}, outputs);
    const bool allocates = !configured || replaces;
    if (replaces) {
        // Resources that recorded commands may still use are replaced only
        // after that work has executed.
        if (!mInterop->CompleteRecordedWork()) {
            mReason = "recorded work did not complete";
            return false;
        }
        instance.HasFrame = false;
        ++mStats.Reconfigurations;
    }
    if (!instance.Pass.Configure(extent, passSettings, {}, outputs)) {
        mReason = instance.Pass.UnavailableReason();
        return false;
    }
    if (allocates)
        mStats.AllocationMilliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    instance.Extent = extent;
    return true;
}

bool ScreenSpaceGiProvider::Transition(uint32_t frameSlot, const NriEffectGraphTransientImageBinding& output,
                                       ResourceAccess access) {
#ifdef ENABLE_OOT3D_NRI
    const ResourceTransition transition =
        output.StateTracker->PlanTransition(reinterpret_cast<uintptr_t>(output.Texture.Image), {access, 0});
    if (!NriInteropAccess::CmdTextureBarrier(*mInterop, frameSlot, output.Texture.Image, transition)) return false;
    mBarriers.RecordGraphTransition(transition);
    output.StateTracker->Commit(transition);
    return true;
#else
    (void)frameSlot;
    (void)output;
    (void)access;
    return false;
#endif
}

bool ScreenSpaceGiProvider::Execute(VkCommandBuffer command, const ScreenSpaceGiFrame& frame,
                                    const GlobalIlluminationSettings& settings,
                                    const NriEffectGraphTransientImageBinding& output,
                                    const EffectPassBarrierPlan& barriers) {
    mBarriers = barriers.BeginExecution();
    if (!Prepare()) return false;
    const auto* declared = barriers.Find(EffectResource::DiffuseIllumination);
    if (declared == nullptr || !declared->Managed() || !output.Valid() || output.StateTracker == nullptr ||
        output.Texture.StorageView == VK_NULL_HANDLE || frame.View == nullptr || frame.Temporal == nullptr) {
        mReason = "GlobalIllumination output or view undeclared";
        return false;
    }
    Instance* instance = InstanceFor(frame);
    if (instance == nullptr || !ConfigureInstance(*instance, frame, settings, output)) return false;
    const bool republish = instance->HasFrame && instance->LastFrameId == frame.FrameId;
    if (!Transition(frame.FrameSlot, output, declared->DispatchAccess)) {
        mReason = "output transition failed";
        return false;
    }
    DGI::PassImageState illumination, accessibility;
    DGI::PassOutputAccess access;
    access.ExternalTransitions = true;
    DGI::PassBarrierRecord record;
    bool executed = false;
    if (republish) {
        executed = instance->Pass.RepublishOutputs(command, illumination, accessibility, access, record);
        if (executed) ++mStats.Republications;
    } else {
        const DGI::DepthCamera camera =
            DiffuseGiGuideCamera(*frame.View, frame.Coordinates, frame.WBuffer, frame.JitterUv);
        const DiffuseGiAdapterParams params = MakeDiffuseGiAdapterParams(instance->Extent, camera);
        DGI::PassFrame passFrame;
        passFrame.Camera = camera;
        passFrame.NormalSource = DGI::GeometricNormalSource::RendererFaceNormal;
        passFrame.FrameIndex = frame.FrameId;
        bool cut = !frame.Temporal->HistoryValid || frame.Temporal->CameraCut;
        if (!cut) {
            if (const auto transform = DiffuseGiCurrentToPrevious(frame.View->Projection, *frame.Temporal))
                passFrame.CurrentToPrevious = *transform;
            else {
                cut = true;
                ++mStats.CameraTransformFallbacks;
            }
        }
        passFrame.CameraCut = cut;
        passFrame.JitterX = frame.JitterUv[0] * static_cast<double>(frame.Width);
        passFrame.JitterY = frame.JitterUv[1] * static_cast<double>(frame.Height);
        passFrame.Motion = {DGI::MotionDirection::CurrentToPrevious, DGI::MotionUnits::NormalizedViewportUv,
                            DGI::JitterInMotion::Included};
        passFrame.Validation = DGI::HistoryValidation::FlowAndDepth;
        const VkDescriptorSet set = instance->AdapterSets[frame.FrameSlot % instance->AdapterSets.size()];
        executed = RecordDiffuseGiAdapter(mDevice, command, *mAdapter, set, frame.Guides, instance->Pass, params) &&
                   instance->Pass.Execute(command, passFrame, illumination, accessibility, access, record);
        if (executed) {
            instance->LastFrameId = frame.FrameId;
            instance->HasFrame = true;
            ++mStats.Executions;
            mStats.LastResetReasons = instance->Pass.Stats().LastResetReasons;
            if (mStats.LastResetReasons != 0U) ++mStats.ResetFrames;
        } else {
            mReason = instance->Pass.UnavailableReason().empty() ? "adapter inputs unavailable"
                                                                 : instance->Pass.UnavailableReason();
        }
    }
    // The pass's private barriers, counted with the graph's.
    for (uint32_t k = 0; k < record.PrivateBarriers; ++k)
        mBarriers.RecordPrivateTransition({0U, {ResourceAccess::ComputeWrite, 0U}, {ResourceAccess::ShaderRead, 0U}});
    const bool completed = Transition(frame.FrameSlot, output, declared->CompletionAccess);
    UpdateStats();
    return executed && completed;
}

bool ScreenSpaceGiProvider::ClearOutput(VkCommandBuffer command, const ScreenSpaceGiFrame& frame,
                                        const GlobalIlluminationSettings& settings,
                                        const NriEffectGraphTransientImageBinding& output,
                                        const EffectPassBarrierPlan& barriers) {
    mBarriers = barriers.BeginExecution();
    if (!mReady) return false;
    const auto* declared = barriers.Find(EffectResource::DiffuseIllumination);
    if (declared == nullptr || !declared->Managed() || !output.Valid() || output.StateTracker == nullptr ||
        output.Texture.StorageView == VK_NULL_HANDLE)
        return false;
    Instance* instance = InstanceFor(frame);
    if (instance == nullptr || !ConfigureInstance(*instance, frame, settings, output)) return false;
    if (!Transition(frame.FrameSlot, output, declared->DispatchAccess)) return false;
    DGI::PassImageState illumination, accessibility;
    DGI::PassOutputAccess access;
    access.ExternalTransitions = true;
    DGI::PassBarrierRecord record;
    const bool cleared = instance->Pass.ClearOutputs(command, illumination, accessibility, access, record);
    instance->HasFrame = false;
    for (uint32_t k = 0; k < record.PrivateBarriers; ++k)
        mBarriers.RecordPrivateTransition({0U, {ResourceAccess::ComputeWrite, 0U}, {ResourceAccess::ShaderRead, 0U}});
    const bool completed = Transition(frame.FrameSlot, output, declared->CompletionAccess);
    if (cleared) ++mStats.Clears;
    UpdateStats();
    return cleared && completed;
}

VkImageView ScreenSpaceGiProvider::SampledDepthView(VkImage image, VkFormat format, uint32_t width,
                                                    uint32_t height) {
#ifdef ENABLE_OOT3D_NRI
    if (mInterop == nullptr || image == VK_NULL_HANDLE ||
        !mInterop->WrapTexture(image, format, VK_IMAGE_TYPE_2D, VK_IMAGE_USAGE_SAMPLED_BIT, width, height))
        return VK_NULL_HANDLE;
    nri::Descriptor* view = NriInteropAccess::TextureView(*mInterop, image, false);
    nri::CoreInterface* core = NriInteropAccess::Core(*mInterop);
    if (view == nullptr || core == nullptr) return VK_NULL_HANDLE;
    return reinterpret_cast<VkImageView>(core->GetDescriptorNativeObject(view));
#else
    (void)image;
    (void)format;
    (void)width;
    (void)height;
    return VK_NULL_HANDLE;
#endif
}

void ScreenSpaceGiProvider::ResetHistory() noexcept {
    for (auto& instance : mInstances) {
        instance->Pass.ResetHistory();
        instance->HasFrame = false;
    }
    ++mStats.HistoryResets;
}

void ScreenSpaceGiProvider::InvalidateScreenResources() noexcept {
    for (auto& instance : mInstances) {
        instance->Pass.Shutdown();
        if (instance->Pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(mDevice, instance->Pool, nullptr);
        ++mStats.InstancesRetired;
    }
    mInstances.clear();
    UpdateStats();
}

void ScreenSpaceGiProvider::Shutdown() noexcept {
    if (mCreation.valid()) mCreation.wait();
    mCreation = {};
    if (mDevice != VK_NULL_HANDLE) InvalidateScreenResources();
    mInstances.clear();
    mAdapter.reset();
    mKernels.reset();
    mInterop = nullptr;
    mStarted = mReady = mFailed = false;
    mStats.PipelinesReady = false;
    mReason = "not initialized";
}

void ScreenSpaceGiProvider::UpdateStats() noexcept {
    mStats.Instances = static_cast<uint32_t>(mInstances.size());
    mStats.BufferBytes = 0;
    for (const auto& instance : mInstances) mStats.BufferBytes += instance->Pass.Stats().BufferBytes;
}

} // namespace Fast::Oot3d
#endif
