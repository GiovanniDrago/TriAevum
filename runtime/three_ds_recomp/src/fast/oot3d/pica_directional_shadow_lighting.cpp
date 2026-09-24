#include "fast/oot3d/pica_directional_shadow_lighting.h"

#include "fast/oot3d/pica_directional_shadow_semantics.h"

#include <algorithm>
#include <string_view>

namespace Fast::Oot3d {
namespace {

uint64_t LightingVariantKey(uint64_t original, bool fragmentPrimary) {
    constexpr uint64_t kOffset = 1469598103934665603ULL;
    constexpr uint64_t kPrime = 1099511628211ULL;
    constexpr std::string_view kSalt = "OOT3D_PICA_VIRTUAL_SHADOW_LIGHTING_V4";
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
    static_assert(Renderer3ds::kVirtualShadowPageTexels == 128 && Renderer3ds::kVirtualShadowPageBorder == 3 &&
                  Renderer3ds::kVirtualShadowPhysicalPageTexels == 134 && Renderer3ds::kVirtualShadowGridPages == 8 &&
                  Renderer3ds::kVirtualShadowLevelCount == 10 && Renderer3ds::kVirtualShadowTableEntries == 640);
    return R"glsl(layout(set=0,binding=10) uniform sampler2D oot3d_directional_shadow_map;
layout(set=0,binding=12,std140) uniform Oot3dDirectionalShadowReceiver {
    mat4 view_to_light;
    vec4 shadow_state;
    vec4 shadowed_direct_fraction;
    uvec4 extent;
    vec4 camera_light;
    vec4 levels[10];
    uvec4 page_table[160];
} oot3d_directional_shadow;
uint oot3d_vsm_page(int level, ivec2 cell) {
    ivec2 relative = cell - ivec2(oot3d_directional_shadow.levels[level].xy);
    if (any(lessThan(relative, ivec2(0))) || any(greaterThanEqual(relative, ivec2(8))))
        return 0u;
    int index = level * 64 + relative.y * 8 + relative.x;
    return oot3d_directional_shadow.page_table[index >> 2][index & 3];
}
ivec2 oot3d_vsm_physical(uint entry, ivec2 local) {
    uint side = oot3d_directional_shadow.extent.x / 134u;
    uint page = entry - 1u;
    // NRI originBottomLeft uses a positive-height Vulkan viewport: increasing
    // light-space Y increases the physical texel row. Do not mirror the tile.
    return ivec2(page % side, page / side) * 134 + local + ivec2(3);
}
float oot3d_vsm_axis_weight(int offset, int radius, float fraction) {
    return offset == -radius ? 1.0 - fraction : (offset == radius + 1 ? fraction : 1.0);
}
float oot3d_vsm_filter(int level, vec3 light, vec2 depth_gradient) {
    float page_world = oot3d_directional_shadow.levels[level].z;
    ivec2 center = ivec2(floor(light.xy / page_world));
    uint entry = oot3d_vsm_page(level, center);
    if (entry == 0u) return -1.0;
    if (entry == 0xffffffffu) return 1.0;
    vec2 position = light.xy * (128.0 / page_world) - vec2(0.5);
    ivec2 base = ivec2(floor(position));
    vec2 fraction = fract(position);
    int radius = int(min(oot3d_directional_shadow.extent.z, 2u));
    float texel_world = page_world / 128.0;
    // D32 cancellation has a finite floor even when the optional artistic bias
    // is zero. Express the geometric allowance in virtual, not atlas, texels.
    float inverse_depth = 1.0 / (2.0 * max(oot3d_directional_shadow.shadow_state.z * 4.0, 65536.0));
    float depth = light.z - max(2.0 * 5.960464477539063e-8,
        0.25 * texel_world * inverse_depth) -
        oot3d_directional_shadow.shadow_state.w * exp2(float(level));
    vec2 sample_gradient = depth_gradient * texel_world;
    ivec2 local = base - center * 128;
    // The rasterized three-texel gutter contains the complete PCF footprint.
    // One translation per level, including at virtual page boundaries.
    vec2 physical_base = vec2(oot3d_vsm_physical(entry, local));
    float visibility = 0.0;
    // The same continuous box-PCF footprint as independent comparisons,
    // combined into 1/4/9 gather operations for radius 0/1/2.
    for (int iy = 0; iy < 3; ++iy) {
        if (iy > radius) break;
        int y = -radius + iy * 2;
        vec2 wy = vec2(oot3d_vsm_axis_weight(y, radius, fraction.y),
                       oot3d_vsm_axis_weight(y + 1, radius, fraction.y));
        for (int ix = 0; ix < 3; ++ix) {
            if (ix > radius) break;
            int x = -radius + ix * 2;
            vec2 wx = vec2(oot3d_vsm_axis_weight(x, radius, fraction.x),
                           oot3d_vsm_axis_weight(x + 1, radius, fraction.x));
            vec4 weights = vec4(wx.x*wy.y, wx.y*wy.y, wx.y*wy.x, wx.x*wy.x);
            vec2 uv = (physical_base + vec2(x + 1, y + 1)) * oot3d_directional_shadow.shadow_state.y;
            vec4 sample_depth = vec4(depth) +
                sample_gradient.x * (vec4(x, x + 1, x + 1, x) - fraction.x) +
                sample_gradient.y * (vec4(y + 1, y + 1, y, y) - fraction.y);
            vec4 samples = step(sample_depth, textureGather(oot3d_directional_shadow_map, uv, 0));
            visibility += dot(samples, weights);
        }
    }
    float width = float(2 * radius + 1);
    return visibility / (width * width);
}
vec3 oot3d_directional_shadow_visibility() {
    if ((oot3d_directional_shadow.extent.w & 1u) == 0u) return vec3(1.0);
    vec4 position = oot3d_directional_shadow.view_to_light * vec4(-pica_view, 1.0);
    if (abs(position.w) <= 1.0e-7) return vec3(1.0);
    vec3 light = position.xyz / position.w;
    // Evaluate derivatives before divergent page/LOD selection. Each PCF tap
    // compares against the receiver plane at that tap, not the center depth.
    // Differentiate before adding the normalized-depth translation: near 0.5,
    // subtracting adjacent D32 values loses the small slopes of close geometry.
    vec3 dx = mat3(oot3d_directional_shadow.view_to_light) * dFdx(-pica_view);
    vec3 dy = mat3(oot3d_directional_shadow.view_to_light) * dFdy(-pica_view);
    float determinant = dx.x * dy.y - dx.y * dy.x;
    vec2 depth_gradient = abs(determinant) > 1.0e-12
        ? vec2(dx.z * dy.y - dy.z * dx.y, dx.x * dy.z - dy.x * dx.z) / determinant
        : vec2(0.0);
    if (light.z < 0.0 || light.z > 1.0) return vec3(1.0);
    vec3 relative = light - oot3d_directional_shadow.camera_light.xyz;
    float maximum_distance = oot3d_directional_shadow.shadow_state.z;
    float depth_extent = max(maximum_distance * 4.0, 65536.0);
    float world_distance = length(vec3(relative.xy, relative.z * (2.0 * depth_extent)));
    if (world_distance >= maximum_distance) return vec3(1.0);
    float distance_plane = max(abs(relative.x), abs(relative.y));
    int count = int(oot3d_directional_shadow.camera_light.w);
    float finest_page = oot3d_directional_shadow.levels[0].z;
    int first = clamp(int(ceil(log2(max(distance_plane / (3.0 * finest_page), 1.0)))), 0, count - 1);
    float visibility = 1.0;
    for (int level = first; level < count; ++level) {
        float sample_visibility = oot3d_vsm_filter(level, light, depth_gradient);
        if (sample_visibility < 0.0) continue;
        visibility = sample_visibility;
        float page_world = oot3d_directional_shadow.levels[level].z;
        // A 32-texel transition keeps scale changes continuous without
        // evaluating two full filters across most of every clipmap.
        float blend = smoothstep(2.75 * page_world, 3.0 * page_world, distance_plane);
        if (blend > 0.0) {
            for (int coarse = level + 1; coarse < count; ++coarse) {
                float coarse_visibility = oot3d_vsm_filter(coarse, light, depth_gradient);
                if (coarse_visibility < 0.0) continue;
                visibility = mix(visibility, coarse_visibility, blend);
                break;
            }
        }
        break;
    }
    float fade = 1.0 - smoothstep(0.9 * maximum_distance, maximum_distance, world_distance);
    float strength = clamp(oot3d_directional_shadow.shadow_state.x, 0.0, 4.0);
    return vec3(max(0.0, 1.0 - (1.0 - visibility) * strength * fade));
}
)glsl";
}

PicaDirectionalShadowReceiverUniforms PackReceiverUniforms(const DirectionalShadowMatrix& viewToWorld,
                                                           const PicaDirectionalShadowHistoryState& history) noexcept {
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

} // namespace

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
    result.Uniforms = PackReceiverUniforms(currentViewToWorld, history);
    result.Uniforms.ShadowedDirectFraction[3] = static_cast<float>(mask);
    return result;
}

} // namespace Fast::Oot3d
