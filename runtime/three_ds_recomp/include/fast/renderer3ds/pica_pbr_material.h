#pragma once

#include <array>
#include <cstdint>

namespace Fast::Renderer3ds {

enum class PicaPbrTextureRole : uint8_t { Normal, Orm, Emissive, Count };

enum class PicaPbrDebugView : uint8_t { Shaded, SurfaceNormals, Roughness };

struct PicaPbrControls {
    float NormalStrength = 1.0F;
    float GlossinessStrength = 1.0F;
    PicaPbrDebugView DebugView = PicaPbrDebugView::Shaded;

    bool operator==(const PicaPbrControls&) const = default;
};

struct PicaPbrMaterialParameters {
    std::array<float, 4> BaseColorFactor{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 3> EmissiveFactor{};
    float NormalScale = 1.0F;
    float NormalYSign = 1.0F;
    float OcclusionStrength = 1.0F;
    float RoughnessFactor = 1.0F;
    float MetallicFactor = 0.0F;
    // No selector means the native TEV graph must identify one color texture.
    uint8_t TextureSlot = 0xffU;

    bool operator==(const PicaPbrMaterialParameters&) const = default;
};

} // namespace Fast::Renderer3ds
