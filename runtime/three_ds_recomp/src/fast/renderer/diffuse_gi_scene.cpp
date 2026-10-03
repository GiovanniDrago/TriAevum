#include "fast/renderer/diffuse_gi_scene.h"

// Synthetic scene generator for the diffuse GI tests (M2). Own code: ray/box
// slabs, ray/rectangle planes, perspective depth encodings of the declared
// adapter contract, and the section-plane cross-sections that give M1's
// continuous integrator its breakpoints.

#include <algorithm>
#include <cmath>
#include <limits>

namespace Fast::Renderer::DiffuseGi::Synthetic {
namespace {

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.X - b.X, a.Y - b.Y, a.Z - b.Z}; }

struct Face {
    Vec3 Centre, Normal, U, V;
    double HalfU = 0.0, HalfV = 0.0;
    size_t Primitive = 0;
};

void AppendFaces(const Primitive& p, size_t index, std::vector<Face>& faces) {
    if (p.Shape == Primitive::Kind::Rectangle) {
        faces.push_back({p.Centre, p.Axes[2], p.Axes[0], p.Axes[1], p.Half.X, p.Half.Y, index});
        return;
    }
    const double half[3] = {p.Half.X, p.Half.Y, p.Half.Z};
    for (int axis = 0; axis < 3; ++axis)
        for (const double sign : {1.0, -1.0}) {
            const Vec3 normal = Scale(p.Axes[axis], sign);
            const Vec3 centre = Add(p.Centre, Scale(normal, half[axis]));
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;
            faces.push_back({centre, normal, p.Axes[u], p.Axes[v], half[u], half[v], index});
        }
}

double Cross2(double ax, double ay, double bx, double by) { return ax * by - ay * bx; }

double WrapAngle(double angle) {
    angle = std::remainder(angle, 2.0 * kPi);
    return angle <= -kPi ? angle + 2.0 * kPi : angle;
}

} // namespace

Primitive Primitive::Box(const Vec3& centre, const Vec3& half, const Rgb& radiance) {
    Primitive p;
    p.Shape = Kind::Box;
    p.Centre = centre;
    p.Half = half;
    p.Radiance = radiance;
    return p;
}

Primitive Primitive::TiltedBox(const Vec3& centre, const Vec3& half, double pitch, const Rgb& radiance) {
    Primitive p = Box(centre, half, radiance);
    const double c = std::cos(pitch), s = std::sin(pitch);
    p.Axes = {Vec3{1.0, 0.0, 0.0}, Vec3{0.0, c, -s}, Vec3{0.0, s, c}};
    return p;
}

Primitive Primitive::Rectangle(const Vec3& centre, const Vec3& axisU, const Vec3& axisV, double halfU, double halfV,
                               const Rgb& radiance, bool twoSided) {
    Primitive p;
    p.Shape = Kind::Rectangle;
    p.Centre = centre;
    const Vec3 u = Normalize(axisU);
    const Vec3 w = Normalize(Cross(u, axisV));
    p.Axes = {u, Cross(w, u), w};
    p.Half = {halfU, halfV, 0.0};
    p.Radiance = radiance;
    p.TwoSided = twoSided;
    return p;
}

