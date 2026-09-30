#pragma once

#include "fast/oot3d/grass_shading_environment.h"
#include "fast/oot3d/toon_surface_response.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Fast::Oot3d {

// One per grass placement: the std430 GrassEnvironmentRecord of the grass
// vertex and fragment shaders (grass_shader_sources.cpp).
struct alignas(16) GrassEnvironmentRecord {
    // W is 0 when disabled, 1 for regular depth and 2 for flipped depth.
    std::array<float, 4> ColorAndMode{};
    std::array<std::array<float, 2>, kGrassFogLutEntryCount> Lut{};
    // direction X/Z, visual seconds, strength
    std::array<float, 4> WindDirectionTime{};
    // speed, spatial scale, gust strength, gust frequency
    std::array<float, 4> WindPrimary{};
    // turbulence, per-blade randomness, maximum bend, enabled
    std::array<float, 4> WindDetail{};
    std::array<float, 4> CameraPosition{};
    std::array<float, 4> AppearanceRoot{};
    std::array<float, 4> AppearanceTip{};
    // Average texture RGB and effective texture-color influence.
    std::array<float, 4> TextureColor{};
    // Root/tip brightness, receive-lighting flag, active light count.
    std::array<float, 4> TextureBrightnessFlags{};
    std::array<std::array<float, 4>, kGrassNativeLightCount>
        LightDirections{};
    std::array<std::array<float, 4>, kGrassNativeLightCount>
        LightDiffuse{};
    std::array<std::array<float, 4>, kGrassNativeLightCount>
        LightAmbient{};
    std::array<float, 4> ViewForward{};
    std::array<float, 4> ViewSide{};
    std::array<float, 4> ViewUp{};
    std::array<float, 4> BladeShape{};
    std::array<float, 4> TuftLod{};
    // Draw distance, density start/end, per-anchor fade softness.
    std::array<float, 4> DistanceLod{};
    // Tuft spread/density, final fade range, enabled.
    std::array<float, 4> TuftStyle{};
    ToonSurfaceParameters Toon;
    std::array<uint32_t, 4> NativeLighting{};
    // Rim start/end, enabled, VSM receiver strength.
    std::array<float, 4> RimDistance{};
    // The placement's previous frame, for exact motion vectors.
    std::array<float, 16> PreviousPositionToClip{};
    // Previous camera position, previous wind time.
    std::array<float, 4> PreviousCameraTime{};
    // Previous jitter NDC XY, motion enabled, previous bends bound.
    std::array<float, 4> MotionState{};
};
static_assert(offsetof(GrassEnvironmentRecord, Toon) == 1424U);
static_assert(offsetof(GrassEnvironmentRecord, PreviousPositionToClip) == 1584U);
static_assert(sizeof(GrassEnvironmentRecord) == 1680U);

// Push constants of the grass draws (GrassState in the shaders).
struct GrassPushConstants {
    std::array<float, 16> PositionToClip{};
    std::array<float, 4> JitterNdc{};
    std::array<float, 4> DepthState{}; // scale, offset, w-buffer, valid
    // native projection, segments, planes, fog-record index
    std::array<uint32_t, 4> Flags{};
};
static_assert(sizeof(GrassPushConstants) == 112U);

} // namespace Fast::Oot3d
