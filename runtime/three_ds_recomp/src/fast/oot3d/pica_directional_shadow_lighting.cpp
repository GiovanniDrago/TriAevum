#include "fast/oot3d/pica_directional_shadow_lighting.h"

#include "fast/oot3d/pica_directional_shadow_semantics.h"
#include "fast/renderer3ds/virtual_shadow_receiver_shader.h"

#include <algorithm>
#include <string_view>

namespace Fast::Oot3d {
namespace {

uint64_t LightingVariantKey(uint64_t original, bool fragmentPrimary, bool previousFrameReceiver) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    const std::string_view kSalt = previousFrameReceiver ? "OOT3D_PICA_VIRTUAL_SHADOW_LIGHTING_V7_PREVIOUS_VIEW"
                                                         : "OOT3D_PICA_VIRTUAL_SHADOW_LIGHTING_V5";
    uint64_t hash = kOffset;
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<uint8_t>(original >> shift);
        hash *= kPrime;
    }
    for (const char value : kSalt) {
        hash ^= static_cast<uint8_t>(value);
        hash *= kPrime;
    }
    hash ^= fragmentPrimary ? 1U : 0U;
    return hash * kPrime;
}

std::string LightingDeclarations(bool previousFrameReceiver) {
    if (!previousFrameReceiver)
        return std::string(Renderer3ds::VirtualShadowReceiverShader()) + R"glsl(
vec3 oot3d_directional_shadow_visibility() {
    if ((pica_vsm.extent.w & 1u) == 0u) return vec3(1.0);
    vec4 position = pica_vsm.input_to_light * vec4(-pica_view, 1.0);
    if (abs(position.w) <= 1.0e-7) return vec3(1.0);
    // Differentiate before normalized-depth translation to retain D32 slopes.
    vec3 dx = mat3(pica_vsm.input_to_light) * dFdx(-pica_view);
    vec3 dy = mat3(pica_vsm.input_to_light) * dFdy(-pica_view);
    return vec3(pica_vsm_visibility(position.xyz / position.w, pica_vsm_depth_gradient(dx, dy)));
}
)glsl";
    // The map was rendered from the previous frame's casters: a surface with
    // an exact previous frame (oot3d_draw.rigid_motion.w) samples it at its
    // previous-frame view position (the casters' own math), so an animated
    // actor meets its own caster depth of that frame. Derivatives stay in
    // uniform control flow.
    return std::string(Renderer3ds::VirtualShadowReceiverShader()) + R"glsl(
vec3 oot3d_directional_shadow_visibility() {
    if ((pica_vsm.extent.w & 1u) == 0u) return vec3(1.0);
    vec4 position = pica_vsm.input_to_light * vec4(-pica_view, 1.0);
    vec3 dx = mat3(pica_vsm.input_to_light) * dFdx(-pica_view);
    vec3 dy = mat3(pica_vsm.input_to_light) * dFdy(-pica_view);
    vec4 previous = pica_vsm.previous_view_to_light * vec4(-oot3d_previous_view, 1.0);
    vec3 previous_light = previous.xyz / (abs(previous.w) > 1.0e-7 ? previous.w : 1.0);
    vec3 previous_dx = mat3(pica_vsm.previous_view_to_light) * dFdx(-oot3d_previous_view);
    vec3 previous_dy = mat3(pica_vsm.previous_view_to_light) * dFdy(-oot3d_previous_view);
    if ((pica_vsm.extent.w & 2u) != 0u && oot3d_draw.rigid_motion.w > 0.5 && abs(previous.w) > 1.0e-7)
        return vec3(pica_vsm_visibility(previous_light, pica_vsm_depth_gradient(previous_dx, previous_dy)));
    if (abs(position.w) <= 1.0e-7) return vec3(1.0);
    return vec3(pica_vsm_visibility(position.xyz / position.w, pica_vsm_depth_gradient(dx, dy)));
}
)glsl";
}

} // namespace

