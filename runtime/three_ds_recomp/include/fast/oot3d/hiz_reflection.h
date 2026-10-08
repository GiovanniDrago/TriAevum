#pragma once

#include "fast/renderer3ds/pica_scene_water.h"

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Oot3d {

[[nodiscard]] std::string BuildHiZReflectionComputeShader();
[[nodiscard]] std::string BuildHiZReflectionBilateralFilterShader();

// Gameplay water for the trace. The title's transparent water writes no
// depth, so the trace finds the water surface itself: per pixel, the nearest
// listed body's horizontal surface the view ray meets inside its footprint,
// in front of the scene depth. Waves and Fresnel as the surface shader.
inline constexpr uint32_t kHiZWaterPlanes = 8U;

struct alignas(16) HiZWaterUniforms {
    // View to world, rows (xyz linear, w translation: the camera).
    std::array<std::array<float, 4>, 3> ViewToWorld{};
    // Bodies' footprints (min x, min z, max x, max z) and heights.
    std::array<std::array<float, 4>, kHiZWaterPlanes> Footprints{};
    std::array<std::array<float, 4>, kHiZWaterPlanes / 4U> Heights{};
    // Body count, wave time (seconds), Fresnel F0, water only (1: nothing
    // else reflects).
    std::array<float, 4> Parameters{};
};
static_assert(sizeof(HiZWaterUniforms) == 224U);

// The bodies below the camera (seen from above), nearest first; without a
// valid view, none. waterOnly is kept either way.
[[nodiscard]] HiZWaterUniforms BuildHiZWaterUniforms(const std::array<float, 16>* worldToView,
                                                     const Renderer3ds::PicaSceneWaterBodies* bodies,
                                                     float seconds, bool waterOnly);

} // namespace Fast::Oot3d
