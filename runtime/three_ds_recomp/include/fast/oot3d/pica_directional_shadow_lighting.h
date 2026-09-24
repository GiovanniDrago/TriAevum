#pragma once

#include "fast/oot3d/directional_shadows.h"
#include "fast/oot3d/pica_directional_shadow_semantics.h"
#include "fast/oot3d/pica_scene_semantics.h"
#include "oot3d/renderer/pica_shader_hooks.h"

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Oot3d {

inline constexpr uint32_t kPicaDirectionalShadowTextureBinding = 10U;
inline constexpr uint32_t kPicaDirectionalShadowSamplerBinding = 11U;
inline constexpr uint32_t kPicaDirectionalShadowUniformBinding = 12U;

enum class PicaDirectionalShadowLightingEligibility : uint8_t {
    Disabled,
    UnsupportedShader,
    VertexPrimary,
    FragmentPrimary,
};

struct alignas(16) PicaDirectionalShadowReceiverUniforms {
    std::array<float, 16> ViewToLight{};
    // strength, inverse atlas resolution, reserved, finest-level depth bias
    std::array<float, 4> ShadowState{};
    // RGB: native direct fraction; W: selected shader-light mask for surface lighting.
    std::array<float, 4> ShadowedDirectFraction{};
    // shadow-map width, shadow-map height, PCF radius, flags
    std::array<uint32_t, 4> Extent{};
    // Light-plane camera position and active clipmap level count.
    std::array<float, 4> CameraLight{};
    std::array<std::array<float, 4>, Renderer3ds::kVirtualShadowLevelCount> Levels{};
    // Packed as uvec4[] in std140, not scalar uint[].
    std::array<uint32_t, Renderer3ds::kVirtualShadowTableEntries> PageTable{};
};

static_assert(sizeof(PicaDirectionalShadowReceiverUniforms) == 2848U);


struct PicaDirectionalShadowReceiverBinding {
    PicaDirectionalShadowReceiverUniforms Uniforms;
    PicaDirectionalShadowReceiverClass Classification =
        PicaDirectionalShadowReceiverClass::LightingUnavailable;

    [[nodiscard]] bool Bound() const noexcept {
        return (Uniforms.Extent[3] & 1U) != 0U;
    }
};

struct PicaDirectionalShadowHistoryState {
    Renderer3ds::VirtualShadowLayout Virtual;
    std::array<uint32_t, Renderer3ds::kVirtualShadowTableEntries> PageTable{};
    DirectionalShadowVector WorldLightDirectionTowardSource{};
    uint64_t ProducedFrameId = 0U;
    float Strength = 0.0F;
    float DepthBias = 0.0F;
    uint8_t PcfRadius = 0U;

    [[nodiscard]] bool Valid() const noexcept {
        return ProducedFrameId != 0U && Virtual.Valid();
    }
};

struct PicaDirectionalShadowLightingInstrumentation {
    std::string Declarations;
    uint64_t FragmentKey = 0U;
    PicaDirectionalShadowLightingEligibility Eligibility =
        PicaDirectionalShadowLightingEligibility::Disabled;

    [[nodiscard]] bool Applied() const noexcept {
        return Eligibility ==
                   PicaDirectionalShadowLightingEligibility::VertexPrimary ||
               Eligibility ==
                   PicaDirectionalShadowLightingEligibility::FragmentPrimary;
    }
};

[[nodiscard]] PicaDirectionalShadowLightingInstrumentation
BuildPicaDirectionalShadowLightingInstrumentation(
    uint64_t originalFragmentKey, bool enabled,
    const ::Oot3d::Renderer::PicaShaderHookLayout& hooks);


[[nodiscard]] PicaDirectionalShadowReceiverBinding
BuildPicaSurfaceShadowReceiverBinding(
    const DirectionalShadowMatrix& currentViewToWorld,
    std::span<const PicaSurfaceShadowLight> lights,
    const PicaDirectionalShadowHistoryState& history) noexcept;

} // namespace Fast::Oot3d
