#pragma once

// Synthetic depth-buffer scenes for the diffuse GI tests (milestone M2).
//
// The generator ray-casts analytic primitives (boxes and rectangles defined in
// view space) through a declared pinhole camera into the buffers of the
// adapter contract (depth, class, outgoing radiance, face normals). The same
// analytic scene is also an AngularField seen from a receiver, so M1's
// continuous integrator gives the 3D ground truth of the finite-radius model
// (true first hit within R) against which screen-space limits are measured.
// Everything is deterministic: no random numbers, no files, no captures.

#include "fast/renderer/diffuse_gi_gather.h"
#include "fast/renderer/diffuse_gi_reference.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Fast::Renderer::DiffuseGi::Synthetic {

struct Primitive {
    enum class Kind : uint8_t { Box, Rectangle };
    Kind Shape = Kind::Box;
    Vec3 Centre;
    // Orthonormal axes; a rectangle spans Axes[0], Axes[1] and faces Axes[2].
    std::array<Vec3, 3> Axes{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
    Vec3 Half; // half extents along the axes (rectangle: Z unused)
    Rgb Radiance;
    bool TwoSided = false; // rectangles only; boxes emit from their outside

    [[nodiscard]] static Primitive Box(const Vec3& centre, const Vec3& half, const Rgb& radiance);
    // Box rotated about the view X axis by `pitch` radians (then translated).
    [[nodiscard]] static Primitive TiltedBox(const Vec3& centre, const Vec3& half, double pitch, const Rgb& radiance);
    [[nodiscard]] static Primitive Rectangle(const Vec3& centre, const Vec3& axisU, const Vec3& axisV, double halfU,
                                             double halfV, const Rgb& radiance, bool twoSided);
};

enum class DepthStorage : uint8_t { Float32, Unorm24 };

struct PinholeCamera {
    uint32_t Width = 128, Height = 96;
    double TanHalfY = 0.57735026918962576; // 60 degree vertical field of view
    double TanHalfX = 0.0;                 // 0: TanHalfY * Width / Height (square pixels)
    double ShiftX = 0.0, ShiftY = 0.0;     // principal point offset, tangent units
    DepthEncoding Encoding = DepthEncoding::PerspectiveZeroToOne;
    double Near = 0.1, Far = 100.0;
    DepthStorage Storage = DepthStorage::Float32;
};

struct Scene {
    std::string Name;
    PinholeCamera Camera;
    std::vector<Primitive> Primitives;
    // Shifted viewport: the image is embedded in a larger buffer whose other
    // texels hold poison (NaN depth, geometry class, huge radiance).
    uint32_t PadLeft = 0, PadTop = 0, PadRight = 0, PadBottom = 0;
    // Viewport rectangles [x0, x1) x [y0, y1) declared Unavailable (e.g. HUD).
    std::vector<std::array<uint32_t, 4>> Unavailable;
};

struct RayHit {
    double Distance = 0.0; // ray parameter (view depth for (rx, ry, -1) rays)
    Vec3 Normal;           // outward face normal
    size_t Primitive = 0;
    bool FrontFace = true; // the ray sees the emitting side
};

// First entering intersection with parameter in [tMin, tMax]; exits are
// ignored, so a ray leaving the surface it starts on sees nothing there.
[[nodiscard]] std::optional<RayHit> Intersect(const std::vector<Primitive>& primitives, const Vec3& origin,
                                              const Vec3& direction, double tMin, double tMax);

[[nodiscard]] DepthCamera MakeDepthCamera(const PinholeCamera& camera);
[[nodiscard]] float EncodeDepth(double viewDepth, const PinholeCamera& camera);

// Buffers of the adapter contract. FaceNormals holds the exact face normals
// (float) and NormalSource is DepthReconstruction; tests switch it.
[[nodiscard]] GatherInput Rasterize(const Scene& scene);
// Replaces FaceNormals by their RGBA8 encoding 0.5 n + 0.5 decoded back
// (the precision of the renderer's 8-bit shading-normal guide).
void QuantizeFaceNormals8(GatherInput& input);
// Same scene with geometry, near and far multiplied by `factor`.
[[nodiscard]] Scene Scaled(const Scene& scene, double factor);

// 3D ground truth of the finite-radius model, seen from `receiver`: the first
// primitive hit within `radius`, its radiance when the receiver faces the
// emitting side, otherwise zero; open beyond R.
class SceneField final : public AngularField {
  public:
    SceneField(const std::vector<Primitive>& primitives, const Vec3& receiver, double radius);
    [[nodiscard]] FieldSample Sample(const Vec3& omega) const override;
    void SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const override;

  private:
    const std::vector<Primitive>& mPrimitives;
    Vec3 mReceiver;
    double mRadius = 1.0;
    double mTMin = 0.0;
};

struct GroundTruth {
    Rgb J;
    double A = 0.0;
    bool Converged = false;
    uint32_t Panels = 0;
    double LastCorrection = 0.0;
};
// M1 continuous integrator on SceneField with the receiver's position and
// integration normal taken from the prepared geometry (same P and n as the
// screen-space gather).
[[nodiscard]] GroundTruth IntegrateGroundTruth(const Scene& scene, const GatherInput& input,
                                               const PreparedGeometry& geometry, uint32_t x, uint32_t y,
                                               double radius, double tolerance = 1.0e-6);

// ---------------------------------------------------------------------------
// Scene catalogue (one or more per test; see the document for the layouts).

[[nodiscard]] PinholeCamera StandardCamera(uint32_t width, uint32_t height);
[[nodiscard]] Primitive BlackFloor(double radiance = 0.0);
// T06: bright wall behind a black box, receivers on a black floor.
[[nodiscard]] Scene BlackOccluderScene(uint32_t width, uint32_t height, bool withOccluder);
// T07: thin dark bar in front of a bright wall.
[[nodiscard]] Scene ThinBarScene(uint32_t width, uint32_t height);
// T07: deep dark block beside the receivers, bright wall behind it.
[[nodiscard]] Scene ThickBlockScene(uint32_t width, uint32_t height);
// T08: bright box protruding from a back face up to just beyond the near plane.
[[nodiscard]] Scene NearPlaneScene(uint32_t width, uint32_t height);
// Sloped plane filling the view (self-intersection and normal tests).
[[nodiscard]] Scene SlopedPlaneScene(uint32_t width, uint32_t height, double pitch);
// Corner room and pillars: realistic angular sizes for the sector study.
[[nodiscard]] Scene CornerRoomScene(uint32_t width, uint32_t height);
[[nodiscard]] Scene PillarsScene(uint32_t width, uint32_t height);
// T05: boxes whose intervals cross 32-bit word boundaries for N = 96.
[[nodiscard]] Scene WordBoundaryScene(uint32_t width, uint32_t height);
// Bright panel facing the camera (one- or two-sided); floor receivers behind
// its plane see its back side.
[[nodiscard]] Scene PanelScene(uint32_t width, uint32_t height, bool twoSided);
// T09: overhang whose only camera-visible face lies beyond R while its
// underside, hidden from the camera, is the first 3D hit within R.
[[nodiscard]] Scene OverhangScene(uint32_t width, uint32_t height);

} // namespace Fast::Renderer::DiffuseGi::Synthetic
