#pragma once
#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/effect_graph_pass_barriers.h"
#include "fast/oot3d/pica_surface_coordinates.h"
#include "fast/oot3d/scene_view_runtime.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <string>

namespace Fast::Oot3d {
class NriInteropContext;

// iMMERSE LAUNCHPAD normal smoothing (see fast/immerse/launchpad_normals_shader.h).
// It refines the native normal guide the AO providers read into a private
// RGBA16F guide with the same encoding and coverage. It runs inside the AO
// stage on that stage's declared inputs and has no history.
class LaunchpadNormalsPass final {
  public:
    LaunchpadNormalsPass();
    ~LaunchpadNormalsPass();
    LaunchpadNormalsPass(const LaunchpadNormalsPass&) = delete;
    LaunchpadNormalsPass& operator=(const LaunchpadNormalsPass&) = delete;
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop);
    bool Configure(uint32_t width, uint32_t height, VkImage depth, VkFormat depthFormat, VkImage normal,
                   VkFormat normalFormat);
    // Leaves the output shader-readable for the provider that follows.
    bool Execute(uint32_t frameSlot, const PerspectiveViewState& view, bool wBuffer,
                 PicaSurfaceCoordinates coordinates);
    void InvalidateScreenResources();
    void Shutdown();
    [[nodiscard]] bool Available() const;
    [[nodiscard]] VkImage OutputImage() const;
    [[nodiscard]] VkImageView OutputView() const;
    [[nodiscard]] VkFormat OutputFormat() const;
    [[nodiscard]] const EffectPassBarrierExecution& LastBarrierExecution() const;
    [[nodiscard]] const std::string& UnavailableReason() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace Fast::Oot3d
#endif