std::optional<RayHit> Intersect(const std::vector<Primitive>& primitives, const Vec3& origin, const Vec3& direction,
                                double tMin, double tMax) {
    std::optional<RayHit> best;
    for (size_t index = 0; index < primitives.size(); ++index) {
        const Primitive& p = primitives[index];
        const Vec3 local = Sub(origin, p.Centre);
        if (p.Shape == Primitive::Kind::Rectangle) {
            const double denominator = Dot(direction, p.Axes[2]);
            if (!(std::abs(denominator) > 0.0)) continue;
            const double t = -Dot(local, p.Axes[2]) / denominator;
            if (!(t >= tMin && t <= tMax) || (best && t >= best->Distance)) continue;
            const Vec3 hit = Add(local, Scale(direction, t));
            if (std::abs(Dot(hit, p.Axes[0])) > p.Half.X || std::abs(Dot(hit, p.Axes[1])) > p.Half.Y) continue;
            const bool emitting = p.TwoSided || -denominator > 0.0;
            best = RayHit{t, p.Axes[2], index, emitting};
            continue;
        }
        const double half[3] = {p.Half.X, p.Half.Y, p.Half.Z};
        double tNear = -std::numeric_limits<double>::infinity();
        double tFar = std::numeric_limits<double>::infinity();
        Vec3 normal;
        bool miss = false;
        for (int axis = 0; axis < 3 && !miss; ++axis) {
            const double o = Dot(local, p.Axes[axis]);
            const double d = Dot(direction, p.Axes[axis]);
            if (!(std::abs(d) > 0.0)) {
                if (std::abs(o) > half[axis]) miss = true;
                continue;
            }
            double t1 = (-half[axis] - o) / d;
            double t2 = (half[axis] - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            if (t1 > tNear) {
                tNear = t1;
                normal = Scale(p.Axes[axis], d > 0.0 ? -1.0 : 1.0);
            }
            tFar = std::min(tFar, t2);
        }
        if (miss || tNear > tFar || !(tNear >= tMin && tNear <= tMax)) continue;
        if (best && tNear >= best->Distance) continue;
        best = RayHit{tNear, normal, index, true};
    }
    return best;
}

DepthCamera MakeDepthCamera(const PinholeCamera& c) {
    DepthCamera depth;
    const double tx = c.TanHalfX > 0.0 ? c.TanHalfX : c.TanHalfY * c.Width / c.Height;
    depth.Encoding = c.Encoding;
    depth.Near = c.Near;
    depth.Far = c.Far;
    depth.ViewX = {2.0 * tx, 0.0, -tx + c.ShiftX};
    depth.ViewY = {0.0, -2.0 * c.TanHalfY, c.TanHalfY + c.ShiftY};
    return depth;
}

float EncodeDepth(double z, const PinholeCamera& c) {
    const double n = c.Near, f = c.Far;
    double d = 0.0;
    switch (c.Encoding) {
    case DepthEncoding::PerspectiveZeroToOne:
        d = f * (z - n) / (z * (f - n));
        break;
    case DepthEncoding::ReversedPerspectiveZeroToOne:
        d = std::isinf(f) ? n / z : n * (f - z) / (z * (f - n));
        break;
    case DepthEncoding::LinearWBuffer:
        d = (z - n) / (f - n);
        break;
    case DepthEncoding::ReversedLinearWBuffer:
        d = (f - z) / (f - n);
        break;
    default:
        return std::numeric_limits<float>::quiet_NaN();
    }
    if (c.Storage == DepthStorage::Unorm24) {
        constexpr double levels = 16777215.0;
        d = std::round(std::clamp(d, 0.0, 1.0) * levels) / levels;
    }
    return static_cast<float>(d);
}

GatherInput Rasterize(const Scene& scene) {
    const PinholeCamera& c = scene.Camera;
    GatherInput in;
    in.BufferWidth = c.Width + scene.PadLeft + scene.PadRight;
    in.BufferHeight = c.Height + scene.PadTop + scene.PadBottom;
    in.Viewport = {scene.PadLeft, scene.PadTop, c.Width, c.Height};
    in.Camera = MakeDepthCamera(c);
    const size_t texels = static_cast<size_t>(in.BufferWidth) * in.BufferHeight;
    // Poison outside the viewport: a near bright surface. Any read of it would
    // dominate the result.
    in.Depth.assign(texels, EncodeDepth(std::max(2.0 * c.Near, 0.5), c));
    in.Class.assign(texels, static_cast<uint8_t>(TexelClass::OneSided));
    in.Radiance.assign(3 * texels, 1.0e30F);
    in.FaceNormals.assign(3 * texels, 0.0F);
    for (size_t i = 0; i < texels; ++i) in.FaceNormals[3 * i + 2] = 1.0F;
    in.NormalSource = GeometricNormalSource::DepthReconstruction;
    for (uint32_t j = 0; j < c.Height; ++j)
        for (uint32_t i = 0; i < c.Width; ++i) {
            const size_t bi = static_cast<size_t>(j + scene.PadTop) * in.BufferWidth + i + scene.PadLeft;
            const double u = (i + 0.5) / c.Width, v = (j + 0.5) / c.Height;
            const Vec3 ray{in.Camera.ViewX[0] * u + in.Camera.ViewX[1] * v + in.Camera.ViewX[2],
                           in.Camera.ViewY[0] * u + in.Camera.ViewY[1] * v + in.Camera.ViewY[2], -1.0};
            const auto hit = Intersect(scene.Primitives, {}, ray, c.Near, std::isinf(c.Far) ? 1.0e300 : c.Far);
            if (!hit) {
                in.Depth[bi] = EncodeDepth(c.Far, c);
                in.Class[bi] = static_cast<uint8_t>(TexelClass::Sky);
                in.Radiance[3 * bi] = in.Radiance[3 * bi + 1] = in.Radiance[3 * bi + 2] = 0.0F;
                continue;
            }
            const Primitive& p = scene.Primitives[hit->Primitive];
            in.Depth[bi] = EncodeDepth(hit->Distance, c);
            in.Class[bi] = static_cast<uint8_t>(p.TwoSided ? TexelClass::TwoSided : TexelClass::OneSided);
            const Rgb radiance = hit->FrontFace ? p.Radiance : Rgb{};
            in.Radiance[3 * bi] = static_cast<float>(radiance.R);
            in.Radiance[3 * bi + 1] = static_cast<float>(radiance.G);
            in.Radiance[3 * bi + 2] = static_cast<float>(radiance.B);
            const Vec3 visible = Dot(hit->Normal, ray) < 0.0 ? hit->Normal : Scale(hit->Normal, -1.0);
            in.FaceNormals[3 * bi] = static_cast<float>(visible.X);
            in.FaceNormals[3 * bi + 1] = static_cast<float>(visible.Y);
            in.FaceNormals[3 * bi + 2] = static_cast<float>(visible.Z);
        }
    for (const auto& rect : scene.Unavailable)
        for (uint32_t j = rect[1]; j < std::min(rect[3], c.Height); ++j)
            for (uint32_t i = rect[0]; i < std::min(rect[2], c.Width); ++i)
                in.Class[static_cast<size_t>(j + scene.PadTop) * in.BufferWidth + i + scene.PadLeft] =
                    static_cast<uint8_t>(TexelClass::Unavailable);
    return in;
}

void QuantizeFaceNormals8(GatherInput& input) {
    for (float& component : input.FaceNormals) {
        const double code = std::round((0.5 * std::clamp(static_cast<double>(component), -1.0, 1.0) + 0.5) * 255.0);
        component = static_cast<float>(code / 255.0 * 2.0 - 1.0);
    }
}

Scene Scaled(const Scene& scene, double factor) {
    Scene out = scene;
    for (auto& p : out.Primitives) {
        p.Centre = Scale(p.Centre, factor);
        p.Half = Scale(p.Half, factor);
    }
    out.Camera.Near *= factor;
    out.Camera.Far *= factor;
    return out;
}

SceneField::SceneField(const std::vector<Primitive>& primitives, const Vec3& receiver, double radius)
    : mPrimitives(primitives), mReceiver(receiver), mRadius(radius),
      mTMin(1.0e-7 * std::sqrt(Dot(receiver, receiver))) {}

FieldSample SceneField::Sample(const Vec3& omega) const {
    const auto hit = Intersect(mPrimitives, mReceiver, omega, mTMin, mRadius);
    if (!hit) return {};
    FieldSample sample;
    sample.State = DirectionState::Hit;
    if (hit->FrontFace) sample.Radiance = mPrimitives[hit->Primitive].Radiance;
    return sample;
}

void SceneField::SectionBreakpoints(const Vec3& v, const Vec3& t, std::vector<double>& thetas) const {
    // Cross-section of every face with the section plane through the
    // receiver: a 2D segment in (v, t) coordinates. The first hit (and its
    // emitting side) changes only at segment endpoints, at crossings of two
    // segments and where a segment crosses the R circle.
    std::vector<Face> faces;
    for (size_t i = 0; i < mPrimitives.size(); ++i) AppendFaces(mPrimitives[i], i, faces);
    struct Segment {
        double X0, Y0, X1, Y1;
        size_t Primitive;
    };
    std::vector<Segment> segments;
    const auto angle = [&](double x, double y) { thetas.push_back(WrapAngle(std::atan2(y, x))); };
    for (const Face& f : faces) {
        const Vec3 offset = Sub(mReceiver, f.Centre);
        const double a = Dot(f.Normal, v), b = Dot(f.Normal, t), c = Dot(f.Normal, offset);
        const double norm2 = a * a + b * b;
        if (!(norm2 > 1.0e-24)) continue;
        const double x0 = -c * a / norm2, y0 = -c * b / norm2;
        const double norm = std::sqrt(norm2);
        const double ex = -b / norm, ey = a / norm;
        double sMin = -std::numeric_limits<double>::infinity(), sMax = std::numeric_limits<double>::infinity();
        bool empty = false;
        for (int k = 0; k < 2 && !empty; ++k) {
            const Vec3& axis = k == 0 ? f.U : f.V;
            const double half = k == 0 ? f.HalfU : f.HalfV;
            const double base = Dot(offset, axis) + x0 * Dot(v, axis) + y0 * Dot(t, axis);
            const double slope = ex * Dot(v, axis) + ey * Dot(t, axis);
            if (!(std::abs(slope) > 1.0e-15)) {
                if (std::abs(base) > half) empty = true;
                continue;
            }
            double s1 = (-half - base) / slope, s2 = (half - base) / slope;
            if (s1 > s2) std::swap(s1, s2);
            sMin = std::max(sMin, s1);
            sMax = std::min(sMax, s2);
        }
        if (empty || !(sMax > sMin) || !std::isfinite(sMin) || !std::isfinite(sMax)) continue;
        const Segment segment{x0 + sMin * ex, y0 + sMin * ey, x0 + sMax * ex, y0 + sMax * ey, f.Primitive};
        segments.push_back(segment);
        angle(segment.X0, segment.Y0);
        angle(segment.X1, segment.Y1);
        // R circle: |x0 + s e| = R.
        const double proj = x0 * ex + y0 * ey;
        const double disc = proj * proj - (x0 * x0 + y0 * y0 - mRadius * mRadius);
        if (disc >= 0.0)
            for (const double s : {-proj - std::sqrt(disc), -proj + std::sqrt(disc)})
                if (s > sMin && s < sMax) angle(x0 + s * ex, y0 + s * ey);
    }
    for (size_t i = 0; i < segments.size(); ++i)
        for (size_t j = i + 1; j < segments.size(); ++j) {
            const Segment& p = segments[i];
            const Segment& q = segments[j];
            if (p.Primitive == q.Primitive) continue;
            const double rx = p.X1 - p.X0, ry = p.Y1 - p.Y0, sx = q.X1 - q.X0, sy = q.Y1 - q.Y0;
            const double denominator = Cross2(rx, ry, sx, sy);
            if (!(std::abs(denominator) > 0.0)) continue;
            const double qpx = q.X0 - p.X0, qpy = q.Y0 - p.Y0;
            const double tp = Cross2(qpx, qpy, sx, sy) / denominator;
            const double tq = Cross2(qpx, qpy, rx, ry) / denominator;
            if (tp > 0.0 && tp < 1.0 && tq > 0.0 && tq < 1.0) angle(p.X0 + tp * rx, p.Y0 + tp * ry);
        }
}

GroundTruth IntegrateGroundTruth(const Scene& scene, const GatherInput& input, const PreparedGeometry& geometry,
                                 uint32_t x, uint32_t y, double radius, double tolerance) {
    GroundTruth out;
    const size_t index = static_cast<size_t>(y) * geometry.Width + x;
    const Vec3 p = geometry.Position(input.Camera, x, y);
    const Vec3 n = geometry.Normal[index];
    const SceneField field(scene.Primitives, p, radius);
    const SectionFrame frame = SectionFrame::FromView(Scale(p, -1.0));
    ContinuousOptions options;
    options.Tolerance = tolerance;
    options.MaxPanels = 1024;
    options.Inner = {1.0e-10, 1.0e-14, 64, true};
    const ContinuousResult result = IntegrateContinuous(field, frame, n, {}, options);
    out.J = result.J;
    out.A = result.A;
    out.Converged = result.Converged;
    out.Panels = result.Panels;
    out.LastCorrection = result.LastCorrection;
    return out;
}

// ---------------------------------------------------------------------------
// Catalogue. View space: camera at the origin looking down -Z, +Y up; the
// floor's top face is y = -1. Units are scene units (metre-like).

PinholeCamera StandardCamera(uint32_t width, uint32_t height) {
    PinholeCamera camera;
    camera.Width = width;
    camera.Height = height;
    return camera;
}

Primitive BlackFloor(double radiance) {
    return Primitive::Box({0.0, -1.5, -50.0}, {60.0, 0.5, 50.0}, {radiance, radiance, radiance});
}

Scene BlackOccluderScene(uint32_t width, uint32_t height, bool withOccluder) {
    Scene scene;
    scene.Name = withOccluder ? "t06_black_occluder" : "t06_wall_only";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -9.5}, {8.0, 3.0, 0.5}, {4.0, 3.0, 2.0}));
    if (withOccluder) scene.Primitives.push_back(Primitive::Box({0.3, -0.55, -6.8}, {0.6, 0.45, 0.5}, {}));
    return scene;
}

