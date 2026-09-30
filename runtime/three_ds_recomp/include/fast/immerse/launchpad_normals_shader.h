#pragma once

// iMMERSE LAUNCHPAD normal smoothing, ported from MartysMods_LAUNCHPAD.fx.
// Copyright (c) Pascal Gilcher. All rights reserved.
// Integrated and modified with the author's explicit permission; see
// THIRD_PARTY_NOTICES.md ("iMMERSE").

#include <array>
#include <cstdint>
#include <string>

namespace Fast::Immerse {

// Screen radius of the two smoothing iterations, as a fraction of the shorter
// screen side (the original's 0.04 and 0.008 of the screen UV).
inline constexpr float kLaunchpadWideRadius = 0.04F;
inline constexpr float kLaunchpadNarrowRadius = 0.008F;

// The G-buffer and the wide iteration cover every other row, as the original's
// half-height G-buffer does; the narrow iteration refines each pixel from them.
[[nodiscard]] constexpr uint32_t LaunchpadHalfHeight(uint32_t height) noexcept {
    return (height + 1U) / 2U;
}

// View reconstruction is shared with the AO kernels: ViewX/ViewY map
// normalized storage UV to view XY / positive view depth, Depth linearizes
// the hardware depth.
struct LaunchpadNormalsConstants {
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint32_t Iteration = 0;
    uint32_t Reserved = 0;
    std::array<float, 4> ViewX{};
    std::array<float, 4> ViewY{};
    std::array<float, 4> Depth{};
    // View units per LAUNCHPAD unit (the near plane: ReShade's linearized
    // depth is one at the near plane), kernel radius in pixels, unused.
    std::array<float, 4> Scale{};
};
static_assert(sizeof(LaunchpadNormalsConstants) == 80);

// Half-height G-buffer: the native guide and depth of every other row, packed
// as RG32UI (16-bit octahedral normal in view space mirrored along Z, depth in
// near-plane units; zero depth where there is no surface).
[[nodiscard]] std::string BuildLaunchpadGbufferShader();
// Wide iteration: the G-buffer smoothed, in the same packed layout.
[[nodiscard]] std::string BuildLaunchpadSmoothWideShader();
// Narrow iteration: the refined guide in the native encoding (RGBA16F normal
// * 0.5 + 0.5, the native coverage in alpha).
[[nodiscard]] std::string BuildLaunchpadSmoothNarrowShader();

} // namespace Fast::Immerse
