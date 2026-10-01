#pragma once

#ifdef ENABLE_OOT3D_VULKAN

#include "fast/oot3d/directional_shadows.h"
#include "fast/oot3d/effect_graph_pass_barriers.h"
#include "fast/oot3d/native_scene_view.h"
#include "fast/oot3d/pica_directional_shadow_lighting.h"
#include "fast/oot3d/point_shadows.h"
#include "fast/renderer3ds/point_shadow_map.h"
#include "fast/renderer/extension_schedule.h"
#include "fast/renderer3ds/pica_environment_lighting.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>
#include <optional>

namespace Fast::Oot3d {

class NriInteropContext;

enum class NriDirectionalShadowExecutionStage : uint8_t {
    NotAttempted,
    InputAccepted,
    CastersMatched,
    NativeLightResolved,
    FramePlanReady,
    ResourcesReady,
    ShadowRasterized,
    HistoryPublished,
};

struct NriDirectionalShadowExecuteDesc {
    uint32_t FrameSlot = 0;
    uint64_t FrameId = 0;
    uint64_t RenderTargetNamespace = 0;
    uint32_t ColorPhysicalAddress = 0;
    ::Fast::Renderer::ExtensionPassAuthorization ScheduleAuthorization;
    ::Fast::Renderer::ExtensionSurfaceIdentity ExpectedSurface;
    DirectionalShadowSettings Settings;
    std::optional<Renderer3ds::PicaEnvironmentDirectionalLight> EnvironmentKey;
    float NativeDirectStrength = 1.0F;
};

struct NriDirectionalShadowHistorySnapshot {
    VkImage Image = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE;
    VkSampler Sampler = VK_NULL_HANDLE;
    VkFormat Format = VK_FORMAT_D32_SFLOAT;
    PicaDirectionalShadowHistoryState State;

    [[nodiscard]] bool Valid() const noexcept {
        return Image != VK_NULL_HANDLE && View != VK_NULL_HANDLE &&
               Sampler != VK_NULL_HANDLE && State.Valid();
    }
};

struct NriPointShadowExecuteDesc {
    uint32_t FrameSlot = 0;
    uint64_t FrameId = 0;
    uint64_t RenderTargetNamespace = 0;
    uint32_t ColorPhysicalAddress = 0;
    ::Fast::Renderer::ExtensionPassAuthorization ScheduleAuthorization;
    ::Fast::Renderer::ExtensionSurfaceIdentity ExpectedSurface;
    PointShadowSettings Settings;
    // Point-light groups (enable, intensity, radius scale) and the native
    // local-light gain, exactly as the receivers' lighting applies them.
    Renderer3ds::PicaEnvironmentLighting Environment;
};

// The published cube atlas of one target: its rows, and the light of each.
struct NriPointShadowHistorySnapshot {
    VkImage Image = VK_NULL_HANDLE;
    VkFormat Format = VK_FORMAT_D32_SFLOAT;
    Renderer3ds::PointShadowLayout Layout;
    Renderer3ds::PointShadowSlots Slots{};
    float Range = 0.0F;
    float Strength = 0.0F;
    uint64_t ProducedFrameId = 0U;

    [[nodiscard]] bool Valid() const noexcept {
        return Image != VK_NULL_HANDLE && Layout.Valid() && ProducedFrameId != 0U;
    }
};

struct NriPointShadowTelemetry {
    uint32_t CandidateLights = 0U;
    uint32_t Lights = 0U;
    uint32_t RenderedLights = 0U;
    uint32_t CasterDraws = 0U;
    uint32_t AlphaCasterDraws = 0U;
    uint64_t AtlasBytes = 0U;
};

struct NriDirectionalShadowLightTelemetry {
    DirectionalShadowVector WorldDirectionTowardSource{};
    uint32_t CandidateCount = 0U;
    uint32_t ClusterCount = 0U;
    float GeometryLightWeight = 0.0F;
    Renderer3ds::VirtualShadowPageStatistics Pages;
    uint32_t UpdatedPages = 0U;
    uint64_t AtlasBytes = 0U;

    [[nodiscard]] bool Valid() const noexcept {
        return CandidateCount != 0U && ClusterCount != 0U &&
               GeometryLightWeight > 0.0F;
    }
};

// Graph-owned persistent virtual shadow atlas. Native receivers sample the
// latest published immutable page table; the declared native anchor updates
// dirty pages, then publishes the table for subsequent native draws/frames.
// The scene view holds the sample's casters prepared ahead of native order, so
// the BeforeOpaque anchor publishes them before the first opaque receiver; a
// target without that boundary uses the graph's BeforeUi anchor.
// Scene color, native depth and canonical raster state are never modified.
class NriDirectionalShadowPass final {
  public:
    NriDirectionalShadowPass();
    ~NriDirectionalShadowPass();
    NriDirectionalShadowPass(const NriDirectionalShadowPass&) = delete;
    NriDirectionalShadowPass& operator=(const NriDirectionalShadowPass&) = delete;

    bool Initialize(VkPhysicalDevice physicalDevice, VkDevice device, NriInteropContext& interop);
    bool ExecuteMap(const NriDirectionalShadowExecuteDesc& desc,
                    const NativeSceneView& sceneView,
                    const EffectPassBarrierPlan& shadowMapBarrierPlan);
    [[nodiscard]] NriDirectionalShadowHistorySnapshot
    FindLatestHistory(uint64_t renderTargetNamespace,
                      uint32_t colorPhysicalAddress,
                      uint64_t atFrameId) const noexcept;
    // Point-light cube maps from the same prepared casters, one atlas row per
    // selected light; a light shadows only within its radius times the range.
    bool ExecutePointMaps(const NriPointShadowExecuteDesc& desc,
                          const NativeSceneView& sceneView,
                          const EffectPassBarrierPlan& pointShadowMapBarrierPlan);
    [[nodiscard]] NriPointShadowHistorySnapshot
    FindLatestPointHistory(uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
                           uint64_t atFrameId) const noexcept;
    [[nodiscard]] const NriPointShadowTelemetry& LastPointTelemetry() const;
    void InvalidateHistory() noexcept;
    void InvalidateScreenResources();
    void Shutdown();

    [[nodiscard]] bool Available() const;
    [[nodiscard]] bool LastExecutionUsedNativeLight() const;
    [[nodiscard]] uint32_t LastCasterCount() const;
    // Caster draws of the last update that ran their native alpha test.
    [[nodiscard]] uint32_t LastAlphaCasterCount() const;
    [[nodiscard]] const NriDirectionalShadowLightTelemetry&
    LastLightTelemetry() const;
    [[nodiscard]] NriDirectionalShadowExecutionStage LastExecutionStage() const;
    [[nodiscard]] const EffectPassBarrierExecution& LastShadowMapBarrierExecution() const;
    [[nodiscard]] const std::string& UnavailableReason() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Fast::Oot3d

#endif
