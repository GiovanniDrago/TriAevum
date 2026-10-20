#include "fast/oot3d/grass_distant_field.h"
#include "fast/oot3d/grass_anchor_codec.h"
#include "fast/oot3d/grass_parallel_work.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace Fast::Oot3d {
namespace {

using Vec3 = std::array<double, 3>;

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double Length(const Vec3& a) { return std::sqrt(Dot(a, a)); }

// The extractor's and the world placement's model-to-world mapping.
Vec3 ToWorld(const GrassDistantFieldInput& input, const std::array<float, 3>& p) {
    if (input.TransformBakedIntoVertices) return {p[0], p[1], p[2]};
    const auto& m = input.ModelToWorld;
    const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
    const float inverse = std::abs(w) > 1.0e-6F ? 1.0F / w : 1.0F;
    return {(m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]) * inverse,
            (m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7]) * inverse,
            (m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]) * inverse};
}

struct BuildTriangle {
    std::array<uint32_t, 3> Vertices{};
    std::array<Vec3, 3> Position{};
    Vec3 UpNormal{0.0, 1.0, 0.0};
    double Area = 0.0;
    uint32_t Resolution = 1U;
    uint32_t GridOffset = 0U;
    // Per corner: the root closest to it in barycentric terms (shadow point).
    std::array<double, 3> NearestWeight{-1.0, -1.0, -1.0};
    std::array<uint32_t, 3> NearestRoot{};
};

// Roots bucketed by world XZ cell: a kernel query reads the 3x3 cells around it.
class RootIndex {
  public:
    RootIndex(const std::vector<GrassWorldAnchor>& anchors, double cell) : mCell(cell) {
        std::unordered_map<uint64_t, uint32_t> counts;
        counts.reserve(anchors.size() / 16U + 1U);
        for (const auto& anchor : anchors) ++counts[Key(anchor.BaseHeight[0], anchor.BaseHeight[2])];
        mRanges.reserve(counts.size());
        uint32_t offset = 0U;
        for (const auto& [key, count] : counts) {
            mRanges.emplace(key, std::array<uint32_t, 2>{offset, offset});
            offset += count;
        }
        mRoots.resize(anchors.size());
        for (uint32_t index = 0; index < anchors.size(); ++index) {
            auto& range = mRanges[Key(anchors[index].BaseHeight[0], anchors[index].BaseHeight[2])];
            mRoots[range[1]++] = index;
        }
    }

    template <typename Visit> void Query(const Vec3& position, Visit&& visit) const {
        const auto cx = static_cast<int64_t>(std::floor(position[0] / mCell));
        const auto cz = static_cast<int64_t>(std::floor(position[2] / mCell));
        for (int64_t dz = -1; dz <= 1; ++dz)
            for (int64_t dx = -1; dx <= 1; ++dx) {
                const auto found = mRanges.find(Pack(cx + dx, cz + dz));
                if (found == mRanges.end()) continue;
                for (uint32_t i = found->second[0]; i < found->second[1]; ++i) visit(mRoots[i]);
            }
    }

  private:
    static uint64_t Pack(int64_t x, int64_t z) {
        return (static_cast<uint64_t>(x) & 0xffffffffULL) | (static_cast<uint64_t>(z) << 32U);
    }
    uint64_t Key(float x, float z) const {
        return Pack(static_cast<int64_t>(std::floor(x / mCell)), static_cast<int64_t>(std::floor(z / mCell)));
    }
    double mCell;
    std::unordered_map<uint64_t, std::array<uint32_t, 2>> mRanges;
    std::vector<uint32_t> mRoots;
};

} // namespace

float GrassFieldSpacingKeep(float instancesPerSquareMeter, float minimumSpacing) noexcept {
    const double density = std::max(0.0, double(instancesPerSquareMeter)) * 1.0e-4; // per square world unit
    const double radius = std::max(0.0, double(minimumSpacing));
    return static_cast<float>(std::exp(-0.5 * std::numbers::pi * radius * radius * density));
}

