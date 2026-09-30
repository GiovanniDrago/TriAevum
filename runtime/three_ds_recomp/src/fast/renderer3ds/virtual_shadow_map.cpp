#include "fast/renderer3ds/virtual_shadow_map.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Fast::Renderer3ds {
namespace {
constexpr auto kCenterFirstPages = [] {
    std::array<std::array<uint32_t, 2>, kVirtualShadowGridPages * kVirtualShadowGridPages> result{};
    size_t index = 0;
    constexpr int center = kVirtualShadowGridPages / 2;
    for (int radius = 0; radius <= center; ++radius)
        for (int y = -radius; y <= radius; ++y)
            for (int x = -radius; x <= radius; ++x) {
                if (x != -radius && x != radius && y != -radius && y != radius)
                    continue;
                const int column = center + x, row = center + y;
                if (column < 0 || row < 0 || column >= int(kVirtualShadowGridPages) ||
                    row >= int(kVirtualShadowGridPages))
                    continue;
                result[index++] = { uint32_t(column), uint32_t(row) };
            }
    return result;
}();

ShadowVector Transform(const ShadowMatrix& m, const ShadowVector& p) noexcept {
    return { m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
             m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] };
}
float Dot(const ShadowVector& a, const ShadowVector& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
ShadowVector Cross(const ShadowVector& a, const ShadowVector& b) noexcept {
    return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
}
bool Normalize(ShadowVector& v) noexcept {
    const float n = std::sqrt(Dot(v, v));
    if (!std::isfinite(n) || n < 1.0e-6F)
        return false;
    for (auto& x : v)
        x /= n;
    return true;
}
void Include(ShadowBounds& bounds, const ShadowVector& point) noexcept {
    if (!bounds.Available) {
        bounds.Minimum = bounds.Maximum = point;
        bounds.Available = true;
    } else {
        for (size_t axis = 0; axis < 3; ++axis) {
            bounds.Minimum[axis] = std::min(bounds.Minimum[axis], point[axis]);
            bounds.Maximum[axis] = std::max(bounds.Maximum[axis], point[axis]);
        }
    }
}
bool Overlaps(const ShadowBounds& bounds, float x, float y, float size, float guard) noexcept {
    return !bounds.Available || (bounds.Maximum[0] >= x - guard && bounds.Minimum[0] <= x + size + guard &&
                                 bounds.Maximum[1] >= y - guard && bounds.Minimum[1] <= y + size + guard);
}
uint64_t Mix(uint64_t value) noexcept {
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}
} // namespace

ShadowBounds TransformShadowBounds(const ShadowBounds& bounds, const ShadowMatrix& transform) noexcept {
    ShadowBounds result;
    if (!bounds.Available)
        return result;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        ShadowVector point;
        for (uint32_t axis = 0; axis < 3; ++axis)
            point[axis] = (corner & (1U << axis)) ? bounds.Maximum[axis] : bounds.Minimum[axis];
        Include(result, Transform(transform, point));
    }
    return result;
}

VirtualShadowLayout BuildVirtualShadowLayout(const ShadowVector& cameraWorld, const ShadowVector& directionTowardLight,
                                             uint32_t atlasResolution, float maximumDistance,
                                             const VirtualShadowLayout* previousLayout) noexcept {
    VirtualShadowLayout result;
    if (atlasResolution < 1024 || atlasResolution > 4096 || atlasResolution % kVirtualShadowPageTexels != 0 ||
        !std::isfinite(maximumDistance) || maximumDistance <= 0 || maximumDistance > 65536.0F ||
        !std::all_of(cameraWorld.begin(), cameraWorld.end(),
                     [](float v) { return std::isfinite(v) && std::abs(v) < 1.0e8F; }))
        return result;
    auto light = directionTowardLight;
    if (!Normalize(light))
        return result;
    if (previousLayout && previousLayout->Valid()) {
        double difference = 0;
        for (size_t axis = 0; axis < 3; ++axis) {
            const double delta = double(light[axis]) - previousLayout->LightDirection[axis];
            difference += delta * delta;
        }
        // Suppress float32 view/inverse-view roundoff, not actual sun motion.
        if (difference <= 1.0e-10)
            light = previousLayout->LightDirection;
    }
    const ShadowVector forward{ -light[0], -light[1], -light[2] };
    const ShadowVector upHint = std::abs(light[1]) < 0.98F ? ShadowVector{ 0, 1, 0 } : ShadowVector{ 0, 0, 1 };
    auto side = Cross(forward, upHint);
    if (!Normalize(side))
        return result;
    const auto up = Cross(side, forward);
    const float extent = std::max(maximumDistance * 4.0F, 65536.0F);
    const float depthOrigin = std::round(Dot(light, cameraWorld) / extent) * extent;
    const float inverseDepth = 1.0F / (2.0F * extent);
    result.WorldToLight = { side[0], up[0], -light[0] * inverseDepth,          0,
                            side[1], up[1], -light[1] * inverseDepth,          0,
                            side[2], up[2], -light[2] * inverseDepth,          0,
                            0,       0,     0.5F + depthOrigin * inverseDepth, 1 };
    result.CameraLight = Transform(result.WorldToLight, cameraWorld);
    result.LightDirection = light;
    result.DepthExtent = extent;
    result.AtlasResolution = atlasResolution;
    result.MaximumDistance = maximumDistance;
    // Fixed half-unit finest texels, independent of camera rotation, frustum
    // dimensions, atlas budget, and distance. Each coarser level doubles scale.
    float pageWorld = 0.5F * static_cast<float>(kVirtualShadowPageTexels);
    for (uint32_t level = 0; level < kVirtualShadowLevelCount; ++level) {
        auto& destination = result.Levels[level];
        destination.PageWorldSize = pageWorld;
        destination.OriginX = static_cast<int32_t>(std::floor(result.CameraLight[0] / pageWorld)) - 4;
        destination.OriginY = static_cast<int32_t>(std::floor(result.CameraLight[1] / pageWorld)) - 4;
        result.LevelCount = level + 1;
        // Leave one whole page of hysteresis/PCF coverage at either edge.
        if (pageWorld * 3.0F >= maximumDistance)
            break;
        pageWorld *= 2.0F;
    }
    return result;
}

