#include "fast/oot3d/grass_overlay_mask.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_async_placement_builder.h"
#include "fast/oot3d/grass_static_placement_cache.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
using namespace Fast::Oot3d;
GrassOverlayAlpha Alpha() {
    GrassOverlayAlpha a;
    a.Translucent = true;
    // Texture alpha * primary alpha; subsequent stages replace with previous.
    a.Program.Stages[0] = { 3U << 16, 0, 1U << 16, 0 };
    for (size_t i = 1; i < 6; ++i)
        a.Program.Stages[i] = { 15U << 16, 0, 0, 0 };
    return a;
}
auto Mask(std::initializer_list<uint8_t> values) {
    auto p = std::make_shared<GrassScalarMask>();
    p->Width = uint16_t(values.size());
    p->Height = 1;
    p->Channel = GrassSampleChannel::Alpha;
    p->Samples = values;
    return p;
}
GrassSceneMesh Mesh(uint64_t id, uint64_t order, float height = 0) {
    GrassSceneMesh m;
    m.GeometryId = m.InstanceId = id;
    m.ContentVersion = m.AnchorVersion = 1;
    m.DrawOrder = order;
    m.TextureHash = m.ObservedTextureHash = 123;
    m.FrameId = 1;
    m.RenderTargetNamespace = 1;
    m.TextureWidth = m.TextureHeight = 1;
    m.Vertices = std::make_shared<const std::vector<GrassSourceVertex>>(
        std::initializer_list<GrassSourceVertex>{ { { 0, height, 0 }, { 0, 1, 0 }, { 0, 0 } },
                                                  { { 200, height, 0 }, { 0, 1, 0 }, { 1, 0 } },
                                                  { { 0, height, 200 }, { 0, 1, 0 }, { 0, 1 } } });
    m.Indices = std::make_shared<const std::vector<uint32_t>>(std::initializer_list<uint32_t>{ 0, 1, 2 });
    return m;
}
struct OverlayFixture : testing::Test {
    GrassSceneBridge& Bridge = GrassSceneBridge::Instance();
    GrassSceneMesh Source = Mesh(1, 10);
    GrassSceneMesh Overlay = Mesh(2, 20);
    std::shared_ptr<GrassOverlayAlpha> Material = std::make_shared<GrassOverlayAlpha>();
    void SetUp() override {
        Bridge.Clear();
        Overlay.Overlay = Material;
        Material->Version = 1;
        Bridge.Publish(Source);
    }
    void TearDown() override {
        Bridge.Clear();
    }
    auto Extract(std::shared_ptr<const GrassOverlaySet> masks = {}) {
        GrassSourceSurface s;
        s.GeometryId = Source.GeometryId;
        s.Vertices = *Source.Vertices;
        s.Indices = *Source.Indices;
        s.Overlays = masks;
        GrassPlacementRule rule;
        rule.InputBlack = 0;
        rule.InputWhite = 1;
        GrassGenerationSettings g;
        g.InstancesPerSquareMeter = 1000;
        g.MinimumSpacing = 0;
        return GrassSurfaceExtractor::Extract(s, rule, g, *Mask({ 255 }), 10000);
    }
};
TEST_F(OverlayFixture, FullyOpaqueOverlayRemovesRootsWithoutBecomingSource) {
    ASSERT_FALSE(Extract().empty());
    Bridge.Publish(Overlay);
    EXPECT_TRUE(Extract(Bridge.OverlaysFor(Source)).empty());
    GrassTextureSelector selector;
    selector.Rgba8Hash = 123;
    EXPECT_EQ(Bridge.Matching(selector).size(), 1);
}
TEST_F(OverlayFixture, TransparentEdgesAndSoftAlphaThinInsteadOfCuttingRectangle) {
    *Material = Alpha();
    Material->Texture[3] = Mask({ 0, 128, 255 });
    Material->WrapS = GrassTextureWrap::Clamp;
    Material->Version = 2;
    Bridge.Publish(Overlay);
    const auto masked = Extract(Bridge.OverlaysFor(Source)), original = Extract();
    ASSERT_GT(masked.size(), 0);
    ASSERT_LT(masked.size(), original.size());
    size_t left = 0, leftOriginal = 0, middle = 0, middleOriginal = 0;
    for (const auto& a : original) {
        leftOriginal += a.Uv[0] < 1.0F / 3;
        middleOriginal += a.Uv[0] >= 1.0F / 3 && a.Uv[0] < 2.0F / 3;
    }
    for (const auto& a : masked) {
        left += a.Uv[0] < 1.0F / 3;
        middle += a.Uv[0] >= 1.0F / 3 && a.Uv[0] < 2.0F / 3;
        EXPECT_LT(a.Uv[0], 2.0F / 3);
    }
    EXPECT_EQ(left, leftOriginal);
    EXPECT_NEAR(double(middle) / middleOriginal, .5, .08);
}
TEST_F(OverlayFixture, PlaneToleranceOrderAndScopeAreRequired) {
    Overlay = Mesh(2, 20, 2);
    Overlay.Overlay = Material;
    Bridge.Publish(Overlay);
    EXPECT_EQ(Extract(Bridge.OverlaysFor(Source)).size(), Extract().size());
    Bridge.Clear();
    Overlay = Mesh(2, 9);
    Overlay.Overlay = Material;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Overlay.DrawOrder = 20;
    Overlay.RenderTargetNamespace = 2;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Overlay.RenderTargetNamespace = 1;
    Overlay.FrameId = 2;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Overlay.FrameId = Source.FrameId;
    Overlay.SequenceId = Source.SequenceId + 1;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
}
TEST_F(OverlayFixture, SlopedSourceUsesItsNormalAndWallDoesNotMask) {
    auto vertices = std::make_shared<std::vector<GrassSourceVertex>>(*Source.Vertices);
    (*vertices)[1].Position[1] = 40;
    Source.Vertices = vertices;
    auto lifted = std::make_shared<std::vector<GrassSourceVertex>>(*vertices);
    for (auto& v : *lifted) {
        v.Position[0] -= .1F;
        v.Position[1] += .5F;
    }
    Overlay.Vertices = lifted;
    Bridge.Publish(Overlay);
    EXPECT_TRUE(Extract(Bridge.OverlaysFor(Source)).empty());
    Bridge.Clear();
    Overlay = Mesh(2, 20);
    Overlay.Overlay = Material;
    auto wall = std::make_shared<std::vector<GrassSourceVertex>>(*Overlay.Vertices);
    (*wall)[1].Position = { 0, 200, 0 };
    Overlay.Vertices = wall;
    Bridge.Publish(Overlay);
    EXPECT_EQ(Extract(Bridge.OverlaysFor(Source)).size(), Extract().size());
}
TEST_F(OverlayFixture, SameDrawTrianglesUnionAndDifferentDrawsComposite) {
    *Material = Alpha();
    Material->Texture[3] = Mask({ 128 });
    Material->Version = 2;
    Overlay.Indices =
        std::make_shared<const std::vector<uint32_t>>(std::initializer_list<uint32_t>{ 0, 1, 2, 0, 1, 2 });
    Bridge.Publish(Overlay);
    auto set = Bridge.OverlaysFor(Source);
    auto tris = GrassOverlaysForTriangle(
        { (*Source.Vertices)[0].Position, (*Source.Vertices)[1].Position, (*Source.Vertices)[2].Position }, *set);
    EXPECT_NEAR(GrassOverlaySurvival({ 10, 0, 10 }, tris), 127.0F / 255, 1e-6);
    auto second = Overlay;
    second.InstanceId = 3;
    second.GeometryId = 3;
    second.DrawOrder = 30;
    Bridge.Publish(second);
    set = Bridge.OverlaysFor(Source);
    tris = GrassOverlaysForTriangle(
        { (*Source.Vertices)[0].Position, (*Source.Vertices)[1].Position, (*Source.Vertices)[2].Position }, *set);
    EXPECT_NEAR(GrassOverlaySurvival({ 10, 0, 10 }, tris), (127.0F / 255) * (127.0F / 255), 1e-6);
}
TEST_F(OverlayFixture, CacheChangesWithMaterialGeometryOrderAndRemoval) {
    Bridge.Publish(Overlay);
    auto a = Bridge.OverlaysFor(Source);
    ASSERT_TRUE(a);
    EXPECT_EQ(a, Bridge.OverlaysFor(Source));
    auto next = std::make_shared<GrassOverlayAlpha>(*Material);
    next->Version = 2;
    Overlay.Overlay = next;
    Bridge.Publish(Overlay);
    auto b = Bridge.OverlaysFor(Source);
    ASSERT_TRUE(b);
    EXPECT_NE(a->Version, b->Version);
    Overlay.ContentVersion = 2;
    Bridge.Publish(Overlay);
    auto c = Bridge.OverlaysFor(Source);
    EXPECT_NE(b->Version, c->Version);
    Overlay.DrawOrder = 5;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Overlay.DrawOrder = 20;
    Bridge.Publish(Overlay);
    ASSERT_TRUE(Bridge.OverlaysFor(Source));
    Bridge.RemoveInstance(Overlay.InstanceId);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
}
TEST_F(OverlayFixture, ScrollingUvIsExcludedUntilReset) {
    Bridge.Publish(Overlay);
    ASSERT_TRUE(Bridge.OverlaysFor(Source));
    Overlay.CoordinateVersion = 2;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Overlay.CoordinateVersion = 0;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
    Bridge.RemoveRoom(1);
    Bridge.Publish(Overlay);
    EXPECT_TRUE(Bridge.OverlaysFor(Source));
}
TEST_F(OverlayFixture, MovingRigidTransformIsExcludedButCameraRoundoffIsTolerated) {
    Overlay.TransformBakedIntoVertices = false;
    Overlay.ModelToWorld = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    Overlay.PreserveWorldAnchor = true;
    Bridge.Publish(Overlay);
    auto original = Bridge.OverlaysFor(Source);
    ASSERT_TRUE(original);
    Overlay.ModelToWorld[3] = .001F;
    Bridge.Publish(Overlay);
    EXPECT_EQ(Bridge.OverlaysFor(Source), original);
    Overlay.ModelToWorld[3] = 2;
    Bridge.Publish(Overlay);
    EXPECT_FALSE(Bridge.OverlaysFor(Source));
}
TEST(Oot3dGrassOverlayAlpha, NativeTevPrimaryConstantOperandsScaleAndAlphaTest) {
    auto a = Alpha();
    a.Texture[3] = Mask({ 128 });
    auto inputs = GrassOverlayRequiredAlphaInputs(a);
    EXPECT_TRUE(inputs.Supported);
    EXPECT_TRUE(inputs.Primary);
    EXPECT_EQ(inputs.TextureChannels, 8);
    EXPECT_NEAR(EvaluateGrassOverlayCoverage(a, { .5, .5 }, .5), 64.0F / 255, 1e-6);
    a.Program.Stages[1] = { uint32_t(15 | (14 << 4)) << 16, 0, 1U << 16, 0 };
    a.Constants[1][3] = .5F;
    EXPECT_NEAR(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1), 64.0F / 255, 1e-6);
    a.Program.Stages[1][3] = 1U << 16;
    EXPECT_NEAR(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1), 128.0F / 255, 1e-6);
    a.Translucent = false;
    a.AlphaTest = true;
    a.Compare = Fast::Renderer3ds::PicaCompareFunction::Greater;
    a.Reference = 128;
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1), 0);
    a.Reference = 127;
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1), 1);
    a.AlphaTest = false;
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 0), 1);
}
TEST(Oot3dGrassOverlayAlpha, BilinearTransparentBordersWrapAndUnsupportedDependencies) {
    auto a = Alpha();
    a.Texture[3] = Mask({ 0, 255 });
    a.Linear = true;
    a.WrapS = GrassTextureWrap::Clamp;
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { .1, .5 }, 1), 0);
    EXPECT_NEAR(EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1), 128.0F / 255, 1e-6);
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { .9, .5 }, 1), 1);
    a.WrapS = GrassTextureWrap::Repeat;
    EXPECT_EQ(EvaluateGrassOverlayCoverage(a, { 1.5, .5 }, 1), EvaluateGrassOverlayCoverage(a, { .5, .5 }, 1));
    a.Program.Stages[5][0] = 1U << 16;
    EXPECT_FALSE(GrassOverlayRequiredAlphaInputs(a).Supported);
    a.Program.Stages[5][0] = 14U << 16;
    EXPECT_TRUE(GrassOverlayRequiredAlphaInputs(a).Supported);
    EXPECT_FALSE(GrassOverlayRequiredAlphaInputs(a).Primary); // overwritten unsupported stages have no effect
}
TEST(Oot3dGrassOverlayAlpha, NativeLitAlphaSumsEnabledLightsAndUnlitTintReplacesMaterial) {
    Fast::Renderer3ds::PicaVertexShaderHookLayout h;
    auto& s = h.Surface;
    s.ViewNormalTemporary = 14;
    s.LightingBooleanUniform = 9;
    s.DiffuseUniform = 8;
    s.AmbientUniform = 9;
    s.TintBooleanUniform = 5;
    s.TintInputRegister = 2;
    s.TintScale = { 90, 2 };
    s.LightEnabledValue = { 93, 1 };
    s.LightDirectionUniforms = { 80, 83, 86 };
    std::vector<uint8_t> bytes(80 + 96 * 16);
    const auto scalar = [&](int slot, int component, float x) {
        std::memcpy(bytes.data() + 80 + slot * 16 + component * 4, &x, 4);
    };
    const auto flag = [&](uint32_t x) { std::memcpy(bytes.data(), &x, 4); };
    scalar(8, 3, .6F);
    scalar(93, 1, 1);
    scalar(80, 3, 1);
    scalar(83, 3, 1);
    scalar(86, 3, 0);
    scalar(81, 3, .5F);
    scalar(84, 3, .25F);
    scalar(87, 3, 99);
    scalar(90, 2, 1.0F / 255);
    flag(512);
    auto p = DecodeGrassOverlayPrimaryAlpha(h, bytes);
    ASSERT_TRUE(p.Valid);
    EXPECT_FLOAT_EQ(p.Scale, .45F);
    EXPECT_EQ(p.InputRegister, 255);
    flag(512 | 32);
    p = DecodeGrassOverlayPrimaryAlpha(h, bytes);
    EXPECT_NEAR(p.Scale, .45F / 255, 1e-7);
    EXPECT_EQ(p.InputRegister, 2);
    flag(32);
    p = DecodeGrassOverlayPrimaryAlpha(h, bytes);
    EXPECT_FLOAT_EQ(p.Scale, 1.0F / 255);
    flag(0);
    EXPECT_FLOAT_EQ(DecodeGrassOverlayPrimaryAlpha(h, bytes).Scale, .6F);
    EXPECT_FALSE(DecodeGrassOverlayPrimaryAlpha({}, bytes).Valid);
}
TEST_F(OverlayFixture, AsyncAndWorldCachesIncludeOverlayVersion) {
    GrassAsyncPlacementRequest r;
    r.PlacementKey = { 1, 1, 123, 1 };
    r.SourceContentVersion = 1;
    r.WorldIdentity = 1;
    r.Vertices = Source.Vertices;
    r.Indices = Source.Indices;
    r.Mask = Mask({ 255 });
    r.Budget = 5000;
    r.Generation.InstancesPerSquareMeter = 100;
    r.Generation.MinimumSpacing = 0;
    r.Rule.InputBlack = 0;
    r.Rule.InputWhite = 1;
    GrassStaticPlacementCache cache;
    auto base = cache.Resolve(std::span(&r, 1));
    ASSERT_TRUE(base[0].Placement);
    ASSERT_FALSE(base[0].Placement->Anchors.empty());
    Bridge.Publish(Overlay);
    r.Overlays = Bridge.OverlaysFor(Source);
    auto masked = cache.Resolve(std::span(&r, 1));
    ASSERT_TRUE(masked[0].Placement);
    EXPECT_TRUE(masked[0].Placement->Anchors.empty());
    EXPECT_NE(base[0].Placement->ContentVersion, masked[0].Placement->ContentVersion);
    EXPECT_FALSE(cache.Resolve(std::span(&r, 1))[0].Queued);
    r.Overlays.reset();
    auto restored = cache.Resolve(std::span(&r, 1));
    EXPECT_EQ(restored[0].Placement, base[0].Placement);
}

