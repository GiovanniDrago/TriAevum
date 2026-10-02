#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

namespace Fast::Renderer3ds {

// Supplemental source data for the lights already selected by native code.
// Slot order agrees with the captured vertex-light uniforms. It never adds a
// light, replaces native enable flags, or refers back to live guest memory.
struct PicaNativeLightSource {
    enum class Type : uint8_t { Unavailable, Directional, Point };

    Type Kind = Type::Unavailable;
    uint64_t SourceIdentity = 0;
    std::array<float, 3> PositionWorld{};
    std::array<float, 3> UnattenuatedColor{};
    std::array<float, 3> ReferenceColor{};
    float Radius = 0.0F;

    bool operator==(const PicaNativeLightSource&) const = default;
};

inline constexpr uint32_t kMaximumPicaScenePointLights = 32U;
inline constexpr uint32_t kPicaScenePointLightGroups = 2U;

struct PicaScenePointLight {
    uint64_t SourceIdentity = 0;
    std::array<float, 3> PositionWorld{};
    std::array<float, 3> Color{};
    float Radius = 0.0F;
    uint32_t Group = 0;
    bool operator==(const PicaScenePointLight&) const = default;
};

// One immutable scene snapshot shared by all draws from a native frame.
// Zero-radius records retain source identity so stale selected native slots
// cannot restore a contribution after that source has been turned off.
struct PicaScenePointLights {
    std::array<PicaScenePointLight, kMaximumPicaScenePointLights> Lights{};
    uint32_t Count = 0;
    bool operator==(const PicaScenePointLights&) const = default;
};

inline constexpr uint32_t kMaximumPicaSceneDirectionalLights = 2U;

struct PicaSceneDirectionalLight {
    uint64_t SourceIdentity = 0;
    std::array<float, 3> DirectionTowardSource{};
    std::array<float, 3> Color{};
    bool operator==(const PicaSceneDirectionalLight&) const = default;
};

// Native environment sources, independent of each draw's selected lights.
// Separate from moving points so unchanged environment snapshots stay shared.
struct PicaSceneDirectionalLights {
    std::array<PicaSceneDirectionalLight, kMaximumPicaSceneDirectionalLights> Lights{};
    uint32_t Count = 0;
    bool operator==(const PicaSceneDirectionalLights&) const = default;
};

// Rec. 709 luminance of a source's color.
[[nodiscard]] inline float PicaSceneDirectionalEnergy(const PicaSceneDirectionalLight& light) noexcept {
    return 0.2126F * std::max(light.Color[0], 0.0F) + 0.7152F * std::max(light.Color[1], 0.0F) +
           0.0722F * std::max(light.Color[2], 0.0F);
}

// The unit direction of a source that emits light, if it has one.
[[nodiscard]] inline std::optional<std::array<float, 3>> PicaSceneDirectionalDirection(
    const PicaSceneDirectionalLight& light) noexcept {
    constexpr float kMinimum = 1.0e-6F;
    const float energy = PicaSceneDirectionalEnergy(light);
    auto direction = light.DirectionTowardSource;
    const float lengthSquared =
        direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2];
    if (!std::isfinite(energy) || energy <= kMinimum || !std::isfinite(lengthSquared) ||
        lengthSquared <= kMinimum)
        return std::nullopt;
    const float inverseLength = 1.0F / std::sqrt(lengthSquared);
    for (float& component : direction) component *= inverseLength;
    return direction;
}

// The scene's main directional light: the brightest source with a direction,
// the lower identity on a tie. The native directional shadows follow it.
[[nodiscard]] inline const PicaSceneDirectionalLight* FindPicaSceneMainDirectional(
    const PicaSceneDirectionalLights& lights) noexcept {
    if (lights.Count > lights.Lights.size()) return nullptr;
    const PicaSceneDirectionalLight* main = nullptr;
    float best = 0.0F;
    for (uint32_t i = 0; i < lights.Count; ++i) {
        const auto& light = lights.Lights[i];
        if (!PicaSceneDirectionalDirection(light)) continue;
        const float energy = PicaSceneDirectionalEnergy(light);
        if (energy > best || (energy == best && main != nullptr && light.SourceIdentity < main->SourceIdentity)) {
            main = &light;
            best = energy;
        }
    }
    return main;
}

struct PicaNativeLightBinding {
    std::array<PicaNativeLightSource, 3> Sources{};
    // Column-major world-to-view captured with the native geometry commands.
    // Replayed/interpolated with this draw, never read from a live camera.
    std::optional<std::array<float, 16>> WorldToView;
    std::shared_ptr<const PicaScenePointLights> ScenePoints;
    std::shared_ptr<const PicaSceneDirectionalLights> SceneDirectionals;

    bool operator==(const PicaNativeLightBinding&) const = default;
};

} // namespace Fast::Renderer3ds
