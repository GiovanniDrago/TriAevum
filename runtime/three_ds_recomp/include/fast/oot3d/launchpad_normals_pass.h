#pragma once
#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/effect_graph_pass_barriers.h"
#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/pica_surface_coordinates.h"
#include "fast/oot3d/scene_view_runtime.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <string>

namespace Fast::Oot3d {
class NriInteropContext;

// iMMERSE LAUNCHPAD normals (see fast/immerse/launchpad_normals_shader.h): the
// smoothing and the texture detail of the AO normals. It refines the native
// normal guide the AO providers read into a private RGBA16F guide with the same
// encoding and coverage. It runs inside the AO stage on that stage's declared
// inputs (and the albedo estimate derived from its scene colour) and has no
// history.
class LaunchpadNormalsPass final {
  public:
    LaunchpadNormalsPass();
    ~LaunchpadNormalsPass();
    LaunchpadNormalsPass(const LaunchpadNormalsPass&) = delete;
    LaunchpadNormalsPass& operator=(const LaunchpadNormalsPass&) = delete;
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop);
    // albedo: the R16F albedo estimate for texture detail, or null.
    bool Configure(uint32_t width, uint32_t height, VkImage depth, VkFormat depthFormat, VkImage normal,
                   VkFormat normalFormat, VkImage albedo);
    // Applies the smoothing and, with a configured albedo, the texture detail
    // the AO normals setting selects. Leaves the output shader-readable for the
    // provider that follows; false when neither applies.
    bool Execute(uint32_t frameSlot, const PerspectiveViewState& view, bool wBuffer,
                 PicaSurfaceCoordinates coordinates, const EffectsSettings& settings);
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
