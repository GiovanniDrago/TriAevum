// TriAevum's stand-in for the optional iMMERSE module (fast/immerse, under its
// own licence) in builds that leave it out: MXAO reports that it was not
// compiled, so the capability check keeps it unselectable, and it has no F1
// controls. Builds with the module (ENABLE_OOT3D_IMMERSE) compile nothing here.
#include "fast/oot3d/graphics_settings_window.h"
#include "fast/oot3d/mxao_pass.h"

#ifndef ENABLE_OOT3D_IMMERSE
namespace Fast::Oot3d {
bool GraphicsSettingsPanel::DrawMxaoSettings(EffectsSettings&) {
    return false;
}

#ifdef ENABLE_OOT3D_VULKAN
struct MxaoPass::Impl {
    EffectPassBarrierExecution LastBarriers;
    std::string Reason = "iMMERSE MXAO support was not compiled into this build";
};

MxaoPass::MxaoPass() : mImpl(std::make_unique<Impl>()) {
}
MxaoPass::~MxaoPass() = default;
bool MxaoPass::Initialize(VkPhysicalDevice, VkDevice, NriInteropContext&) {
    return false;
}
bool MxaoPass::Configure(const NriEffectGraphTransientImageBinding&, VkImage, VkFormat, VkImage, VkFormat, VkImage,
                         const MxaoMotionInput*) {
    return false;
}
bool MxaoPass::Execute(uint32_t, uint64_t, const PerspectiveViewState&, bool, const EffectsSettings&,
                       const EffectPassBarrierPlan&, PicaSurfaceCoordinates, const MxaoTemporalFrame*) {
    return false;
}
void MxaoPass::InvalidateScreenResources() {
}
void MxaoPass::Shutdown() {
}
bool MxaoPass::Available() const {
    return false;
}
VkImage MxaoPass::OutputImage() const {
    return VK_NULL_HANDLE;
}
VkImageView MxaoPass::OutputView() const {
    return VK_NULL_HANDLE;
}
bool MxaoPass::OutputOwnedByNri() const {
    return false;
}
bool MxaoPass::LastHistoryValid() const {
    return false;
}
const EffectPassBarrierExecution& MxaoPass::LastBarrierExecution() const {
    return mImpl->LastBarriers;
}
const std::string& MxaoPass::UnavailableReason() const {
    return mImpl->Reason;
}
#endif
} // namespace Fast::Oot3d
#endif