Scene ThinBarScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t07_thin_bar";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -8.5}, {8.0, 3.0, 0.5}, {2.0, 2.5, 3.0}));
    scene.Primitives.push_back(Primitive::Box({0.0, -0.45, -6.0}, {4.0, 0.05, 0.05}, {}));
    return scene;
}

Scene ThickBlockScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t07_thick_block";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -11.5}, {10.0, 3.5, 0.5}, {3.0, 3.0, 3.0}));
    scene.Primitives.push_back(Primitive::Box({-0.5, 0.0, -7.0}, {0.8, 1.0, 1.75}, {}));
    return scene;
}

Scene NearPlaneScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t08_near_plane";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    // Back face at z = -0.13 and a bright box protruding from it up to 0.1%
    // beyond the near plane (front face at z = -0.1001).
    scene.Primitives.push_back(Primitive::Box({0.0, 0.0, -0.18}, {0.3, 0.3, 0.05}, {0.2, 0.2, 0.2}));
    scene.Primitives.push_back(Primitive::Box({0.015, 0.0, -0.11505}, {0.006, 0.02, 0.01495}, {3.0, 2.0, 1.0}));
    return scene;
}

Scene SlopedPlaneScene(uint32_t width, uint32_t height, double pitch) {
    Scene scene;
    scene.Name = "sloped_plane";
    scene.Camera = StandardCamera(width, height);
    const double yaw = 17.0 * kPi / 180.0;
    // Plane axes: X and the pitched Y axis, both rotated about Y by the yaw
    // ((x, z) -> (x cos + z sin, -x sin + z cos)) so no axis is screen-aligned.
    const Vec3 u{std::cos(yaw), 0.0, -std::sin(yaw)};
    const Vec3 v{-std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw)};
    scene.Primitives.push_back(Primitive::Rectangle({0.0, -0.5, -6.0}, u, v, 200.0, 200.0, {1.0, 1.0, 1.0}, false));
    return scene;
}