GrassDistantField BuildGrassDistantField(const GrassDistantFieldInput& input) {
    const auto start = std::chrono::steady_clock::now();
    GrassDistantField field;
    if (!input.Placement || !input.Vertices) return field;
    const auto& placement = *input.Placement;
    const auto& vertices = *input.Vertices;
    field.Identity = placement.Identity;
    field.ContentVersion = placement.ContentVersion;
    const auto& anchors = placement.Anchors;
    const bool shadows = placement.ShadowReferences.size() == anchors.size() && !anchors.empty();
    const double cellLength = std::max(1.0, double(input.CellLength));

    // Triangles in first-root order: deterministic for one placement. Every
    // root also elects the closest root to each corner (its shadow point).
    std::unordered_map<uint64_t, uint32_t> triangleIndex;
    std::vector<BuildTriangle> triangles;
    for (size_t a = 0; a < anchors.size(); ++a) {
        const auto& anchor = anchors[a];
        const auto& reference = anchor.SurfaceReference;
        const std::array<uint32_t, 3> corners{reference[0] & 0xffffU, reference[0] >> 16U, reference[1] & 0xffffU};
        if (corners[0] >= vertices.size() || corners[1] >= vertices.size() || corners[2] >= vertices.size()) {
            ++field.RejectedRoots;
            continue;
        }
        const uint64_t key = uint64_t(corners[0]) | (uint64_t(corners[1]) << 16U) | (uint64_t(corners[2]) << 32U);
        auto [entry, inserted] = triangleIndex.try_emplace(key, static_cast<uint32_t>(triangles.size()));
        if (inserted) {
            BuildTriangle triangle;
            triangle.Vertices = corners;
            for (size_t c = 0; c < 3; ++c) triangle.Position[c] = ToWorld(input, vertices[corners[c]].Position);
            const auto face = Cross(Sub(triangle.Position[1], triangle.Position[0]),
                                    Sub(triangle.Position[2], triangle.Position[0]));
            const double doubled = Length(face);
            if (!(doubled > 1.0e-6) || !std::isfinite(doubled)) {
                triangleIndex.erase(entry);
                ++field.RejectedRoots;
                continue;
            }
            triangle.Area = doubled * 0.5;
            const double sign = face[1] < 0.0 ? -1.0 : 1.0;
            triangle.UpNormal = {face[0] * sign / doubled, face[1] * sign / doubled, face[2] * sign / doubled};
            double longest = 0.0;
            for (size_t c = 0; c < 3; ++c)
                longest = std::max(longest, Length(Sub(triangle.Position[(c + 1U) % 3U], triangle.Position[c])));
            triangle.Resolution = static_cast<uint32_t>(
                std::clamp(std::ceil(longest / cellLength), 1.0, double(kGrassFieldMaximumResolution)));
            triangles.push_back(triangle);
        }
        auto& triangle = triangles[entry->second];
        field.SlabHeight = std::max(field.SlabHeight, anchor.BaseHeight[3]);
        ++field.Roots;
        if (!shadows) continue;
        const Vec3 p{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]};
        const auto e1 = Sub(triangle.Position[1], triangle.Position[0]);
        const auto e2 = Sub(triangle.Position[2], triangle.Position[0]);
        const auto v = Sub(p, triangle.Position[0]);
        const double d00 = Dot(e1, e1), d01 = Dot(e1, e2), d11 = Dot(e2, e2);
        const double d20 = Dot(v, e1), d21 = Dot(v, e2);
        const double denominator = d00 * d11 - d01 * d01;
        const double b1 = denominator != 0.0 ? (d11 * d20 - d01 * d21) / denominator : 1.0 / 3.0;
        const double b2 = denominator != 0.0 ? (d00 * d21 - d01 * d20) / denominator : 1.0 / 3.0;
        const std::array<double, 3> barycentric{1.0 - b1 - b2, b1, b2};
        for (size_t c = 0; c < 3; ++c)
            if (barycentric[c] > triangle.NearestWeight[c]) {
                triangle.NearestWeight[c] = barycentric[c];
                triangle.NearestRoot[c] = static_cast<uint32_t>(a);
            }
    }

    uint32_t gridSize = 0U;
    for (auto& triangle : triangles) {
        triangle.GridOffset = gridSize;
        gridSize += GrassFieldGridSize(triangle.Resolution);
    }

    // Smoothed up-facing normals per native vertex, so neighbouring slab tops meet.
    std::unordered_map<uint32_t, Vec3> smooth;
    for (const auto& triangle : triangles)
        for (const uint32_t vertex : triangle.Vertices) {
            auto& normal = smooth[vertex];
            for (size_t axis = 0; axis < 3; ++axis) normal[axis] += triangle.UpNormal[axis] * triangle.Area;
        }
    for (auto& [vertex, normal] : smooth) {
        const double length = Length(normal);
        normal = length > 1.0e-12 ? Vec3{normal[0] / length, normal[1] / length, normal[2] / length} : Vec3{0, 1, 0};
    }

    // Coverage, surface colour and blade height at every grid vertex: a cone
    // kernel over the roots around it in world space, whatever triangle they
    // grew on, so the estimate is continuous across triangle edges. The
    // expected mass at full mask level is the candidate density (scaled by
    // the candidate capacity), the share surviving the spacing test, times
    // the kernel's integral over a plane (pi R^2 / 3).
    const double radius = cellLength * 1.5;
    const double keep = GrassFieldSpacingKeep(input.InstancesPerSquareMeter, input.MinimumSpacing);
    const double expected = std::max(0.0, double(input.InstancesPerSquareMeter)) * 1.0e-4 *
        std::clamp(input.CandidateScale, 0.0, 1.0) * keep * std::numbers::pi * radius * radius / 3.0;
    const RootIndex index(anchors, radius);
    const float slab = std::max(field.SlabHeight, 1.0e-3F);
    field.Grid.resize(gridSize);
    const size_t workers = std::min<size_t>(4U, GrassParallelWorkerCount(gridSize * 64ULL));
    RunGrassParallelWork(workers, [&](size_t worker) {
        for (size_t t = worker; t < triangles.size(); t += workers) {
            const auto& triangle = triangles[t];
            const uint32_t n = triangle.Resolution;
            const auto e1 = Sub(triangle.Position[1], triangle.Position[0]);
            const auto e2 = Sub(triangle.Position[2], triangle.Position[0]);
            for (uint32_t j = 0; j <= n; ++j)
                for (uint32_t i = 0; i + j <= n; ++i) {
                    Vec3 position = triangle.Position[0];
                    for (size_t axis = 0; axis < 3; ++axis)
                        position[axis] += e1[axis] * i / n + e2[axis] * j / n;
                    double mass = 0.0, height = 0.0, colorMass = 0.0;
                    std::array<double, 3> color{};
                    index.Query(position, [&](uint32_t root) {
                        const auto& anchor = anchors[root];
                        const Vec3 p{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]};
                        const double distance = Length(Sub(p, position));
                        if (distance >= radius) return;
                        const double weight = 1.0 - distance / radius;
                        mass += weight;
                        height += weight * anchor.BaseHeight[3];
                        if ((anchor.SurfaceColor >> 24U) != 0U) {
                            colorMass += weight;
                            for (size_t channel = 0; channel < 3; ++channel)
                                color[channel] += weight * ((anchor.SurfaceColor >> (8U * channel)) & 0xffU) / 255.0;
                        }
                    });
                    const auto byte = [](double value) {
                        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
                    };
                    const auto unorm16 = [](double value) {
                        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0, 1.0) * 65535.0));
                    };
                    auto& target = field.Grid[triangle.GridOffset + GrassFieldGridIndex(n, i, j)];
                    target[0] = colorMass > 1.0e-6
                        ? byte(color[0] / colorMass) | (byte(color[1] / colorMass) << 8U) |
                              (byte(color[2] / colorMass) << 16U) | (255U << 24U)
                        : 0U;
                    const double coverage = expected > 0.0 ? mass / expected : 0.0;
                    const double meanHeight = mass > 1.0e-6 ? height / mass : slab;
                    target[1] = unorm16(coverage) | (unorm16(meanHeight / slab) << 16U);
                }
        }
    });

    // World cell table: the cells (edge cellLength) that contain roots, each
    // with the same kernel estimate evaluated at its roots' mean position, so
    // the value is continuous with the triangle grids. The march reads it for
    // samples outside their entry triangle.
    std::array<uint32_t, 4> cellsHeader{};
    {
        const auto cellOf = [&](const GrassWorldAnchor& anchor) {
            return std::array<int64_t, 3>{static_cast<int64_t>(std::floor(anchor.BaseHeight[0] / cellLength)),
                                          static_cast<int64_t>(std::floor(anchor.BaseHeight[1] / cellLength)),
                                          static_cast<int64_t>(std::floor(anchor.BaseHeight[2] / cellLength))};
        };
        std::array<int64_t, 3> origin{INT64_MAX, INT64_MAX, INT64_MAX};
        std::array<int64_t, 3> extent{INT64_MIN, INT64_MIN, INT64_MIN};
        for (const auto& anchor : anchors) {
            const auto cell = cellOf(anchor);
            for (size_t axis = 0; axis < 3; ++axis) {
                origin[axis] = std::min(origin[axis], cell[axis]);
                extent[axis] = std::max(extent[axis], cell[axis]);
            }
        }
        const bool fits = !anchors.empty() &&
            extent[0] - origin[0] < (int64_t(1) << kGrassFieldCellKeyBitsXZ) &&
            extent[2] - origin[2] < (int64_t(1) << kGrassFieldCellKeyBitsXZ) &&
            extent[1] - origin[1] < (int64_t(1) << kGrassFieldCellKeyBitsY);
        if (fits) {
            struct CellRoots { Vec3 Sum{}; uint32_t Count = 0U; };
            std::unordered_map<uint32_t, CellRoots> cells;
            cells.reserve(anchors.size() / 8U + 1U);
            for (const auto& anchor : anchors) {
                const auto cell = cellOf(anchor);
                auto& roots = cells[GrassFieldCellKey(static_cast<uint32_t>(cell[0] - origin[0]),
                                                      static_cast<uint32_t>(cell[1] - origin[1]),
                                                      static_cast<uint32_t>(cell[2] - origin[2]))];
                for (size_t axis = 0; axis < 3; ++axis) roots.Sum[axis] += anchor.BaseHeight[axis];
                ++roots.Count;
            }
            std::vector<std::pair<uint32_t, Vec3>> occupied;
            occupied.reserve(cells.size());
            for (const auto& [key, roots] : cells)
                occupied.emplace_back(key, Vec3{roots.Sum[0] / roots.Count, roots.Sum[1] / roots.Count,
                                                roots.Sum[2] / roots.Count});
            std::vector<uint32_t> coverage(occupied.size());
            const size_t cellWorkers = std::min<size_t>(4U, GrassParallelWorkerCount(occupied.size() * 64ULL));
            RunGrassParallelWork(cellWorkers, [&](size_t worker) {
                for (size_t c = worker; c < occupied.size(); c += cellWorkers) {
                    double mass = 0.0;
                    index.Query(occupied[c].second, [&](uint32_t root) {
                        const auto& anchor = anchors[root];
                        const Vec3 p{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]};
                        const double distance = Length(Sub(p, occupied[c].second));
                        if (distance < radius) mass += 1.0 - distance / radius;
                    });
                    coverage[c] = static_cast<uint32_t>(
                        std::lround(std::clamp(expected > 0.0 ? mass / expected : 0.0, 0.0, 1.0) * 65535.0));
                }
            });
            uint32_t bits = 1U;
            while ((size_t(1) << bits) < occupied.size() * 2U) ++bits;
            const uint32_t mask = (1U << bits) - 1U;
            field.Cells.assign(size_t(1) << bits, GrassFieldCellEntry{kGrassFieldEmptyCell, 0U});
            for (size_t c = 0; c < occupied.size(); ++c) {
                uint32_t slot = GrassFieldCellHash(occupied[c].first) & mask;
                while (field.Cells[slot][0] != kGrassFieldEmptyCell) slot = (slot + 1U) & mask;
                field.Cells[slot] = {occupied[c].first, coverage[c]};
            }
            field.OccupiedCells = occupied.size();
            cellsHeader = {std::bit_cast<uint32_t>(static_cast<int32_t>(origin[0])),
                           std::bit_cast<uint32_t>(static_cast<int32_t>(origin[1])),
                           std::bit_cast<uint32_t>(static_cast<int32_t>(origin[2])), bits};
        }
    }

    field.Triangles.reserve(triangles.size());
    for (const auto& triangle : triangles) {
        GrassFieldTriangle out;
        out.Cells = cellsHeader;
        for (size_t c = 0; c < 3; ++c) {
            const auto& normal = smooth[triangle.Vertices[c]];
            for (size_t axis = 0; axis < 3; ++axis)
                out.Corner[c][axis] = static_cast<float>(triangle.Position[c][axis] + normal[axis] * input.NormalOffset);
            out.Corner[c][3] = std::bit_cast<float>(
                PackGrassNormal({float(normal[0]), float(normal[1]), float(normal[2])}));
            out.Atlas[c] = triangle.Vertices[c];
            out.ShadowFirst[c] = out.ShadowSecond[c] = kGrassFieldNoShadowPoint;
            if (shadows && triangle.NearestWeight[c] >= -0.5) {
                const auto& reference = placement.ShadowReferences[triangle.NearestRoot[c]];
                out.ShadowFirst[c] = reference.First;
                out.ShadowSecond[c] = reference.Second;
                out.ShadowWeight[c] = reference.Weight;
            }
        }
        out.Atlas[3] = triangle.GridOffset;
        out.ShadowFirst[3] = triangle.Resolution;
        field.Triangles.push_back(out);
    }
    field.BuildMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return field;
}