size_t VirtualShadowMapCache::KeyHash::operator()(const VirtualShadowPageKey& key) const noexcept {
    return static_cast<size_t>(Mix(static_cast<uint32_t>(key.X)) ^
                               Mix(static_cast<uint64_t>(static_cast<uint32_t>(key.Y)) << 1) ^ Mix(key.Level + 1));
}

void VirtualShadowMapCache::Invalidate() noexcept {
    for (auto& page : mPages) {
        page.Valid = false;
        page.StaticValid = false;
    }
}

void VirtualShadowMapCache::Prepare(const VirtualShadowLayout& layout, std::span<const VirtualShadowCaster> casters,
                                    uint64_t frameId) {
    mStatistics = {};
    mTable.fill(0);
    mUpdates.clear();
    mStaticCasterIndices.clear();
    mDynamicCasterIndices.clear();
    if (!layout.Valid() || frameId == 0)
        return;
    if (mResolution != layout.AtlasResolution) {
        mResolution = layout.AtlasResolution;
        const uint32_t side = mResolution / kVirtualShadowPhysicalPageTexels;
        mPages.assign(side * side, {});
        mResidency.clear();
        mResidency.reserve(mPages.size());
        mUpdates.reserve(mPages.size());
    }
    if (mWorldToLight != layout.WorldToLight) {
        mWorldToLight = layout.WorldToLight;
        Invalidate();
    }
    mLightBounds.resize(casters.size());
    for (size_t index = 0; index < casters.size(); ++index)
        mLightBounds[index] = TransformShadowBounds(casters[index].WorldBounds, layout.WorldToLight);
    // Reserve the coarse fallback, then the square LOD rings actually sampled
    // by receivers (including their 2.75..3-page transition), then spare pages.
    // Reserving all surviving mappings before allocation prevents an arriving
    // page from evicting another selected page that happens to be visited later.
    std::array<bool, kVirtualShadowTableEntries> selected{};
    size_t remaining = mPages.size();
    for (uint32_t priority = 0; priority < 3; ++priority) {
        for (uint32_t level = 0; level < layout.LevelCount; ++level) {
            const auto& grid = layout.Levels[level];
            const float size = grid.PageWorldSize;
            const float guard = float(kVirtualShadowPageBorder) * size / kVirtualShadowPageTexels;
            for (const auto& [x, y] : kCenterFirstPages) {
                const VirtualShadowPageKey key{ grid.OriginX + int32_t(x), grid.OriginY + int32_t(y), level };
                const float worldX = float(key.X) * size, worldY = float(key.Y) * size;
                const float dx = (worldX - layout.CameraLight[0]) / size;
                const float dy = (worldY - layout.CameraLight[1]) / size;
                const float nearest = std::max({ dx, -dx - 1.0F, dy, -dy - 1.0F, 0.0F });
                const float farthest =
                    std::max({ std::abs(dx), std::abs(dx + 1.0F), std::abs(dy), std::abs(dy + 1.0F) });
                const bool receiverRing = nearest <= 3.0F && (level == 0 || farthest >= 1.375F);
                const uint32_t pagePriority = level + 1 == layout.LevelCount ? 0U : receiverRing ? 1U : 2U;
                if (pagePriority != priority)
                    continue;
                ++mStatistics.Requested;
                const uint32_t tableIndex = (level * kVirtualShadowGridPages + y) * kVirtualShadowGridPages + x;
                if (std::none_of(mLightBounds.begin(), mLightBounds.end(),
                                 [&](const auto& bounds) { return Overlaps(bounds, worldX, worldY, size, guard); })) {
                    mTable[tableIndex] = kVirtualShadowEmptyPage;
                    ++mStatistics.Empty;
                    continue;
                }
                if (remaining == 0) {
                    ++mStatistics.Unmapped;
                    continue;
                }
                --remaining;
                selected[tableIndex] = true;
                const auto found = mResidency.find(key);
                if (found != mResidency.end())
                    mPages[found->second].LastUsed = frameId;
            }
        }
    }
    for (uint32_t reverse = layout.LevelCount; reverse > 0; --reverse) {
        const uint32_t level = reverse - 1;
        const auto& grid = layout.Levels[level];
        const float size = grid.PageWorldSize;
        const float guard = float(kVirtualShadowPageBorder) * size / kVirtualShadowPageTexels;
        for (const auto& [x, y] : kCenterFirstPages) {
            const uint32_t tableIndex = (level * kVirtualShadowGridPages + y) * kVirtualShadowGridPages + x;
            if (!selected[tableIndex])
                continue;
            const VirtualShadowPageKey key{ grid.OriginX + static_cast<int32_t>(x),
                                            grid.OriginY + static_cast<int32_t>(y), level };
            const float worldX = static_cast<float>(key.X) * size;
            const float worldY = static_cast<float>(key.Y) * size;
            const uint32_t firstStatic = static_cast<uint32_t>(mStaticCasterIndices.size());
            const uint32_t firstDynamic = static_cast<uint32_t>(mDynamicCasterIndices.size());
            uint64_t staticSignature = 0, dynamicSignature = 0;
            for (uint32_t caster = 0; caster < casters.size(); ++caster) {
                if (!Overlaps(mLightBounds[caster], worldX, worldY, size, guard))
                    continue;
                const uint64_t version = casters[caster].ContentVersion;
                if (casters[caster].Dynamic || version == 0) {
                    mDynamicCasterIndices.push_back(caster);
                    dynamicSignature += Mix(version != 0 ? version : Mix(frameId) ^ Mix(caster + 1));
                } else {
                    mStaticCasterIndices.push_back(caster);
                    staticSignature += Mix(version);
                }
            }
            const uint32_t staticCount = static_cast<uint32_t>(mStaticCasterIndices.size()) - firstStatic;
            const uint32_t dynamicCount = static_cast<uint32_t>(mDynamicCasterIndices.size()) - firstDynamic;
            staticSignature = Mix(staticSignature ^ Mix(staticCount));
            dynamicSignature = Mix(dynamicSignature ^ Mix(dynamicCount));
            const uint64_t signature = Mix(staticSignature) ^ dynamicSignature;
            auto found = mResidency.find(key);
            uint32_t physical = 0;
            if (found != mResidency.end())
                physical = found->second;
            else {
                size_t selected = mPages.size();
                uint64_t oldest = std::numeric_limits<uint64_t>::max();
                for (size_t index = 0; index < mPages.size(); ++index) {
                    const auto& page = mPages[index];
                    if (!page.Occupied) {
                        selected = index;
                        break;
                    }
                    if (page.LastUsed != frameId && page.LastUsed < oldest) {
                        oldest = page.LastUsed;
                        selected = index;
                    }
                }
                if (selected == mPages.size()) {
                    ++mStatistics.Unmapped;
                    mStaticCasterIndices.resize(firstStatic);
                    mDynamicCasterIndices.resize(firstDynamic);
                    continue;
                }
                physical = static_cast<uint32_t>(selected);
                auto& page = mPages[physical];
                if (page.Occupied) {
                    mResidency.erase(page.Key);
                    ++mStatistics.Evicted;
                }
                page = {};
                page.Key = key;
                page.Occupied = true;
                mResidency.emplace(key, physical);
            }
            auto& page = mPages[physical];
            page.LastUsed = frameId;
            page.PendingSignature = signature;
            page.PendingStaticSignature = staticSignature;
            mTable[tableIndex] = physical + 1;
            ++mStatistics.Resident;
            const bool refreshStatic = !page.StaticValid || page.StaticSignature != staticSignature;
            if (refreshStatic)
                ++mStatistics.StaticUpdated;
            else
                ++mStatistics.StaticReused;
            if (page.Valid && page.Signature == signature) {
                ++mStatistics.Reused;
                mStaticCasterIndices.resize(firstStatic);
                mDynamicCasterIndices.resize(firstDynamic);
            } else {
                page.Valid = false;
                if (refreshStatic)
                    page.StaticValid = false;
                else
                    mStaticCasterIndices.resize(firstStatic);
                mUpdates.push_back({ key, physical, firstStatic, refreshStatic ? staticCount : 0U,
                                     firstDynamic, dynamicCount, refreshStatic });
            }
        }
    }
}

void VirtualShadowMapCache::Commit() noexcept {
    for (const auto& update : mUpdates) {
        auto& page = mPages[update.PhysicalPage];
        page.Signature = page.PendingSignature;
        if (update.RefreshStatic) {
            page.StaticSignature = page.PendingStaticSignature;
            page.StaticValid = true;
        }
        page.Valid = true;
    }
}

} // namespace Fast::Renderer3ds
