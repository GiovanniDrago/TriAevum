#pragma once

#include "fast/renderer3ds/pica_light_binding.h"
#include "fast/renderer3ds/pica_shader_hooks.h"
#include "fast/renderer3ds/pica_environment_lighting.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Fast::Renderer3ds {

struct alignas(16) PicaSurfaceLightUniforms {
    std::array<float, 4> DirectionEnabled{};
    std::array<float, 4> Diffuse{};
    std::array<float, 4> Ambient{};
    // A positive radius identifies a point light; zero preserves the native direction.
    std::array<float, 4> PositionRadius{};
};

struct alignas(16) PicaScenePointLightUniforms {
    std::array<float, 4> PositionRadius{};
    std::array<float, 4> Color{};
};

struct alignas(16) PicaSurfaceLightingUniforms {
    std::array<float, 4> MaterialDiffuse{};
    std::array<float, 4> MaterialAmbient{};
    std::array<uint32_t, 4> Control{};
    std::array<std::array<float, 4>, 3> ViewToWorldLinear{};
    std::array<PicaSurfaceLightUniforms, 3> Lights{};
    std::array<float, 4> EnvironmentParameters{}; // native local, native ambient, fog, enabled
    std::array<float, 4> EnvironmentAmbient{};
    std::array<std::array<float, 4>, 2> EnvironmentDirections{};
    std::array<std::array<float, 4>, 2> EnvironmentColors{};
    std::array<std::array<float, 4>, kMaximumPicaSceneDirectionalLights> SceneDirections{};
    std::array<std::array<float, 4>, kMaximumPicaSceneDirectionalLights> SceneDirectionalColors{};
    std::array<PicaScenePointLightUniforms, kMaximumPicaScenePointLights> ScenePoints{};
};

static_assert(sizeof(PicaSurfaceLightingUniforms) == 1472U);

// Float-uniform bytes contain consecutive native vec4 values. Their enclosing
// packed-buffer layout belongs to the caller, not to the surface evaluator.
// Authored lighting requires the column-major WorldToView captured with this
// geometry sample. Neither a live camera nor a model/bone matrix is a substitute.
[[nodiscard]] std::optional<PicaSurfaceLightingUniforms> BuildPicaSurfaceLightingUniforms(
    const PicaVertexShaderHookLayout& hooks, uint32_t booleanUniforms,
    std::span<const uint8_t> floatUniformBytes,
    const PicaNativeLightBinding& binding,
    const PicaEnvironmentLighting& environment,
    const std::array<float, 16>* cameraWorldToView) noexcept;

[[nodiscard]] std::string_view PicaSurfaceLightingDeclarations() noexcept;
[[nodiscard]] std::string_view PicaSurfaceLightingBody() noexcept;

} // namespace Fast::Renderer3ds
