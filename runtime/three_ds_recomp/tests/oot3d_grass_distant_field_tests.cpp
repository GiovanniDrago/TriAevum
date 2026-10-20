#include "fast/oot3d/grass_distant_field.h"
#include "fast/oot3d/grass_shader_sources.h"
#include "fast/oot3d/grass_surface_reference.h"
#include "fast/oot3d/graphics_settings.h"
#include "fast/oot3d/graphics_settings_persistence.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
#include <set>
#include <vector>

namespace {
using namespace Fast::Oot3d;

// One flat 1000 x 1000 right triangle, roots on a lattice of the given spacing
// wherever keep(x, z) holds.
template <typename Keep>
GrassDistantFieldInput LatticeInput(float spacing, Keep keep, float density) {
    auto vertices = std::make_shared<std::vector<GrassSourceVertex>>(3);
    (*vertices)[1].Position = {1000.0F, 0.0F, 0.0F};
    (*vertices)[2].Position = {0.0F, 0.0F, 1000.0F};
    auto placement = std::make_shared<GrassWorldPlacement>();
    placement->Identity = 3U;
    placement->ContentVersion = 5U;
    for (float x = spacing * 0.5F; x < 1000.0F; x += spacing)
        for (float z = spacing * 0.5F; x + z < 1000.0F; z += spacing) {
            if (!keep(x, z)) continue;
            GrassWorldAnchor anchor;
            anchor.BaseHeight = {x, 0.0F, z, 10.0F};
            anchor.SurfaceColor = 0xff204080U;
            anchor.SurfaceReference = PackGrassSurfaceReference({0U, 1U, 2U}, x / 1000.0F, z / 1000.0F);
            placement->Anchors.push_back(anchor);
        }
    GrassDistantFieldInput input;
    input.Placement = placement;
    input.Vertices = vertices;
    input.InstancesPerSquareMeter = density;
    input.MinimumSpacing = 0.0F;
    input.CellLength = 50.0F;
    return input;
}

// The grid value at world (x, z) of the single triangle (its interior vertices).
float CoverageAt(const GrassDistantField& field, float x, float z) {
    const uint32_t n = field.Triangles.at(0).ShadowFirst[3];
    const auto i = static_cast<uint32_t>(std::lround(x / 1000.0F * n));
    const auto j = static_cast<uint32_t>(std::lround(z / 1000.0F * n));
    return GrassFieldCoverage(field.Grid.at(field.Triangles[0].Atlas[3] + GrassFieldGridIndex(n, i, j)));
}

TEST(Oot3dGrassDistantField, GridIndicesCoverEveryVertexOnce) {
    for (uint32_t n = 1U; n <= 9U; ++n) {
        std::set<uint32_t> seen;
        for (uint32_t j = 0U; j <= n; ++j)
            for (uint32_t i = 0U; i + j <= n; ++i) seen.insert(GrassFieldGridIndex(n, i, j));
        EXPECT_EQ(seen.size(), GrassFieldGridSize(n));
        EXPECT_EQ(*seen.rbegin(), GrassFieldGridSize(n) - 1U);
    }
}

TEST(Oot3dGrassDistantField, FullLatticeReadsFullAndHalfLatticeReadsHalf) {
    // Spacing 3 world units: 1/9 roots per square unit = 1111.1 per square metre.
    const float density = 1.0e4F / 9.0F;
    const auto full = BuildGrassDistantField(LatticeInput(3.0F, [](float, float) { return true; }, density));
    ASSERT_EQ(full.Triangles.size(), 1U);
    EXPECT_NEAR(full.SlabHeight, 10.0F, 1.0e-4F);
    const uint32_t n = full.Triangles[0].ShadowFirst[3];
    EXPECT_EQ(n, 29U); // the longest edge (1414 units) over the 50-unit cell
    for (const auto [x, z] : {std::pair{200.0F, 200.0F}, std::pair{400.0F, 300.0F}, std::pair{100.0F, 600.0F}})
        EXPECT_NEAR(CoverageAt(full, x, z), 1.0F, 0.08F) << x << "," << z;
    // Every other row: half the density.
    const auto half = BuildGrassDistantField(LatticeInput(3.0F, [](float, float z) {
        return static_cast<int>(std::floor(z / 3.0F)) % 2 == 0;
    }, density));
    EXPECT_NEAR(CoverageAt(half, 200.0F, 200.0F), 0.5F, 0.08F);
    // Colour and height come from the roots.
    const auto& entry = full.Grid[full.Triangles[0].Atlas[3] + GrassFieldGridIndex(n, 4U, 4U)];
    EXPECT_EQ(entry[0], 0xff204080U);
    EXPECT_NEAR(GrassFieldHeightFraction(entry), 1.0F, 1.0e-3F);
}

TEST(Oot3dGrassDistantField, BareGroundStaysBare) {
    const float density = 1.0e4F / 9.0F;
    const auto field = BuildGrassDistantField(LatticeInput(3.0F, [](float x, float) { return x < 400.0F; }, density));
    EXPECT_NEAR(CoverageAt(field, 200.0F, 200.0F), 1.0F, 0.08F);
    EXPECT_EQ(CoverageAt(field, 600.0F, 100.0F), 0.0F);
    // The world cell table reads the same estimate where roots stand (also a
    // slab height above them) and nothing one cell past the last root: a ray
    // marching out of its entry triangle never finds columns over bare ground.
    ASSERT_FALSE(field.Cells.empty());
    EXPECT_GE(field.Cells.size(), field.OccupiedCells * 2U);
    EXPECT_EQ(field.Cells.size() & (field.Cells.size() - 1U), 0U);
    EXPECT_NE(field.Triangles[0].Cells[3], 0U);
    EXPECT_NEAR(GrassFieldCellCoverage(field, {200.0F, 0.0F, 200.0F}, 50.0F), 1.0F, 0.08F);
    EXPECT_NEAR(GrassFieldCellCoverage(field, {200.0F, 10.0F, 200.0F}, 50.0F), 1.0F, 0.08F);
    EXPECT_GT(GrassFieldCellCoverage(field, {390.0F, 0.0F, 100.0F}, 50.0F), 0.3F);
    EXPECT_EQ(GrassFieldCellCoverage(field, {460.0F, 0.0F, 100.0F}, 50.0F), 0.0F);
    EXPECT_EQ(GrassFieldCellCoverage(field, {200.0F, 200.0F, 200.0F}, 50.0F), 0.0F);
    // No root, no triangle: the field never adds a surface.
    const auto empty = BuildGrassDistantField(LatticeInput(3.0F, [](float, float) { return false; }, density));
    EXPECT_TRUE(empty.Triangles.empty());
    EXPECT_TRUE(empty.Grid.empty());
    EXPECT_TRUE(empty.Cells.empty());
}

TEST(Oot3dGrassDistantField, CellKeysAndHashMatchTheShader) {
    const auto fragment = BuildGrassFieldFragmentShader();
    EXPECT_NE(fragment.find("uint key = uint(cell.x) | (uint(cell.z) << 11u) | (uint(cell.y) << 22u);"),
              std::string::npos);
    EXPECT_NE(fragment.find("const float GRASS_FIELD_CELL = 25.000000;"), std::string::npos);
    EXPECT_NE(fragment.find("grass_field_cell_coverage(entry_triangle, p)"), std::string::npos);
    EXPECT_EQ(GrassFieldCellKey(5U, 7U, 9U), 5U | (9U << 11U) | (7U << 22U));
    EXPECT_EQ(kGrassFieldCellKeyBitsXZ, 11U);
    EXPECT_EQ(kGrassFieldCellKeyBitsY, 10U);
    // The GLSL hash: key * 0x9e3779b1, ^= >> 15, *= 0x85ebca77, ^= >> 13.
    uint32_t value = 12345U * 0x9e3779b1U;
    value ^= value >> 15U;
    value *= 0x85ebca77U;
    value ^= value >> 13U;
    EXPECT_EQ(GrassFieldCellHash(12345U), value);
}

TEST(Oot3dGrassDistantField, CandidateCapacityAndSpacingScaleTheExpectation) {
    const float density = 1.0e4F / 9.0F;
    auto input = LatticeInput(4.2426F, [](float, float) { return true; }, density); // half the roots
    input.CandidateScale = 0.5; // the room could only extract half its candidates
    EXPECT_NEAR(CoverageAt(BuildGrassDistantField(input), 300.0F, 300.0F), 1.0F, 0.1F);
    EXPECT_NEAR(GrassFieldSpacingKeep(1048.8F, 0.0F), 1.0F, 1.0e-6F);
    EXPECT_NEAR(GrassFieldSpacingKeep(1048.8F, 0.6F), 0.9424F, 1.0e-3F);
}

TEST(Oot3dGrassDistantField, ShadersComposeTheBladeHelpers) {
    const auto vertex = BuildGrassFieldVertexShader();
    const auto fragment = BuildGrassFieldFragmentShader();
    const auto blade = BuildGrassVertexShader();
    for (const char* shared : {"struct GrassEnvironmentRecord {", "vec3 grass_native_material_rgb(",
                               "vec4 grass_clip_position("}) {
        const auto start = blade.find(shared);
        ASSERT_NE(start, std::string::npos) << shared;
        EXPECT_NE(vertex.find(shared), std::string::npos) << shared;
    }
    EXPECT_NE(fragment.find("vec2 evaluate_wind("), std::string::npos);
    // The blend band is measured at each column's own distance (entry distance
    // + descent), never at the slab-top entry, and the vertex cull keeps every
    // slab whose longest descent can still reach the band.
    EXPECT_NE(fragment.find("grass_field_blend(distance + descent)"), std::string::npos);
    EXPECT_NE(fragment.find("if (ground_distance < grass.field.x) discard;"), std::string::npos);
    EXPECT_EQ(fragment.find("if (distance < grass.field.x) discard;"), std::string::npos);
    EXPECT_NE(vertex.find("farthest + slab / GRASS_FIELD_MINIMUM_SLOPE < grass.field.x"), std::string::npos);
    EXPECT_NE(BuildGrassFieldFragmentShader(true).find("layout(depth_greater) out float gl_FragDepth"),
              std::string::npos);
}

TEST(Oot3dGrassDistantField, SettingsPersistAndNormalize) {
    auto settings = GraphicsSettingsService::Preset(GraphicsPreset::Enhanced);
    settings.Grass.DistantFieldEnabled = false;
    settings.Grass.DistantFieldStartDistance = 1234.0F;
    settings.Grass.DistantFieldBlendFraction = 0.4F;
    settings.Grass.DistantFieldHeightScale = 1.5F;
    settings.Grass.DistantFieldSteps = 9U;
    const auto restored = DeserializeGraphicsSettings(SerializeGraphicsSettings(settings)).Value.Grass;
    EXPECT_FALSE(restored.DistantFieldEnabled);
    EXPECT_EQ(restored.DistantFieldStartDistance, 1234.0F);
    EXPECT_EQ(restored.DistantFieldBlendFraction, 0.4F);
    EXPECT_EQ(restored.DistantFieldHeightScale, 1.5F);
    EXPECT_EQ(restored.DistantFieldSteps, 9U);
    // Older profiles without the keys get the field with its defaults.
    auto document = SerializeGraphicsSettings(GraphicsSettingsService::Preset(GraphicsPreset::Enhanced));
    document["Grass"]["Performance"].erase("DistantFieldEnabled");
    document["Grass"]["Performance"].erase("DistantFieldStartDistance");
    const auto legacy = DeserializeGraphicsSettings(document).Value.Grass;
    EXPECT_TRUE(legacy.DistantFieldEnabled);
    EXPECT_EQ(legacy.DistantFieldStartDistance, 700.0F);
}

} // namespace
