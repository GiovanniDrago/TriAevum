#include "fast/renderer3ds/pica_actor_normal_smoothing.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace Fast::Renderer3ds;

namespace {

int checks = 0;
void expect(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        std::cerr << "FAILED: " << what << "\n";
        std::exit(1);
    }
}

using V3 = std::array<float, 3>;
using M3 = std::array<float, 9>;
const M3 kIdentity{1, 0, 0, 0, 1, 0, 0, 0, 1};

// A rotation about y (column-major).
M3 RotationY(float degrees) {
    const float c = std::cos(degrees * 0.017453292F), s = std::sin(degrees * 0.017453292F);
    return {c, 0, -s, 0, 1, 0, s, 0, c};
}
M3 Times(const M3& a, const M3& b) {
    M3 r{};
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            for (int k = 0; k < 3; ++k) r[col * 3 + row] += a[k * 3 + row] * b[col * 3 + k];
    return r;
}
V3 Apply(const M3& m, const V3& v) {
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
            m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}
V3 Unit(V3 v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return {v[0] / l, v[1] / l, v[2] / l};
}
float Dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 Direction(float degrees) {
    return {std::sin(degrees * 0.017453292F), std::cos(degrees * 0.017453292F), 0.0F};
}

// A piece with one vertex per (position, source normal); posed by the
// matrix (the same deformation for all its vertices), positions moved by it.
PicaNormalSmoothingPiece Piece(uint64_t geometry, std::vector<std::pair<V3, V3>> vertices, const M3& pose,
                               V3 offset = {}) {
    PicaNormalSmoothingPiece piece;
    piece.Geometry = geometry;
    for (uint32_t i = 0; i < vertices.size(); ++i) {
        PicaNormalSmoothingVertex v;
        const auto p = Apply(pose, vertices[i].first);
        v.Position = {p[0] + offset[0], p[1] + offset[1], p[2] + offset[2]};
        v.NormalMatrix = pose;
        v.Source = {vertices[i].second[0], vertices[i].second[1], vertices[i].second[2], 0.0F};
        piece.Indices.push_back(10U + i);
        piece.Vertices.push_back(v);
    }
    return piece;
}
// The posed normal of a piece's vertex with its override, if any.
V3 Posed(const PicaActorNormalSmoother& smoother, const PicaNormalSmoothingPiece& piece, uint32_t local) {
    V3 source{piece.Vertices[local].Source[0], piece.Vertices[local].Source[1], piece.Vertices[local].Source[2]};
    if (const auto* result = smoother.Find(piece.Geometry))
        for (const auto& [index, normal] : result->Normals)
            if (index == piece.Indices[local]) source = {normal[0], normal[1], normal[2]};
    return Unit(Apply(piece.Vertices[local].NormalMatrix, source));
}

void Seam() {
    // A body and a hand meeting at two points, their normals 30 degrees
    // apart: one surface, so after three poses both sides share a normal.
    PicaActorNormalSmoother smoother;
    const auto build = [](const M3& pose) {
        return std::vector<PicaNormalSmoothingPiece>{
            Piece(1, {{{0, 0, 0}, Direction(-15)}, {{1, 0, 0}, Direction(-15)}, {{5, 5, 5}, Direction(0)}}, pose),
            Piece(2, {{{0, 0, 0}, Direction(15)}, {{1, 0, 0}, Direction(15)}}, pose)};
    };
    auto pieces = build(kIdentity);
    smoother.Observe(7, pieces);
    smoother.Observe(7, pieces);
    expect(smoother.Find(1) == nullptr && smoother.Unsettled(7), "not joined before enough poses");
    smoother.Observe(7, pieces);
    expect(smoother.Find(1) && smoother.Find(2) && !smoother.Unsettled(7), "joined after three");
    const auto a = Posed(smoother, pieces[0], 0), b = Posed(smoother, pieces[1], 0);
    expect(Dot(a, b) > 0.9999F && Dot(a, Direction(0)) > 0.9999F, "one normal, the average");
    expect(smoother.Find(1)->Normals.size() == 2U, "the far vertex untouched");
    const auto report = smoother.Report();
    expect(report.Joined == 2U && report.Smoothed == 4U && report.Pieces == 2U, "report");
}

void PosedSpace() {
    // The pieces posed by a rotation and a scale: the override, solved in each
    // piece's own source space, stays shared when both move together.
    PicaActorNormalSmoother smoother;
    const M3 scale{2, 0, 0, 0, 1, 0, 0, 0, 0.5F};
    const auto build = [&](float turn) {
        const M3 pose = Times(RotationY(turn), scale);
        return std::vector<PicaNormalSmoothingPiece>{
            Piece(1, {{{0, 0, 0}, Direction(-10)}}, pose),
            Piece(2, {{{0, 0, 0}, Direction(12)}}, pose)};
    };
    for (float turn : {10.0F, 40.0F, 70.0F}) smoother.Observe(3, build(turn));
    const auto later = build(130.0F);
    expect(Dot(Posed(smoother, later[0], 0), Posed(smoother, later[1], 0)) > 0.9999F,
           "shared in a later pose");
}

