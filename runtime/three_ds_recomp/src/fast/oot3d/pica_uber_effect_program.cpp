#include "fast/oot3d/pica_uber_effect_program.h"

#include "fast/oot3d/pica_directional_shadow_lighting.h"
#include "fast/oot3d/pica_reactive_mask.h"
#include "fast/oot3d/pica_toon_shader.h"
#include "fast/renderer3ds/pica_material_guide.h"
#include "fast/renderer3ds/pica_surface_lighting.h"
#include "fast/renderer3ds/point_shadow_map.h"

#include <bit>

namespace Fast::Oot3d {
namespace {

using Decision = PicaEffectDecisions;
using ::Oot3d::Renderer::PicaShaderHook;

constexpr unsigned kNormalShift = kPicaUberNormalShift, kNormalAtShift = kPicaUberNormalAtShift,
                   kMaterialAtShift = kPicaUberMaterialAtShift, kCoverageShift = kPicaUberCoverageShift,
                   kAlbedoShift = kPicaUberAlbedoShift, kAmbientShift = kPicaUberAmbientShift,
                   kReactiveShift = kPicaUberReactiveShift;
constexpr unsigned kFieldMask = (1U << kPicaUberFieldBits) - 1U;

std::string Bit(uint32_t bit) { return "OOT3D_U(" + std::to_string(bit) + "u)"; }
std::string Field(unsigned shift, unsigned value) {
    return "(((oot3d_uber.control.y >> " + std::to_string(shift) + "u) & " + std::to_string(kFieldMask) + "u) == " +
        std::to_string(value) + "u)";
}
template <class Enum> std::string Is(unsigned shift, Enum value) {
    return Field(shift, static_cast<unsigned>(value));
}

std::string Guarded(const std::string& condition, std::string_view body) {
    return "    if (" + condition + ") {\n" + std::string(body) + "    }\n";
}

// Branches whose identifiers the base or the variant does not declare are
// unreachable there and omitted.
std::string NormalWrite(bool surfaceInputs, bool materialNormal) {
    const std::string native_view = "oot3d_normal_guide_surface(pica_normquat, pica_view)";
    const std::string native_rotate = "oot3d_normal_guide_rotate_z(pica_normquat)";
    std::string out;
    if (surfaceInputs)
        out += Guarded(Is(kNormalShift, Decision::NormalPath::Surface),
                       "    pica_normal_guide = vec4(pica_surface_unit(pica_surface_normal) * 0.5 + 0.5, 1.0);\n");
    if (materialNormal)
        out += Guarded(Is(kNormalShift, Decision::NormalPath::Material),
                       "    pica_normal_guide = vec4(normal * 0.5 + 0.5, 1.0);\n");
    if (surfaceInputs)
        out += Guarded(Is(kNormalShift, Decision::NormalPath::Typed),
                       "    pica_normal_guide = vec4(oot3d_normal_guide_typed(pica_surface_normal, " + native_view +
                       ") * 0.5 + 0.5, 1.0);\n");
    out += Guarded(Is(kNormalShift, Decision::NormalPath::NativeView),
                   "    pica_normal_guide = vec4(" + native_view + " * 0.5 + 0.5, 1.0);\n");
    out += Guarded(Is(kNormalShift, Decision::NormalPath::NativeRotate),
                   "    pica_normal_guide = vec4(" + native_rotate + " * 0.5 + 0.5, 1.0);\n");
    return out;
}

constexpr std::string_view kNormalHelpers = R"glsl(
vec3 oot3d_normal_guide_rotate_z(vec4 q) {
    float qlen = dot(q, q);
    if (qlen < 0.000001) return vec3(0.0, 0.0, 1.0);
    q *= inversesqrt(qlen);
    return vec3(2.0 * (q.x * q.z + q.w * q.y),
                2.0 * (q.y * q.z - q.w * q.x),
                1.0 - 2.0 * (q.x * q.x + q.y * q.y));
}

vec3 oot3d_normal_guide_surface(vec4 q, vec3 to_eye) {
    // Vertex-lit PICA programs can disable the normal-quaternion output.
    // Their view vector still describes geometry in the same camera basis.
    vec3 geometric = cross(dFdx(to_eye), dFdy(to_eye));
    float area = dot(geometric, geometric);
    if (dot(q, q) >= 0.000001) return oot3d_normal_guide_rotate_z(q);
    if (area <= 1.0e-20) return vec3(0.0, 0.0, 1.0);
    geometric *= inversesqrt(area);
    return dot(geometric, to_eye) < 0.0 ? -geometric : geometric;
}
)glsl";

constexpr std::string_view kTypedNormalHelper = R"glsl(
vec3 oot3d_normal_guide_typed(vec3 authored, vec3 native_normal) {
    float length2 = dot(authored, authored);
    return length2 > 1.0e-20 ? authored * inversesqrt(length2) : native_normal;
}
)glsl";

