#include "fast/oot3d/pica_directional_shadow_lighting.h"

#include "fast/oot3d/pica_directional_shadow_semantics.h"
#include "fast/renderer3ds/virtual_shadow_receiver_shader.h"

#include <algorithm>
#include <string_view>

namespace Fast::Oot3d {
namespace {

uint64_t LightingVariantKey(uint64_t original, bool fragmentPrimary) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    constexpr std::string_view kSalt = "OOT3D_PICA_VIRTUAL_SHADOW_LIGHTING_V5";
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

std::string LightingDeclarations() {
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
}

} // namespace

std::string PicaDirectionalShadowLightingDeclarations() { return LightingDeclarations(); }

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
                                                  const ::Oot3d::Renderer::PicaShaderHookLayout& hooks) {
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

    result.Declarations = LightingDeclarations();
    result.FragmentKey = LightingVariantKey(originalFragmentKey, fragmentPrimary);
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

} // namespace Fast::Oot3d
