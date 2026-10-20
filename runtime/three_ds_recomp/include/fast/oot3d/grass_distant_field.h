#pragma once

#include "fast/oot3d/grass_surface_extractor.h"
#include "fast/oot3d/grass_world_placement_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace Fast::Oot3d {

// Distant grass field (docs/TRIAEVUM_GRASS_DISTANT_FIELD_DESIGN.md). Beyond the
// field start distance the blades stop and the accepted terrain triangles are
// drawn once as a thin grass slab: one draw per placement, cost proportional
// to covered pixels instead of distant roots.
//
// Everything a slab shows is derived from the placement's accepted roots, so
// the field inherits every placement decision the extractor made (mask, levels,
// active rectangles, overlays, slope, spacing, candidate capacity): it adds no
// surface and no root of its own. Per triangle, a regular barycentric grid of
// resolution n holds the local root coverage, the mean surface colour and the
// mean blade height, estimated by a lumped linear projection (each root splats
// its barycentric weights onto the three vertices of its grid cell).

inline constexpr uint32_t kGrassFieldNoShadowPoint = 0xffffffffU;
inline constexpr uint32_t kGrassFieldMaximumResolution = 128U;
// Empty slot of the world cell table.
inline constexpr uint32_t kGrassFieldEmptyCell = 0xffffffffU;
// Cell key bits relative to the field's cell origin: x and z 11 bits, y 10.
inline constexpr uint32_t kGrassFieldCellKeyBitsXZ = 11U;
inline constexpr uint32_t kGrassFieldCellKeyBitsY = 10U;
inline constexpr uint32_t kGrassFieldCellProbeLimit = 32U;
// Grid edge and world cell edge of the distant field (world units,
// GrassDistantFieldInput::CellLength in the game; the shaders embed it).
inline constexpr float kGrassFieldCellLength = 25.0F;

// std430, read by the field vertex and fragment shaders. 128 bytes.
struct GrassFieldTriangle {
    // xyz: base world position (with the rule's normal offset), w: the
    // octahedral (snorm2x16) smoothed up-facing normal bits of the corner.
    std::array<std::array<float, 4>, 3> Corner{};
    // xyz: native vertex index of each corner (surface lighting atlas);
    // w: first grid entry of this triangle.
    std::array<uint32_t, 4> Atlas{};
    // xyz: placement-local coarse shadow point of each corner, or
    // kGrassFieldNoShadowPoint; w: grid resolution n.
    std::array<uint32_t, 4> ShadowFirst{};
    std::array<uint32_t, 4> ShadowSecond{};
    std::array<float, 4> ShadowWeight{};
    // xyz: the field's cell origin (int32 cell coordinates), w: the field's
    // slice of the world cell table, first slot << 5 | log2(slots); 0 when
    // the field has no table.
    std::array<uint32_t, 4> Cells{};
};
static_assert(sizeof(GrassFieldTriangle) == 128U);

// Push constants of a field draw (GrassFieldState in the field shaders).
struct GrassFieldPushConstants {
    std::array<float, 16> PositionToClip{};
    // xy: jitter (NDC), z: column cell size, w: slab height (world units).
    std::array<float, 4> JitterNdc{};
    std::array<float, 4> DepthState{};
    // native projection, march steps, shadow point base, environment record
    std::array<uint32_t, 4> Flags{};
    // blend start, field start, draw distance, draw fade fraction
    std::array<float, 4> Field{};
};
static_assert(sizeof(GrassFieldPushConstants) == 128U);

// Grid entry: x = surface colour (RGBA8, A = 255 when valid, like
// GrassWorldAnchor::SurfaceColor), y = coverage (unorm16) | blade height over
// the slab height (unorm16) << 16.
using GrassFieldGridEntry = std::array<uint32_t, 2>;

// World cell table entry: x = cell key (kGrassFieldEmptyCell when free),
// y = coverage (unorm16) at the cell's roots. An open-addressing hash table
// per field over the cells (edge CellLength) that contain roots: the march
// reads it for samples that have left their entry triangle, so a slab's
// columns never stand over a neighbouring surface without roots (water,
// paths) while rays still cross into neighbouring grass triangles, whose
// prisms have no side faces to enter.
using GrassFieldCellEntry = std::array<uint32_t, 2>;

