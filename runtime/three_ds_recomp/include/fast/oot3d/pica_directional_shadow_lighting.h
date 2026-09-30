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
// Receiver flags (Extent.w): bound; the previous-frame clip position of the
// surface locates it in the map (PreviousViewToLight).
inline constexpr uint32_t kPicaDirectionalShadowReceiverBound = 1U;
inline constexpr uint32_t kPicaDirectionalShadowPreviousFrameReceiver = 2U;

enum class PicaDirectionalShadowLightingEligibility : uint8_t {
    Disabled,
    UnsupportedShader,
    VertexPrimary,
    FragmentPrimary,
};

struct alignas(16) PicaDirectionalShadowReceiverUniforms {
    std::array<float, 16> ViewToLight{};
    // Previous-frame view-space position to the map's light space: the map a
    // receiver samples was rendered from the previous frame's casters.
    std::array<float, 16> PreviousViewToLight{};
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

static_assert(sizeof(PicaDirectionalShadowReceiverUniforms) == 2912U);


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

// `previousFrameReceiver`: the fragment shader has the temporal inputs
// (oot3d_previous_clip, oot3d_draw) declared before these declarations.
[[nodiscard]] PicaDirectionalShadowLightingInstrumentation
BuildPicaDirectionalShadowLightingInstrumentation(
    uint64_t originalFragmentKey, bool enabled,
    const ::Oot3d::Renderer::PicaShaderHookLayout& hooks,
    bool previousFrameReceiver = false);

[[nodiscard]] PicaDirectionalShadowReceiverUniforms PackPicaDirectionalShadowReceiverUniforms(
    const DirectionalShadowMatrix& viewToWorld, const PicaDirectionalShadowHistoryState& history) noexcept;


// The map receivers sample is the previous frame's (it is rendered after the
// opaque surfaces, from their casters). A surface that moved since (an
// animated actor) samples it where it was then, so it meets its own caster
// depth of that frame instead of a displaced one: self-shadows stay in place.
// `previousViewToWorld` is the draw's previous-frame view-to-world; the
// previous view position comes from the vertex shader's previous body, the
// same quantity (and math) the casters of that frame used.
void AlignPicaDirectionalShadowReceiverToPreviousFrame(
    PicaDirectionalShadowReceiverUniforms& uniforms, const DirectionalShadowMatrix& previousViewToWorld,
    const PicaDirectionalShadowHistoryState& history) noexcept;

[[nodiscard]] PicaDirectionalShadowReceiverBinding
BuildPicaSurfaceShadowReceiverBinding(
    const DirectionalShadowMatrix& currentViewToWorld,
    std::span<const PicaSurfaceShadowLight> lights,
    const PicaDirectionalShadowHistoryState& history) noexcept;

} // namespace Fast::Oot3d
