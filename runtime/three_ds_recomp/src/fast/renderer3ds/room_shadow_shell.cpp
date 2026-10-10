#include "fast/renderer3ds/room_shadow_shell.h"
#include "fast/renderer/cache_file.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>

namespace Fast::Renderer3ds {
namespace {
constexpr uint64_t kBasis = 1469598103934665603ULL; // Renderer FNV basis (intentionally nonstandard).
constexpr uint32_t kMagic = 0x48535352U;            // RSSH
constexpr size_t kHeaderBytes = 32;
constexpr size_t kMaximumBytes = kHeaderBytes + kMaximumRoomShadowTriangles * (3 * 12 + 3 * 4 + 8) + 8;
double Dot(const ShadowVector& a, const ShadowVector& b) {
    return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2];
}
ShadowVector Sub(const ShadowVector& a, const ShadowVector& b) {
    return { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
}
ShadowVector Cross(const ShadowVector& a, const ShadowVector& b) {
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
ShadowVector Unit(ShadowVector a) {
    const double length = std::sqrt(Dot(a, a));
    if (!std::isfinite(length) || length < 1.e-12)
        return {};
    for (auto& v : a)
        v = float(v / length);
    return a;
}
bool Finite(const ShadowVector& p) {
    return std::all_of(p.begin(), p.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1.e8F; });
}
uint64_t Checksum(std::span<const uint8_t> bytes) {
    uint64_t hash = kBasis;
    for (auto b : bytes)
        hash = (hash ^ b) * 1099511628211ULL;
    return hash;
}
void Put(std::vector<uint8_t>& out, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i)
        out.push_back(uint8_t(v >> (i * 8)));
}
uint64_t Get(std::span<const uint8_t> in, size_t at, size_t n) {
    if (at > in.size() || n > in.size() - at)
        throw std::runtime_error("truncated room shadow cache");
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= uint64_t(in[at + i]) << (i * 8);
    return v;
}
void Bounds(RoomShadowShell& mesh) {
    if (mesh.Positions.empty())
        return;
    mesh.Bounds = { mesh.Positions.front(), mesh.Positions.front(), true };
    for (const auto& p : mesh.Positions)
        for (size_t k = 0; k < 3; ++k) {
            mesh.Bounds.Minimum[k] = std::min(mesh.Bounds.Minimum[k], p[k]);
            mesh.Bounds.Maximum[k] = std::max(mesh.Bounds.Maximum[k], p[k]);
        }
}
bool Valid(const RoomShadowShell& m) {
    if (m.RasterSize < 8 || m.RasterSize > 2048 || m.ShellMasks.size() > kMaximumRoomShadowTriangles ||
        m.Indices.size() != m.ShellMasks.size() * 3 || m.Positions.size() > kMaximumRoomShadowTriangles * 3)
        return false;
    for (const auto& p : m.Positions)
        if (!Finite(p))
            return false;
    for (auto i : m.Indices)
        if (i >= m.Positions.size())
            return false;
    return true;
}
struct Projected {
    double X, Y, Z;
};
double Edge(const Projected& a, const Projected& b, double x, double y) {
    return (b.X - a.X) * (y - a.Y) - (b.Y - a.Y) * (x - a.X);
}
// Identical sample coverage in both passes; stable lower-index ownership of
// exact depth ties. Double-sided occluders take part in the first-depth pass.
template <class F> void Raster(const std::array<Projected, 3>& p, uint32_t size, F visit) {
    const double area = Edge(p[0], p[1], p[2].X, p[2].Y);
    if (std::abs(area) < 1.e-10)
        return;
    const int x0 = std::max(0, int(std::ceil(std::min({ p[0].X, p[1].X, p[2].X }) - .5)));
    const int x1 = std::min(int(size) - 1, int(std::floor(std::max({ p[0].X, p[1].X, p[2].X }) - .5)));
    const int y0 = std::max(0, int(std::ceil(std::min({ p[0].Y, p[1].Y, p[2].Y }) - .5)));
    const int y1 = std::min(int(size) - 1, int(std::floor(std::max({ p[0].Y, p[1].Y, p[2].Y }) - .5)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double a = Edge(p[1], p[2], x + .5, y + .5) / area;
            const double b = Edge(p[2], p[0], x + .5, y + .5) / area;
            const double c = 1 - a - b;
            if (a >= -1.e-10 && b >= -1.e-10 && c >= -1.e-10)
                visit(size_t(y) * size + x, a * p[0].Z + b * p[1].Z + c * p[2].Z);
        }
}
} // namespace

bool RoomShadowShellScene::Ready() const noexcept {
    return !Rooms.empty() && std::all_of(Rooms.begin(), Rooms.end(),
                                         [](const auto& r) { return r.Mesh && r.Mesh->Fingerprint == r.Fingerprint; });
}
const std::array<ShadowVector, kRoomShadowSectorCount>& RoomShadowDirections() {
    static const auto directions = [] {
        std::array<ShadowVector, kRoomShadowSectorCount> result{};
        const double angle = std::numbers::pi * (3 - std::sqrt(5.0));
        for (uint32_t s = 0; s < result.size(); ++s) {
            const double y = (s + .5) / result.size(), r = std::sqrt(1 - y * y);
            result[s] = { float(r * std::cos(s * angle)), float(y), float(r * std::sin(s * angle)) };
        }
        return result;
    }();
    return directions;
}
uint32_t RoomShadowSector(const ShadowVector& light) noexcept {
    double best = -std::numeric_limits<double>::infinity();
    uint32_t result = 0;
    for (uint32_t i = 0; i < kRoomShadowSectorCount; ++i) {
        const double dot = Dot(light, RoomShadowDirections()[i]);
        if (dot > best) {
            best = dot;
            result = i;
        }
    }
    return result;
}
std::shared_ptr<const RoomShadowShell> BuildRoomShadowShell(uint64_t fingerprint,
                                                            std::span<const RoomShadowTriangle> input, uint32_t size,
                                                            std::stop_token stop) {
    if (input.size() > kMaximumRoomShadowTriangles || size < 8 || size > 2048)
        return {};
    auto result = std::make_shared<RoomShadowShell>();
    result->Fingerprint = fingerprint;
    result->RasterSize = size;
    std::map<ShadowVector, uint32_t> welded;
    std::vector<ShadowVector> normals;
    std::vector<bool> eligible;
    for (const auto& t : input) {
        if (stop.stop_requested())
            return {};
        if (!t.Opaque || t.AlphaTest)
            continue;
        for (const auto& p : t.Positions)
            if (!Finite(p))
                return {};
        for (auto p : t.Positions) {
            for (auto& v : p)
                if (v == 0)
                    v = 0; // canonical +0
            auto [it, added] = welded.try_emplace(p, uint32_t(result->Positions.size()));
            if (added)
                result->Positions.push_back(p);
            result->Indices.push_back(it->second);
        }
        normals.push_back(Unit(Cross(Sub(t.Positions[1], t.Positions[0]), Sub(t.Positions[2], t.Positions[0]))));
        eligible.push_back(t.CullBack);
    }
    result->ShellMasks.resize(normals.size());
    Bounds(*result);
    if (!normals.empty())
        for (uint32_t sector = 0; sector < kRoomShadowSectorCount; ++sector) {
            if (stop.stop_requested())
                return {};
            const auto direction = RoomShadowDirections()[sector];
            const auto right = Unit(
                Cross(std::abs(direction[1]) > .9F ? ShadowVector{ 1, 0, 0 } : ShadowVector{ 0, 1, 0 }, direction));
            const auto up = Cross(direction, right);
            std::vector<Projected> positions;
            double minX = 1.e300, minY = 1.e300, maxX = -1.e300, maxY = -1.e300;
            for (const auto& p : result->Positions) {
                Projected q{ Dot(p, right), Dot(p, up), Dot(p, direction) };
                positions.push_back(q);
                minX = std::min(minX, q.X);
                maxX = std::max(maxX, q.X);
                minY = std::min(minY, q.Y);
                maxY = std::max(maxY, q.Y);
            }
            const double scale = double(size - 2) / std::max({ maxX - minX, maxY - minY, 1.e-6 });
            for (auto& p : positions) {
                p.X = 1 + (p.X - minX) * scale;
                p.Y = 1 + (p.Y - minY) * scale;
            }
            std::vector<double> depth(size_t(size) * size, -std::numeric_limits<double>::infinity());
            std::vector<uint32_t> first(depth.size(), UINT32_MAX), covered(normals.size()), visible(normals.size());
            for (uint32_t i = 0; i < normals.size(); ++i) {
                if ((i & 1023U) == 0 && stop.stop_requested())
                    return {};
                std::array<Projected, 3> p;
                for (size_t j = 0; j < 3; ++j)
                    p[j] = positions[result->Indices[size_t(i) * 3 + j]];
                Raster(p, size, [&](size_t pixel, double z) {
                    ++covered[i];
                    if (z > depth[pixel]) {
                        depth[pixel] = z;
                        first[pixel] = i;
                    }
                });
            }
            for (auto i : first)
                if (i != UINT32_MAX)
                    ++visible[i];
            for (size_t i = 0; i < normals.size(); ++i)
                if (eligible[i] && Dot(normals[i], direction) < -.25 && covered[i] != 0 &&
                    uint64_t(visible[i]) * 2 >= covered[i])
                    result->ShellMasks[i] |= uint64_t{ 1 } << sector;
        }
    const auto encoded = EncodeRoomShadowShell(*result);
    result->ContentVersion = Get(encoded, encoded.size() - 8, 8);
    return result;
}
std::vector<uint8_t> EncodeRoomShadowShell(const RoomShadowShell& mesh) {
    if (!Valid(mesh))
        return {};
    std::vector<uint8_t> bytes;
    bytes.reserve(kHeaderBytes + mesh.Positions.size() * 12 + mesh.Indices.size() * 4 + mesh.ShellMasks.size() * 8 + 8);
    Put(bytes, kMagic, 4);
    Put(bytes, kRoomShadowShellVersion, 4);
    Put(bytes, mesh.Fingerprint, 8);
    Put(bytes, kRoomShadowSectorCount, 4);
    Put(bytes, mesh.RasterSize, 4);
    Put(bytes, mesh.Positions.size(), 4);
    Put(bytes, mesh.ShellMasks.size(), 4);
    for (const auto& p : mesh.Positions)
        for (auto v : p)
            Put(bytes, std::bit_cast<uint32_t>(v), 4);
    for (auto i : mesh.Indices)
        Put(bytes, i, 4);
    for (auto m : mesh.ShellMasks)
        Put(bytes, m, 8);
    Put(bytes, Checksum(bytes), 8);
    return bytes;
}
std::shared_ptr<const RoomShadowShell> DecodeRoomShadowShell(std::span<const uint8_t> bytes, uint64_t fingerprint) {
    try {
        if (bytes.size() < kHeaderBytes + 8 || bytes.size() > kMaximumBytes || Get(bytes, 0, 4) != kMagic ||
            Get(bytes, 4, 4) != kRoomShadowShellVersion || Get(bytes, 8, 8) != fingerprint ||
            Get(bytes, 16, 4) != kRoomShadowSectorCount || Get(bytes, 20, 4) < 8 || Get(bytes, 20, 4) > 2048)
            return {};
        const size_t vertices = Get(bytes, 24, 4), triangles = Get(bytes, 28, 4);
        if (triangles > kMaximumRoomShadowTriangles || vertices > kMaximumRoomShadowTriangles * 3 ||
            bytes.size() != kHeaderBytes + vertices * 12 + triangles * 20 + 8)
            return {};
        const uint64_t checksum = Get(bytes, bytes.size() - 8, 8);
        if (Checksum(bytes.first(bytes.size() - 8)) != checksum)
            return {};
        auto mesh = std::make_shared<RoomShadowShell>();
        mesh->Fingerprint = fingerprint;
        mesh->ContentVersion = checksum;
        mesh->RasterSize = uint32_t(Get(bytes, 20, 4));
        size_t at = kHeaderBytes;
        mesh->Positions.resize(vertices);
        mesh->Indices.resize(triangles * 3);
        mesh->ShellMasks.resize(triangles);
        for (auto& p : mesh->Positions)
            for (auto& v : p) {
                v = std::bit_cast<float>(uint32_t(Get(bytes, at, 4)));
                at += 4;
            }
        for (auto& i : mesh->Indices) {
            i = uint32_t(Get(bytes, at, 4));
            at += 4;
        }
        for (auto& m : mesh->ShellMasks) {
            m = Get(bytes, at, 8);
            at += 8;
        }
        if (!Valid(*mesh))
            return {};
        Bounds(*mesh);
        return mesh;
    } catch (const std::exception&) { return {}; }
}
std::filesystem::path RoomShadowShellPath(const std::filesystem::path& directory, uint64_t fingerprint) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(fingerprint));
    return directory / "shadow-shells" / name;
}
std::shared_ptr<const RoomShadowShell> LoadRoomShadowShell(const std::filesystem::path& directory,
                                                           uint64_t fingerprint) {
    if (directory.empty())
        return {};
    std::ifstream file(RoomShadowShellPath(directory, fingerprint), std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length < 0 || uint64_t(length) > kMaximumBytes)
        return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())))
        return {};
    auto mesh = DecodeRoomShadowShell(bytes, fingerprint);
    return mesh && mesh->RasterSize == kRoomShadowRasterSize ? mesh : nullptr;
}
bool SaveRoomShadowShell(const std::filesystem::path& directory, const RoomShadowShell& mesh, std::string* error) {
    if (directory.empty()) {
        if (error) *error = "cache directory is empty";
        return false;
    }
    const auto bytes = EncodeRoomShadowShell(mesh);
    if (bytes.empty()) {
        if (error) *error = "invalid shell mesh";
        return false;
    }
    return Renderer::WriteCacheFileAtomically(RoomShadowShellPath(directory, mesh.Fingerprint), {}, bytes, error);
}
std::vector<RoomShadowShellVertex> RoomShadowShellVertices(const RoomShadowShell& mesh) {
    std::vector<RoomShadowShellVertex> result;
    if (!Valid(mesh))
        return result;
    result.reserve(mesh.Indices.size());
    for (size_t i = 0; i < mesh.Indices.size(); ++i) {
        const auto mask = mesh.ShellMasks[i / 3];
        result.push_back({ mesh.Positions[mesh.Indices[i]], { uint32_t(mask), uint32_t(mask >> 32) } });
    }
    return result;
}
std::string_view RoomShadowShellVertexShader() {
    return R"glsl(#version 450
layout(location=0) in vec3 position_world;
layout(location=1) in uvec2 shell_mask;
layout(set=0,binding=30,std140) uniform ShadowTransform {
    mat4 view_to_world;
    mat4 world_to_light_clip;
} shadow;
layout(push_constant) uniform ShadowPage {
    vec4 scale_offset;
    uint sector;
} page;
void main() {
    if ((shell_mask[page.sector >> 5u] & (1u << (page.sector & 31u))) != 0u) {
        gl_Position=vec4(0.0,0.0,0.0,1.0);
    } else {
        gl_Position=shadow.world_to_light_clip*vec4(position_world,1.0);
        gl_Position.xy=gl_Position.xy*page.scale_offset.xy+page.scale_offset.zw*gl_Position.w;
    }
}
)glsl";
}
VirtualShadowCaster RoomShadowCaster(const RoomShadowShell& mesh, uint32_t sector, uint64_t rasterState) noexcept {
    auto hash = [](uint64_t seed, uint64_t value) {
        for (size_t i = 0; i < 8; ++i)
            seed = (seed ^ uint8_t(value >> (i * 8))) * 1099511628211ULL;
        return seed;
    };
    const uint64_t content = hash(hash(mesh.ContentVersion, sector), rasterState);
    return { mesh.Bounds, content ? content : 1, false, hash(0x5348454c4c524f4fULL, mesh.Fingerprint) };
}
} // namespace Fast::Renderer3ds
