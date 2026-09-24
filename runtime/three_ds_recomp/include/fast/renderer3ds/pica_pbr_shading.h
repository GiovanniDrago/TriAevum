#pragma once

#include "fast/renderer3ds/pica_lighting_program.h"
#include "fast/renderer3ds/pica_pbr_material.h"
#include "fast/renderer3ds/pica_shader_hooks.h"

#include <optional>
#include <string>
#include <string_view>

namespace Fast::Renderer3ds {

struct alignas(16) PicaPbrUniforms {
    std::array<float, 4> BaseColorFactor{};
    std::array<float, 4> EmissiveNormalScale{};
    std::array<float, 4> OrmNormalY{};
    // Map mask, float bits of glossiness strength, diagnostic view, reserved.
    std::array<uint32_t, 4> Control{};
    PicaLightingProgram NativeLighting;
};
static_assert(sizeof(PicaPbrUniforms) == 320U);

struct PicaPbrShaderOptions {
    uint8_t TextureSlot = 0xffU;
    bool NativeFragmentLighting = false;
    bool NativeAttenuationLuts = false;
};

struct PicaPbrShader {
    std::string Declarations;
    std::string UnlitInputs;
    std::string ShadingBody;
    std::string_view DiagnosticOutput;
};

[[nodiscard]] PicaPbrUniforms BuildPicaPbrUniforms(
    const PicaPbrMaterialParameters& parameters, uint8_t textureMask,
    const PicaLightingProgram& nativeLighting, const PicaPbrControls& controls) noexcept;
[[nodiscard]] std::optional<PicaPbrShader> BuildPicaPbrShader(
    const PicaShaderHookLayout& hooks, const PicaPbrShaderOptions& options);

} // namespace Fast::Renderer3ds
