#pragma once

#include "fast/renderer3ds/pica_light_binding.h"
#include <array>
#include <cstdint>

namespace Fast::Renderer3ds {

struct PicaEnvironmentDirectionalLight {
    std::array<float, 3> DirectionTowardSource{ 0.0F, 1.0F, 0.0F };
    std::array<float, 3> Color{ 1.0F, 1.0F, 1.0F };
    float Intensity = 0.0F;
    bool operator==(const PicaEnvironmentDirectionalLight&) const = default;
};

struct PicaScenePointLightSettings {
    bool Enabled = true;
    float Intensity = 1.0F;
    float RadiusScale = 1.0F;
    std::array<float, 3> ColorTint{1.0F, 1.0F, 1.0F};
    bool operator==(const PicaScenePointLightSettings&) const = default;
};

// Explicit extension inputs, never a replacement for native PICA state.
// The title owns profile identity, persistence and world-axis conventions.
struct PicaEnvironmentLighting {
    uint64_t ScopeIdentity = 0;
    bool Enabled = false;
    // Keep the native low-frequency reference independently of source gains.
    bool PreserveSceneBrightness = true;
    float NativeDirectStrength = 0.0F;
    float NativeLocalStrength = 0.0F;
    float NativeAmbientStrength = 0.0F;
    std::array<float, 3> AmbientColor{ 1.0F, 1.0F, 1.0F };
    float AmbientIntensity = 0.2F;
    float FogStrength = 1.0F;
    // One shadow map: light 0 casts shadows; light 1 is unshadowed fill.
    std::array<PicaEnvironmentDirectionalLight, 2> Lights{};
    // Independent of the directional/ambient override; title-owned groups.
    std::array<PicaScenePointLightSettings, kPicaScenePointLightGroups> PointLights{};
    bool operator==(const PicaEnvironmentLighting&) const = default;
};

} // namespace Fast::Renderer3ds
