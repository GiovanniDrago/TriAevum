#pragma once

#include "fast/renderer3ds/pica_lighting_program.h"
#include "fast/renderer3ds/pica_pbr_material.h"
#include "fast/renderer3ds/pica_shader_hooks.h"

#include <optional>
#include <string>
#include <string_view>

namespace Fast::Renderer3ds {

struct alignas(16) PicaPbrUniforms {
    // RGB base-color factor; dielectric normal-incidence reflectance in W.
    std::array<float, 4> BaseColorDielectricF0{};
    // Emissive RGB; normal scale with the normal-Y convention in its sign.
    std::array<float, 4> EmissiveNormalScale{};
    std::array<float, 4> OrmMinimumRoughness{};
    // Map mask, float bits of glossiness strength, diagnostic view, height scale bits.
    std::array<uint32_t, 4> Control{};
    // X: complete specular lobe weight; Y: combined screen-space reflection weight.
    std::array<float, 4> SpecularReflectionWeight{};
    // Strength, edge-relative width, channel bits, native primitive count bits.
    std::array<float, 4> SeamSettings{};
    PicaLightingProgram NativeLighting;
};
static_assert(sizeof(PicaPbrUniforms) == 352U);

struct PicaPbrShaderOptions {
    uint8_t TextureSlot = 0xffU;
    bool NativeFragmentLighting = false;
    bool NativeAttenuationLuts = false;
    bool SeamSoftening = false;
};

struct PicaPbrShader {
    std::string Declarations;
    std::string UnlitInputs;
    std::string NativeCoordinates;
    std::string ShadingBody;
    std::string_view DiagnosticOutput;
};

[[nodiscard]] PicaPbrUniforms BuildPicaPbrUniforms(
    const PicaPbrMaterialParameters& parameters, uint8_t textureMask,
    const PicaLightingProgram& nativeLighting) noexcept;
[[nodiscard]] std::optional<PicaPbrShader> BuildPicaPbrShader(
    const PicaShaderHookLayout& hooks, const PicaPbrShaderOptions& options);

} // namespace Fast::Renderer3ds
