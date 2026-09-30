#pragma once
#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/effect_graph_pass_barriers.h"
#include <vulkan/vulkan.h>
#include <memory>
#include <string>

namespace Fast::Oot3d {
class NriInteropContext;

// iMMERSE LAUNCHPAD albedo estimate (see fast/immerse/launchpad_albedo_shader.h).
// It equalizes the lighting out of the AO stage's declared scene colour into a
// private R16F height the textured AO normals differentiate. No history.
class LaunchpadAlbedoPass final {
  public:
    LaunchpadAlbedoPass();
    ~LaunchpadAlbedoPass();
    LaunchpadAlbedoPass(const LaunchpadAlbedoPass&) = delete;
    LaunchpadAlbedoPass& operator=(const LaunchpadAlbedoPass&) = delete;
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop);
    // width x height: the AO guides; the scene colour is sampled by UV.
    bool Configure(uint32_t width, uint32_t height, VkImage sceneColor, VkFormat sceneFormat, uint32_t sceneWidth,
                   uint32_t sceneHeight);
    // linearSource: the scene colour samples as linear values rather than the
    // display encoding the original expects. Leaves the output shader-readable.
    bool Execute(uint32_t frameSlot, bool linearSource);
    void InvalidateScreenResources();
    void Shutdown();
    [[nodiscard]] bool Available() const;
    [[nodiscard]] VkImage OutputImage() const;
    [[nodiscard]] const EffectPassBarrierExecution& LastBarrierExecution() const;
    [[nodiscard]] const std::string& UnavailableReason() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace Fast::Oot3d
#endif