void Crease() {
    // Partners 90 degrees apart (an authored edge) keep their normals.
    PicaActorNormalSmoother smoother;
    const auto pieces = std::vector<PicaNormalSmoothingPiece>{
        Piece(1, {{{0, 0, 0}, {0, 1, 0}}}, kIdentity), Piece(2, {{{0, 0, 0}, {1, 0, 0}}}, kIdentity)};
    for (int i = 0; i < 3; ++i) smoother.Observe(1, pieces);
    expect(smoother.Find(1) == nullptr && smoother.Find(2) == nullptr, "the edge kept");
    expect(smoother.Report().Creases == 1U, "reported as a crease");
    // A wider crease angle joins them; one near the angle is ambiguous.
    PicaNormalSmoothingOptions wide;
    wide.CreaseDegrees = 95.0F;
    smoother.SetOptions(wide);
    for (int i = 0; i < 3; ++i) smoother.Observe(1, pieces);
    expect(smoother.Find(1) != nullptr, "joined within a wider angle");
    expect(smoother.Report().Ambiguous == 1U, "90 degrees is within 8 of 95: ambiguous");
}

void Joint() {
    // A hand meeting its wrist: one point in every pose, moved by other
    // bones: joined, and its shared normal solved again for each pose.
    PicaActorNormalSmoother smoother;
    const auto build = [](float bend) {
        return std::vector<PicaNormalSmoothingPiece>{Piece(1, {{{0, 0, 0}, Direction(-10)}}, kIdentity),
                                                     Piece(2, {{{0, 0, 0}, Direction(10)}}, RotationY(bend))};
    };
    for (float bend : {20.0F, 30.0F, 40.0F}) smoother.Observe(1, build(bend));
    expect(smoother.Report().Joints == 1U && smoother.JointVertices(1) && smoother.JointVertices(2),
           "a joint is joined and posed every frame");
    const auto later = build(70.0F);
    smoother.PoseVertex(1, 10U, later[0].Vertices[0]);
    smoother.PoseVertex(2, 10U, later[1].Vertices[0]);
    smoother.SolveJoints();
    expect(Dot(Posed(smoother, later[0], 0), Posed(smoother, later[1], 0)) > 0.9999F, "shared in the new pose");
    expect(smoother.Report().Following == 2U, "both sides follow the pose");
}

void Parted() {
    // Together in one pose, apart in the next: two points, never joined.
    PicaActorNormalSmoother smoother;
    const auto together = std::vector<PicaNormalSmoothingPiece>{
        Piece(1, {{{0, 0, 0}, Direction(-10)}}, kIdentity), Piece(2, {{{0, 0, 0}, Direction(10)}}, kIdentity)};
    const auto apart = std::vector<PicaNormalSmoothingPiece>{
        Piece(1, {{{0, 0, 0}, Direction(-10)}}, kIdentity),
        Piece(2, {{{0, 0, 0}, Direction(10)}}, kIdentity, {0, 3, 0})};
    smoother.Observe(1, together);
    smoother.Observe(1, apart);
    for (int i = 0; i < 4; ++i) smoother.Observe(1, together);
    expect(smoother.Find(1) == nullptr && smoother.Report().Parted == 1U, "parted pairs stay apart");
}

void Controls() {
    const auto pieces = std::vector<PicaNormalSmoothingPiece>{
        Piece(1, {{{0, 0, 0}, Direction(-20)}}, kIdentity), Piece(2, {{{0, 0, 0}, Direction(20)}}, kIdentity)};
    {
        // Excluded: neither smoothed nor a partner.
        PicaActorNormalSmoother smoother;
        auto excluded = pieces;
        excluded[1].Excluded = true;
        for (int i = 0; i < 3; ++i) smoother.Observe(1, excluded);
        expect(smoother.Find(1) == nullptr && smoother.Find(2) == nullptr, "an excluded piece");
    }
    {
        // Half strength: half way.
        PicaActorNormalSmoother smoother;
        PicaNormalSmoothingOptions half;
        half.Strength = 0.5F;
        smoother.SetOptions(half);
        for (int i = 0; i < 3; ++i) smoother.Observe(1, pieces);
        const float angle = std::acos(std::clamp(Dot(Posed(smoother, pieces[0], 0), {0, 1, 0}), -1.0F, 1.0F)) * 57.29578F;
        expect(angle > 9.0F && angle < 11.0F, "half strength: half the angle");
    }
    {
        // A mesh drawn twice in one frame (both hands): left alone.
        PicaActorNormalSmoother smoother;
        auto twice = pieces;
        twice.push_back(Piece(2, {{{0, 0, 0}, Direction(20)}}, kIdentity, {0, 9, 0}));
        for (int i = 0; i < 3; ++i) smoother.Observe(1, twice);
        expect(smoother.Find(1) == nullptr && smoother.Report().Shared == 1U, "shared geometry left alone");
    }
    {
        // Within a piece: its own split vertices, unless turned off.
        const auto split = std::vector<PicaNormalSmoothingPiece>{
            Piece(1, {{{0, 0, 0}, Direction(-20)}, {{0, 0, 0}, Direction(20)}}, kIdentity)};
        PicaActorNormalSmoother within;
        for (int i = 0; i < 3; ++i) within.Observe(1, split);
        expect(within.Find(1) != nullptr, "a piece's own split vertices");
        PicaActorNormalSmoother across;
        PicaNormalSmoothingOptions off;
        off.WithinPieces = false;
        across.SetOptions(off);
        for (int i = 0; i < 3; ++i) across.Observe(1, split);
        expect(across.Find(1) == nullptr, "only across pieces when turned off");
    }
}

} // namespace

int main() {
    Seam();
    PosedSpace();
    Crease();
    Joint();
    Parted();
    Controls();
    std::cout << "pica_actor_normal_smoothing_tests: " << checks << " checks passed\n";
    return 0;
}
