#include "fast/renderer3ds/pica_surface_lighting.h"
#include "vulkan_compute_test.h"
#include <array>
#include <cmath>
#include <iostream>
#include <regex>

int main() {
    try {
        using namespace Fast::Renderer3ds;
        using Vec4 = std::array<float, 4>;
        PicaVertexShaderHookLayout hooks;
        hooks.Transform = { PicaVertexTransformOperation::ModelViewProjection3x4, 0, 4, 4, 3, 20, 3, 0, 1 };
        hooks.Skeleton = { PicaVertexSkeletonOperation::MatrixPalette3x4, 2, 3, 20, 3, 6, 7, 4 };
        auto& surface = hooks.Surface;
        surface.ViewNormalTemporary = 14;
        surface.LightingBooleanUniform = 9;
        surface.DiffuseUniform = 8;
        surface.AmbientUniform = 9;
        surface.TintBooleanUniform = 5;
        surface.TintInputRegister = 2;
        surface.TintScale = { 90, 2 };
        surface.LightEnabledValue = { 93, 1 };
        surface.LightDirectionUniforms = { 80, 83, 86 };
        surface.BoneIndexScale = { 93, 3 };
        surface.BoneWeightScale = { 94, 3 };
        surface.SkinnedNormalScale = { 90, 1 };
        surface.InfluenceCount = { 92, 3 };
        surface.AdditionalInfluenceThresholds = { PicaVertexUniformComponent{ 95, 0 },
                                                  PicaVertexUniformComponent{ 95, 1 } };
        std::array<Vec4, 96> native{};
        native[8] = native[9] = { .5F, .5F, .5F, 1 };
        native[80] = { 0, -1, 0, 1 };
        native[81] = { .8F, .8F, .8F, 0 };
        native[82] = { .4F, .4F, .4F, 0 };
        native[93][1] = 1;
        struct Case {
            PicaSurfaceLightingUniforms Lighting;
            Vec4 View{}, Normal{ 0, 1, 0, 0 }, Base{ .2F, .3F, .4F, 1 };
        };
        std::array<Case, 15> cases{};
        const std::array<Vec4, 15> expected{ { { .575F, .2F, .2F, 1 },
                                               { .2F, .2F, .2F, 1 },
                                               { .2F, .2F, .2F, 1 },
                                               { .2F, .3F, .4F, 1 },
                                               { .575F, .3F, .4F, 1 },
                                               { .575F, .2F, .2F, 1 },
                                               { .66875F, .2F, .2F, 1 },
                                               { .3F, 0, 0, 1 },
                                               { .3F, 0, 0, 1 },
                                               { 0, 0, .4F, 1 },
                                               { 0, .15F, 0, 1 },
                                               { .3F, 0, 0, 1 },
                                               { .2F, .2F, .2F, 1 },
                                               { .5F, .3F, .4F, 1 },
                                               { 0, 0, 0, 1 } } };
        for (size_t i = 0; i < cases.size(); ++i) {
            PicaNativeLightBinding binding;
            auto points = std::make_shared<PicaScenePointLights>();
            points->Count = 1;
            points->Lights[0] = { 42, { 0, 10, 0 }, { 1, 0, 0 }, 20, 0 };
            auto& selected = binding.Sources[0];
            selected.Kind = PicaNativeLightSource::Type::Point;
            selected.SourceIdentity = 42;
            selected.PositionWorld = { 0, 10, 0 };
            selected.Radius = 20;
            selected.UnattenuatedColor = selected.ReferenceColor = { 1, 1, 1 };
            binding.ScenePoints = points;
            PicaEnvironmentLighting settings;
            std::array<float, 16> camera{ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
            if (i == 1)
                settings.PointLights[0].Enabled = false;
            if (i == 2)
                points->Lights[0].Radius = 0;
            if (i == 3)
                cases[i].View[1] = 20; // A natively unlit pixel outside the sphere.
            if (i == 5)
                camera[5] = 2; // Native world radius must not become a view-space radius.
            if (i == 6) {
                points->Lights[0].Group = 1;
                settings.PointLights[0].Enabled = false;
                settings.PointLights[1].Intensity = 2;
                settings.PointLights[1].RadiusScale = 2;
                settings.PointLights[1].ColorTint = { .5F, 1, 1 };
            }
            auto uniforms = native;
            if (i >= 7) {
                binding = {};
                auto directions = std::make_shared<PicaSceneDirectionalLights>();
                directions->Count = 2;
                directions->Lights[0] = {51, {0,1,0}, {.6F,0,0}};
                directions->Lights[1] = {52, {0,-1,0}, {0,0,.8F}};
                binding.SceneDirectionals = directions;
                settings.Enabled = i != 13;
                settings.AmbientIntensity = 0;
                settings.NativeDirectStrength = i == 10 || i == 12 ? 0.0F : 1.0F;
                settings.NativeLocalStrength = i == 8 || i == 10 ? 1.0F : 0.0F;
                settings.NativeAmbientStrength = i == 12 ? 1.0F : 0.0F;
                if (i != 7 && i != 13) {
                    binding.Sources[0].Kind = PicaNativeLightSource::Type::Directional;
                    binding.Sources[0].SourceIdentity = 51;
                }
                if (i == 9) cases[i].Normal[1] = -1;
                if (i == 10 || i == 11) {
                    auto& local = binding.Sources[1];
                    local.Kind = PicaNativeLightSource::Type::Point;
                    local.SourceIdentity = 73;
                    local.PositionWorld = {0,10,0};
                    local.UnattenuatedColor = local.ReferenceColor = {0,.4F,0};
                    local.Radius = 20;
                    uniforms[83] = {0,-1,0,1};
                    uniforms[84] = {0,.4F,0,0};
                }
                if (i == 14) {
                    // Native lighting is disabled, but unused slot uniforms
                    // may still contain the preceding draw's values.
                    binding.Sources = {};
                    settings.NativeDirectStrength = 0;
                    settings.NativeLocalStrength = 1;
                }
            }
            const auto lighting = BuildPicaSurfaceLightingUniforms(
                hooks, i == 3 || i == 4 || i == 7 || i == 13 || i == 14 ? 0U : 1U << 9U,
                { reinterpret_cast<const uint8_t*>(uniforms.data()), sizeof(uniforms) }, binding, settings, &camera);
            if (!lighting)
                throw std::runtime_error("Scene point-light surface rejected");
            cases[i].Lighting = *lighting;
        }
        // Change stage/storage declarations only; execute the production lighting
        // arithmetic verbatim against CPU-built native binding snapshots.
        auto declarations =
            std::regex_replace(std::string(PicaSurfaceLightingDeclarations()), std::regex(R"(layout\([^)]*\)\s*)"), "");
        declarations = std::regex_replace(declarations, std::regex(R"(\bin\s+)"), "");
        declarations = std::regex_replace(declarations, std::regex(R"(\buniform\b)"), "struct");
        std::string shader = "#version 450\nlayout(local_size_x=1) in;\nvec3 pica_view;\n" + declarations;
        shader += R"glsl(
struct Case { PicaSurfaceLighting lighting; vec4 view; vec4 normal; vec4 base; };
layout(set=0,binding=0,std430) readonly buffer Cases { Case cases[]; } inputs;
layout(set=0,binding=1,std430) writeonly buffer Results { vec4 values[]; } outputs;
vec3 pica_surface_shadow(int slot) { return vec3(1.0); }
void main() {
    uint index = gl_GlobalInvocationID.x;
    pica_surface = inputs.cases[index].lighting;
    pica_view = inputs.cases[index].view.xyz;
    pica_surface_normal = inputs.cases[index].normal.xyz;
    pica_surface_tint = vec4(1.0);
    outputs.values[index] = vec4(pica_surface_primary(inputs.cases[index].base.rgb, pica_surface_normal), 1.0);
}
)glsl";
        std::array<Vec4, cases.size()> output{};
        RunComputeTest(shader, std::as_bytes(std::span(cases)), std::as_writable_bytes(std::span(output)),
                       cases.size());
        unsigned failures = 0;
        for (size_t i = 0; i < cases.size(); ++i)
            for (size_t c = 0; c < 4; ++c)
                if (!std::isfinite(output[i][c]) || std::abs(output[i][c] - expected[i][c]) > 2e-5F) {
                    std::cerr << "case=" << i << " channel=" << c << " actual=" << output[i][c]
                              << " expected=" << expected[i][c] << '\n';
                    ++failures;
                }
        std::cout << "Scene lighting: " << cases.size() << " GPU cases, " << failures << " failures\n";
        return failures ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
