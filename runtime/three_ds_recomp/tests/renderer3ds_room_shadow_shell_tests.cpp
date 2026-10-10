#include "fast/renderer3ds/room_shadow_shell.h"
#include "fast/renderer3ds/pica_room_shadow_selection.h"
#include <gtest/gtest.h>
#include <bit>
#include <chrono>
#include <fstream>
#include <cmath>
#ifdef ROOM_SHADOW_HAS_SHADERC
#include <shaderc/shaderc.hpp>
#endif

using namespace Fast::Renderer3ds;
namespace {
void Roof(std::vector<RoomShadowTriangle>& tris, float y, bool cull = true, bool alpha = false) {
    tris.push_back({ { { { -100, y, -100 }, { 100, y, -100 }, { 100, y, 100 } } }, cull, alpha });
    tris.push_back({ { { { -100, y, -100 }, { 100, y, 100 }, { -100, y, 100 } } }, cull, alpha });
}
bool Shell(const RoomShadowShell& m, size_t triangle, uint32_t sector) {
    return (m.ShellMasks[triangle] & (uint64_t{ 1 } << sector)) != 0;
}
} // namespace
TEST(RoomShadowShell, ClassifiesNativeOpaqueReplacementAndRejectedCasterReasons) {
    using Class = PicaRoomShadowCasterClass;
    PicaResolvedDrawRecord room;
    room.CompositionDomain = PicaCompositionDomain::Scene;
    room.Composition = { PicaCompositionLayer::OpaqueWorld, PicaCompositionProvenance::NativeCmbDrawPass };
    // Deku Tree's captured PICA 0x100=0x00e40100, 0x101=0x01010000:
    // enabled blend unit, ADD/ADD, ONE/ZERO for both RGB and alpha.
    room.Material.Blend.Enabled = true;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(room), Class::Room);
    auto draw = room;
    draw.Material.Blend.Enabled = false;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::Room);
    draw = room;
    draw.CompositionDomain = PicaCompositionDomain::Unknown;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::UnknownDomain);
    draw.CompositionDomain = PicaCompositionDomain::Ui;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::OtherDomain);
    draw = room;
    draw.Composition.NativeActorGeometry = true;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::Actor);
    for (const auto layer :
         { PicaCompositionLayer::Unknown, PicaCompositionLayer::Atmosphere, PicaCompositionLayer::TransparentWorld }) {
        draw = room;
        draw.Composition.Layer = layer;
        EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::OtherPass);
    }
    draw = room;
    draw.Composition.Provenance = PicaCompositionProvenance::Unknown;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::OtherPass);
    draw = room;
    draw.Material.AlphaTestEnabled = true;
    for (const auto compare : { PicaCompareFunction::Always, PicaCompareFunction::Greater }) {
        draw.Material.AlphaCompare = compare;
        EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::AlphaTest);
    }
    draw = room;
    draw.Material.Blend.DestRgb = NativeBlendFactor::One;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::Translucent);
    draw = room;
    draw.Material.Blend.SourceRgb = NativeBlendFactor::SourceAlpha;
    draw.Material.Blend.DestRgb = NativeBlendFactor::OneMinusSourceAlpha;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::Translucent);
    draw = room;
    draw.Material.Blend.EquationAlpha = NativeBlendEquation::Subtract;
    EXPECT_EQ(ClassifyPicaRoomShadowCaster(draw), Class::Translucent);
}

