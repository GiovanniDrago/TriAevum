#pragma once

// iMMERSE MXAO, ported from MartysMods_MXAO.fx.
// Copyright (c) Pascal Gilcher. All rights reserved.
// Integrated and modified with the author's explicit permission; see
// THIRD_PARTY_NOTICES.md ("iMMERSE").

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Immerse {

// Pixels are deinterleaved into kMxaoTiles x kMxaoTiles tiles: a tile holds
// every fourth pixel of each axis, so the taps of one tile stay cache local.
inline constexpr uint32_t kMxaoTiles = 4U;

enum class MxaoMethod : uint32_t {
    GroundTruth = 0,
    SolidAngle = 1,
    VisibilityBitmask = 2,
    VisibilityBitmaskSolidAngle = 3,
};

// Slice/step presets of the original, Low through Extreme.
inline constexpr uint32_t kMxaoQualityPresets = 6U;

// Filter flags of the regression passes.
inline constexpr uint32_t kMxaoFilterMirrored = 1U << 4U;
inline constexpr uint32_t kMxaoFilterCopy = 1U << 5U;

// View reconstruction is shared with the GTAO kernels: ViewX/ViewY map
// normalized storage UV to view XY / positive view depth, Depth linearizes
// the hardware depth. The adapter supplies orientation and projection.
struct MxaoConstants {
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Method = 0;
    // Bits 0-2: quality preset; filter flags above.
    uint32_t Flags = 0;
    std::array<float, 4> ViewX{};
    std::array<float, 4> ViewY{};
    std::array<float, 4> Depth{};
    // World-space radius, bitmask thickness, fade start, fade end (view units).
    std::array<float, 4> RadiusFade{};
    // Amount, 1 / far plane (the filter's normalized depth), squared normal
    // decoding uncertainty, unused.
    std::array<float, 4> Response{};
};
static_assert(sizeof(MxaoConstants) == 96);

[[nodiscard]] constexpr uint32_t MxaoTiledExtent(uint32_t extent) noexcept {
    return (extent + kMxaoTiles - 1U) / kMxaoTiles * kMxaoTiles;
}

// Linear view depth of every pixel, written in tile order.
[[nodiscard]] std::string BuildMxaoDeinterleaveShader();
// Occlusion per tile pixel: RG16F (ambient occlusion, normalized depth,
// negated where the filter must smooth across a depth discontinuity).
[[nodiscard]] std::string BuildMxaoOcclusionShader();
// MXAO's depth-guided regression filter. The intermediate pass keeps the RG16F
// layout; the final pass writes the published R32F visibility (1 - occlusion).
[[nodiscard]] std::string BuildMxaoFilterShader(bool finalPass);

} // namespace Fast::Immerse
