#include "fast/renderer3ds/pica_actor_normal_smoothing.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

namespace Fast::Renderer3ds {
namespace {

using V3 = std::array<float, 3>;
using M3 = std::array<float, 9>;

V3 Apply(const M3& m, const V3& v) {
    return {m[0] * v[0] + m[3] * v[1] + m[6] * v[2], m[1] * v[0] + m[4] * v[1] + m[7] * v[2],
            m[2] * v[0] + m[5] * v[1] + m[8] * v[2]};
}
float Dot(const V3& a, const V3& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
bool Normalize(V3& v) {
    const float length = std::sqrt(Dot(v, v));
    if (!(length > 1.0e-20F)) return false;
    for (auto& c : v) c /= length;
    return true;
}
// Solves m x = v (column-major 3x3); false when singular.
bool SolveLinear(const M3& m, const V3& v, V3& x) {
    const V3 c0{m[0], m[1], m[2]}, c1{m[3], m[4], m[5]}, c2{m[6], m[7], m[8]};
    const auto cross = [](const V3& a, const V3& b) {
        return V3{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const V3 r0 = cross(c1, c2), r1 = cross(c2, c0), r2 = cross(c0, c1);
    const float determinant = Dot(c0, r0);
    if (!(std::abs(determinant) > 1.0e-20F)) return false;
    x = {Dot(r0, v) / determinant, Dot(r1, v) / determinant, Dot(r2, v) / determinant};
    return std::all_of(x.begin(), x.end(), [](float value) { return std::isfinite(value); });
}
// The same deformation: equal normal matrices (relative to their size).
bool SameDeformation(const M3& a, const M3& b) {
    float difference = 0.0F, size = 0.0F;
    for (size_t i = 0; i < 9; ++i) {
        difference += (a[i] - b[i]) * (a[i] - b[i]);
        size = std::max({size, a[i] * a[i], b[i] * b[i]});
    }
    return difference <= 1.0e-6F * std::max(size, 1.0e-12F);
}
uint64_t Mix(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    return hash;
}
// Geometry drawn more than once in one observation (a mesh used twice, as
// both hands): one set of source normals cannot serve both, so neither.
std::unordered_set<uint64_t> SharedGeometries(std::span<const PicaNormalSmoothingPiece> pieces) {
    std::unordered_set<uint64_t> seen, shared;
    for (const auto& piece : pieces)
        if (!seen.insert(piece.Geometry).second) shared.insert(piece.Geometry);
    return shared;
}

} // namespace

namespace {
std::mutex gReportMutex;
PicaNormalSmoothingReport gReport;
} // namespace

void PublishPicaNormalSmoothingReport(const PicaNormalSmoothingReport& report) {
    std::scoped_lock lock(gReportMutex);
    gReport = report;
}

PicaNormalSmoothingReport LatestPicaNormalSmoothingReport() {
    std::scoped_lock lock(gReportMutex);
    return gReport;
}

size_t PicaActorNormalSmoother::VertexHash::operator()(const VertexKey& key) const noexcept {
    return static_cast<size_t>(Mix(key.Geometry, key.Index));
}
size_t PicaActorNormalSmoother::PairHash::operator()(const PairKey& key) const noexcept {
    return static_cast<size_t>(Mix(Mix(Mix(key.A.Geometry, key.A.Index), key.B.Geometry), key.B.Index));
}

void PicaActorNormalSmoother::SetOptions(const PicaNormalSmoothingOptions& options) {
    if (options == mOptions) return;
    mOptions = options;
    Clear();
}

void PicaActorNormalSmoother::Clear() {
    mActors.clear();
    mFixed.clear();
    mFollowing.clear();
    mJointVertices.clear();
    mPosed.clear();
    mResults.clear();
}

bool PicaActorNormalSmoother::Unsettled(uint64_t actor) const {
    const auto found = mActors.find(actor);
    return found == mActors.end() || found->second.Unsettled;
}

const PicaNormalSmoothingResult* PicaActorNormalSmoother::Find(uint64_t geometry) const {
    const auto found = mResults.find(geometry);
    return found == mResults.end() ? nullptr : &found->second;
}

const std::vector<uint32_t>* PicaActorNormalSmoother::JointVertices(uint64_t geometry) const {
    const auto found = mJointVertices.find(geometry);
    return found == mJointVertices.end() ? nullptr : &found->second;
}

void PicaActorNormalSmoother::PoseVertex(uint64_t geometry, uint32_t index, const PicaNormalSmoothingVertex& vertex) {
    // A per-frame pose keeps the material its observation gave.
    auto& slot = mPosed[{geometry, index}];
    const uint64_t material = slot.Material;
    slot = vertex;
    if (slot.Material == 0U) slot.Material = material;
}

PicaNormalSmoothingReport PicaActorNormalSmoother::Report() const {
    PicaNormalSmoothingReport total;
    std::unordered_set<uint64_t> pieces;
    for (const auto& [actor, state] : mActors) {
        ++total.Actors;
        pieces.insert(state.Geometries.begin(), state.Geometries.end());
        const auto& r = state.Report;
        total.Joined += r.Joined;
        total.Pending += r.Pending;
        total.Parted += r.Parted;
        total.Joints += r.Joints;
        total.Creases += r.Creases;
        total.Ambiguous += r.Ambiguous;
        total.Smoothed += r.Smoothed;
        total.Shared += r.Shared;
    }
    for (const auto& [geometry, normals] : mFollowing) total.Following += static_cast<uint32_t>(normals.size());
    total.Pieces = static_cast<uint32_t>(pieces.size());
    return total;
}

void PicaActorNormalSmoother::Observe(uint64_t actor, std::span<const PicaNormalSmoothingPiece> pieces) {
    auto& state = mActors[actor];
    const float weld = std::max(mOptions.WeldDistance, 1.0e-6F);
    // Every vertex in a grid of weld-sized cells.
    struct Item {
        uint32_t Piece, Local;
    };
    std::vector<Item> items;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
    const auto cellOf = [weld](const V3& p) {
        return std::array<int64_t, 3>{int64_t(std::floor(p[0] / weld)), int64_t(std::floor(p[1] / weld)),
                                      int64_t(std::floor(p[2] / weld))};
    };
    const auto cellKey = [](const std::array<int64_t, 3>& c) {
        return Mix(Mix(uint64_t(c[0]), uint64_t(c[1])), uint64_t(c[2]));
    };
    std::unordered_set<uint64_t> present;
    const auto shared = SharedGeometries(pieces);
    for (uint32_t p = 0; p < pieces.size(); ++p) {
        present.insert(pieces[p].Geometry);
        state.Geometries.insert(pieces[p].Geometry);
        if (pieces[p].Excluded || shared.contains(pieces[p].Geometry)) continue;
        for (uint32_t i = 0; i < pieces[p].Vertices.size() && i < pieces[p].Indices.size(); ++i) {
            cells[cellKey(cellOf(pieces[p].Vertices[i].Position))].push_back(uint32_t(items.size()));
            items.push_back({p, i});
        }
    }
    // Coincident pairs this time (and whether the same bones move them).
    std::unordered_set<PairKey, PairHash> seen;
    for (uint32_t a = 0; a < items.size(); ++a) {
        const auto& pa = pieces[items[a].Piece];
        const auto& va = pa.Vertices[items[a].Local];
        const auto c = cellOf(va.Position);
        for (int64_t dz = -1; dz <= 1; ++dz)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dx = -1; dx <= 1; ++dx) {
                    const auto found = cells.find(cellKey({c[0] + dx, c[1] + dy, c[2] + dz}));
                    if (found == cells.end()) continue;
                    for (const uint32_t b : found->second) {
                        if (b <= a) continue;
                        const auto& pb = pieces[items[b].Piece];
                        const auto& vb = pb.Vertices[items[b].Local];
                        if (items[a].Piece == items[b].Piece && !mOptions.WithinPieces) continue;
                        float distance = 0.0F;
                        for (size_t k = 0; k < 3; ++k)
                            distance += (va.Position[k] - vb.Position[k]) * (va.Position[k] - vb.Position[k]);
                        if (distance > weld * weld) continue;
                        VertexKey ka{pa.Geometry, pa.Indices[items[a].Local]};
                        VertexKey kb{pb.Geometry, pb.Indices[items[b].Local]};
                        if (ka == kb) continue;
                        if (kb.Geometry < ka.Geometry || (kb.Geometry == ka.Geometry && kb.Index < ka.Index))
                            std::swap(ka, kb);
                        const PairKey key{ka, kb};
                        if (!seen.insert(key).second) continue;
                        auto& evidence = state.Pairs[key];
                        ++evidence.Together;
                        if (!SameDeformation(va.NormalMatrix, vb.NormalMatrix)) evidence.Joint = true;
                    }
                }
    }
    // Known pairs drawn this time but apart: two points, never joined.
    for (auto& [key, evidence] : state.Pairs)
        if (!evidence.Parted && present.contains(key.A.Geometry) && present.contains(key.B.Geometry) &&
            !seen.contains(key))
            evidence.Parted = true;
    state.Unsettled = std::any_of(state.Pairs.begin(), state.Pairs.end(), [this](const auto& entry) {
        return !entry.second.Parted && entry.second.Together < mOptions.RequiredObservations;
    });
    Solve(state, pieces);
}

void PicaActorNormalSmoother::SmoothCluster(std::span<const VertexKey> keys,
                                            std::span<const PicaNormalSmoothingVertex* const> vertices,
                                            std::unordered_map<uint64_t, Normals>& out,
                                            PicaNormalSmoothingReport& report) const {
    const float crease = std::cos(mOptions.CreaseDegrees * 0.017453292F);
    const float materialCrease = std::cos(std::min(mOptions.MaterialCreaseDegrees, mOptions.CreaseDegrees) * 0.017453292F);
    const float strength = std::clamp(mOptions.Strength, 0.0F, 1.0F);
    std::vector<V3> posed(keys.size());
    std::vector<bool> valid(keys.size());
    for (size_t m = 0; m < keys.size(); ++m) {
        const auto& v = *vertices[m];
        posed[m] = Apply(v.NormalMatrix, {v.Source[0], v.Source[1], v.Source[2]});
        valid[m] = Normalize(posed[m]);
    }
    for (size_t m = 0; m < keys.size(); ++m) {
        if (!valid[m]) continue;
        // Its partners within the crease angle (itself included).
        V3 sum{};
        for (size_t o = 0; o < keys.size(); ++o) {
            if (!valid[o]) continue;
            const float cosine = std::clamp(Dot(posed[m], posed[o]), -1.0F, 1.0F);
            const bool sameMaterial = vertices[m]->Material == vertices[o]->Material;
            const float limit = sameMaterial ? crease : materialCrease;
            if (o > m) {
                const float angle = std::acos(cosine) * 57.29578F;
                const float degrees = sameMaterial ? mOptions.CreaseDegrees
                                                   : std::min(mOptions.MaterialCreaseDegrees, mOptions.CreaseDegrees);
                if (cosine < limit) ++report.Creases;
                if (std::abs(angle - degrees) <= mOptions.AmbiguityDegrees) ++report.Ambiguous;
            }
            if (cosine >= limit)
                for (size_t k = 0; k < 3; ++k) sum[k] += posed[o][k];
        }
        if (!Normalize(sum)) continue;
        V3 target{};
        for (size_t k = 0; k < 3; ++k) target[k] = posed[m][k] + (sum[k] - posed[m][k]) * strength;
        if (!Normalize(target) || Dot(target, posed[m]) > 0.999999F) continue;
        // Back to the vertex's source normal (its own deformation).
        const auto& v = *vertices[m];
        V3 source{};
        if (!SolveLinear(v.NormalMatrix, target, source)) continue;
        const float length =
            std::sqrt(v.Source[0] * v.Source[0] + v.Source[1] * v.Source[1] + v.Source[2] * v.Source[2]);
        const float scale = length / std::max(std::sqrt(Dot(source, source)), 1.0e-20F);
        out[keys[m].Geometry].push_back(
            {keys[m].Index, {source[0] * scale, source[1] * scale, source[2] * scale, v.Source[3]}});
        ++report.Smoothed;
    }
}

void PicaActorNormalSmoother::Compose(uint64_t geometry) {
    Normals merged;
    if (const auto fixed = mFixed.find(geometry); fixed != mFixed.end()) merged = fixed->second;
    if (const auto following = mFollowing.find(geometry); following != mFollowing.end())
        for (const auto& entry : following->second) {
            const auto at = std::find_if(merged.begin(), merged.end(),
                                         [&](const auto& m) { return m.first == entry.first; });
            if (at != merged.end()) at->second = entry.second;
            else merged.push_back(entry);
        }
    if (merged.empty()) {
        mResults.erase(geometry);
        return;
    }
    std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    auto& result = mResults[geometry];
    const bool same = result.Normals.size() == merged.size() &&
        std::equal(merged.begin(), merged.end(), result.Normals.begin(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return false;
            for (size_t k = 0; k < 4; ++k)
                if (std::abs(a.second[k] - b.second[k]) > 1.0e-4F) return false;
            return true;
        });
    if (!same) {
        result.Normals = std::move(merged);
        result.Version = ++mVersion;
    }
}

void PicaActorNormalSmoother::Solve(ActorState& state, std::span<const PicaNormalSmoothingPiece> pieces) {
    auto& report = state.Report;
    report = {};
    std::unordered_map<VertexKey, const PicaNormalSmoothingVertex*, VertexHash> where;
    const auto shared = SharedGeometries(pieces);
    report.Shared = static_cast<uint32_t>(shared.size());
    for (const auto& piece : pieces) {
        if (piece.Excluded || shared.contains(piece.Geometry)) continue;
        for (uint32_t i = 0; i < piece.Vertices.size() && i < piece.Indices.size(); ++i)
            where[{piece.Geometry, piece.Indices[i]}] = &piece.Vertices[i];
    }
    std::vector<VertexKey> nodes;
    std::unordered_map<VertexKey, uint32_t, VertexHash> node;
    std::vector<uint32_t> parent;
    std::vector<bool> joint;
    const auto id = [&](const VertexKey& key) {
        const auto [entry, inserted] = node.try_emplace(key, uint32_t(nodes.size()));
        if (inserted) {
            nodes.push_back(key);
            parent.push_back(entry->second);
            joint.push_back(false);
        }
        return entry->second;
    };
    const auto root = [&](uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    std::vector<std::pair<uint32_t, uint32_t>> jointPairs;
    for (const auto& [key, evidence] : state.Pairs) {
        if (evidence.Parted) {
            ++report.Parted;
            continue;
        }
        if (evidence.Together < mOptions.RequiredObservations) {
            ++report.Pending;
            continue;
        }
        ++report.Joined;
        report.Joints += evidence.Joint ? 1U : 0U;
        if (!where.contains(key.A) || !where.contains(key.B)) continue;
        const uint32_t a = id(key.A), b = id(key.B);
        if (evidence.Joint) jointPairs.push_back({a, b});
        parent[root(a)] = root(b);
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> clusters;
    for (uint32_t n = 0; n < nodes.size(); ++n) clusters[root(n)].push_back(n);
    std::unordered_set<uint32_t> jointRoots;
    for (const auto& [a, b] : jointPairs) jointRoots.insert(root(a));

    std::unordered_map<uint64_t, Normals> fixed;
    state.JointClusters.clear();
    for (const auto& [rootId, members] : clusters) {
        if (members.size() < 2U) continue;
        std::vector<VertexKey> keys;
        std::vector<const PicaNormalSmoothingVertex*> vertices;
        for (const uint32_t m : members) {
            keys.push_back(nodes[m]);
            vertices.push_back(where.at(nodes[m]));
        }
        if (jointRoots.contains(rootId)) {
            // Across a joint: solved every frame from the current pose.
            for (size_t m = 0; m < keys.size(); ++m) mPosed[keys[m]] = *vertices[m];
            state.JointClusters.push_back(std::move(keys));
            continue;
        }
        SmoothCluster(keys, vertices, fixed, report);
    }
    for (const auto& piece : pieces) {
        if (auto found = fixed.find(piece.Geometry); found != fixed.end())
            mFixed[piece.Geometry] = std::move(found->second);
        else
            mFixed.erase(piece.Geometry);
        Compose(piece.Geometry);
    }
    // The joint vertices every piece poses each frame.
    mJointVertices.clear();
    for (const auto& [actor, other] : mActors)
        for (const auto& cluster : other.JointClusters)
            for (const auto& key : cluster) mJointVertices[key.Geometry].push_back(key.Index);
    for (auto& [geometry, indices] : mJointVertices) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    }
}

void PicaActorNormalSmoother::SolveJoints() {
    std::unordered_map<uint64_t, Normals> following;
    PicaNormalSmoothingReport ignored;
    for (const auto& [actor, state] : mActors)
        for (const auto& cluster : state.JointClusters) {
            std::vector<const PicaNormalSmoothingVertex*> vertices;
            for (const auto& key : cluster) {
                const auto found = mPosed.find(key);
                if (found == mPosed.end()) break;
                vertices.push_back(&found->second);
            }
            if (vertices.size() == cluster.size()) SmoothCluster(cluster, vertices, following, ignored);
        }
    std::unordered_set<uint64_t> touched;
    for (const auto& [geometry, normals] : mFollowing) touched.insert(geometry);
    for (const auto& [geometry, normals] : following) touched.insert(geometry);
    mFollowing = std::move(following);
    for (const uint64_t geometry : touched) Compose(geometry);
}

} // namespace Fast::Renderer3ds