TEST(RoomShadowShell, SelectsPublishedSnapshotIndependentlyOfRoomCasterRejections) {
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 0);
    auto mesh = BuildRoomShadowShell(5, triangles, 32);
    ASSERT_TRUE(mesh);
    auto scene = std::make_shared<RoomShadowShellScene>();
    scene->ScopeIdentity = 123;
    scene->Rooms.push_back({ 5, std::make_shared<const RoomShadowShell>(*mesh) });
    PicaResolvedDrawRecord room;
    room.CompositionDomain = PicaCompositionDomain::Scene;
    room.Composition = { PicaCompositionLayer::OpaqueWorld, PicaCompositionProvenance::NativeCmbDrawPass };
    room.Material.Blend.Enabled = true;
    room.RoomShadows = scene;
    auto actor = room;
    actor.Composition.NativeActorGeometry = true;
    auto alpha = room;
    alpha.Material.AlphaTestEnabled = true;
    const std::array<const PicaResolvedDrawRecord*, 3> draws{ &actor, &alpha, &room };
    auto selected = SelectPicaRoomShadowScene(draws);
    EXPECT_EQ(selected.ReplacementScene, scene.get());
    EXPECT_STREQ(selected.Status, "ready");
    EXPECT_EQ(selected.Count(PicaRoomShadowCasterClass::Room), 1U);
    EXPECT_EQ(selected.Count(PicaRoomShadowCasterClass::Actor), 1U);
    EXPECT_EQ(selected.Count(PicaRoomShadowCasterClass::AlphaTest), 1U);
    EXPECT_EQ(selected.Snapshots, 3U);
    EXPECT_EQ(selected.ReadySnapshots, 3U);
    selected = SelectPicaRoomShadowScene(std::span(draws).first(2));
    EXPECT_EQ(selected.ReplacementScene, nullptr);
    EXPECT_EQ(selected.PublishedScene, scene.get());
    EXPECT_STREQ(selected.Status, "no_room_casters");
    // An actor's ready snapshot cannot authorize removal of an unbound room.
    room.RoomShadows.reset();
    selected = SelectPicaRoomShadowScene(draws);
    EXPECT_EQ(selected.ReplacementScene, nullptr);
    EXPECT_EQ(selected.PublishedScene, scene.get());
    EXPECT_STREQ(selected.Status, "no_snapshot");
    auto pending = std::make_shared<RoomShadowShellScene>(*scene);
    pending->Rooms.push_back({ 6, {} });
    room.RoomShadows = pending;
    selected = SelectPicaRoomShadowScene(draws);
    EXPECT_EQ(selected.ReplacementScene, nullptr);
    EXPECT_STREQ(selected.Status, "pending_or_failed");
    room.RoomShadows = scene;
    auto otherRoom = room;
    otherRoom.RoomShadows = std::make_shared<RoomShadowShellScene>(*scene);
    const std::array<const PicaResolvedDrawRecord*, 2> mixed{ &room, &otherRoom };
    selected = SelectPicaRoomShadowScene(mixed);
    EXPECT_EQ(selected.ReplacementScene, nullptr);
    EXPECT_STREQ(selected.Status, "mixed_snapshots");
    // Disabling shells must not recover a stale snapshot from the last frame.
    room.RoomShadows.reset();
    otherRoom.RoomShadows.reset();
    selected = SelectPicaRoomShadowScene(mixed);
    EXPECT_EQ(selected.PublishedScene, nullptr);
    EXPECT_EQ(selected.Snapshots, 0U);
    EXPECT_STREQ(selected.Status, "no_snapshot");
}

