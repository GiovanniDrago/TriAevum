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
struct NriEffectGraphTransientImageBinding;

// iMMERSE MXAO provider (see fast/immerse/mxao_shader.h). It reads the same
// native depth and normal guide as GTAO and publishes the same R32F ambient
// visibility; there is no history, so a frame depends only on its own inputs.
class MxaoPass final {
  public:
    MxaoPass();
    ~MxaoPass();
    MxaoPass(const MxaoPass&) = delete;
    MxaoPass& operator=(const MxaoPass&) = delete;
    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop);
    bool Configure(const NriEffectGraphTransientImageBinding& output, VkImage depth, VkFormat depthFormat,
                   VkImage normal, VkFormat normalFormat);
    bool Execute(uint32_t frameSlot, const PerspectiveViewState& view, bool wBuffer, const EffectsSettings& settings,
                 const EffectPassBarrierPlan& barriers, PicaSurfaceCoordinates coordinates);
    void InvalidateScreenResources();
    void Shutdown();
    [[nodiscard]] bool Available() const;
    [[nodiscard]] VkImage OutputImage() const;
    [[nodiscard]] VkImageView OutputView() const;
    [[nodiscard]] bool OutputOwnedByNri() const;
    [[nodiscard]] const EffectPassBarrierExecution& LastBarrierExecution() const;
    [[nodiscard]] const std::string& UnavailableReason() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace Fast::Oot3d
#endif
