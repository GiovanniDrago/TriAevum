#pragma once

// Diffuse screen-space GI, milestone M2: gathering on a depth buffer and sector
// masks (docs/TRIAEVUM_SCREEN_SPACE_GI.md; specification sections 1, 4, 9 and
// 10, tests T05-T09).
//
// Title-neutral and Vulkan-free. The double-precision functions are the
// reference. GatherImageF32 is an FP32 twin that follows the GPU kernel
// (diffuse_gi_gather_shader.h) operation by operation, so CPU/GPU differences
// can be separated from FP32 rounding.
//
// Conventions (all declared by the adapter, never inferred):
// - View space: camera at the origin, forward -Z, positive view depth z = -Z.
// - Viewport pixel coordinates: texel (i, j) covers [i, i+1) x [j, j+1); its
//   centre is (i + 0.5, j + 0.5). Normalized viewport UV = pixel / extent.
// - Camera rays: X/z = ViewX . (u, v, 1), Y/z = ViewY . (u, v, 1); this affine
//   map carries storage orientation and off-centre projection.
// - Depth is decoded with the declared DepthEncoding, near and far planes.
// - TexelClass distinguishes geometry (one- or two-sided), sky and unavailable
//   pixels; the module does not infer it from colour or depth.

#include "fast/renderer/diffuse_gi_reference.h"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace Fast::Renderer::DiffuseGi {

inline constexpr uint32_t kMaxSectors = 256;
inline constexpr uint32_t kMaskWords = kMaxSectors / 32;

enum class TexelClass : uint8_t {
    Unavailable = 0, // no data (HUD, outside the composed surface, invalid)
    OneSided = 1,    // opaque geometry emitting only from its visible side
    TwoSided = 2,    // geometry emitting from both sides
    Sky = 3,         // no geometry along the camera ray
};

// Source of the geometric normal (section 1): a renderer-provided face normal
// or the reconstruction from depth implemented here.
enum class GeometricNormalSource : uint8_t { DepthReconstruction, RendererFaceNormal };

struct DepthCamera {
    DepthEncoding Encoding = DepthEncoding::PerspectiveZeroToOne;
    double Near = 0.1;
    // +infinity is accepted only with ReversedPerspectiveZeroToOne.
    double Far = 100.0;
    std::array<double, 3> ViewX{};
    std::array<double, 3> ViewY{};
};

struct ViewportRect {
    uint32_t X = 0, Y = 0, Width = 0, Height = 0;
};

struct GatherInput {
    uint32_t BufferWidth = 0, BufferHeight = 0;
    // Texels outside the viewport are never read.
    ViewportRect Viewport;
    DepthCamera Camera;
    std::vector<float> Depth;    // BufferWidth * BufferHeight, encoded per Camera
    std::vector<uint8_t> Class;  // TexelClass per texel
    std::vector<float> Radiance; // 3 floats per texel: linear outgoing radiance
    GeometricNormalSource NormalSource = GeometricNormalSource::DepthReconstruction;
    // RendererFaceNormal only: 3 floats per texel, view space. Oriented toward
    // the camera by the module (the visible side is the front side).
    std::vector<float> FaceNormals;
};

// Empty when the input is consistent, otherwise the first violated rule.
[[nodiscard]] const char* ValidateGatherInput(const GatherInput& input) noexcept;

enum class CandidateSchedule : uint8_t {
    // Reference: every texel crossed by the section line inside the projected
    // radius, represented by the line point nearest to the texel centre.
    EveryTexel,
    // Production candidate: K positions per side at screen distances
    // lambda_k = lambda_0 + (L - lambda_0) ((k + jitter) / K)^exponent.
    Radial,
};

enum class MaskMode : uint8_t {
    CentreRule,          // uniform theta sectors, sector-centre coverage (baseline)
    ContinuousIntervals, // exact union of occupied intervals (reference only)
};