Scene CornerRoomScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "corner_room";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.2));
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -8.5}, {6.0, 3.0, 0.5}, {0.3, 0.3, 0.3}));
    scene.Primitives.push_back(Primitive::Box({-3.5, 1.5, -5.0}, {0.5, 3.0, 4.0}, {2.5, 0.5, 0.3}));
    scene.Primitives.push_back(Primitive::Box({3.5, 1.5, -5.0}, {0.5, 3.0, 4.0}, {0.05, 0.05, 0.05}));
    scene.Primitives.push_back(Primitive::Box({-2.4, -0.6, -7.4}, {0.4, 0.4, 0.4}, {0.2, 0.8, 0.3}));
    return scene;
}

Scene PillarsScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "pillars";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.1));
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -10.5}, {8.0, 3.0, 0.5}, {1.5, 1.5, 1.2}));
    scene.Primitives.push_back(Primitive::Box({-1.2, 0.0, -5.5}, {0.2, 1.0, 0.2}, {0.2, 0.4, 2.0}));
    scene.Primitives.push_back(Primitive::Box({0.4, 0.0, -6.5}, {0.2, 1.0, 0.2}, {2.0, 0.8, 0.1}));
    scene.Primitives.push_back(Primitive::Box({1.6, 0.0, -5.0}, {0.2, 1.0, 0.2}, {}));
    scene.Primitives.push_back(Primitive::Box({-0.3, -0.75, -4.2}, {0.25, 0.25, 0.25}, {0.9, 0.9, 0.9}));
    return scene;
}