// Optional, private ROM-derived input; the repository carries no scene payload.
TEST(Oot3dGrassOverlayBenchmark, OfflineRoomFixture) {
    const char* path = std::getenv("TRIAEVUM_GRASS_MASK_FIXTURE");
    if (!path)
        GTEST_SKIP() << "Set TRIAEVUM_GRASS_MASK_FIXTURE to a private room fixture";
    nlohmann::json fixture;
    std::ifstream(path) >> fixture;
    auto& bridge = GrassSceneBridge::Instance();
    bridge.Clear();
    std::vector<GrassSceneMesh> meshes;
    GrassSceneMesh source;
    GrassScalarMask sourceMask;
    for (const auto& j : fixture.at("meshes")) {
        auto m = Mesh(j.at("order").get<uint64_t>(), j.at("order").get<uint64_t>());
        auto vertices = std::make_shared<std::vector<GrassSourceVertex>>();
        auto indices = std::make_shared<std::vector<uint32_t>>();
        for (const auto& v : j.at("vertices")) {
            GrassSourceVertex vertex;
            vertex.Position = v.at("position").get<std::array<float, 3>>();
            vertex.Uv = v.at("uv").get<std::array<float, 2>>();
            indices->push_back(uint32_t(vertices->size()));
            vertices->push_back(vertex);
        }
        m.Vertices = vertices;
        m.Indices = indices;
        m.NativeTextureIdentity = m.TextureHash = std::stoull(j.at("hash").get<std::string>(), nullptr, 16);
        auto mask = std::make_shared<GrassScalarMask>();
        mask->Width = m.TextureWidth = j.at("width");
        mask->Height = m.TextureHeight = j.at("height");
        mask->Samples = j.at("samples").get<std::vector<uint8_t>>();
        m.PreserveWorldAnchor = true;
        if (m.DrawOrder == fixture.at("sourceOrder").get<uint64_t>()) {
            source = m;
            sourceMask = *mask;
        } else {
            auto alpha = std::make_shared<GrassOverlayAlpha>(Alpha());
            alpha->Texture[3] = mask;
            alpha->Version = 1;
            alpha->Translucent = j.at("translucent");
            alpha->AlphaTest = j.at("alphaTest");
            alpha->Linear = true;
            alpha->Compare = Fast::Renderer3ds::PicaCompareFunction::Greater;
            alpha->Reference = 128;
            m.Overlay = alpha;
        }
        meshes.push_back(m);
        bridge.Publish(m);
    }
    ASSERT_TRUE(source.Vertices);
    using Clock = std::chrono::steady_clock;
    const auto elapsed = [](auto begin) {
        return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    };
    auto begin = Clock::now();
    auto overlays = bridge.OverlaysFor(source);
    const auto prepareMs = elapsed(begin);
    ASSERT_TRUE(overlays);
    size_t sourceTriangles = 0, pairs = 0;
    for (size_t i = 0; i + 2 < source.Indices->size(); i += 3) {
        auto candidates = GrassOverlaysForTriangle(
            { (*source.Vertices)[i].Position, (*source.Vertices)[i + 1].Position, (*source.Vertices)[i + 2].Position },
            *overlays);
        sourceTriangles += !candidates.empty();
        pairs += candidates.size();
    }
    GrassSourceSurface surface;
    surface.GeometryId = source.GeometryId;
    surface.Vertices = *source.Vertices;
    surface.Indices = *source.Indices;
    GrassPlacementRule rule;
    const auto& r = fixture.at("settings").at("Sources")[0];
    rule.InputBlack = r.at("InputBlack");
    rule.InputWhite = r.at("InputWhite");
    rule.ResponseExponent = r.at("ResponseExponent");
    rule.MaximumSlopeDegrees = r.at("MaximumSlopeDegrees");
    rule.NormalOffset = r.at("NormalOffset");
    GrassGenerationSettings generation;
    const auto& g = fixture.at("settings").at("Generation");
    generation.InstancesPerSquareMeter = g.at("InstancesPerSquareMeter");
    generation.MinimumSpacing = g.at("MinimumSpacing");
    generation.IndividualRandomness = g.at("IndividualRandomness");
    generation.ClusterStrength = g.at("ClusterStrength");
    generation.Seed = g.at("Seed");
    const uint32_t budget = fixture.value("budget", 8000000U);
    std::array<double, 5> unmaskedTimes, maskedTimes;
    size_t unmasked = 0, masked = 0;
    for (size_t i = 0; i < 5; ++i) {
        surface.Overlays.reset();
        begin = Clock::now();
        unmasked = GrassSurfaceExtractor::Extract(surface, rule, generation, sourceMask, budget).size();
        unmaskedTimes[i] = elapsed(begin);
        surface.Overlays = overlays;
        begin = Clock::now();
        masked = GrassSurfaceExtractor::Extract(surface, rule, generation, sourceMask, budget).size();
        maskedTimes[i] = elapsed(begin);
    }
    std::sort(unmaskedTimes.begin(), unmaskedTimes.end());
    std::sort(maskedTimes.begin(), maskedTimes.end());
    begin = Clock::now();
    for (size_t i = 0; i < 10000; ++i) {
        auto p = bridge.OverlaysFor(source);
        ASSERT_EQ(p, overlays);
    }
    const auto warmUs = elapsed(begin) * .1;
    begin = Clock::now();
    for (size_t i = 0; i < 10000; ++i) {
        for (const auto& m : meshes)
            bridge.Publish(m);
        auto p = bridge.OverlaysFor(source);
        ASSERT_EQ(p, overlays);
    }
    const auto publishUs = elapsed(begin) * .1;
    bridge.Clear();
    begin = Clock::now();
    for (size_t i = 0; i < 100000; ++i)
        EXPECT_FALSE(bridge.OverlaysFor(source));
    const auto emptyUs = elapsed(begin) * .01;
    EXPECT_GT(sourceTriangles, 0U);
    EXPECT_LT(masked, unmasked);
    std::printf("GRASS_OVERLAY_OFFLINE source_triangles=%zu overlay_triangles=%zu coplanar_source_triangles=%zu "
                "pairs=%zu budget=%u anchors_before=%zu anchors_after=%zu prepare_ms=%.3f extraction_before_ms=%.3f "
                "extraction_after_ms=%.3f warm_lookup_us=%.3f publish_plus_lookup_us=%.3f empty_lookup_us=%.3f\n",
                source.Indices->size() / 3, overlays->Triangles.size(), sourceTriangles, pairs, budget, unmasked,
                masked, prepareMs, unmaskedTimes[2], maskedTimes[2], warmUs, publishUs, emptyUs);
}
} // namespace
