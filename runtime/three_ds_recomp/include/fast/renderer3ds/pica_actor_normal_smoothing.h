#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fast::Renderer3ds {

// Shading normals made continuous across the pieces of one articulated model
// (its separate draws: a body, hands, eyes, each with its own vertices where
// they meet), edge-aware: vertices are joined only where they are one point
// of the surface in every pose seen (the same place and the same deformation,
// so they stay together when animated), and each vertex's normal averages only
// its partners within the crease angle, so authored edges and elements that
// meet at an angle keep their discontinuity. Meshes are not merged: each
// piece keeps its vertices; only the source normals it submits change.

struct PicaNormalSmoothingOptions {
    // Partners whose posed normals differ by more keep their edge.
    float CreaseDegrees = 60.0F;
    // The same between pieces of different materials (a belt on a tunic, a
    // strap: distinct elements meeting at an edge): a tighter angle.
    float MaterialCreaseDegrees = 20.0F;
    // Vertices this close (posed, view units) are one point.
    float WeldDistance = 0.05F;
    // 0: the original normals; 1: fully smoothed.
    float Strength = 1.0F;
    // Observations (poses) a pair must hold together in before it is joined.
    uint32_t RequiredObservations = 3U;
    // Also a piece's own split vertices (texture seams).
    bool WithinPieces = true;
    // Partners within this of the crease angle are reported as ambiguous.
    float AmbiguityDegrees = 8.0F;
    bool operator==(const PicaNormalSmoothingOptions&) const = default;
};

struct PicaNormalSmoothingVertex {
    // Posed position (view space).
    std::array<float, 3> Position{};
    // Posed normal = NormalMatrix * source normal (column-major 3x3, the
    // shader's blend of its bones' inverse transposes).
    std::array<float, 9> NormalMatrix{};
    // The source normal input as submitted (xyz; w kept).
    std::array<float, 4> Source{};
    // The piece's material (its texture): partners of another material
    // join only within MaterialCreaseDegrees.
    uint64_t Material = 0;
};

// One draw of a model: its distinct vertices (by vertex buffer index).
struct PicaNormalSmoothingPiece {
    uint64_t Geometry = 0;
    // Never smoothed nor a partner (a user exclusion).
    bool Excluded = false;
    std::vector<uint32_t> Indices;
    std::vector<PicaNormalSmoothingVertex> Vertices;
    // Its triangles (vertex buffer indices; a strip or a list), for tools.
    std::vector<uint32_t> Corners;
    bool Strip = false;
};

// A piece's smoothed source normals, by vertex buffer index. The version
// changes with them (0: none).
struct PicaNormalSmoothingResult {
    uint64_t Version = 0;
    std::vector<std::pair<uint32_t, std::array<float, 4>>> Normals;
};

struct PicaNormalSmoothingReport {
    uint32_t Actors = 0;
    uint32_t Pieces = 0;
    // Coincident vertex pairs: joined (held together in enough poses),
    // waiting for more poses, parted (apart in some pose: two points).
    uint32_t Joined = 0;
    uint32_t Pending = 0;
    uint32_t Parted = 0;
    // Joined pairs across a joint (each side moved by other bones, as a hand
    // meeting its wrist): their shared normal follows the pose every frame.
    uint32_t Joints = 0;
    // Joined partners beyond the crease angle (edges kept), and those within
    // AmbiguityDegrees of it (an edge or a seam: tune the angle or exclude).
    uint32_t Creases = 0;
    uint32_t Ambiguous = 0;
    // Vertices whose normal changed (fixed, and per frame at joints).
    uint32_t Smoothed = 0;
    uint32_t Following = 0;
    // Geometry a model drew more than once (left alone).
    uint32_t Shared = 0;
};

// The renderer's latest report, for settings panels.
void PublishPicaNormalSmoothingReport(const PicaNormalSmoothingReport& report);
[[nodiscard]] PicaNormalSmoothingReport LatestPicaNormalSmoothingReport();

class PicaActorNormalSmoother {
  public:
    // A change clears what was learned.
    void SetOptions(const PicaNormalSmoothingOptions& options);
    [[nodiscard]] const PicaNormalSmoothingOptions& Options() const noexcept { return mOptions; }
    // One observation of a model: the pieces it drew in one frame.
    void Observe(uint64_t actor, std::span<const PicaNormalSmoothingPiece> pieces);
    // Whether a model still needs observations (some pair undecided, or not
    // yet seen); settled models are observed again only now and then.
    [[nodiscard]] bool Unsettled(uint64_t actor) const;
    // The vertices of a piece at joints: posed every frame (PoseVertex), and
    // their normals solved again (SolveJoints) at the next frame's start.
    [[nodiscard]] const std::vector<uint32_t>* JointVertices(uint64_t geometry) const;
    void PoseVertex(uint64_t geometry, uint32_t index, const PicaNormalSmoothingVertex& vertex);
    void SolveJoints();
    [[nodiscard]] const PicaNormalSmoothingResult* Find(uint64_t geometry) const;
    [[nodiscard]] PicaNormalSmoothingReport Report() const;
    void Clear();

  private:
    struct VertexKey {
        uint64_t Geometry;
        uint32_t Index;
        bool operator==(const VertexKey&) const = default;
    };
    struct PairKey {
        VertexKey A, B;
        bool operator==(const PairKey&) const = default;
    };
    struct VertexHash {
        size_t operator()(const VertexKey& key) const noexcept;
    };
    struct PairHash {
        size_t operator()(const PairKey& key) const noexcept;
    };
    struct Evidence {
        uint32_t Together = 0;
        bool Parted = false;
        bool Joint = false;
    };
    struct ActorState {
        std::unordered_map<PairKey, Evidence, PairHash> Pairs;
        std::unordered_set<uint64_t> Geometries;
        // Clusters across joints, solved every frame.
        std::vector<std::vector<VertexKey>> JointClusters;
        PicaNormalSmoothingReport Report;
        bool Unsettled = true;
    };
    using Normals = std::vector<std::pair<uint32_t, std::array<float, 4>>>;
    void Solve(ActorState& state, std::span<const PicaNormalSmoothingPiece> pieces);
    // The smoothed source normals of a cluster's members (posed vertices).
    void SmoothCluster(std::span<const VertexKey> keys, std::span<const PicaNormalSmoothingVertex* const> vertices,
                       std::unordered_map<uint64_t, Normals>& out, PicaNormalSmoothingReport& report) const;
    void Compose(uint64_t geometry);

    PicaNormalSmoothingOptions mOptions;
    std::unordered_map<uint64_t, ActorState> mActors;
    // Fixed (same deformation) and per-frame (joints) normals by geometry,
    // composed into the results.
    std::unordered_map<uint64_t, Normals> mFixed;
    std::unordered_map<uint64_t, Normals> mFollowing;
    std::unordered_map<uint64_t, std::vector<uint32_t>> mJointVertices;
    std::unordered_map<VertexKey, PicaNormalSmoothingVertex, VertexHash> mPosed;
    std::unordered_map<uint64_t, PicaNormalSmoothingResult> mResults;
    uint64_t mVersion = 0;
};

} // namespace Fast::Renderer3ds