struct GatherSettings {
    double Radius = 1.0;    // R, scene units: finite and positive
    double Thickness = 0.5; // tau, scene units, along the camera ray behind q
    uint32_t Sections = 8;  // S; phi_s = pi (s + Rotation) / S, p = 1/pi
    uint32_t Sectors = 64;  // N in [1, kMaxSectors]; independent of S (default: sector study)
    double Rotation = 0.5;
    CandidateSchedule Schedule = CandidateSchedule::EveryTexel;
    uint32_t CandidatesPerSide = 16; // Radial only
    double RadialExponent = 2.0;     // Radial only, >= 1
    double RadialJitter = 0.5;       // Radial only, in [0, 1)
    MaskMode Mask = MaskMode::CentreRule;
    // Self-intersection: a candidate whose texel-centre point lies within
    // SelfIntersectionSine * |Q - P| + SelfIntersectionDepth * z_P above the
    // receiver's tangent plane (or below it) is the receiver's own surface.
    bool RejectSelfIntersection = true;
    double SelfIntersectionSine = 2.0e-3;
    double SelfIntersectionDepth = 1.0e-5;
    // Baseline interleaves both sides by screen distance (ties: + side first).
    // false processes the whole + side first; it exists to test the tie-break.
    bool InterleaveSides = true;
    // Texel chords shorter than this (pixels) are corner touches: skipped.
    double ChordEpsilon = 1.0e-3;
    uint32_t Threads = 1; // image gather only
};

[[nodiscard]] const char* ValidateGatherSettings(const GatherSettings& settings) noexcept;

struct GatherCounters {
    uint64_t Scheduled = 0;   // candidates programmed (denominator of C)
    uint64_t Valid = 0;       // geometry or sky with a decodable state (numerator of C)
    uint64_t Sky = 0;
    uint64_t SelfRejected = 0;
    uint64_t BeyondRadius = 0;
    uint64_t Degenerate = 0;  // segment meeting the receiver: rejected
    uint64_t BackFacing = 0;  // one-sided source seen from behind: occupies, emits zero
    uint64_t MissingSourceNormal = 0;
    uint64_t Duplicates = 0;  // Radial positions falling in an already used texel
    uint64_t Occupying = 0;   // candidates with a non-empty interval
    GatherCounters& operator+=(const GatherCounters& other) noexcept;
};

struct PixelGather {
    bool Valid = false; // receiver is geometry with a usable geometric normal
    Rgb J;              // raw estimate, never clamped or renormalized
    double ARaw = 1.0;
    double A = 1.0;     // clamp(ARaw, 0, 1)
    double Confidence = 0.0; // Valid / Scheduled; 0 when nothing was scheduled
    uint64_t OccupiedSectors = 0;
    GatherCounters Counters;
};

// Whole viewport, row-major (Viewport.Width * Viewport.Height). Non-geometry
// receivers get the neutral result J = 0, A = 1, C = 0, Valid = false.
[[nodiscard]] std::vector<PixelGather> GatherImage(const GatherInput& input, const GatherSettings& settings);
[[nodiscard]] std::vector<PixelGather> GatherImageF32(const GatherInput& input, const GatherSettings& settings);

// ---------------------------------------------------------------------------
// Geometry preparation (double): decoded view depth, effective class and
// geometric normal per viewport texel.

struct PreparedGeometry {
    uint32_t Width = 0, Height = 0;
    std::vector<double> ViewDepth;        // 0 when not geometry
    std::vector<uint8_t> Class;           // effective TexelClass
    std::vector<Vec3> Normal;             // zero vector when unavailable
    [[nodiscard]] Vec3 Ray(const DepthCamera& camera, double px, double py) const noexcept;
    [[nodiscard]] Vec3 Position(const DepthCamera& camera, uint32_t x, uint32_t y) const noexcept;
};
[[nodiscard]] PreparedGeometry PrepareGeometry(const GatherInput& input);
// FP32 twin of the preparation (values widened to double), for diagnosis and
// CPU/GPU comparisons of the normal reconstruction.
[[nodiscard]] PreparedGeometry PrepareGeometryF32(const GatherInput& input);
[[nodiscard]] double DecodeViewDepth(double encoded, const DepthCamera& camera) noexcept;

