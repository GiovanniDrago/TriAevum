#include "fast/renderer/diffuse_gi_gather.h"

// Gathering and sector masks of the diffuse screen-space GI (milestone M2).
// Every rule here is taken from the specification (sections 1, 4, 9, 10) or
// derived in docs/TRIAEVUM_SCREEN_SPACE_GI.md. The template core runs in
// double (reference) and in float (twin of the GPU kernel, same operation
// order); see diffuse_gi_gather_shader.cpp for the GLSL counterpart.

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <thread>
#include <type_traits>

namespace Fast::Renderer::DiffuseGi {
namespace {

struct Vec3F {
    float X = 0.0F, Y = 0.0F, Z = 0.0F;
};
template <typename T>
using VecT = std::conditional_t<std::is_same_v<T, double>, Vec3, Vec3F>;

template <typename V>
V VAdd(const V& a, const V& b) {
    return {a.X + b.X, a.Y + b.Y, a.Z + b.Z};
}
template <typename V>
V VSub(const V& a, const V& b) {
    return {a.X - b.X, a.Y - b.Y, a.Z - b.Z};
}
template <typename V, typename T>
V VScale(const V& a, T s) {
    return {a.X * s, a.Y * s, a.Z * s};
}
template <typename V>
auto VDot(const V& a, const V& b) {
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}
template <typename V>
V VCross(const V& a, const V& b) {
    return {a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}
template <typename V>
auto VLength(const V& a) {
    return std::sqrt(VDot(a, a));
}
template <typename V>
V VNormalize(const V& a) {
    const auto length = VLength(a);
    using T = std::remove_const_t<decltype(length)>;
    return length > T(0) ? VScale(a, T(1) / length) : V{};
}

template <typename T>
constexpr T Pi() {
    return static_cast<T>(kPi);
}

bool IsGeometry(uint8_t c) {
    return c == static_cast<uint8_t>(TexelClass::OneSided) || c == static_cast<uint8_t>(TexelClass::TwoSided);
}

template <typename T>
T DecodeDepthT(T d, DepthEncoding encoding, T n, T f, bool infiniteFar) {
    switch (encoding) {
    case DepthEncoding::PerspectiveZeroToOne:
        return n * f / (f - d * (f - n));
    case DepthEncoding::ReversedPerspectiveZeroToOne:
        return infiniteFar ? n / d : n * f / (n + d * (f - n));
    case DepthEncoding::LinearWBuffer:
        return n + d * (f - n);
    case DepthEncoding::ReversedLinearWBuffer:
        return f - d * (f - n);
    default:
        return std::numeric_limits<T>::quiet_NaN();
    }
}

// Read-only view of the prepared geometry of one viewport.
template <typename T>
struct View {
    const GatherInput* In = nullptr;
    uint32_t W = 0, H = 0;
    T Ax{}, Bx{}, Cx{}, Ay{}, By{}, Cy{};
    const T* Z = nullptr;
    const uint8_t* Cls = nullptr;
    const VecT<T>* N = nullptr;

    [[nodiscard]] VecT<T> Ray(T px, T py) const {
        const T u = px / static_cast<T>(W);
        const T v = py / static_cast<T>(H);
        return {Ax * u + Bx * v + Cx, Ay * u + By * v + Cy, T(-1)};
    }
    [[nodiscard]] bool Inside(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && x < static_cast<int32_t>(W) && y < static_cast<int32_t>(H);
    }
    [[nodiscard]] size_t Index(int32_t x, int32_t y) const {
        return static_cast<size_t>(y) * W + static_cast<size_t>(x);
    }
    [[nodiscard]] size_t BufferIndex(int32_t x, int32_t y) const {
        return (static_cast<size_t>(In->Viewport.Y) + static_cast<size_t>(y)) * In->BufferWidth + In->Viewport.X +
               static_cast<size_t>(x);
    }
};

template <typename T>
View<T> MakeView(const GatherInput& in) {
    View<T> view;
    view.In = &in;
    view.W = in.Viewport.Width;
    view.H = in.Viewport.Height;
    view.Ax = static_cast<T>(in.Camera.ViewX[0]);
    view.Bx = static_cast<T>(in.Camera.ViewX[1]);
    view.Cx = static_cast<T>(in.Camera.ViewX[2]);
    view.Ay = static_cast<T>(in.Camera.ViewY[0]);
    view.By = static_cast<T>(in.Camera.ViewY[1]);
    view.Cy = static_cast<T>(in.Camera.ViewY[2]);
    return view;
}

template <typename T>
struct GeometryData {
    std::vector<T> Z;
    std::vector<uint8_t> Cls;
    std::vector<VecT<T>> N;
};

// Geometric normal from depth (own derivation, see the document). On a plane
// w = 1/z is affine in screen space. A quadrant (sx, sy) around the receiver
// is scored by the planarity errors it can test: the 2x2 term
// |w(d) - w(x) - w(y) + w(p)| (d the diagonal) and the second differences
// |w(p) - 2 w(x) + w(2x)| and |w(p) - 2 w(y) + w(2y)| along its two sides.
// The best quadrant gives both tangents, so a texel on an edge never mixes
// the tangents of two planes: the 2x2 term catches creases oblique to the
// pixel grid, the second differences catch creases along a screen axis whose
// planes share that axis' 1/z gradient. Errors within kNormalTie * w(p) are
// ties (FP32 rounding must not decide between two planar quadrants); a tie
// goes to the quadrant with more verified second differences (a missing
// second neighbour verifies nothing), then to the fixed priority (+x+y, -x+y,
// +x-y, -x-y). A quadrant without its diagonal ranks after every full one and
// adds its depth steps. No quadrant: no normal.
inline constexpr double kNormalTie = 4.0e-6;

template <typename T>
VecT<T> ReconstructNormal(const View<T>& g, int32_t x, int32_t y) {
    const auto geometry = [&](int32_t xx, int32_t yy) { return g.Inside(xx, yy) && IsGeometry(g.Cls[g.Index(xx, yy)]); };
    const auto w = [&](int32_t xx, int32_t yy) { return T(1) / g.Z[g.Index(xx, yy)]; };
    const T w0 = w(x, y);
    const T tie = static_cast<T>(kNormalTie) * w0;
    constexpr int kOrder[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    int best = -1, bestRank = 2, bestVerified = 0;
    T bestError = T(0);
    for (int k = 0; k < 4; ++k) {
        const int sx = kOrder[k][0], sy = kOrder[k][1];
        if (!geometry(x + sx, y) || !geometry(x, y + sy)) continue;
        const T wx = w(x + sx, y);
        const T wy = w(x, y + sy);
        int rank = 1;
        int verified = 0;
        T error = std::abs(wx - w0) + std::abs(wy - w0);
        if (geometry(x + sx, y + sy)) {
            rank = 0;
            error = std::abs(w(x + sx, y + sy) - wx - wy + w0);
        }
        if (geometry(x + 2 * sx, y)) {
            error += std::abs(w0 - T(2) * wx + w(x + 2 * sx, y));
            ++verified;
        }
        if (geometry(x, y + 2 * sy)) {
            error += std::abs(w0 - T(2) * wy + w(x, y + 2 * sy));
            ++verified;
        }
        const bool better = best < 0 || rank < bestRank ||
                            (rank == bestRank && (error < bestError - tie ||
                                                  (!(error > bestError + tie) && verified > bestVerified)));
        if (better) {
            best = k;
            bestRank = rank;
            bestError = error;
            bestVerified = verified;
        }
    }
    if (best < 0) return {};
    const int sx = kOrder[best][0], sy = kOrder[best][1];
    const VecT<T> p0 = VScale(g.Ray(T(x) + T(0.5), T(y) + T(0.5)), g.Z[g.Index(x, y)]);
    const VecT<T> px = VScale(g.Ray(T(x + sx) + T(0.5), T(y) + T(0.5)), g.Z[g.Index(x + sx, y)]);
    const VecT<T> py = VScale(g.Ray(T(x) + T(0.5), T(y + sy) + T(0.5)), g.Z[g.Index(x, y + sy)]);
    const VecT<T> tx = VScale(VSub(px, p0), T(sx));
    const VecT<T> ty = VScale(VSub(py, p0), T(sy));
    VecT<T> n = VNormalize(VCross(tx, ty));
    if (VDot(n, p0) > T(0)) n = VScale(n, T(-1));
    return n;
}

template <typename T>
GeometryData<T> PrepareT(const GatherInput& in, View<T>& view) {
    view = MakeView<T>(in);
    GeometryData<T> data;
    const size_t count = static_cast<size_t>(view.W) * view.H;
    data.Z.assign(count, T(0));
    data.Cls.assign(count, static_cast<uint8_t>(TexelClass::Unavailable));
    data.N.assign(count, VecT<T>{});
    const bool infiniteFar = std::isinf(in.Camera.Far);
    const T n = static_cast<T>(in.Camera.Near);
    const T f = infiniteFar ? T(0) : static_cast<T>(in.Camera.Far);
    const T low = n * T(1.0 - 1.0e-5);
    const T high = infiniteFar ? std::numeric_limits<T>::infinity() : f * T(1.0 + 1.0e-5);
    for (uint32_t y = 0; y < view.H; ++y)
        for (uint32_t x = 0; x < view.W; ++x) {
            const size_t bi = view.BufferIndex(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const size_t i = view.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
            const uint8_t c = in.Class[bi];
            if (IsGeometry(c)) {
                const T z = DecodeDepthT<T>(static_cast<T>(in.Depth[bi]), in.Camera.Encoding, n, f, infiniteFar);
                if (std::isfinite(z) && z >= low && z <= high) {
                    data.Z[i] = z;
                    data.Cls[i] = c;
                }
            } else if (c == static_cast<uint8_t>(TexelClass::Sky)) {
                data.Cls[i] = c;
            }
        }
    view.Z = data.Z.data();
    view.Cls = data.Cls.data();
    for (uint32_t y = 0; y < view.H; ++y)
        for (uint32_t x = 0; x < view.W; ++x) {
            const auto xi = static_cast<int32_t>(x), yi = static_cast<int32_t>(y);
            const size_t i = view.Index(xi, yi);
            if (!IsGeometry(data.Cls[i])) continue;
            if (in.NormalSource == GeometricNormalSource::RendererFaceNormal) {
                const size_t bi = view.BufferIndex(xi, yi);
                VecT<T> face = VNormalize(VecT<T>{static_cast<T>(in.FaceNormals[3 * bi]),
                                                  static_cast<T>(in.FaceNormals[3 * bi + 1]),
                                                  static_cast<T>(in.FaceNormals[3 * bi + 2])});
                const VecT<T> p = VScale(view.Ray(T(x) + T(0.5), T(y) + T(0.5)), data.Z[i]);
                if (VDot(face, p) > T(0)) face = VScale(face, T(-1));
                data.N[i] = face;
            } else {
                data.N[i] = ReconstructNormal(view, xi, yi);
            }
        }
    view.N = data.N.data();
    return data;
}

// ---------------------------------------------------------------------------
// Section geometry.

template <typename T>
struct SectionT {
    bool Valid = false;
    VecT<T> P, V, Tn, N;
    T A{}, B{}, ZP{}, DirX{}, DirY{};
    T Extent[2]{};
};

template <typename T>
SectionT<T> SetupT(const View<T>& g, uint32_t x, uint32_t y, T phi, T radius) {
    SectionT<T> s;
    const size_t index = g.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (!IsGeometry(g.Cls[index])) return s;
    const VecT<T> n = g.N[index];
    if (!(VDot(n, n) > T(0))) return s;
    s.ZP = g.Z[index];
    s.P = VScale(g.Ray(T(x) + T(0.5), T(y) + T(0.5)), s.ZP);
    s.V = VNormalize(VScale(s.P, T(-1)));
    // Deterministic frame completion of M1 (SectionFrame::FromView).
    const VecT<T> helper = std::abs(s.V.X) < T(0.9) ? VecT<T>{T(1), T(0), T(0)} : VecT<T>{T(0), T(1), T(0)};
    const VecT<T> e1 = VNormalize(VCross(helper, s.V));
    const VecT<T> e2 = VCross(s.V, e1);
    s.Tn = VAdd(VScale(e1, std::cos(phi)), VScale(e2, std::sin(phi)));
    s.N = n;
    s.A = VDot(n, s.V);
    s.B = VDot(n, s.Tn);
    // Screen direction of +t: derivative of (X/z, Y/z) along P + mu t, mapped
    // through the inverse of the declared UV -> ray map.
    const T zp = -s.P.Z;
    const T gx = s.Tn.X * zp + s.P.X * s.Tn.Z;
    const T gy = s.Tn.Y * zp + s.P.Y * s.Tn.Z;
    const T det = g.Ax * g.By - g.Bx * g.Ay;
    const T du = (g.By * gx - g.Bx * gy) / det;
    const T dv = (-g.Ay * gx + g.Ax * gy) / det;
    const T pxDir = du * static_cast<T>(g.W);
    const T pyDir = dv * static_cast<T>(g.H);
    const T length = std::sqrt(pxDir * pxDir + pyDir * pyDir);
    if (!(length > T(0)) || !std::isfinite(length)) return s;
    s.DirX = pxDir / length;
    s.DirY = pyDir / length;
    // Projected radius per side: rays of the section plane at angle
    // asin(R / |P|) from the receiver's ray bound the R-ball (exact).
    const T cap = std::sqrt(static_cast<T>(g.W) * static_cast<T>(g.W) + static_cast<T>(g.H) * static_cast<T>(g.H));
    const T distance = VLength(s.P);
    for (int side = 0; side < 2; ++side) {
        T extent = cap;
        if (radius < distance) {
            const T sa = radius / distance;
            const T ca = std::sqrt(std::max(T(1) - sa * sa, T(0)));
            const T sign = side == 0 ? T(1) : T(-1);
            const VecT<T> r = VAdd(VScale(s.V, -ca), VScale(s.Tn, sign * sa));
            if (r.Z < T(0)) {
                const T tx = r.X / -r.Z - s.P.X / zp;
                const T ty = r.Y / -r.Z - s.P.Y / zp;
                const T eu = (g.By * tx - g.Bx * ty) / det * static_cast<T>(g.W);
                const T ev = (-g.Ay * tx + g.Ax * ty) / det * static_cast<T>(g.H);
                extent = std::min(std::sqrt(eu * eu + ev * ev), cap);
            }
        }
        s.Extent[side] = extent;
    }
    s.Valid = true;
    return s;
}

// ---------------------------------------------------------------------------
// Candidate schedule.

template <typename T>
struct Position {
    T Lambda{};
    int Side = 1;
    int32_t CellX = 0, CellY = 0;
};

template <typename T>
void EnumerateSide(const View<T>& g, const SectionT<T>& s, int side, const GatherSettings& settings,
                   std::vector<Position<T>>& out, GatherCounters& counters) {
    out.clear();
    const T dx = s.DirX * T(side);
    const T dy = s.DirY * T(side);
    const T adx = std::abs(dx);
    const T ady = std::abs(dy);
    const T infinity = std::numeric_limits<T>::infinity();
    const T invX = adx > T(0) ? T(1) / adx : infinity;
    const T invY = ady > T(0) ? T(1) / ady : infinity;
    const int32_t sx = dx > T(0) ? 1 : (dx < T(0) ? -1 : 0);
    const int32_t sy = dy > T(0) ? 1 : (dy < T(0) ? -1 : 0);
    const T extent = s.Extent[side > 0 ? 0 : 1];
    if (settings.Schedule == CandidateSchedule::EveryTexel) {
        // Grid traversal from the receiver centre; boundary parameters are
        // (k + 1/2) / |d| exactly (no accumulated increments).
        int32_t cx = 0, cy = 0;
        uint32_t kx = 0, ky = 0;
        T enter = T(0);
        const T epsilon = static_cast<T>(settings.ChordEpsilon);
        const uint32_t limit = 2U * (g.W + g.H) + 16U;
        for (uint32_t iteration = 0; iteration < limit; ++iteration) {
            const T tx = adx > T(0) ? (T(kx) + T(0.5)) * invX : infinity;
            const T ty = ady > T(0) ? (T(ky) + T(0.5)) * invY : infinity;
            const T exit = std::min(tx, ty);
            if (cx != 0 || cy != 0) {
                if (!(enter < extent)) break;
                if (exit - enter > epsilon) {
                    // Line point nearest to the texel centre, within the chord.
                    const T foot = std::clamp(T(cx) * dx + T(cy) * dy, enter, exit);
                    out.push_back({foot, side, cx, cy});
                }
            }
            if (tx < ty) {
                cx += sx;
                enter = tx;
                ++kx;
            } else if (ty < tx) {
                cy += sy;
                enter = ty;
                ++ky;
            } else {
                // Exact corner: both corner-touching texels have empty chords.
                cx += sx;
                cy += sy;
                enter = tx;
                ++kx;
                ++ky;
            }
        }
        return;
    }
    const T lambda0 = std::min(T(0.5) * invX, T(0.5) * invY);
    if (!(extent > lambda0)) return;
    const uint32_t count = std::max(settings.CandidatesPerSide, 1U);
    int32_t previousX = 0, previousY = 0;
    for (uint32_t k = 0; k < count; ++k) {
        const T u = (T(k) + static_cast<T>(settings.RadialJitter)) / T(count);
        const T shaped = u > T(0) ? std::pow(u, static_cast<T>(settings.RadialExponent)) : T(0);
        const T lambda = lambda0 + (extent - lambda0) * shaped;
        const auto cx = static_cast<int32_t>(std::floor(lambda * dx + T(0.5)));
        const auto cy = static_cast<int32_t>(std::floor(lambda * dy + T(0.5)));
        if ((cx == 0 && cy == 0) || (cx == previousX && cy == previousY)) {
            ++counters.Duplicates; // one texel is one surface sample
            continue;
        }
        previousX = cx;
        previousY = cy;
        out.push_back({lambda, side, cx, cy});
    }
}

template <typename T>
void MergeSides(const std::vector<Position<T>>& plus, const std::vector<Position<T>>& minus, bool interleave,
                std::vector<Position<T>>& out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < plus.size() || j < minus.size()) {
        bool takePlus = false;
        if (j >= minus.size()) takePlus = true;
        else if (i >= plus.size()) takePlus = false;
        else takePlus = !interleave || plus[i].Lambda <= minus[j].Lambda; // tie: + side first
        out.push_back(takePlus ? plus[i++] : minus[j++]);
    }
}

// ---------------------------------------------------------------------------
// Candidate evaluation.

template <typename T>
struct CandidateT {
    T Lambda{};
    int Side = 1;
    int32_t CellX = 0, CellY = 0;
    uint32_t Pieces = 0;
    T Lo[2]{}, Hi[2]{};
    T R{}, G{}, B{};
    VecT<T> Front, Back;
};

template <typename T>
bool EvaluateCandidate(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, const Position<T>& p,
                       const GatherSettings& settings, GatherCounters& counters, CandidateT<T>& out) {
    ++counters.Scheduled;
    const int32_t xx = static_cast<int32_t>(x) + p.CellX;
    const int32_t yy = static_cast<int32_t>(y) + p.CellY;
    if (!g.Inside(xx, yy)) return false; // outside the viewport: unavailable
    const size_t index = g.Index(xx, yy);
    const uint8_t cls = g.Cls[index];
    if (cls == static_cast<uint8_t>(TexelClass::Unavailable)) return false;
    ++counters.Valid;
    if (cls == static_cast<uint8_t>(TexelClass::Sky)) {
        ++counters.Sky; // observed and empty: open
        return false;
    }
    const T zq = g.Z[index];
    const T radius = static_cast<T>(settings.Radius);
    if (settings.RejectSelfIntersection) {
        // Coplanarity of the exactly reconstructed texel-centre point with the
        // receiver's tangent plane (the line point carries the footprint error).
        const VecT<T> centre = VScale(g.Ray(T(xx) + T(0.5), T(yy) + T(0.5)), zq);
        const VecT<T> d = VSub(centre, s.P);
        const T elevation = VDot(d, s.N);
        if (elevation <= static_cast<T>(settings.SelfIntersectionSine) * VLength(d) +
                             static_cast<T>(settings.SelfIntersectionDepth) * s.ZP) {
            ++counters.SelfRejected;
            return false;
        }
    }
    const T sign = T(p.Side);
    const T px = T(x) + T(0.5) + p.Lambda * s.DirX * sign;
    const T py = T(y) + T(0.5) + p.Lambda * s.DirY * sign;
    const VecT<T> q = VScale(g.Ray(px, py), zq);
    const VecT<T> rel = VSub(q, s.P);
    const T dist = VLength(rel);
    if (dist > radius) {
        ++counters.BeyondRadius;
        return false;
    }
    // Back segment along the camera ray toward q, clipped to the R-sphere.
    const VecT<T> dir = VNormalize(q);
    const T along = VDot(rel, dir);
    const T clip = -along + std::sqrt(std::max(along * along - (dist * dist - radius * radius), T(0)));
    const T length = std::max(std::min(static_cast<T>(settings.Thickness), clip), T(0));
    const VecT<T> back = VAdd(q, VScale(dir, length));
    // Policy: a segment that meets the receiver is degenerate and rejected.
    const T closestAt = std::clamp(-along, T(0), length);
    if (VLength(VAdd(rel, VScale(dir, closestAt))) <= T(1.0e-6) * s.ZP) {
        ++counters.Degenerate;
        return false;
    }
    const VecT<T> relBack = VSub(back, s.P);
    const T thetaF = std::atan2(VDot(rel, s.Tn), VDot(rel, s.V));
    const T thetaB = std::atan2(VDot(relBack, s.Tn), VDot(relBack, s.V));
    const T pi = Pi<T>();
    const T twoPi = T(2) * pi;
    T delta = thetaB - thetaF;
    if (delta > pi) delta -= twoPi;
    else if (delta <= -pi) delta += twoPi;
    const T lo = delta >= T(0) ? thetaF : thetaF + delta;
    const T hi = delta >= T(0) ? thetaF + delta : thetaF;
    out.Pieces = 0;
    if (hi > lo) {
        if (lo < -pi) {
            out.Lo[0] = lo + twoPi, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi;
            out.Pieces = 2;
        } else if (hi > pi) {
            out.Lo[0] = lo, out.Hi[0] = pi, out.Lo[1] = -pi, out.Hi[1] = hi - twoPi;
            out.Pieces = 2;
        } else {
            out.Lo[0] = lo, out.Hi[0] = hi;
            out.Pieces = 1;
        }
        ++counters.Occupying;
    }
    // One-sided source radiance toward the receiver (no source cosine).
    const size_t bi = g.BufferIndex(xx, yy);
    T r = static_cast<T>(g.In->Radiance[3 * bi]);
    T gg = static_cast<T>(g.In->Radiance[3 * bi + 1]);
    T b = static_cast<T>(g.In->Radiance[3 * bi + 2]);
    if (cls == static_cast<uint8_t>(TexelClass::OneSided)) {
        const VecT<T>& nq = g.N[index];
        if (VDot(nq, nq) > T(0)) {
            if (!(VDot(nq, VSub(s.P, q)) > T(0))) {
                r = gg = b = T(0);
                ++counters.BackFacing;
            }
        } else {
            ++counters.MissingSourceNormal;
        }
    }
    out.Lambda = p.Lambda;
    out.Side = p.Side;
    out.CellX = p.CellX;
    out.CellY = p.CellY;
    out.R = r, out.G = gg, out.B = b;
    out.Front = q;
    out.Back = back;
    return true;
}

// ---------------------------------------------------------------------------
// Masks.

uint32_t RangeBits(uint32_t start, uint32_t count) {
    // count in [1, 32], start + count <= 32: no shift by the word width.
    return (count >= 32U ? 0xFFFFFFFFU : ((1U << count) - 1U)) << start;
}

uint32_t ValidBits(uint32_t word, uint32_t sectors) {
    const uint32_t lo = word * 32U;
    const uint32_t hi = std::min(sectors, lo + 32U);
    return hi > lo ? RangeBits(0U, hi - lo) : 0U;
}

template <typename T>
std::pair<uint32_t, uint32_t> CentreRangeT(T lo, T hi, uint32_t sectors) {
    if (!(hi > lo) || sectors == 0U) return {0U, 0U};
    const T step = T(2) * Pi<T>() / static_cast<T>(sectors);
    const T x0 = (lo + Pi<T>()) / step - T(0.5);
    const T x1 = (hi + Pi<T>()) / step - T(0.5);
    const auto index = [sectors](T value) -> uint32_t {
        if (!(value > T(0))) return 0U;
        if (value >= static_cast<T>(sectors)) return sectors;
        return static_cast<uint32_t>(value);
    };
    return {index(std::ceil(x0)), index(std::ceil(x1))};
}

void SetRangeWords(uint32_t* words, uint32_t first, uint32_t end) {
    for (uint32_t w = first / 32U; w * 32U < end; ++w) {
        const uint32_t lo = std::max(first, w * 32U);
        const uint32_t hi = std::min(end, w * 32U + 32U);
        if (hi > lo) words[w] |= RangeBits(lo - w * 32U, hi - lo);
    }
}

template <typename Function>
void ForEachRun(const uint32_t* words, uint32_t sectors, Function&& function) {
    for (uint32_t w = 0; w * 32U < sectors; ++w) {
        uint32_t bits = words[w];
        while (bits != 0U) {
            const auto start = static_cast<uint32_t>(std::countr_zero(bits));
            const uint32_t shifted = bits >> start;
            const uint32_t length =
                shifted == 0xFFFFFFFFU ? 32U - start : static_cast<uint32_t>(std::countr_zero(~shifted));
            function(w * 32U + start, w * 32U + start + length);
            bits &= ~RangeBits(start, length);
        }
    }
}

template <typename T>
T BoundaryT(uint32_t j, uint32_t sectors) {
    if (j >= sectors) return Pi<T>();
    return -Pi<T>() + static_cast<T>(j) * (T(2) * Pi<T>() / static_cast<T>(sectors));
}

template <typename T>
T WeightT(T a, T b, T lo, T hi) {
    if constexpr (std::is_same_v<T, double>) return SectorWeightAnalytic(a, b, lo, hi);
    else return SectorWeightAnalyticF32(a, b, lo, hi);
}

template <typename T>
struct SectionSums {
    T R{}, G{}, B{}, FA{};
    uint64_t Occupied = 0;
};

// Centre rule. Each sector is claimed once per section, by the first
// candidate whose interval covers its centre; occupied sectors take that
// candidate's radiance even when it is zero.
template <typename T>
SectionSums<T> AccumulateCentre(const SectionT<T>& s, const CandidateT<T>* candidates, size_t count,
                                uint32_t sectors, uint32_t* maskOut, int32_t* claimant) {
    SectionSums<T> sums;
    uint32_t mask[kMaskWords] = {};
    for (size_t c = 0; c < count; ++c) {
        const CandidateT<T>& candidate = candidates[c];
        uint32_t cover[kMaskWords] = {};
        for (uint32_t piece = 0; piece < candidate.Pieces; ++piece) {
            const auto [first, end] = CentreRangeT<T>(candidate.Lo[piece], candidate.Hi[piece], sectors);
            SetRangeWords(cover, first, end);
        }
        uint32_t fresh[kMaskWords] = {};
        for (uint32_t w = 0; w < kMaskWords; ++w) {
            fresh[w] = cover[w] & ~mask[w];
            mask[w] |= cover[w];
        }
        ForEachRun(fresh, sectors, [&](uint32_t j0, uint32_t j1) {
            const T weight = WeightT<T>(s.A, s.B, BoundaryT<T>(j0, sectors), BoundaryT<T>(j1, sectors));
            sums.R += weight * candidate.R;
            sums.G += weight * candidate.G;
            sums.B += weight * candidate.B;
            if (claimant != nullptr)
                for (uint32_t j = j0; j < j1; ++j) claimant[j] = static_cast<int32_t>(c);
        });
    }
    uint32_t open[kMaskWords] = {};
    for (uint32_t w = 0; w < kMaskWords; ++w) open[w] = ~mask[w] & ValidBits(w, sectors);
    ForEachRun(open, sectors, [&](uint32_t j0, uint32_t j1) {
        sums.FA += WeightT<T>(s.A, s.B, BoundaryT<T>(j0, sectors), BoundaryT<T>(j1, sectors));
    });
    for (uint32_t w = 0; w < kMaskWords; ++w) sums.Occupied += static_cast<uint64_t>(std::popcount(mask[w]));
    if (maskOut != nullptr) std::copy(mask, mask + kMaskWords, maskOut);
    return sums;
}

// Continuous intervals (reference only): the exact union of occupied
// intervals; new pieces take the candidate radiance with exact weights.
SectionSums<double> AccumulateContinuousIntervals(const SectionT<double>& s, const CandidateT<double>* candidates,
                                                  size_t count, std::vector<std::pair<double, double>>& occupied) {
    SectionSums<double> sums;
    occupied.clear();
    for (size_t c = 0; c < count; ++c) {
        const auto& candidate = candidates[c];
        for (uint32_t piece = 0; piece < candidate.Pieces; ++piece) {
            const double lo = candidate.Lo[piece];
            const double hi = candidate.Hi[piece];
            double cursor = lo;
            const auto addFresh = [&](double from, double to) {
                if (!(to > from)) return;
                const double weight = SectorWeightAnalytic(s.A, s.B, from, to);
                sums.R += weight * candidate.R;
                sums.G += weight * candidate.G;
                sums.B += weight * candidate.B;
            };
            for (const auto& [o0, o1] : occupied) {
                if (o1 <= cursor) continue;
                if (o0 >= hi) break;
                addFresh(cursor, std::min(o0, hi));
                cursor = std::max(cursor, o1);
                if (cursor >= hi) break;
            }
            if (cursor < hi) addFresh(cursor, hi);
            // Insert and merge.
            occupied.emplace_back(lo, hi);
            std::sort(occupied.begin(), occupied.end());
            std::vector<std::pair<double, double>> merged;
            for (const auto& interval : occupied) {
                if (!merged.empty() && interval.first <= merged.back().second)
                    merged.back().second = std::max(merged.back().second, interval.second);
                else merged.push_back(interval);
            }
            occupied.swap(merged);
        }
    }
    double cursor = -kPi;
    for (const auto& [o0, o1] : occupied) {
        if (o0 > cursor) sums.FA += SectorWeightAnalytic(s.A, s.B, cursor, o0);
        cursor = std::max(cursor, o1);
    }
    if (cursor < kPi) sums.FA += SectorWeightAnalytic(s.A, s.B, cursor, kPi);
    return sums;
}

// ---------------------------------------------------------------------------
// Pixel and image gather.

template <typename T>
struct Scratch {
    std::vector<Position<T>> Plus, Minus, Merged;
    std::vector<CandidateT<T>> Candidates;
    std::vector<std::pair<double, double>> Occupied;
};

template <typename T>
void BuildCandidates(const View<T>& g, const SectionT<T>& s, uint32_t x, uint32_t y, const GatherSettings& settings,
                     Scratch<T>& scratch, GatherCounters& counters) {
    EnumerateSide(g, s, 1, settings, scratch.Plus, counters);
    EnumerateSide(g, s, -1, settings, scratch.Minus, counters);
    MergeSides(scratch.Plus, scratch.Minus, settings.InterleaveSides, scratch.Merged);
    scratch.Candidates.clear();
    CandidateT<T> candidate;
    for (const auto& position : scratch.Merged)
        if (EvaluateCandidate(g, s, x, y, position, settings, counters, candidate))
            scratch.Candidates.push_back(candidate);
}

// M3 rotation sequence: double divides exactly, float multiplies by the
// float reciprocal of M (same operations as the GPU kernel).
template <typename T>
T PixelRotationT(const GatherSettings& settings, uint32_t x, uint32_t y) {
    if (settings.Rotations.Mode != RotationMode::Stratified) return static_cast<T>(settings.Rotation);
    const uint32_t k = StratumIndex(settings.Rotations, x, y);
    if constexpr (std::is_same_v<T, double>)
        return (static_cast<double>(k) + 0.5) / static_cast<double>(StratumCount(settings.Rotations));
    else
        return (static_cast<float>(k) + 0.5F) * StratumScaleF32(settings.Rotations);
}

template <typename T>
T PixelRadialJitterT(const GatherSettings& settings, uint32_t x, uint32_t y) {
    if (settings.Rotations.Mode != RotationMode::Stratified || !settings.SequenceRadialJitter)
        return static_cast<T>(settings.RadialJitter);
    const uint32_t strata = StratumCount(settings.Rotations);
    const uint32_t k = StratumIndex(settings.Rotations, x, y);
    const uint32_t j = static_cast<uint32_t>((static_cast<uint64_t>(k) * LatticeMultiplier(strata)) % strata);
    if constexpr (std::is_same_v<T, double>)
        return (static_cast<double>(j) + 0.5) / static_cast<double>(strata);
    else
        return (static_cast<float>(j) + 0.5F) * StratumScaleF32(settings.Rotations);
}

template <typename T>
PixelGather GatherPixelT(const View<T>& g, uint32_t x, uint32_t y, const GatherSettings& settings,
                         Scratch<T>& scratch) {
    PixelGather out;
    const size_t index = g.Index(static_cast<int32_t>(x), static_cast<int32_t>(y));
    if (!IsGeometry(g.Cls[index]) || !(VDot(g.N[index], g.N[index]) > T(0))) return out;
    T sumR = T(0), sumG = T(0), sumB = T(0), sumA = T(0);
    const T inversePdf = Pi<T>();
    const T rotation = PixelRotationT<T>(settings, x, y);
    // Per-pixel radial jitter (M3): a local copy keeps the schedule code unchanged.
    GatherSettings jittered;
    const GatherSettings* active = &settings;
    if (settings.Rotations.Mode == RotationMode::Stratified && settings.SequenceRadialJitter) {
        jittered = settings;
        jittered.RadialJitter = static_cast<double>(PixelRadialJitterT<T>(settings, x, y));
        active = &jittered;
    }
    for (uint32_t section = 0; section < settings.Sections; ++section) {
        const T phi = Pi<T>() * (T(section) + rotation) / static_cast<T>(settings.Sections);
        const SectionT<T> s = SetupT(g, x, y, phi, static_cast<T>(settings.Radius));
        if (!s.Valid) return PixelGather{};
        BuildCandidates(g, s, x, y, *active, scratch, out.Counters);
        SectionSums<T> sums;
        if constexpr (std::is_same_v<T, double>) {
            if (settings.Mask == MaskMode::ContinuousIntervals)
                sums = AccumulateContinuousIntervals(s, scratch.Candidates.data(), scratch.Candidates.size(),
                                                     scratch.Occupied);
            else
                sums = AccumulateCentre<T>(s, scratch.Candidates.data(), scratch.Candidates.size(), settings.Sectors,
                                           nullptr, nullptr);
        } else {
            sums = AccumulateCentre<T>(s, scratch.Candidates.data(), scratch.Candidates.size(), settings.Sectors,
                                       nullptr, nullptr);
        }
        sumR += sums.R * inversePdf;
        sumG += sums.G * inversePdf;
        sumB += sums.B * inversePdf;
        sumA += sums.FA * inversePdf;
        out.OccupiedSectors += sums.Occupied;
    }
    const T inverse = T(1) / static_cast<T>(settings.Sections);
    out.Valid = true;
    out.J = {static_cast<double>(sumR * inverse), static_cast<double>(sumG * inverse),
             static_cast<double>(sumB * inverse)};
    out.ARaw = static_cast<double>(sumA * inverse);
    out.A = std::clamp(out.ARaw, 0.0, 1.0);
    out.Confidence = out.Counters.Scheduled == 0U ? 0.0
                                                  : static_cast<double>(out.Counters.Valid) /
                                                        static_cast<double>(out.Counters.Scheduled);
    return out;
}

template <typename T>
std::vector<PixelGather> GatherImageT(const View<T>& g, const GatherSettings& settings) {
    std::vector<PixelGather> results(static_cast<size_t>(g.W) * g.H);
    const uint32_t threads = std::clamp(settings.Threads, 1U, 16U);
    const auto worker = [&](uint32_t first) {
        Scratch<T> scratch;
        for (uint32_t y = first; y < g.H; y += threads)
            for (uint32_t x = 0; x < g.W; ++x) results[static_cast<size_t>(y) * g.W + x] = GatherPixelT(g, x, y, settings, scratch);
    };
    if (threads == 1U) {
        worker(0U);
    } else {
        std::vector<std::thread> pool;
        for (uint32_t t = 0; t < threads; ++t) pool.emplace_back(worker, t);
        for (auto& thread : pool) thread.join();
    }
    return results;
}

View<double> ViewOf(const GatherInput& input, const PreparedGeometry& geometry) {
    View<double> view = MakeView<double>(input);
    view.Z = geometry.ViewDepth.data();
    view.Cls = geometry.Class.data();
    view.N = geometry.Normal.data();
    return view;
}

template <typename V>
Vec3 Widen(const V& v) {
    return {static_cast<double>(v.X), static_cast<double>(v.Y), static_cast<double>(v.Z)};
}

template <typename T>
SectionSetup ToPublic(const SectionT<T>& s) {
    SectionSetup out;
    out.Valid = s.Valid;
    out.P = Widen(s.P);
    out.V = Widen(s.V);
    out.T = Widen(s.Tn);
    out.Normal = Widen(s.N);
    out.NV = s.A;
    out.NT = s.B;
    out.DirX = s.DirX;
    out.DirY = s.DirY;
    out.Extent[0] = s.Extent[0];
    out.Extent[1] = s.Extent[1];
    return out;
}

SectionT<double> FromPublic(const SectionSetup& s) {
    SectionT<double> out;
    out.Valid = s.Valid;
    out.P = s.P;
    out.V = s.V;
    out.Tn = s.T;
    out.N = s.Normal;
    out.A = s.NV;
    out.B = s.NT;
    out.ZP = -s.P.Z;
    out.DirX = s.DirX;
    out.DirY = s.DirY;
    out.Extent[0] = s.Extent[0];
    out.Extent[1] = s.Extent[1];
    return out;
}

template <typename T>
Candidate ToPublic(const CandidateT<T>& c) {
    Candidate out;
    out.Lambda = c.Lambda;
    out.Side = c.Side;
    out.CellX = c.CellX;
    out.CellY = c.CellY;
    out.Pieces = c.Pieces;
    out.Lo[0] = c.Lo[0], out.Lo[1] = c.Lo[1], out.Hi[0] = c.Hi[0], out.Hi[1] = c.Hi[1];
    out.Radiance = {c.R, c.G, c.B};
    out.Front = Widen(c.Front);
    out.Back = Widen(c.Back);
    return out;
}

CandidateT<double> FromPublic(const Candidate& c) {
    CandidateT<double> out;
    out.Lambda = c.Lambda;
    out.Side = c.Side;
    out.CellX = c.CellX;
    out.CellY = c.CellY;
    out.Pieces = std::min(c.Pieces, 2U);
    out.Lo[0] = c.Lo[0], out.Lo[1] = c.Lo[1], out.Hi[0] = c.Hi[0], out.Hi[1] = c.Hi[1];
    out.R = c.Radiance.R, out.G = c.Radiance.G, out.B = c.Radiance.B;
    out.Front = c.Front;
    out.Back = c.Back;
    return out;
}

} // namespace

GatherCounters& GatherCounters::operator+=(const GatherCounters& o) noexcept {
    Scheduled += o.Scheduled;
    Valid += o.Valid;
    Sky += o.Sky;
    SelfRejected += o.SelfRejected;
    BeyondRadius += o.BeyondRadius;
    Degenerate += o.Degenerate;
    BackFacing += o.BackFacing;
    MissingSourceNormal += o.MissingSourceNormal;
    Duplicates += o.Duplicates;
    Occupying += o.Occupying;
    return *this;
}

const char* ValidateGatherInput(const GatherInput& in) noexcept {
    const size_t texels = static_cast<size_t>(in.BufferWidth) * in.BufferHeight;
    if (texels == 0U) return "empty buffers";
    if (in.Viewport.Width == 0U || in.Viewport.Height == 0U) return "empty viewport";
    if (static_cast<uint64_t>(in.Viewport.X) + in.Viewport.Width > in.BufferWidth ||
        static_cast<uint64_t>(in.Viewport.Y) + in.Viewport.Height > in.BufferHeight)
        return "viewport outside the buffers";
    if (in.Depth.size() != texels || in.Class.size() != texels || in.Radiance.size() != 3U * texels)
        return "buffer sizes do not match the declared extent";
    if (in.NormalSource == GeometricNormalSource::RendererFaceNormal && in.FaceNormals.size() != 3U * texels)
        return "renderer face normals declared but not provided";
    const auto& c = in.Camera;
    if (c.Encoding == DepthEncoding::Unknown) return "depth encoding is undeclared";
    if (!(c.Near > 0.0) || !std::isfinite(c.Near)) return "near plane must be finite and positive";
    if (std::isinf(c.Far)) {
        if (c.Encoding != DepthEncoding::ReversedPerspectiveZeroToOne || c.Far < 0.0)
            return "an infinite far plane needs reversed perspective depth";
    } else if (!(c.Far > c.Near)) {
        return "far plane must lie beyond the near plane";
    }
    const double det = c.ViewX[0] * c.ViewY[1] - c.ViewX[1] * c.ViewY[0];
    if (!(std::abs(det) > 0.0) || !std::isfinite(det)) return "UV to ray map is singular";
    return "";
}

const char* ValidateGatherSettings(const GatherSettings& s) noexcept {
    if (!(s.Radius > 0.0) || !std::isfinite(s.Radius)) return "radius R must be finite and positive";
    if (!(s.Thickness >= 0.0) || !std::isfinite(s.Thickness)) return "thickness must be finite and non-negative";
    if (s.Sections == 0U) return "at least one section";
    if (s.Sectors == 0U || s.Sectors > kMaxSectors) return "sector count outside [1, 256]";
    if (!(s.Rotation >= 0.0 && s.Rotation < 1.0)) return "rotation outside [0, 1)";
    if (s.Schedule == CandidateSchedule::Radial) {
        if (s.CandidatesPerSide == 0U) return "radial schedule needs candidates";
        if (!(s.RadialExponent >= 1.0) || !std::isfinite(s.RadialExponent)) return "radial exponent must be >= 1";
        if (!(s.RadialJitter >= 0.0 && s.RadialJitter < 1.0)) return "radial jitter outside [0, 1)";
    }
    if (!(s.SelfIntersectionSine >= 0.0) || !(s.SelfIntersectionDepth >= 0.0)) return "negative self tolerance";
    if (!(s.ChordEpsilon >= 0.0 && s.ChordEpsilon < 0.5)) return "chord epsilon outside [0, 0.5)";
    if (s.Rotations.Mode == RotationMode::Stratified) {
        if (s.Rotations.TileLog2 > kMaxRotationTileLog2) return "rotation tile larger than 8 x 8";
        if (s.Rotations.Cycle == 0U || s.Rotations.Cycle > kMaxRotationCycle) return "rotation cycle outside [1, 64]";
    }
    return "";
}

uint32_t TileOrder(uint32_t x, uint32_t y, uint32_t tileLog2) noexcept {
    // Bit pair (x_k, y_k) -> 0, 2, 3, 1; pair k weighted by 4^(L - 1 - k).
    uint32_t index = 0U;
    for (uint32_t k = 0; k < tileLog2; ++k) {
        const uint32_t bx = (x >> k) & 1U, by = (y >> k) & 1U;
        const uint32_t code = bx == 0U ? (by == 0U ? 0U : 3U) : (by == 0U ? 2U : 1U);
        index |= code << (2U * (tileLog2 - 1U - k));
    }
    return index;
}

uint32_t StratumCount(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    return (1U << (2U * log2)) * std::clamp(sequence.Cycle, 1U, kMaxRotationCycle);
}

uint32_t FrameShift(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    if (log2 == 0U) return 0U;
    const uint32_t tiles = 1U << (2U * log2);
    // Pair i of the T^2-frame shift period; even shifts 2 bitrev(i) over
    // log2(T^2 / 2) bits, so that with the XOR flip every stratum visits every
    // tile position once per period.
    const auto pair = static_cast<uint32_t>(sequence.Frame % tiles) >> 1U;
    const uint32_t bits = 2U * log2 - 1U;
    uint32_t reversed = 0U;
    for (uint32_t b = 0; b < bits; ++b) reversed |= ((pair >> b) & 1U) << (bits - 1U - b);
    return 2U * reversed;
}

uint32_t FrameFlip(const RotationSequence& sequence) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    return log2 == 0U ? 0U : static_cast<uint32_t>(sequence.Frame & 1U);
}

uint32_t StratumIndex(const RotationSequence& sequence, uint32_t x, uint32_t y) noexcept {
    const uint32_t log2 = std::min(sequence.TileLog2, kMaxRotationTileLog2);
    const uint32_t tiles = 1U << (2U * log2);
    const uint32_t cycle = std::clamp(sequence.Cycle, 1U, kMaxRotationCycle);
    const auto phase = static_cast<uint32_t>(sequence.Frame % cycle);
    const uint32_t coarse = ((TileOrder(x, y, log2) ^ FrameFlip(sequence)) + FrameShift(sequence)) % tiles;
    return cycle * coarse + phase;
}

uint32_t LatticeMultiplier(uint32_t strata) noexcept {
    if (strata <= 1U) return 1U;
    const double target = static_cast<double>(strata) * 0.6180339887498949;
    const auto base = static_cast<int64_t>(std::floor(target));
    // Nearest coprime integer in [1, strata - 1]; ties go to the smaller.
    for (int64_t distance = 0; distance < static_cast<int64_t>(strata); ++distance) {
        int64_t best = -1;
        double bestGap = 0.0;
        for (const int64_t candidate : {base - distance, base + 1 - distance, base + distance, base + 1 + distance}) {
            if (candidate < 1 || candidate >= static_cast<int64_t>(strata)) continue;
            if (std::gcd(static_cast<uint64_t>(candidate), static_cast<uint64_t>(strata)) != 1U) continue;
            const double gap = std::abs(static_cast<double>(candidate) - target);
            if (best < 0 || gap < bestGap || (gap == bestGap && candidate < best)) {
                best = candidate;
                bestGap = gap;
            }
        }
        if (best >= 0) return static_cast<uint32_t>(best);
    }
    return 1U;
}

float StratumScaleF32(const RotationSequence& sequence) noexcept {
    return 1.0F / static_cast<float>(StratumCount(sequence));
}

double PixelRotation(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRotationT<double>(settings, x, y);
}

float PixelRotationF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRotationT<float>(settings, x, y);
}

double PixelRadialJitter(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRadialJitterT<double>(settings, x, y);
}

float PixelRadialJitterF32(const GatherSettings& settings, uint32_t x, uint32_t y) noexcept {
    return PixelRadialJitterT<float>(settings, x, y);
}

double DecodeViewDepth(double encoded, const DepthCamera& camera) noexcept {
    const bool infiniteFar = std::isinf(camera.Far);
    return DecodeDepthT<double>(encoded, camera.Encoding, camera.Near, infiniteFar ? 0.0 : camera.Far, infiniteFar);
}

Vec3 PreparedGeometry::Ray(const DepthCamera& camera, double px, double py) const noexcept {
    const double u = px / Width;
    const double v = py / Height;
    return {camera.ViewX[0] * u + camera.ViewX[1] * v + camera.ViewX[2],
            camera.ViewY[0] * u + camera.ViewY[1] * v + camera.ViewY[2], -1.0};
}

Vec3 PreparedGeometry::Position(const DepthCamera& camera, uint32_t x, uint32_t y) const noexcept {
    return Scale(Ray(camera, x + 0.5, y + 0.5), ViewDepth[static_cast<size_t>(y) * Width + x]);
}

PreparedGeometry PrepareGeometry(const GatherInput& input) {
    View<double> view;
    auto data = PrepareT<double>(input, view);
    PreparedGeometry out;
    out.Width = input.Viewport.Width;
    out.Height = input.Viewport.Height;
    out.ViewDepth = std::move(data.Z);
    out.Class = std::move(data.Cls);
    out.Normal = std::move(data.N);
    return out;
}

PreparedGeometry PrepareGeometryF32(const GatherInput& input) {
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    PreparedGeometry out;
    out.Width = input.Viewport.Width;
    out.Height = input.Viewport.Height;
    out.ViewDepth.assign(data.Z.begin(), data.Z.end());
    out.Class = data.Cls;
    out.Normal.reserve(data.N.size());
    for (const auto& n : data.N) out.Normal.push_back(Widen(n));
    return out;
}

SectionCandidates BuildSectionF32(const GatherInput& input, uint32_t x, uint32_t y, float phi,
                                  const GatherSettings& settings) {
    SectionCandidates out;
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    const SectionT<float> s = SetupT(view, x, y, phi, static_cast<float>(settings.Radius));
    out.Setup = ToPublic(s);
    if (!s.Valid) return out;
    Scratch<float> scratch;
    BuildCandidates(view, s, x, y, settings, scratch, out.Counters);
    for (const auto& c : scratch.Candidates) out.Ordered.push_back(ToPublic(c));
    return out;
}

SectionSetup SetupSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                          double phi, const GatherSettings& settings) {
    return ToPublic(SetupT(ViewOf(input, geometry), x, y, phi, settings.Radius));
}

SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                               double phi, const GatherSettings& settings) {
    SectionCandidates out;
    const View<double> view = ViewOf(input, geometry);
    const SectionT<double> s = SetupT(view, x, y, phi, settings.Radius);
    out.Setup = ToPublic(s);
    if (!s.Valid) return out;
    Scratch<double> scratch;
    BuildCandidates(view, s, x, y, settings, scratch, out.Counters);
    out.Ordered.reserve(scratch.Candidates.size());
    for (const auto& c : scratch.Candidates) out.Ordered.push_back(ToPublic(c));
    return out;
}

std::pair<uint32_t, uint32_t> CentreRuleRange(double lo, double hi, uint32_t sectors) noexcept {
    return CentreRangeT<double>(lo, hi, std::min(sectors, kMaxSectors));
}

std::pair<uint32_t, uint32_t> CentreRuleRangeF32(float lo, float hi, uint32_t sectors) noexcept {
    return CentreRangeT<float>(lo, hi, std::min(sectors, kMaxSectors));
}

void SetSectorRange(SectorMask& mask, uint32_t first, uint32_t end) noexcept {
    SetRangeWords(mask.Words.data(), std::min(first, kMaxSectors), std::min(end, kMaxSectors));
}

SectorMask CoverInterval(double lo, double hi, uint32_t sectors) noexcept {
    SectorMask mask;
    if (!(hi > lo) || !(hi - lo < 2.0 * kPi)) {
        if (hi - lo >= 2.0 * kPi) SetSectorRange(mask, 0U, sectors);
        return mask;
    }
    const double turns = std::floor((lo + kPi) / (2.0 * kPi));
    lo -= 2.0 * kPi * turns;
    hi -= 2.0 * kPi * turns;
    const auto set = [&](double a, double b) {
        const auto [first, end] = CentreRuleRange(a, b, sectors);
        SetSectorRange(mask, first, end);
    };
    if (hi <= kPi) {
        set(lo, hi);
    } else {
        set(lo, kPi);
        set(-kPi, hi - 2.0 * kPi);
    }
    return mask;
}