float GrassFieldCellCoverage(const GrassDistantField& field, const std::array<float, 3>& position,
                             float cellLength) noexcept {
    if (field.Cells.empty() || field.Triangles.empty()) return 0.0F;
    const auto& header = field.Triangles.front().Cells;
    const uint32_t bits = header[3] & 31U;
    const uint32_t first = header[3] >> 5U;
    const uint32_t mask = (1U << bits) - 1U;
    const auto lookup = [&](std::array<float, 3> p) {
        std::array<int64_t, 3> cell{};
        for (size_t axis = 0; axis < 3; ++axis)
            cell[axis] = static_cast<int64_t>(std::floor(p[axis] / cellLength)) -
                std::bit_cast<int32_t>(header[axis]);
        if (cell[0] < 0 || cell[1] < 0 || cell[2] < 0 || cell[0] >= (int64_t(1) << kGrassFieldCellKeyBitsXZ) ||
            cell[2] >= (int64_t(1) << kGrassFieldCellKeyBitsXZ) || cell[1] >= (int64_t(1) << kGrassFieldCellKeyBitsY))
            return 0.0F;
        const uint32_t key = GrassFieldCellKey(static_cast<uint32_t>(cell[0]), static_cast<uint32_t>(cell[1]),
                                               static_cast<uint32_t>(cell[2]));
        uint32_t slot = GrassFieldCellHash(key) & mask;
        for (uint32_t probe = 0; probe < kGrassFieldCellProbeLimit; ++probe) {
            const auto& entry = field.Cells.at(first + ((slot + probe) & mask));
            if (entry[0] == key) return static_cast<float>(entry[1] & 0xffffU) / 65535.0F;
            if (entry[0] == kGrassFieldEmptyCell) return 0.0F;
        }
        return 0.0F;
    };
    return std::max(lookup(position), lookup({position[0], position[1] - cellLength, position[2]}));
}

} // namespace Fast::Oot3d