// ---------------------------------------------------------------------------
// Section-level reference (double), exposed for the mask, order and oracle
// tests.

struct SectionSetup {
    bool Valid = false;
    Vec3 P, V, T, Normal; // receiver, unit view vector, unit section tangent, integration normal
    double NV = 0.0, NT = 0.0; // a = n.v and b = n.t(phi) of the M1 weights
    double DirX = 0.0, DirY = 0.0; // unit pixel direction of +t on screen
    double Extent[2] = {0.0, 0.0}; // projected radius in pixels: [0] + side, [1] - side
};

struct Candidate {
    double Lambda = 0.0; // screen distance from the receiver centre (pixels)
    int Side = 1;        // +1 or -1
    int32_t CellX = 0, CellY = 0; // texel offset from the receiver
    uint32_t Pieces = 0; // 0: empty interval; 2: split at +-pi
    double Lo[2] = {0.0, 0.0}, Hi[2] = {0.0, 0.0};
    Rgb Radiance; // evaluated toward the receiver (zero when back-facing)
    Vec3 Front, Back; // view-space segment endpoints after clipping to R
};

struct SectionCandidates {
    SectionSetup Setup;
    std::vector<Candidate> Ordered; // occupying or empty-interval geometry, in processing order
    GatherCounters Counters;
};

[[nodiscard]] SectionSetup SetupSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                        uint32_t y, double phi, const GatherSettings& settings);
[[nodiscard]] SectionCandidates BuildSection(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                             uint32_t y, double phi, const GatherSettings& settings);
// FP32 twin of BuildSection (prepares its own FP32 geometry; values widened).
[[nodiscard]] SectionCandidates BuildSectionF32(const GatherInput& input, uint32_t x, uint32_t y, float phi,
                                                const GatherSettings& settings);

struct SectorMask {
    std::array<uint32_t, kMaskWords> Words{};
    bool operator==(const SectorMask&) const = default;
};

// Centre rule: sector j (centre -pi + (j + 1/2) 2 pi / N) is covered by
// [lo, hi) iff lo <= centre < hi. Returns [first, end) clamped to [0, N];
// empty when hi <= lo.
[[nodiscard]] std::pair<uint32_t, uint32_t> CentreRuleRange(double lo, double hi, uint32_t sectors) noexcept;
[[nodiscard]] std::pair<uint32_t, uint32_t> CentreRuleRangeF32(float lo, float hi, uint32_t sectors) noexcept;
// Sets bits [first, end) across words; never shifts by the word width.
void SetSectorRange(SectorMask& mask, uint32_t first, uint32_t end) noexcept;
// Interval of any position with hi - lo < 2 pi (split at +-pi first).
[[nodiscard]] SectorMask CoverInterval(double lo, double hi, uint32_t sectors) noexcept;
[[nodiscard]] uint32_t CountSectors(const SectorMask& mask) noexcept;
// Boundary angle of sector index j in [0, N]: -pi + j 2 pi / N, exactly pi at N.
[[nodiscard]] double SectorBoundary(uint32_t j, uint32_t sectors) noexcept;
// Sum of the M1 weights of the set bits, evaluated per contiguous run.
[[nodiscard]] double MaskWeight(const SectorMask& mask, uint32_t sectors, double nv, double nt) noexcept;

struct SectionResult {
    Rgb F;          // sum_j w_j L_j over occupied sectors
    double FA = 0.0; // sum of the open weights
    SectorMask Mask; // CentreRule only
    std::vector<int32_t> Claimant; // CentreRule: candidate index per sector, -1 open
    std::vector<std::pair<double, double>> Occupied; // ContinuousIntervals only
};
[[nodiscard]] SectionResult AccumulateSection(const SectionSetup& setup, const std::vector<Candidate>& ordered,
                                              uint32_t sectors, MaskMode mode);

// One receiver, all sections (double reference).
[[nodiscard]] PixelGather GatherPixel(const GatherInput& input, const PreparedGeometry& geometry, uint32_t x,
                                      uint32_t y, const GatherSettings& settings);

} // namespace Fast::Renderer::DiffuseGi
