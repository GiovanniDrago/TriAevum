#pragma once

// Screen-space diffuse GI provider of the GlobalIllumination display pass
// (Graphics.Effects.GlobalIllumination.Mode = ScreenSpace;
// docs/TRIAEVUM_SCREEN_SPACE_GI.md, "M5"). OOT3D glue around the
// title-neutral pass (fast/renderer/diffuse_gi_pass.h):
// - pipelines are created off the frame, once per device, on a worker thread
//   started by the first frame that requests the mode; until they are ready
//   the provider reports itself unavailable with the reason and the display
//   plan declares no GI pass;
// - one pass instance per composed scene surface (at most kMaxSurfaces, the
//   least recently used retired after the recorded work completed), so every
//   surface keeps its own history; a second composition of a surface in the
//   same frame republishes the frame's output without advancing the history;
// - the pass runs at the work scale of GlobalIllumination.Resolution (Half:
//   1/2, Quarter: 1/4 per axis) and is upsampled to the guide extent;
// - the DiffuseIllumination output is the effect graph's arena image: its
//   transitions follow the graph's barrier plan and state tracker (NRI), the
//   pass records only its private barriers; A is not composed (no consumer).

#include "fast/oot3d/diffuse_gi_adapter.h"
#include "fast/oot3d/graphics_settings.h"

#include <cstdint>
#include <string>

namespace Fast::Oot3d {

// The runtime pass settings of the persisted screen-space GI settings.
[[nodiscard]] Renderer::DiffuseGi::PassSettings ScreenSpaceGiPassSettings(const GlobalIlluminationSettings& settings);
// Work-scale factor of GlobalIllumination.Resolution (Half 2, Quarter 4).
[[nodiscard]] uint32_t ScreenSpaceGiWorkFactor(GlobalIlluminationResolution resolution) noexcept;

// Declared VRAM budget of the default (Resolution Half) per composed surface,
// in bytes per guide texel: every pass buffer at the work extent (392 bytes
// per work pixel, a quarter of the guide texels) plus the two images the
// upsampler needs at the guide extent (output guide 8, DiffuseIllumination
// 8), with room for the rounding of odd extents (docs/TRIAEVUM_SCREEN_SPACE_GI.md,
// "M5 Stage B", open point 24). The full-extent pass (Stage A) needs 404.
inline constexpr uint32_t kScreenSpaceGiDefaultBudgetBytesPerTexel = 120U;

// Bytes the mode allocates for one composed surface of this guide extent
// (pass buffers, output guide and the DiffuseIllumination image).
[[nodiscard]] uint64_t ScreenSpaceGiSurfaceBytes(uint32_t width, uint32_t height,
                                                 const GlobalIlluminationSettings& settings);

} // namespace Fast::Oot3d

#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/effect_graph_pass_barriers.h"

#include <array>
#include <future>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace Fast::Oot3d {

class NriInteropContext;
struct NriEffectGraphTransientImageBinding;

struct ScreenSpaceGiSurface {
    uint64_t Namespace = 0;
    uint64_t Color = 0;
    uint64_t Depth = 0;
    bool operator==(const ScreenSpaceGiSurface&) const = default;
};

struct ScreenSpaceGiFrame {
    uint64_t FrameId = 0;  // presented sample
    uint32_t FrameSlot = 0;
    ScreenSpaceGiSurface Surface;
    uint32_t Width = 0, Height = 0; // guide extent
    DiffuseGiGuideViews Guides;
    bool WBuffer = false;
    const PerspectiveViewState* View = nullptr;
    const TemporalViewState* Temporal = nullptr;
    PicaSurfaceCoordinates Coordinates;
    std::array<float, 2> JitterUv{}; // storage UV of the guide target
};

