#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Fast::Renderer3ds {

// Gameplay water published by the title (its collision water boxes): the
// horizontal surfaces where water is, with live heights (levels that rise
// and fall). Generic data: no guest addresses or scene rules.
inline constexpr uint32_t kMaximumPicaSceneWaterBodies = 64U;

struct PicaSceneWaterBody {
    std::array<float, 2> MinimumXZ{};
    std::array<float, 2> MaximumXZ{};
    float SurfaceY = 0.0F;
    uint32_t Properties = 0;
    bool operator==(const PicaSceneWaterBody&) const = default;
};

// One immutable snapshot shared by all draws from a native frame.
struct PicaSceneWaterBodies {
    std::array<PicaSceneWaterBody, kMaximumPicaSceneWaterBodies> Bodies{};
    uint32_t Count = 0;
    bool operator==(const PicaSceneWaterBodies&) const = default;
};

// How far (world units) a drawn water surface may lie from its body's
// gameplay height, and how much of its footprint must lie over the body.
inline constexpr float kPicaWaterSurfaceHeightTolerance = 4.0F;
inline constexpr float kPicaWaterSurfaceMinimumCoverage = 0.5F;

// The body a draw's world bounds lie on as a water surface: flat (its height
// range within the tolerance), at the body's surface height, and with most of
// its horizontal footprint over the body. Bridges, banks and the bed are not
// flat at the surface height. Of several bodies, the one covering most.
[[nodiscard]] inline std::optional<uint32_t> MatchPicaWaterSurface(const std::array<float, 3>& minimum,
                                                                   const std::array<float, 3>& maximum,
                                                                   const PicaSceneWaterBodies& bodies) noexcept {
    if (maximum[1] - minimum[1] > kPicaWaterSurfaceHeightTolerance || maximum[0] < minimum[0] ||
        maximum[2] < minimum[2])
        return std::nullopt;
    const float height = 0.5F * (minimum[1] + maximum[1]);
    // A degenerate footprint (a line) still needs a positive area to compare.
    const float width = std::max(maximum[0] - minimum[0], 1.0F);
    const float depth = std::max(maximum[2] - minimum[2], 1.0F);
    std::optional<uint32_t> best;
    float bestCoverage = kPicaWaterSurfaceMinimumCoverage;
    for (uint32_t i = 0; i < std::min(bodies.Count, kMaximumPicaSceneWaterBodies); ++i) {
        const auto& body = bodies.Bodies[i];
        if (std::abs(body.SurfaceY - height) > kPicaWaterSurfaceHeightTolerance) continue;
        const float overlapX = std::min(maximum[0], body.MaximumXZ[0]) - std::max(minimum[0], body.MinimumXZ[0]);
        const float overlapZ = std::min(maximum[2], body.MaximumXZ[1]) - std::max(minimum[2], body.MinimumXZ[1]);
        if (overlapX < 0.0F || overlapZ < 0.0F) continue;
        // A degenerate axis inside the body counts as covered.
        const float coverage = std::max(overlapX, width - (maximum[0] - minimum[0])) *
                               std::max(overlapZ, depth - (maximum[2] - minimum[2])) / (width * depth);
        if (coverage >= bestCoverage) {
            best = i;
            bestCoverage = coverage;
        }
    }
    return best;
}

} // namespace Fast::Renderer3ds