TEST(RoomShadowShell, FirstBackLayerOnlyAndBothFaceOccluders) {
    const auto sector = RoomShadowSector({ 0, 1, 0 });
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 20);
    Roof(triangles, 10);
    Roof(triangles, 0);
    auto mesh = BuildRoomShadowShell(123, triangles, 128);
    ASSERT_TRUE(mesh);
    EXPECT_TRUE(Shell(*mesh, 0, sector));
    EXPECT_TRUE(Shell(*mesh, 1, sector));
    for (size_t i = 2; i < 6; ++i)
        EXPECT_FALSE(Shell(*mesh, i, sector));
    EXPECT_EQ(mesh->Positions.size(), 12U);
    // An outward-facing first layer does not expose a back-facing layer below.
    for (size_t i = 0; i < 2; ++i)
        std::swap(triangles[i].Positions[1], triangles[i].Positions[2]);
    mesh = BuildRoomShadowShell(123, triangles, 128);
    ASSERT_TRUE(mesh);
    for (size_t i = 0; i < 6; ++i)
        EXPECT_FALSE(Shell(*mesh, i, sector));
    triangles.clear();
    Roof(triangles, 20, false);
    Roof(triangles, 10);
    mesh = BuildRoomShadowShell(123, triangles, 128);
    ASSERT_TRUE(mesh);
    for (size_t i = 0; i < 4; ++i)
        EXPECT_FALSE(Shell(*mesh, i, sector));
}
TEST(RoomShadowShell, AlphaTransparencyGrazingAndCancellation) {
    const auto sector = RoomShadowSector({ 0, 1, 0 });
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 20, true, true);
    Roof(triangles, 10);
    auto mesh = BuildRoomShadowShell(1, triangles, 64);
    ASSERT_TRUE(mesh);
    ASSERT_EQ(mesh->ShellMasks.size(), 2U);
    EXPECT_TRUE(Shell(*mesh, 0, sector));
    triangles = { { { { { 0, 0, 0 }, { 0, 100, 0 }, { 0, 0, 100 } } }, true } };
    mesh = BuildRoomShadowShell(1, triangles, 64);
    ASSERT_TRUE(mesh);
    EXPECT_FALSE(Shell(*mesh, 0, sector));
    triangles[0].Positions[0][0] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(BuildRoomShadowShell(1, triangles));
    std::stop_source stop;
    stop.request_stop();
    EXPECT_FALSE(BuildRoomShadowShell(1, triangles, 64, stop.get_token()));
}
TEST(RoomShadowShell, MajorityOfCoveredTexelsRequired) {
    const uint32_t sector = 63;
    const auto d = RoomShadowDirections()[sector];
    // Perpendicular frame with cross(right,up)=-d (back face).
    const float n = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    const ShadowVector right{ d[2] / n, 0, -d[0] / n };
    const ShadowVector up{ d[1] * right[2], d[2] * right[0] - d[0] * right[2], -d[1] * right[0] };
    auto triangle = [&](float scale, float height, bool cull) {
        RoomShadowTriangle t{};
        t.CullBack = cull;
        for (size_t k = 0; k < 3; ++k) {
            t.Positions[0][k] = d[k] * height;
            t.Positions[1][k] = d[k] * height + right[k] * 100 * scale;
            t.Positions[2][k] = d[k] * height - up[k] * 100 * scale;
        }
        return t;
    };
    for (const auto [scale, expected] : { std::pair{ .8F, false }, std::pair{ .6F, true } }) {
        const std::array triangles{ triangle(1, 0, true), triangle(scale, 10, false) };
        auto m = BuildRoomShadowShell(1, triangles, 256);
        ASSERT_TRUE(m);
        EXPECT_EQ(Shell(*m, 0, sector), expected);
    }
}
TEST(RoomShadowShell, SectorsWeldingFormatCorruptionAndPersistence) {
    const auto& directions = RoomShadowDirections();
    for (uint32_t i = 0; i < 64; ++i) {
        EXPECT_GT(directions[i][1], 0);
        EXPECT_EQ(RoomShadowSector(directions[i]), i);
    }
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 0);
    auto mesh = BuildRoomShadowShell(0xabcdef, triangles);
    ASSERT_TRUE(mesh);
    EXPECT_EQ(mesh->Positions.size(), 4U);
    EXPECT_EQ(mesh->Indices.size(), 6U);
    const auto bytes = EncodeRoomShadowShell(*mesh);
    const auto decoded = DecodeRoomShadowShell(bytes, mesh->Fingerprint);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->ShellMasks, mesh->ShellMasks);
    EXPECT_EQ(decoded->ContentVersion, mesh->ContentVersion);
    EXPECT_FALSE(DecodeRoomShadowShell(bytes, 42));
    for (size_t at : { size_t(0), size_t(4), size_t(24), size_t(36), bytes.size() - 1 }) {
        auto bad = bytes;
        bad[at] ^= 1;
        EXPECT_FALSE(DecodeRoomShadowShell(bad, mesh->Fingerprint));
    }
    EXPECT_FALSE(DecodeRoomShadowShell(std::span(bytes).first(bytes.size() - 1), mesh->Fingerprint));
    auto bad = bytes;
    bad.push_back(0);
    EXPECT_FALSE(DecodeRoomShadowShell(bad, mesh->Fingerprint));
    const auto gpu = RoomShadowShellVertices(*mesh);
    ASSERT_EQ(gpu.size(), 6U);
    EXPECT_EQ(gpu[0].Mask, gpu[1].Mask);
    EXPECT_EQ(gpu[1].Mask, gpu[2].Mask);
    EXPECT_EQ(uint64_t(gpu[0].Mask[0]) | (uint64_t(gpu[0].Mask[1]) << 32), mesh->ShellMasks[0]);
    auto path = std::filesystem::temp_directory_path() /
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    ASSERT_TRUE(SaveRoomShadowShell(path, *mesh));
    EXPECT_TRUE(LoadRoomShadowShell(path, mesh->Fingerprint));
    std::filesystem::remove(RoomShadowShellPath(path, mesh->Fingerprint));
    std::filesystem::remove(path / "shadow-shells");
    std::filesystem::remove(path);
}
TEST(RoomShadowShell, PendingRoomPreventsPartialReplacement) {
    std::vector<RoomShadowTriangle> t;
    Roof(t, 0);
    auto mesh = BuildRoomShadowShell(5, t, 32);
    RoomShadowShellScene scene;
    EXPECT_FALSE(scene.Ready());
    scene.Rooms = { { 5, mesh }, { 6, {} } };
    EXPECT_FALSE(scene.Ready());
    scene.Rooms.pop_back();
    EXPECT_TRUE(scene.Ready());
    scene.Rooms[0].Fingerprint = 7;
    EXPECT_FALSE(scene.Ready());
}
TEST(RoomShadowShell, CacheWriteFailureReportsTheFilesystemError) {
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 0);
    auto mesh = BuildRoomShadowShell(5, triangles, 32);
    ASSERT_TRUE(mesh);
    const auto blocked =
        std::filesystem::temp_directory_path() /
        ("shell-cache-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        std::ofstream file(blocked);
        file << "file, not cache directory";
    }
    std::string error;
    EXPECT_FALSE(SaveRoomShadowShell(blocked, *mesh, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(DecodeRoomShadowShell(EncodeRoomShadowShell(*mesh), 5));
    std::filesystem::remove(blocked);
}
TEST(RoomShadowShell, StaticPagesReuseUntilSectorOrRasterStateChanges) {
    std::vector<RoomShadowTriangle> triangles;
    Roof(triangles, 0);
    const auto mesh = BuildRoomShadowShell(5, triangles, 32);
    ASSERT_TRUE(mesh);
    const auto decoded = DecodeRoomShadowShell(EncodeRoomShadowShell(*mesh), 5);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->RasterSize, 32U);
    const auto layout = BuildVirtualShadowLayout({ 0, 0, 0 }, { 0, 1, 0 }, 2048, 500);
    VirtualShadowMapCache cache;
    std::array casters{ RoomShadowCaster(*mesh, 63) };
    EXPECT_FALSE(casters[0].Dynamic);
    cache.Prepare(layout, casters, 1);
    EXPECT_GT(cache.Statistics().StaticUpdated, 0U);
    cache.Commit();
    casters[0] = RoomShadowCaster(*decoded, 63);
    cache.Prepare(layout, casters, 2);
    EXPECT_EQ(cache.Statistics().StaticUpdated, 0U);
    EXPECT_GT(cache.Statistics().StaticReused, 0U);
    cache.Commit();
    casters[0] = RoomShadowCaster(*decoded, 62);
    cache.Prepare(layout, casters, 3);
    EXPECT_GT(cache.Statistics().StaticUpdated, 0U);
    cache.Commit();
    casters[0] = RoomShadowCaster(*decoded, 62, 1);
    cache.Prepare(layout, casters, 4);
    EXPECT_GT(cache.Statistics().StaticUpdated, 0U);
}
#ifdef ROOM_SHADOW_HAS_SHADERC
TEST(RoomShadowShell, VertexShaderCompilesForVulkan) {
    shaderc::Compiler compiler;
    const auto result = compiler.CompileGlslToSpv(std::string(RoomShadowShellVertexShader()), shaderc_vertex_shader,
                                                  "room_shadow_shell.vert");
    EXPECT_EQ(result.GetCompilationStatus(), shaderc_compilation_status_success) << result.GetErrorMessage();
}
#endif