PicaDirectionalShadowReceiverUniforms PackPicaDirectionalShadowReceiverUniforms(
    const DirectionalShadowMatrix& viewToWorld, const PicaDirectionalShadowHistoryState& history) noexcept {
    PicaDirectionalShadowReceiverUniforms uniforms;
    const auto& layout = history.Virtual;
    uniforms.ViewToLight = MultiplyDirectionalShadowMatrices(layout.WorldToLight, viewToWorld);
    uniforms.ShadowState = { std::clamp(history.Strength, 0.0F, 4.0F), 1.0F / float(layout.AtlasResolution),
                             layout.MaximumDistance, std::max(history.DepthBias, 0.0F) };
    uniforms.ShadowedDirectFraction = { 1.0F, 1.0F, 1.0F, 0.0F };
    uniforms.Extent = { layout.AtlasResolution, layout.AtlasResolution, std::min<uint32_t>(history.PcfRadius, 2U), 1U };
    uniforms.CameraLight = { layout.CameraLight[0], layout.CameraLight[1], layout.CameraLight[2],
                             float(layout.LevelCount) };
    for (size_t level = 0; level < layout.LevelCount; ++level) {
        const auto& source = layout.Levels[level];
        uniforms.Levels[level] = { float(source.OriginX), float(source.OriginY), source.PageWorldSize, 0 };
    }
    uniforms.PageTable = history.PageTable;
    return uniforms;
}


PicaDirectionalShadowLightingInstrumentation
BuildPicaDirectionalShadowLightingInstrumentation(uint64_t originalFragmentKey, bool enabled,
                                                  const ::Oot3d::Renderer::PicaShaderHookLayout& hooks,
                                                  bool previousFrameReceiver) {
    using ::Oot3d::Renderer::PicaShaderHook;
    using ::Oot3d::Renderer::PicaShaderSemantic;

    PicaDirectionalShadowLightingInstrumentation result;
    result.FragmentKey = originalFragmentKey;
    if (!enabled) {
        return result;
    }
    if (!hooks.Valid() || !hooks.Supports(PicaShaderHook::PicaLighting)) {
        result.Eligibility = PicaDirectionalShadowLightingEligibility::UnsupportedShader;
        return result;
    }

    const bool fragmentPrimary =
        hooks.Has(PicaShaderSemantic::MaterialLightingPoint) && hooks.Has(PicaShaderSemantic::SecondaryFragmentColor);
    const bool vertexPrimary = hooks.Has(PicaShaderSemantic::VertexLightingPoint) &&
                               hooks.Has(PicaShaderSemantic::PrimaryColorInput) &&
                               hooks.Has(PicaShaderSemantic::PrimaryColorConsumed);
    if (!fragmentPrimary && !vertexPrimary) {
        result.Eligibility = PicaDirectionalShadowLightingEligibility::UnsupportedShader;
        return result;
    }

    result.Declarations = LightingDeclarations(previousFrameReceiver);
    result.FragmentKey = LightingVariantKey(originalFragmentKey, fragmentPrimary, previousFrameReceiver);
    result.Eligibility = fragmentPrimary ? PicaDirectionalShadowLightingEligibility::FragmentPrimary
                                         : PicaDirectionalShadowLightingEligibility::VertexPrimary;
    return result;
}

PicaDirectionalShadowReceiverBinding
BuildPicaSurfaceShadowReceiverBinding(const DirectionalShadowMatrix& currentViewToWorld,
                                      std::span<const PicaSurfaceShadowLight> lights,
                                      const PicaDirectionalShadowHistoryState& history) noexcept {
    PicaDirectionalShadowReceiverBinding result;
    if (!history.Valid())
        return result;
    const uint16_t mask =
        ResolvePicaSurfaceDirectionalShadowMask(lights, currentViewToWorld, history.WorldLightDirectionTowardSource);
    result.Classification = mask != 0U ? PicaDirectionalShadowReceiverClass::DirectMatched
                                       : PicaDirectionalShadowReceiverClass::DirectUnmatched;
    if (mask == 0U)
        return result;
    result.Uniforms = PackPicaDirectionalShadowReceiverUniforms(currentViewToWorld, history);
    result.Uniforms.ShadowedDirectFraction[3] = static_cast<float>(mask);
    return result;
}

void AlignPicaDirectionalShadowReceiverToPreviousFrame(
    PicaDirectionalShadowReceiverUniforms& uniforms, const DirectionalShadowMatrix& previousViewToWorld,
    const PicaDirectionalShadowHistoryState& history) noexcept {
    if ((uniforms.Extent[3] & kPicaDirectionalShadowReceiverBound) == 0U || !history.Valid())
        return;
    uniforms.PreviousViewToLight = MultiplyDirectionalShadowMatrices(history.Virtual.WorldToLight, previousViewToWorld);
    uniforms.Extent[3] |= kPicaDirectionalShadowPreviousFrameReceiver;
}

} // namespace Fast::Oot3d
