#pragma once

// iMMERSE LAUNCHPAD albedo estimate, ported from MartysMods_LAUNCHPAD.fx.
// Copyright (c) Pascal Gilcher. All rights reserved.
// Integrated and modified with the author's explicit permission; see
// THIRD_PARTY_NOTICES.md ("iMMERSE").

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Immerse {

// The pyramid starts at half resolution and stops at the level the original
// picks from its half height: 3 below 128 texels, then one more per doubling.
inline constexpr uint32_t kLaunchpadAlbedoMaxLevels = 10U;

[[nodiscard]] constexpr uint32_t LaunchpadAlbedoLowestLevel(uint32_t width, uint32_t height) noexcept {
    const uint32_t side = (width < height ? width : height) / 2U;
    uint32_t level = 3U;
    for (uint32_t limit = 128U; side >= limit && level < kLaunchpadAlbedoMaxLevels - 1U; limit *= 2U)
        ++level;
    return level;
}

struct LaunchpadAlbedoConstants {
    // Destination extent and the extent of the texture read at binding 2.
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t SourceWidth = 0;
    uint32_t SourceHeight = 0;
    // Downsample axis (1 horizontal, 0 vertical); lowest level for the fusion;
    // 1 when the scene colour samples as linear values instead of sRGB.
    uint32_t Horizontal = 0;
    uint32_t LowestLevel = 0;
    uint32_t LinearSource = 0;
    uint32_t Reserved = 0;
    // Extents of the pyramid levels, for the fusion's B-spline taps.
    std::array<std::array<uint32_t, 2>, kLaunchpadAlbedoMaxLevels> LevelExtents{};
};
static_assert(sizeof(LaunchpadAlbedoConstants) == 112);

// Half-resolution log luminance of the scene colour (RG16F: -log, log).
[[nodiscard]] std::string BuildLaunchpadAlbedoInitShader();
// One separable, variance-weighted (Kuwahara-like) half of a pyramid step.
[[nodiscard]] std::string BuildLaunchpadAlbedoDownsampleShader();
// Edge-stopping fusion of the pyramid (RG16F: illumination bias, guide luma).
[[nodiscard]] std::string BuildLaunchpadAlbedoFuseShader();
// Guided upsampling to full resolution: the albedo estimate's mean (R16F),
// the height the textured normals differentiate.
[[nodiscard]] std::string BuildLaunchpadAlbedoMainShader();

} // namespace Fast::Immerse