constexpr std::string_view kCalibratedMaterial =
    "    float oot3d_specular_peak = max(max("
    "secondary_fragment_color.r, secondary_fragment_color.g), "
    "secondary_fragment_color.b);\n"
    "    float oot3d_specular_energy = dot("
    "secondary_fragment_color.rgb, vec3(0.2126, 0.7152, 0.0722));\n"
    "    float oot3d_specular_response = smoothstep("
    "0.015, 0.300, max(oot3d_specular_peak, "
    "oot3d_specular_energy));\n"
    "    float oot3d_material_reflectivity = mix("
    "0.120, 1.0, oot3d_specular_response);\n"
    "    float oot3d_material_roughness = mix("
    "0.720, 0.160, oot3d_specular_response);\n"
    "    pica_material_guide = vec4("
    "oot3d_material_reflectivity, oot3d_material_roughness, "
    "(127.0 / 255.0), 0.0); // OOT3D calibrated specular material\n";

std::string MaterialBaseline(Decision::MaterialWriter at) {
    return Guarded(Is(kMaterialAtShift, at),
                   "    pica_material_guide = vec4(0.0); // OOT3D material guide baseline\n");
}

} // namespace

bool PicaUberEffectSupports(const PicaEffectDecisions& decisions) noexcept {
    return !decisions.Has(Decision::Pbr) && !decisions.Has(Decision::Relief) &&
           !decisions.Has(Decision::ReflectionPbr) &&
           !(decisions.Has(Decision::AmbientGuide) && decisions.Ambient == Decision::AmbientPath::Exact);
}

PicaUberEffectBlock PackPicaUberEffectBlock(const PicaEffectDecisions& decisions,
                                            const std::array<float, 32>& toon) noexcept {
    PicaUberEffectBlock block;
    block.Toon = toon;
    block.Control[0] = decisions.Bits;
    block.Control[1] = static_cast<uint32_t>(decisions.Normal) << kNormalShift |
        static_cast<uint32_t>(decisions.NormalAt) << kNormalAtShift |
        static_cast<uint32_t>(decisions.MaterialAt) << kMaterialAtShift |
        static_cast<uint32_t>(decisions.AlbedoCoverage) << kCoverageShift |
        static_cast<uint32_t>(decisions.Albedo) << kAlbedoShift |
        static_cast<uint32_t>(decisions.Ambient) << kAmbientShift |
        static_cast<uint32_t>(decisions.ReactiveKind) << kReactiveShift;
    block.Control[2] = static_cast<uint32_t>(decisions.PbrCoordinate) | (decisions.PbrProjected ? 4U : 0U) |
        (decisions.PbrNativeLighting ? 8U : 0U) | (decisions.PbrAttenuationLuts ? 16U : 0U) |
        (decisions.PbrSeams ? 32U : 0U);
    block.Control[3] = static_cast<uint32_t>(decisions.ReliefUnit) |
        static_cast<uint32_t>(decisions.ReliefCoordinate) << 2U;
    block.Values = {decisions.ReliefGain, decisions.ReflectionReflectivity, decisions.ReflectionRoughness, 0.0F};
    return block;
}