// Cell key of cell coordinates relative to the origin, or nullopt outside
// the key range. Mirrored in GLSL.
[[nodiscard]] constexpr uint32_t GrassFieldCellKey(uint32_t x, uint32_t y, uint32_t z) noexcept {
    return x | (z << kGrassFieldCellKeyBitsXZ) | (y << (2U * kGrassFieldCellKeyBitsXZ));
}
[[nodiscard]] constexpr uint32_t GrassFieldCellHash(uint32_t key) noexcept {
    uint32_t value = key * 0x9e3779b1U;
    value ^= value >> 15U;
    value *= 0x85ebca77U;
    value ^= value >> 13U;
    return value;
}

// Grid vertices of a resolution-n triangle and the index of vertex (i, j),
// i + j <= n, i along corner 1 and j along corner 2. Mirrored in GLSL.
[[nodiscard]] constexpr uint32_t GrassFieldGridSize(uint32_t n) noexcept { return (n + 1U) * (n + 2U) / 2U; }
[[nodiscard]] constexpr uint32_t GrassFieldGridIndex(uint32_t n, uint32_t i, uint32_t j) noexcept {
    return j * (n + 1U) - j * (j - 1U) / 2U + i;
}

struct GrassDistantFieldInput {
    std::shared_ptr<const GrassWorldPlacement> Placement;
    std::shared_ptr<const std::vector<GrassSourceVertex>> Vertices;
    std::array<float, 16> ModelToWorld{};
    bool TransformBakedIntoVertices = true;
    float NormalOffset = 0.0F;
    float InstancesPerSquareMeter = 0.0F;
    float MinimumSpacing = 0.0F;
    // Fraction of the surface's candidates the placement could extract (the
    // shared candidate capacity scales large rooms down uniformly).
    double CandidateScale = 1.0;
    // Target grid edge length, world units.
    float CellLength = 50.0F;
};

struct GrassDistantField {
    uint64_t Identity = 0U;
    uint64_t ContentVersion = 0U;
    std::vector<GrassFieldTriangle> Triangles;
    std::vector<GrassFieldGridEntry> Grid;
    // World cell table (a power of two slots, at most half full); every
    // triangle carries its origin and slice. Empty when the placement spans
    // more cells than the key holds (the march then keeps its entry coverage).
    std::vector<GrassFieldCellEntry> Cells;
    uint64_t OccupiedCells = 0U;
    // The tallest blade of the placement: the slab's height.
    float SlabHeight = 0.0F;
    uint64_t Roots = 0U;
    uint64_t RejectedRoots = 0U;
    double BuildMilliseconds = 0.0;
};

[[nodiscard]] GrassDistantField BuildGrassDistantField(const GrassDistantFieldInput& input);

// Expected surviving share of candidates after the minimum spacing test at
// full mask level (a sequential hard-core process keeps about exp(-pi r^2 rho / 2)).
[[nodiscard]] float GrassFieldSpacingKeep(float instancesPerSquareMeter, float minimumSpacing) noexcept;

// The coverage stored at a grid entry, in [0, 1].
[[nodiscard]] inline float GrassFieldCoverage(const GrassFieldGridEntry& entry) noexcept {
    return static_cast<float>(entry[1] & 0xffffU) / 65535.0F;
}
[[nodiscard]] inline float GrassFieldHeightFraction(const GrassFieldGridEntry& entry) noexcept {
    return static_cast<float>(entry[1] >> 16U) / 65535.0F;
}

// The world cell table's coverage at a world position: the cell containing
// it, else the cell below (a sample in the slab stands up to one slab height
// above its surface cell). 0 where no cell holds roots. Mirrors the GLSL
// lookup (grass_field_cell_coverage).
[[nodiscard]] float GrassFieldCellCoverage(const GrassDistantField& field, const std::array<float, 3>& position,
                                           float cellLength) noexcept;

} // namespace Fast::Oot3d
