#include "fast/renderer3ds/pica_scene_water.h"

#include <iostream>
#include <stdexcept>

using namespace Fast::Renderer3ds;

static void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static PicaSceneWaterBody Body(float minX, float minZ, float maxX, float maxZ, float y) {
    PicaSceneWaterBody body;
    body.MinimumXZ = {minX, minZ};
    body.MaximumXZ = {maxX, maxZ};
    body.SurfaceY = y;
    return body;
}

int main() {
    int checks = 0;
    PicaSceneWaterBodies bodies;
    // Zora's Domain (live boxes): the upper pools and the main pool.
    bodies.Bodies[0] = Body(-348, -1746, 205, -966, 877);
    bodies.Bodies[1] = Body(205, -1746, 1052, -1226, 877);
    bodies.Bodies[2] = Body(-1262, -1068, 1218, 350, 0);
    bodies.Count = 3;
    const auto match = [&](std::array<float, 3> minimum, std::array<float, 3> maximum) {
        ++checks;
        return MatchPicaWaterSurface(minimum, maximum, bodies);
    };
    // The main pool's overlay (actor, flat at y 0) lies on body 2.
    expect(match({-1136, 0, -1027}, {1085, 0, 165}) == 2U, "pool surface");
    // A few units off the gameplay height is still the surface.
    expect(match({-500, 3, -500}, {500, 3.5F, 0}) == 2U, "surface within tolerance");
    // A flat floor 20 units under an upper pool (its bed) is not water.
    expect(!match({-275, 857, -1626}, {205, 857, -966}), "pool bed");
    // A mixed room mesh (water and banks, y 0 to 410) is not one surface.
    expect(!match({-1645, -2, -1126}, {938, 411, 598}), "mixed room mesh");
    // A flat draw at the height but mostly outside the footprint (a bank).
    expect(!match({1100, 0, 300}, {2000, 0, 900}), "bank at water height");
    // A line (degenerate footprint) inside the body counts as covered.
    expect(match({10, 0, -400}, {10, 0, -100}) == 2U, "degenerate footprint");
    // No bodies: nothing is water.
    PicaSceneWaterBodies none;
    expect(!MatchPicaWaterSurface({0, 0, 0}, {10, 0, 10}, none), "no bodies");
    ++checks;
    // Two bodies at one height: the one covering most.
    expect(match({150, 877, -1700}, {900, 877, -1300}) == 1U, "largest coverage");
    std::cout << "pica_scene_water_tests: " << checks << " checks passed\n";
    return 0;
}