struct ScreenSpaceGiStats {
    bool PipelinesReady = false;
    double PipelineMilliseconds = 0.0;  // worker time to create the ten pipelines
    double FrameMilliseconds = 0.0;     // the requesting frame's share (pack lookup, worker launch)
    double AllocationMilliseconds = 0.0; // the last Configure that created or replaced pass resources
    uint64_t InstancesCreated = 0, InstancesRetired = 0;
    uint64_t Executions = 0, Republications = 0, Clears = 0;
    uint64_t ResetFrames = 0, Reconfigurations = 0, HistoryResets = 0, CameraTransformFallbacks = 0;
    uint32_t Instances = 0;
    uint64_t BufferBytes = 0;           // all instances
    uint32_t LastResetReasons = 0;
};

class ScreenSpaceGiProvider final {
  public:
    static constexpr uint32_t kMaxSurfaces = 3;

    ScreenSpaceGiProvider();
    ~ScreenSpaceGiProvider();
    ScreenSpaceGiProvider(const ScreenSpaceGiProvider&) = delete;
    ScreenSpaceGiProvider& operator=(const ScreenSpaceGiProvider&) = delete;

    // Whether the device can run the pass (no Vulkan object is created).
    [[nodiscard]] static bool Supported(VkPhysicalDevice physical, std::string& reason);
    void Initialize(VkPhysicalDevice physical, VkDevice device, NriInteropContext& interop, uint32_t framesInFlight);
    // Starts the pipeline creation off the frame the first time; true once the
    // pipelines are ready. Never blocks.
    bool Prepare();
    [[nodiscard]] bool Ready() const noexcept { return mReady; }

    bool Execute(VkCommandBuffer command, const ScreenSpaceGiFrame& frame, const GlobalIlluminationSettings& settings,
                 const NriEffectGraphTransientImageBinding& output, const EffectPassBarrierPlan& barriers);
    // Neutral output (zero irradiance) for a frame without GI.
    bool ClearOutput(VkCommandBuffer command, const ScreenSpaceGiFrame& frame,
                     const GlobalIlluminationSettings& settings, const NriEffectGraphTransientImageBinding& output,
                     const EffectPassBarrierPlan& barriers);
    // A depth-aspect sampled view of a native depth image (the interop's
    // view of the image, released with it); null when unavailable.
    [[nodiscard]] VkImageView SampledDepthView(VkImage image, VkFormat format, uint32_t width, uint32_t height);
    void ResetHistory() noexcept;
    // Drops every instance; the caller completed its recorded work.
    void InvalidateScreenResources() noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] const std::string& UnavailableReason() const noexcept { return mReason; }
    [[nodiscard]] const ScreenSpaceGiStats& Stats() const noexcept { return mStats; }
    [[nodiscard]] const EffectPassBarrierExecution& LastBarrierExecution() const noexcept { return mBarriers; }

  private:
    struct Instance;
    Instance* InstanceFor(const ScreenSpaceGiFrame& frame);
    bool ConfigureInstance(Instance& instance, const ScreenSpaceGiFrame& frame,
                           const GlobalIlluminationSettings& settings,
                           const NriEffectGraphTransientImageBinding& output);
    bool Transition(uint32_t frameSlot, const NriEffectGraphTransientImageBinding& output, ResourceAccess access);
    void UpdateStats() noexcept;

    VkPhysicalDevice mPhysical = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    NriInteropContext* mInterop = nullptr;
    uint32_t mFramesInFlight = 2;
    std::shared_ptr<Renderer::DiffuseGi::DiffuseGiKernels> mKernels;
    std::shared_ptr<DiffuseGiAdapterKernel> mAdapter;
    std::future<bool> mCreation;
    bool mStarted = false, mReady = false, mFailed = false;
    std::vector<std::unique_ptr<Instance>> mInstances;
    uint64_t mUseCounter = 0;
    ScreenSpaceGiStats mStats;
    EffectPassBarrierExecution mBarriers;
    std::string mReason = "not initialized";
};

} // namespace Fast::Oot3d

#endif