Scene WordBoundaryScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t05_word_boundaries";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor(0.3));
    scene.Primitives.push_back(Primitive::Box({-0.8, -0.4, -4.6}, {0.3, 0.6, 0.3}, {3.0, 0.2, 0.2}));
    scene.Primitives.push_back(Primitive::Box({0.7, -0.5, -5.2}, {0.4, 0.5, 0.2}, {0.2, 3.0, 0.2}));
    scene.Primitives.push_back(Primitive::TiltedBox({0.0, -0.8, -6.0}, {1.5, 0.2, 0.4}, 0.6, {0.2, 0.2, 3.0}));
    return scene;
}

Scene PanelScene(uint32_t width, uint32_t height, bool twoSided) {
    Scene scene;
    scene.Name = twoSided ? "panel_two_sided" : "panel_one_sided";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Rectangle({0.9, -0.4, -6.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.5, 0.6,
                                                    {3.0, 2.0, 1.0}, twoSided));
    return scene;
}

Scene OverhangScene(uint32_t width, uint32_t height) {
    Scene scene;
    scene.Name = "t09_overhang";
    scene.Camera = StandardCamera(width, height);
    scene.Primitives.push_back(BlackFloor());
    scene.Primitives.push_back(Primitive::Box({0.0, 1.5, -10.5}, {8.0, 3.0, 0.5}, {2.0, 2.0, 2.0}));
    // Only its front face (z = -2) is visible to the camera; its underside
    // (y = -0.2) spans z in [-5.8, -2].
    scene.Primitives.push_back(Primitive::Box({0.2, 0.3, -3.9}, {0.6, 0.5, 1.9}, {0.5, 0.5, 0.5}));
    return scene;
}

} // namespace Fast::Renderer::DiffuseGi::Synthetic
