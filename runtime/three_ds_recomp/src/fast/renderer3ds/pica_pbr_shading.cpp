#include "fast/renderer3ds/pica_pbr_shading.h"

#include <bit>

namespace Fast::Renderer3ds {

PicaPbrUniforms BuildPicaPbrUniforms(
    const PicaPbrMaterialParameters& parameters, uint8_t textureMask,
    const PicaLightingProgram& nativeLighting, const PicaPbrControls& controls) noexcept {
    PicaPbrUniforms result;
    result.BaseColorFactor = parameters.BaseColorFactor;
    result.EmissiveNormalScale = {parameters.EmissiveFactor[0], parameters.EmissiveFactor[1],
        parameters.EmissiveFactor[2], parameters.NormalScale * controls.NormalStrength};
    result.OrmNormalY = {parameters.OcclusionStrength, parameters.RoughnessFactor,
        parameters.MetallicFactor, parameters.NormalYSign};
    result.Control[0] = textureMask;
    result.Control[1] = std::bit_cast<uint32_t>(controls.GlossinessStrength);
    result.Control[2] = static_cast<uint32_t>(controls.DebugView);
    result.NativeLighting = nativeLighting;
    return result;
}

std::optional<PicaPbrShader> BuildPicaPbrShader(
    const PicaShaderHookLayout& hooks, const PicaPbrShaderOptions& options) {
    const auto* sample = hooks.TextureSample(options.TextureSlot);
    if (sample == nullptr || sample->Coordinate > 2U ||
        (sample->Operation != PicaTextureCoordinateOperation::NativeVFlip &&
         sample->Operation != PicaTextureCoordinateOperation::ProjectedNativeVFlip)) return std::nullopt;
    const auto coordinate = std::to_string(sample->Coordinate);
    const std::string uv = sample->Operation == PicaTextureCoordinateOperation::ProjectedNativeVFlip
        ? "vec2(pica_texcoord0.x / pica_texcoord0_w, 1.0 - pica_texcoord0.y / pica_texcoord0_w)"
        : "vec2(pica_texcoord" + coordinate + ".x, 1.0 - pica_texcoord" + coordinate + ".y)";
    PicaPbrShader result;
    result.Declarations = R"glsl(
layout(set=0,binding=17,std140) uniform PicaPbrMaterial {
    vec4 base_color_factor;
    vec4 emissive_normal_scale;
    vec4 orm_normal_y;
    uvec4 control;
    uvec4 native_control;
    uvec4 native_lights[8];
    vec4 native_luts[7];
} pbr_material;
layout(set=0,binding=18) uniform sampler2D pbr_normal_map;
layout(set=0,binding=19) uniform sampler2D pbr_orm_map;
layout(set=0,binding=20) uniform sampler2D pbr_emissive_map;
vec3 pbr_srgb_to_linear(vec3 value) {
    return mix(pow((value + 0.055) / 1.055, vec3(2.4)), value / 12.92,
               lessThanEqual(value, vec3(0.04045)));
}
vec3 pbr_linear_to_srgb(vec3 value) {
    value = max(value, vec3(0.0));
    return mix(1.055 * pow(value, vec3(1.0 / 2.4)) - 0.055, value * 12.92,
               lessThanEqual(value, vec3(0.0031308)));
}
vec3 pbr_surface_normal(vec2 uv, vec3 geometric, out vec3 tangent) {
    vec3 dpdx = dFdx(-pica_view), dpdy = dFdy(-pica_view);
    vec2 duvdx = dFdx(uv), duvdy = dFdy(uv);
    float determinant = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    tangent = vec3(0.0);
    if (abs(determinant) <= 1.0e-12) return geometric;
    vec3 t = (dpdx * duvdy.y - dpdy * duvdx.y) * sign(determinant);
    vec3 b = (dpdy * duvdx.x - dpdx * duvdy.x) * sign(determinant);
    t -= geometric * dot(geometric, t);
    if (dot(t, t) <= 1.0e-20 || dot(b, b) <= 1.0e-20) return geometric;
    tangent = normalize(t);
    vec3 bitangent = cross(geometric, tangent);
    bitangent *= dot(bitangent, b) < 0.0 ? -1.0 : 1.0;
    if ((pbr_material.control.x & 1u) == 0u || pbr_material.emissive_normal_scale.w <= 0.0) return geometric;
    vec3 mapped = texture(pbr_normal_map, uv).xyz * 2.0 - 1.0;
    mapped.xy *= pbr_material.emissive_normal_scale.w;
    mapped.y *= pbr_material.orm_normal_y.w;
    vec3 transformed = tangent * mapped.x + bitangent * mapped.y + geometric * mapped.z;
    return dot(transformed, transformed) > 1.0e-20 ? normalize(transformed) : geometric;
}
void pbr_ggx(vec3 n, vec3 v, vec3 l, vec3 albedo, float roughness, float metallic,
             bool two_sided, out vec3 diffuse, out vec3 specular) {
    float nl = two_sided ? abs(dot(n, l)) : max(dot(n, l), 0.0);
    float nv = two_sided ? abs(dot(n, v)) : max(dot(n, v), 0.0);
    vec3 h = pica_surface_unit(v + l);
    float nh = two_sided ? abs(dot(n, h)) : max(dot(n, h), 0.0);
    float vh = max(dot(v, h), 0.0);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = f0 + (vec3(1.0) - f0) * pow(1.0 - vh, 5.0);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = nh * nh * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / max(3.14159265359 * denominator * denominator, 1.0e-8);
    float visibility = 0.5 / max(nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2) +
                                 nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2), 1.0e-6);
    // Native light RGB is Lambert irradiance (the native model omits 1/pi).
    // Convert that convention once, rather than adding an artistic light gain.
    diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * albedo * nl;
    specular = fresnel * (3.14159265359 * distribution * visibility * nl);
}
)glsl";
    if (options.NativeFragmentLighting && options.NativeAttenuationLuts) {
        result.Declarations += R"glsl(
float pbr_native_spot(int table, bool two_sided, vec3 n, vec3 t, vec3 v,
                      vec3 l, vec3 h, vec3 spot) {
    vec4 config = pbr_material.native_luts[2];
    float x = 0.0;
    switch (int(config.x)) {
    case 0: x = dot(n, pica_surface_unit(h)); break;
    case 1: x = dot(v, pica_surface_unit(h)); break;
    case 2: x = dot(n, v); break;
    case 3: x = dot(l, n); break;
    case 4: x = dot(-l, spot); break;
    case 5:
        if (pbr_material.native_control.y == 8u) {
            vec3 half_unit = pica_surface_unit(h);
            x = dot(half_unit - n * dot(n, half_unit), t);
        }
        break;
    }
    if (config.y != 0.0)
        return config.z * pica_lighting_lut_unsigned(table, two_sided ? abs(x) : max(x, 0.0));
    return config.z * pica_lighting_lut_signed(table, x);
}
)glsl";
    }
    // TEV receives unlit color, not the diffuse material coefficient. Diffuse
    // and ambient coefficients belong to their respective lighting terms;
    // an ambient-only material may legitimately have a black diffuse term.
    result.UnlitInputs = R"glsl(
    if (pica_surface.control.x != 0u ||
        (pica_surface.environment_parameters.w != 0.0 && (pica_surface.control.w & 1u) == 0u))
        rounded_primary_color.rgb = byteround3(min(abs(pica_surface_tint.rgb), vec3(1.0)));
)glsl";
    if (options.NativeFragmentLighting) {
        result.UnlitInputs += R"glsl(
    primary_fragment_color.rgb = vec3(1.0);
    secondary_fragment_color.rgb = vec3(0.0);
)glsl";
    }
    result.ShadingBody = "\n    vec2 pbr_uv = " + uv + ";\n";
    result.ShadingBody += R"glsl(
    vec3 pbr_geometric_normal = pica_surface_unit(pica_surface_normal);
    vec3 pbr_tangent;
    vec3 pbr_normal = pbr_surface_normal(pbr_uv, pbr_geometric_normal, pbr_tangent);
    vec3 pbr_view = pica_surface_unit(pica_view);
    vec3 pbr_orm = (pbr_material.control.x & 2u) != 0u ? texture(pbr_orm_map, pbr_uv).rgb : vec3(1.0);
    float pbr_ao = mix(1.0, pbr_orm.r, pbr_material.orm_normal_y.x);
    float pbr_authored_roughness = clamp(pbr_orm.g * pbr_material.orm_normal_y.y, 0.0, 1.0);
    float pbr_roughness = clamp(1.0 - (1.0 - pbr_authored_roughness) *
        uintBitsToFloat(pbr_material.control.y), 0.045, 1.0);
    float pbr_metallic = clamp(pbr_orm.b * pbr_material.orm_normal_y.z, 0.0, 1.0);
    vec3 pbr_albedo = pbr_srgb_to_linear(clamp(combiner_output.rgb, 0.0, 1.0)) * pbr_material.base_color_factor.rgb;
    vec3 pbr_emission = (pbr_material.control.x & 4u) != 0u ?
        pbr_srgb_to_linear(texture(pbr_emissive_map, pbr_uv).rgb) : vec3(1.0);
    pbr_emission *= pbr_material.emissive_normal_scale.rgb;
    vec3 pbr_direct = vec3(0.0), pbr_ambient = vec3(0.0);
)glsl";
    result.ShadingBody += options.NativeFragmentLighting
        ? "    bool pbr_unlit_reference = false;\n"
        : R"glsl(
    bool pbr_unlit_reference = (pica_surface.control.w & 1u) != 0u && pica_surface.control.x == 0u;
)glsl";
    result.ShadingBody += R"glsl(
    bool pbr_native_direct = pbr_unlit_reference || pica_surface.environment_parameters.w == 0.0 ||
        (pica_surface.control.x != 0u || pbr_material.native_control.x != 0u
            ? pica_surface.environment_parameters.x : pica_surface.environment_parameters.y) != 0.0;
    bool pbr_native_ambient = pica_surface.environment_parameters.w == 0.0 ||
        ((pica_surface.control.w & 1u) == 0u && pica_surface.environment_parameters.y != 0.0);
)glsl";
    if (options.NativeFragmentLighting) {
        result.ShadingBody += R"glsl(
    for (uint slot = 0u; (pbr_native_direct || pbr_native_ambient) && slot < pbr_material.native_control.x; ++slot) {
        uint i = pbr_material.native_lights[slot].x;
        uint flags = pbr_material.native_lights[slot].y;
        vec3 delta = fragment_uniforms.lighting_position[i].xyz;
        if ((flags & 1u) == 0u) delta += pica_view;
        float light_distance = length(delta);
        vec3 l = pica_surface_unit(delta);
        bool two_sided = (flags & 2u) != 0u;
        float attenuation = 1.0;
)glsl";
        if (options.NativeAttenuationLuts) {
            result.ShadingBody += R"glsl(
        if ((flags & 32u) != 0u)
            attenuation *= pbr_native_spot(int(8u + i), two_sided, pbr_normal, pbr_tangent,
                pbr_view, l, pbr_view + l, fragment_uniforms.lighting_spot_direction[i].xyz);
        if ((flags & 64u) != 0u)
            attenuation *= pica_lighting_lut_unsigned(int(16u + i), clamp(
                fragment_uniforms.lighting_attenuation[i].y * light_distance +
                fragment_uniforms.lighting_attenuation[i].x, 0.0, 1.0));
)glsl";
        }
        result.ShadingBody += R"glsl(
        if (pbr_native_direct) {
        vec3 diffuse, specular;
        pbr_ggx(pbr_normal, pbr_view, l, pbr_albedo, pbr_roughness, pbr_metallic,
                two_sided, diffuse, specular);
        if ((flags & 16u) != 0u) {
            if ((pbr_material.native_control.w & 2u) != 0u) diffuse *= pica_shadow_factor.rgb;
            if ((pbr_material.native_control.w & 4u) != 0u) specular *= pica_shadow_factor.rgb;
        }
        vec3 radiance = fragment_uniforms.lighting_diffuse[i].rgb;
        pbr_direct += (diffuse + specular) * radiance * attenuation * pica_surface_shadow(int(i));
        }
        if (pbr_native_ambient)
            pbr_ambient += fragment_uniforms.lighting_ambient[i].rgb * attenuation;
    }
    pbr_ambient += fragment_uniforms.lighting_global_ambient.rgb;
    pbr_ambient = pica_surface_resolved_ambient(pbr_ambient);
)glsl";
    } else {
        result.ShadingBody += R"glsl(
    if (pica_surface.control.x != 0u) {
        for (int slot = 0; pbr_native_direct && slot < 3; ++slot) {
            if (pica_surface.lights[slot].direction_enabled.w == 0.0) continue;
            if (any(notEqual(pica_surface.lights[slot].diffuse.rgb, vec3(0.0)))) {
            vec3 l, radiance;
            pica_surface_light(slot, l, radiance);
            vec3 diffuse, specular;
            pbr_ggx(pbr_normal, pbr_view, pica_surface_unit(l), pbr_albedo,
                pbr_roughness, pbr_metallic, false, diffuse, specular);
            pbr_direct += (diffuse + specular) * radiance * pica_surface.material_diffuse.rgb * pica_surface_shadow(slot);
            }
        }
    } else if (pbr_native_direct) {
        // The native shader explicitly selected an unlit material.
        pbr_direct = pbr_albedo * pica_surface_shadow(-1);
    }
    pbr_ambient = pica_surface.resolved_ambient.rgb;
)glsl";
    }
    result.ShadingBody += R"glsl(
    vec3 pbr_indirect = pbr_ambient *
        pbr_albedo * (1.0 - pbr_metallic) * pbr_ao;
    if (pica_surface.environment_parameters.w != 0.0) {
        if (!pbr_unlit_reference)
            pbr_direct *= pica_surface.control.x != 0u || pbr_material.native_control.x != 0u
                ? pica_surface.environment_parameters.x : pica_surface.environment_parameters.y;
        for (int slot = 0; slot < 2; ++slot) {
            if (pica_surface.environment_directions[slot].w <= 0.0) continue;
            vec3 diffuse, specular;
            pbr_ggx(pbr_normal, pbr_view,
                pica_surface_unit(pica_surface.environment_directions[slot].xyz),
                pbr_albedo, pbr_roughness, pbr_metallic, false, diffuse, specular);
            pbr_direct += (diffuse + specular) * pica_surface.environment_colors[slot].rgb *
                pica_surface.environment_directions[slot].w * pica_surface_shadow(8 + slot);
        }
    }
    for (uint slot = 0u; slot < pica_surface.control.z; ++slot) {
        if (all(equal(pica_surface.scene_directional_colors[slot].rgb, vec3(0.0)))) continue;
        vec3 diffuse, specular;
        pbr_ggx(pbr_normal, pbr_view,
            pica_surface_unit(pica_surface.scene_directions[slot].xyz),
            pbr_albedo, pbr_roughness, pbr_metallic, false, diffuse, specular);
        pbr_direct += (diffuse + specular) * pica_surface.scene_directional_colors[slot].rgb *
            pica_surface_shadow(10 + int(slot));
    }
    for (uint slot = 0u; slot < pica_surface.control.y; ++slot) {
        vec3 direction, radiance, diffuse, specular;
        pica_surface_scene_point(int(slot), direction, radiance);
        if (all(equal(radiance, vec3(0.0)))) continue;
        pbr_ggx(pbr_normal, pbr_view, direction, pbr_albedo,
            pbr_roughness, pbr_metallic, false, diffuse, specular);
        pbr_direct += (diffuse + specular) * radiance;
    }
    vec3 pbr_lit = max(pbr_direct + pbr_indirect + pbr_emission, vec3(0.0));
    vec3 pbr_ao_response = clamp(pbr_indirect / max(pbr_lit, vec3(1.0e-6)), 0.0, 1.0);
    vec3 pbr_f0 = mix(vec3(0.04), pbr_albedo, pbr_metallic);
    float pbr_reflectivity = clamp(max(max(pbr_f0.r, pbr_f0.g), pbr_f0.b), 0.0, 1.0);
    combiner_output.rgb = clamp(pbr_linear_to_srgb(pbr_lit), 0.0, 1.0);
    if (pbr_material.control.z != 0u) {
        pbr_ao_response = vec3(0.0);
        pbr_reflectivity = 0.0;
    }
)glsl";
    result.DiagnosticOutput = R"glsl(
    if (pbr_material.control.z == 1u)
        pica_color.rgb = pbr_normal * 0.5 + 0.5;
    else if (pbr_material.control.z == 2u)
        pica_color.rgb = vec3(pbr_roughness);
)glsl";
    return result;
}

} // namespace Fast::Renderer3ds