std::string BuildPicaUberEffectProgram(const PicaUberEffectProgramRequest& request) {
    const auto& hooks = request.BaseHooks;
    const auto source = request.BaseSource;
    if (!hooks.ValidFor(source) || !hooks.Supports(PicaShaderHook::PicaLighting) ||
        !hooks.Supports(PicaShaderHook::AfterNativeTev) || !hooks.Supports(PicaShaderHook::BeforeDepth) ||
        !hooks.Supports(PicaShaderHook::MainPrologue))
        return {};
    const size_t global = hooks.Offset(PicaShaderHook::GlobalDeclarations);
    const size_t prologue = hooks.Offset(PicaShaderHook::MainPrologue);
    const size_t lighting = hooks.Offset(PicaShaderHook::PicaLighting);
    const size_t afterTev = hooks.Offset(PicaShaderHook::AfterNativeTev);
    const size_t beforeDepth = hooks.Offset(PicaShaderHook::BeforeDepth);
    const size_t epilogue = hooks.Offset(PicaShaderHook::MainEpilogue);
    if (!(global <= prologue && prologue <= lighting && lighting <= afterTev && afterTev <= beforeDepth &&
          beforeDepth <= epilogue && epilogue <= source.size()))
        return {};
    const bool materialNormal = hooks.Has(::Oot3d::Renderer::PicaShaderSemantic::MaterialNormal);
    const bool explicitDepth = source.find("gl_FragDepth = pica_depth;") != std::string_view::npos;
    const std::string depth = explicitDepth ? "gl_FragDepth" : "gl_FragCoord.z";
    const std::string tev(source.substr(lighting, afterTev - lighting));

    // Declarations: every output, input and function any branch uses.
    std::string declarations = "\n// OOT3D always-available effect layer\n"
        "#define OOT3D_U(bit) ((oot3d_uber.control.x & (bit)) != 0u)\n";
    declarations +=
        "layout(location=1) out vec4 pica_normal_guide;\n"
        "layout(location=2) out vec4 pica_material_guide;\n"
        "layout(location=4) out vec4 pica_ambient_guide;\n"
        "layout(location=5) out vec4 pica_fog_guide;\n"
        "layout(location=6) out vec4 pica_outline_geometry_guide;\n"
        "layout(location=7) out vec4 pica_diffuse_albedo_guide;\n";
    if (request.TemporalInputs) {
        declarations +=
            "layout(location=3) out vec4 pica_rigid_motion_guide;\n"
            "#define pica_transparent_depth_guide pica_rigid_motion_guide\n"
            "layout(location=7) in vec4 oot3d_current_clip;\n"
            "layout(location=8) in vec4 oot3d_previous_clip;\n"
            "layout(push_constant) uniform Oot3dDrawState { vec4 rigid_motion; vec4 jitter; } oot3d_draw;\n";
    } else {
        declarations += "layout(location=3) out vec4 pica_transparent_depth_guide;\n";
    }
    const std::string block =
        "layout(set=0,binding=15,std140) uniform Oot3dUberEffects {\n"
        "    ToonSurfaceParameters toon;\n    uvec4 control;\n    vec4 values;\n} oot3d_uber;\n";
    declarations += PicaToonUberDeclarations(block, Bit(Decision::ToonVertexLighting) + " ? 1.0 : 0.0");
    declarations += kNormalHelpers;
    declarations += Renderer3ds::PicaMaterialGuideShaderLibrary;
    if (request.SurfaceInputs) {
        declarations += Renderer3ds::PicaSurfaceLightingDeclarations();
        declarations += kTypedNormalHelper;
        declarations += PicaDirectionalShadowLightingDeclarations();
        declarations += "\nvec3 pica_surface_shadow(int slot) {\n"
                        "    if (!" + Bit(Decision::DirectionalShadow) + ") return vec3(1.0);\n"
                        "    uint selected = uint(pica_vsm.shadowed_direct_fraction.w);\n"
                        "    if (slot < 0) return vec3(1.0);\n"
                        "    return (selected & (1u << uint(slot))) != 0u\n"
                        "        ? oot3d_directional_shadow_visibility() : vec3(1.0);\n}\n";
        std::string point(Renderer3ds::PointShadowReceiverShader());
        const std::string_view name = "float pica_point_shadow(float slot, vec3 light_to_fragment, float radius) {";
        const size_t at = point.find(name);
        if (at == std::string::npos) return {};
        point.replace(at, name.size(), "float pica_point_shadow_receiver(float slot, vec3 light_to_fragment, float radius) {");
        declarations += point;
        declarations += "\nfloat pica_point_shadow(float slot, vec3 light_to_fragment, float radius) {\n"
                        "    return " + Bit(Decision::PointShadow) +
                        " ? pica_point_shadow_receiver(slot, light_to_fragment, radius) : 1.0;\n}\n";
    }

    // Insertions at the native hooks, in the specialized composition's order.
    std::string atLighting;
    if (request.SurfaceInputs) {
        atLighting += "\n    vec4 surface_native_primary = rounded_primary_color;\n"
                      "    vec4 surface_native_fragment = primary_fragment_color;\n"
                      "    vec4 surface_native_secondary = secondary_fragment_color;\n";
        atLighting += Guarded(Bit(Decision::SurfaceLighting), Renderer3ds::PicaSurfaceLightingBody());
    }
    if (materialNormal)
        atLighting += Guarded(Bit(Decision::ToonMaterialPath),
            "\n    primary_fragment_color.rgb = "
            "oot3d_material_bands(primary_fragment_color.rgb, true);\n"
            "    secondary_fragment_color.rgb = clamp("
            "oot3d_material_bands(secondary_fragment_color.rgb, false) + "
            "oot3d_material_rim(normal), 0.0, 1.0);\n");
    std::string atAfterTev;
    if (request.SurfaceInputs) {
        atAfterTev += Guarded(Bit(Decision::SurfaceLighting), R"glsl(
    {
        vec3 surface_lit_rgb = combiner_output.rgb;
        vec4 rounded_primary_color = surface_native_primary;
        vec4 primary_fragment_color = surface_native_fragment;
        vec4 secondary_fragment_color = surface_native_secondary;
        vec4 combiner_buffer = vec4(0.0);
        vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;
        combiner_output = vec4(0.0);
)glsl" + tev + "\n        combiner_output.rgb = surface_lit_rgb;\n    }\n");
        atAfterTev += "\n    vec3 surface_before_fog = combiner_output.rgb;\n";
        atAfterTev += "    vec3 surface_gi_albedo = vec3(0.0);\n";
        atAfterTev += Guarded(Bit(Decision::AlbedoReplay), R"glsl(
    {
        vec4 surface_gi_saved = combiner_output;
        vec4 rounded_primary_color = vec4(vec3(1.0), surface_native_primary.a);
        vec4 primary_fragment_color = vec4(vec3(1.0), surface_native_fragment.a);
        vec4 secondary_fragment_color = vec4(vec3(0.0), surface_native_secondary.a);
        vec4 combiner_buffer = vec4(0.0);
        vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;
        combiner_output = vec4(0.0);
)glsl" + tev + "\n        surface_gi_albedo = clamp(combiner_output.rgb, 0.0, 1.0);\n"
                       "        combiner_output = surface_gi_saved;\n    }\n");
    }
    const std::string atBeforeDepth = Guarded(
        Bit(Decision::Toon) + " && !" + Bit(Decision::ToonMaterialPath),
        "    combiner_output.rgb = oot3d_apply_toon(combiner_output.rgb);\n");

    // Epilogue: fog, normal, outline, albedo, AO, scene domain, reflection,
    // reactive, baseline, rigid motion, deferred guides, environment mix.
    std::string end;
    end += Guarded(Bit(Decision::FogGuide) + " && " + Bit(Decision::FogGuideFactor),
                   "    pica_fog_guide = vec4(fragment_uniforms.fog_color.rgb, fog_factor);\n");
    end += Guarded(Bit(Decision::FogGuide) + " && !" + Bit(Decision::FogGuideFactor),
                   "    pica_fog_guide = vec4(0.0, 0.0, 0.0, 1.0);\n");
    end += "    if (" + Is(kNormalAtShift, Decision::NormalWriter::NormalGuide) + ") {\n" + NormalWrite(request.SurfaceInputs, materialNormal) + "    }\n";
    end += "    if (" + Is(kNormalAtShift, Decision::NormalWriter::Outline) + ") {\n" + NormalWrite(request.SurfaceInputs, materialNormal) + "    }\n";
    end += Guarded(Bit(Decision::Outline) + " && " + Bit(Decision::OutlineClear),
                   "    pica_outline_geometry_guide = vec4(0.0, 0.0, 0.0, 1.0);\n");
    end += Guarded(Bit(Decision::Outline) + " && !" + Bit(Decision::OutlineClear),
                   "    pica_outline_geometry_guide = vec4(pica_normal_guide.rgb * 2.0 - 1.0, " + depth + ");\n");
    {
        std::string albedo =
            "    float oot3d_albedo_coverage = " + Is(kCoverageShift, Decision::Coverage::One) + " ? 1.0 : " +
            Is(kCoverageShift, Decision::Coverage::Zero) + " ? 0.0 : clamp(pica_color.a, 0.0, 1.0);\n";
        albedo += Guarded(Is(kAlbedoShift, Decision::AlbedoPath::Zero), "    pica_diffuse_albedo_guide = vec4(0.0);\n");
        if (request.SurfaceInputs)
            albedo += Guarded(Is(kAlbedoShift, Decision::AlbedoPath::Surface),
                "    pica_diffuse_albedo_guide = vec4((floor(surface_gi_albedo * 255.0 + 0.5) * 256.0 + "
                "floor(clamp(surface_gi_albedo * pica_surface_gi_direct(pica_surface_unit(pica_surface_normal)), "
                "0.0, 1.0) * 255.0 + 0.5)) / 65535.0, oot3d_albedo_coverage);\n");
        end += Guarded(Bit(Decision::AlbedoGuide), albedo);
    }
    end += Guarded(Bit(Decision::AmbientGuide),
                   "\n    pica_ambient_guide = vec4(1.0, 1.0, 1.0, 1.0);\n");
    end += Guarded(Bit(Decision::SceneReset), "    pica_transparent_depth_guide = vec4(0.0);\n");
    {
        std::string overlay = "    float oot3d_scene_alpha = " + Bit(Decision::SceneBlended) + " ? pica_color.a : 0.0;\n";
        overlay += Guarded(Bit(Decision::SceneOutline), "    pica_normal_guide.a = oot3d_scene_alpha;\n");
        overlay += Guarded(Bit(Decision::SceneAmbient), "    pica_ambient_guide = vec4(1.0, 1.0, 1.0, oot3d_scene_alpha);\n");
        overlay += Guarded(Bit(Decision::SceneAlbedo),
                           "    pica_diffuse_albedo_guide = vec4(0.0, 0.0, 0.0, oot3d_scene_alpha);\n");
        end += Guarded(Bit(Decision::SceneOverlay), overlay);
    }
    end += MaterialBaseline(Decision::MaterialWriter::Reflection);
    end += Guarded(Bit(Decision::Reflection) + " && " + Bit(Decision::ReflectionExplicit),
                   "    pica_material_guide = vec4(oot3d_uber.values.y, oot3d_uber.values.z, (127.0 / 255.0), 0.0);\n");
    end += Guarded(Bit(Decision::Reflection) + " && !" + Bit(Decision::ReflectionExplicit), kCalibratedMaterial);
    end += MaterialBaseline(Decision::MaterialWriter::Reactive);
    {
        std::string reactive;
        for (auto coverage : {PicaReactiveCoverage::SourceAlpha, PicaReactiveCoverage::SourceColor,
                              PicaReactiveCoverage::Full})
            reactive += Guarded(Is(kReactiveShift, coverage), BuildPicaReactiveMaskAssignment({coverage}));
        end += Guarded(Bit(Decision::Reactive), reactive);
    }
    end += MaterialBaseline(Decision::MaterialWriter::Baseline);
    if (request.TemporalInputs)
        end += Guarded(Bit(Decision::RigidMotion),
            "    vec4 oot3d_motion = oot3d_draw.rigid_motion;\n"
            "    if (oot3d_draw.rigid_motion.a > 0.5 &&\n"
            "        oot3d_current_clip.w > 1.0e-7 && oot3d_previous_clip.w > 1.0e-7) {\n"
            "        vec2 oot3d_current_uv = oot3d_current_clip.xy / oot3d_current_clip.w * 0.5 + 0.5;\n"
            "        vec2 oot3d_previous_uv = oot3d_previous_clip.xy / oot3d_previous_clip.w * 0.5 + 0.5;\n"
            "        oot3d_motion = vec4(oot3d_previous_uv - oot3d_current_uv, 1.0, 1.0);\n"
            "    }\n"
            "    pica_rigid_motion_guide = vec4(oot3d_motion.xy, oot3d_motion.z, 0.0);\n");
    {
        std::string transparent = Guarded("!" + Bit(Decision::SceneTransparentRigid),
                                          "    pica_transparent_depth_guide.rgb = vec3(0.0);\n");
        transparent += "    pica_transparent_depth_guide.a = pica_color.a > 0.0039215686 ? clamp(1.0 - " + depth +
                       ", 0.0000152588, 1.0) : 0.0;\n";
        end += Guarded(Bit(Decision::SceneTransparentDepth), transparent);
    }
    if (request.SurfaceInputs)
        end += Guarded(Bit(Decision::EnvironmentMix),
                       "\n    if (pica_surface.environment_parameters.w != 0.0)\n"
                       "        pica_color.rgb = mix(surface_before_fog, pica_color.rgb, pica_surface.environment_parameters.z);\n");

    std::string result;
    result.reserve(source.size() * 3 + declarations.size() + end.size());
    result.append(source.substr(0, global));
    result += declarations;
    result.append(source.substr(global, lighting - global));
    // Hook offsets follow native comments without a line break.
    result += "\n" + atLighting;
    result.append(source.substr(lighting, afterTev - lighting));
    result += "\n" + atAfterTev;
    result.append(source.substr(afterTev, beforeDepth - afterTev));
    result += "\n" + atBeforeDepth;
    result.append(source.substr(beforeDepth, epilogue - beforeDepth));
    result += "\n" + end;
    result.append(source.substr(epilogue));
    return result;
}

} // namespace Fast::Oot3d
