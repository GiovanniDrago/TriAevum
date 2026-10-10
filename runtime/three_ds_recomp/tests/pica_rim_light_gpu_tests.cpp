#include "fast/renderer3ds/pica_surface_lighting.h"
#include "fast/renderer3ds/pica_surface_lighting_shader.h"
#include "fast/renderer3ds/pica_pbr_shading.h"
#include "vulkan_compute_test.h"
#include <cmath>
#include <iostream>
#include <numbers>
#include <regex>
#include <vector>

// Execute the production evaluator, replacing only stage/storage declarations
// and the visibility lookup with controlled test inputs. No ROM or game.
int main() try {
    using namespace Fast::Renderer3ds;
    using V4 = std::array<float, 4>;
    struct Case {
        PicaSurfaceLightingUniforms Lighting;
        V4 View{0, 0, 1, 0}, Normal{1, 0, 0, 0}, Visibility{1, 1, 0, 0};
    };
    std::vector<Case> cases;
    std::vector<V4> expected;
    const auto add = [&](const Case& c, float gain) {
        cases.push_back(c);
        expected.push_back({0.3F * gain, 0.2F * gain, 0.1F * gain, 0});
    };
    constexpr V4 color{0.3F, 0.2F, 0.1F, 0};
    constexpr float window = (1.0F - 0.0625F) * (1.0F - 0.0625F);
    for (unsigned source = 0; source < 12; ++source) {
        Case c;
        auto& s = c.Lighting;
        s.Control[0] = 1;
        s.MaterialDiffuse = {1, 1, 1, 0};
        s.EnvironmentParameters = {1, 0, 0, 1};
        s.GlobalIlluminationSources = {1, 1, 1, 0};
        for (int i = 0; i < 3; ++i) s.ViewToWorldLinear[i][i] = 1;
        PicaRimLightSettings rim;
        rim.Enabled = true; rim.Intensity = 0.3F; rim.Ambient = 0;
        AssignPicaSurfaceRim(s, rim);
        float attenuation = 1;
        if (source < 2) {
            s.EnvironmentDirections[source] = {0, 0, -1, 1};
            s.EnvironmentColors[source] = color;
        } else if (source == 2) {
            s.Control[2] = 1; s.SceneDirections[0] = {0, 0, -1, 0}; s.SceneDirectionalColors[0] = color;
        } else if (source < 5) {
            s.Lights[0].DirectionEnabled = {0, 0, -1, 1}; s.Lights[0].Diffuse = color;
            if (source == 4) { s.Lights[0].PositionRadius = {0, 0, -101, 200}; attenuation = 0.75F; }
        } else if (source == 5) {
            s.Control[1] = 1; s.ScenePoints[0].PositionRadius = {0, 0, -101, 200};
            s.ScenePoints[0].Color = color; attenuation = 0.75F;
        } else {
            auto& p = s.Placed[0];
            const unsigned kind = source - 6; // omni, point, spot, rectangle, disc, sun
            s.Control[3] = 1U << 8U;
            p.PositionReach = {0, 0, -101, 200}; p.Color = color; p.AxisKind = {0, 0, 1, float(kind)};
            p.TangentShape = {10, 0, 0, 10};
            attenuation = kind == 0 ? 0.75F : kind == 5 ? 1.0F : window;
            if (kind == 2) p.TangentShape = {0.8F, 0.95F, 0, 0};
            if (kind == 3) attenuation *= 400.0F / (std::numbers::pi_v<float> * 10000.0F + 400.0F);
            if (kind == 4) attenuation *= 100.0F / 10100.0F;
        }
        // Grazing N.V=0, backlight=1, wrap=1/3: exactly intensity/3.
        const float gain = 0.1F * attenuation;
        add(c, gain);
        auto off = c; off.Lighting.Rim = {}; add(off, 0);
        auto facing = c; facing.Normal = {0, 0, 1, 0}; add(facing, 0);
        auto shadow = c; shadow.Visibility = {}; add(shadow, source == 1 || source == 11 ? gain : 0);
        shadow.Visibility = {0.25F, 0.25F, 0, 0};
        // Native point uses both lookups in the surface path; directional
        // visibility only applies to directional sources in the real shader.
        add(shadow, source == 1 || source == 11 ? gain : gain * 0.25F);
    }
    Case c = cases[0];
    c.Lighting.EnvironmentDirections[0] = {0, 0, 1, 1}; add(c, 0); // Light at the camera: no rim.
    c = cases[0]; c.Lighting.EnvironmentDirections[0] = {0, 1, 0, 1};
    c.Normal = {std::sqrt(0.75F), 0, 0.5F, 0};
    add(c, 0.3F * (1.0F / 16.0F) * (7.0F / 27.0F) / 3.0F);
    c.Lighting.Rim[1] = 8;
    add(c, 0.3F * (1.0F / 256.0F) * (7.0F / 27.0F) / 3.0F);
    c = cases[0]; c.Normal = {}; add(c, 0); // Invalid normal is not a full-strength edge.
    c = cases[0]; c.Lighting.EnvironmentDirections = {}; c.Lighting.ResolvedAmbient = color;
    c.Lighting.Rim = {0, 4, 0.1F, 0}; add(c, 1.1F); // Only the separately controlled ambient edge remains.
    // Outside a point's range: no direct illumination and no rim.
    c = cases[5 * 5]; c.Lighting.ScenePoints[0].PositionRadius[3] = 50; add(c, 0);

    auto declarations = std::regex_replace(std::string(PicaSurfaceLightingDeclarations()),
                                           std::regex(R"(layout\([^)]*\)\s*)"), "");
    declarations = std::regex_replace(declarations, std::regex(R"(\bin\s+)"), "");
    declarations = std::regex_replace(declarations, std::regex(R"(\buniform\b)"), "struct");
    std::string shader = "#version 450\nlayout(local_size_x=1) in;\nvec3 pica_view;\nvec4 test_visibility;\n" + declarations;
    shader += R"glsl(
struct Case { PicaSurfaceLighting lighting; vec4 view; vec4 normal; vec4 visibility; };
layout(set=0,binding=0,std430) readonly buffer Cases { Case cases[]; } inputs;
layout(set=0,binding=1,std430) writeonly buffer Results { vec4 values[]; } outputs;
vec3 pica_surface_shadow(int slot) {
    if (slot == 9 || (slot >= 0 && slot < 3 && pica_surface.lights[slot].position_radius.w > 0.0)) return vec3(1.0);
    return vec3(test_visibility.x);
}
float pica_point_shadow(float slot, vec3 light_to_fragment, float radius) { return test_visibility.y; }
void main() {
    uint i = gl_GlobalInvocationID.x;
    pica_surface = inputs.cases[i].lighting;
    pica_view = inputs.cases[i].view.xyz;
    pica_surface_normal = inputs.cases[i].normal.xyz;
    pica_surface_tint = vec4(1.0);
    test_visibility = inputs.cases[i].visibility;
    outputs.values[i] = vec4(pica_surface_primary(vec3(0), pica_surface_normal),
                             length(pica_surface_gi_direct(pica_surface_normal)));
}
)glsl";
    std::vector<V4> results(cases.size());
    RunComputeTest(shader, std::as_bytes(std::span(cases)), std::as_writable_bytes(std::span(results)),
                   uint32_t(cases.size()));
    unsigned failures = 0;
    for (size_t i = 0; i < cases.size(); ++i)
        for (size_t k = 0; k < 4; ++k)
            if (!std::isfinite(results[i][k]) || std::abs(results[i][k] - expected[i][k]) > 2.0e-5F) {
                std::cerr << "rim case " << i << " channel " << k << ": " << results[i][k]
                          << " expected " << expected[i][k] << '\n';
                ++failures;
            }

    // Compile full production PBR bodies, including the mapped-normal and
    // native-fragment-light branches. The same source supplies specialized
    // and uber materials. Texture content and game appearance are lab checks.
    for (bool native : {false, true}) {
        const auto pbr = BuildPicaPbrUberShader({"true", "0u", "false", native ? "true" : "false", "true", "false"},
                                              native, false);
        std::string fragment = R"glsl(#version 450
vec3 pica_view = vec3(0,0,1);
vec2 pica_texcoord0 = vec2(0), pica_texcoord1 = vec2(0), pica_texcoord2 = vec2(0);
float pica_texcoord0_w = 1.0;
vec4 rounded_primary_color = vec4(1), primary_fragment_color = vec4(1), secondary_fragment_color = vec4(0);
vec4 combiner_output = vec4(0.5), pica_shadow_factor = vec4(1);
layout(location=0) out vec4 pica_color;
struct NativeLightUniforms { vec4 lighting_position[8]; vec4 lighting_diffuse[8]; vec4 lighting_ambient[8];
                            vec4 lighting_attenuation[8]; vec3 lighting_spot_direction[8]; vec4 lighting_global_ambient; };
NativeLightUniforms fragment_uniforms;
float pica_lighting_lut_unsigned(int table, float value) { return value; }
float pica_lighting_lut_signed(int table, float value) { return value; }
vec3 byteround3(vec3 value) { return value; }
)glsl";
        fragment += PicaSurfaceLightingDeclarations();
        fragment += "\nvec3 pica_surface_shadow(int slot) { return vec3(1); }\n"
                    "float pica_point_shadow(float slot, vec3 delta, float radius) { return 1.0; }\n";
        fragment += pbr.Declarations;
        fragment += "\nvoid main(){\n" + pbr.MainDeclarations + pbr.UnlitInputs + pbr.ShadingBody +
                    "pica_color = combiner_output;\n}\n";
        shaderc::Compiler compiler;
        const auto compiled = compiler.CompileGlslToSpv(fragment, shaderc_fragment_shader, "rim_pbr.frag");
        if (compiled.GetCompilationStatus() != shaderc_compilation_status_success)
            throw std::runtime_error(compiled.GetErrorMessage());
    }
    std::cout << "Rim: " << cases.size() << " GPU cases, " << failures
              << " failures; 2 complete PBR shader variants compiled\n";
    return failures == 0 ? 0 : 1;
} catch (const std::exception& e) {
    std::cerr << "Rim tests FAILED: " << e.what() << '\n';
    return 1;
}