uint32_t CountSectors(const SectorMask& mask) noexcept {
    uint32_t count = 0;
    for (const uint32_t word : mask.Words) count += static_cast<uint32_t>(std::popcount(word));
    return count;
}

double SectorBoundary(uint32_t j, uint32_t sectors) noexcept { return BoundaryT<double>(j, sectors); }

double MaskWeight(const SectorMask& mask, uint32_t sectors, double nv, double nt) noexcept {
    double total = 0.0;
    SectorMask clipped = mask;
    for (uint32_t w = 0; w < kMaskWords; ++w) clipped.Words[w] &= ValidBits(w, sectors);
    ForEachRun(clipped.Words.data(), sectors, [&](uint32_t j0, uint32_t j1) {
        total += SectorWeightAnalytic(nv, nt, SectorBoundary(j0, sectors), SectorBoundary(j1, sectors));
    });
    return total;
}

SectionResult AccumulateSection(const SectionSetup& setup, const std::vector<Candidate>& ordered, uint32_t sectors,
                                MaskMode mode) {
    SectionResult result;
    const SectionT<double> s = FromPublic(setup);
    std::vector<CandidateT<double>> candidates;
    candidates.reserve(ordered.size());
    for (const auto& c : ordered) candidates.push_back(FromPublic(c));
    SectionSums<double> sums;
    if (mode == MaskMode::ContinuousIntervals) {
        sums = AccumulateContinuousIntervals(s, candidates.data(), candidates.size(), result.Occupied);
    } else {
        sectors = std::clamp(sectors, 1U, kMaxSectors);
        result.Claimant.assign(sectors, -1);
        sums = AccumulateCentre<double>(s, candidates.data(), candidates.size(), sectors, result.Mask.Words.data(),
                                        result.Claimant.data());
    }
    result.F = {sums.R, sums.G, sums.B};
    result.FA = sums.FA;
    return result;
}

PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                        const GatherSettings& settings) {
    Scratch<double> scratch;
    return GatherPixelT(ViewOf(input, geometry), x, y, settings, scratch);
}

std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings) {
    const PreparedGeometry geometry = PrepareGeometry(input);
    return GatherImageT(ViewOf(input, geometry), settings);
}

std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings) {
    View<float> view;
    const auto data = PrepareT<float>(input, view);
    return GatherImageT(view, settings);
}

} // namespace Fast::Renderer::DiffuseGi
