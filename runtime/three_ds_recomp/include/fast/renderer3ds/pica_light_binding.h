#pragma once

#include <array>
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
