#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace Fast::Renderer3ds {

inline constexpr uint32_t kVirtualShadowPageTexels = 128;
inline constexpr uint32_t kVirtualShadowPageBorder = 3;
inline constexpr uint32_t kVirtualShadowPhysicalPageTexels = kVirtualShadowPageTexels + 2 * kVirtualShadowPageBorder;
inline constexpr uint32_t kVirtualShadowGridPages = 8;
inline constexpr uint32_t kVirtualShadowLevelCount = 10;
inline constexpr uint32_t kVirtualShadowTableEntries =
    kVirtualShadowGridPages * kVirtualShadowGridPages * kVirtualShadowLevelCount;
inline constexpr uint32_t kVirtualShadowEmptyPage = 0xffffffffU;

using ShadowMatrix = std::array<float, 16>;
using ShadowVector = std::array<float, 3>;

struct ShadowBounds {
    ShadowVector Minimum{};
    ShadowVector Maximum{};
    bool Available = false;
};

struct VirtualShadowCaster {
    ShadowBounds WorldBounds;
    // Includes geometry, the world-space deformation, and raster state. Zero
    // means that reuse cannot be proved and this caster invalidates its pages.
    uint64_t ContentVersion = 0;
};

struct VirtualShadowLevel {
    int32_t OriginX = 0;
    int32_t OriginY = 0;
    float PageWorldSize = 0.0F;
    uint32_t Reserved = 0;
};
static_assert(sizeof(VirtualShadowLevel) == 16);

struct VirtualShadowLayout {
    // XY are world-anchored light-plane coordinates; Z is normalized depth.
    ShadowMatrix WorldToLight{};
    ShadowVector LightDirection{};
    ShadowVector CameraLight{};
    std::array<VirtualShadowLevel, kVirtualShadowLevelCount> Levels{};
    uint32_t LevelCount = 0;
    uint32_t AtlasResolution = 0;
    float DepthExtent = 0.0F;
    float MaximumDistance = 0.0F;

    [[nodiscard]] bool Valid() const noexcept {
        return LevelCount != 0;
    }
};

[[nodiscard]] VirtualShadowLayout
BuildVirtualShadowLayout(const ShadowVector& cameraWorld, const ShadowVector& directionTowardLight,
                         uint32_t atlasResolution, float maximumDistance,
                         const VirtualShadowLayout* previousLayout = nullptr) noexcept;

[[nodiscard]] ShadowBounds TransformShadowBounds(const ShadowBounds& bounds, const ShadowMatrix& transform) noexcept;

struct VirtualShadowPageKey {
    int32_t X = 0;
    int32_t Y = 0;
    uint32_t Level = 0;
    bool operator==(const VirtualShadowPageKey&) const = default;
};

struct VirtualShadowPageUpdate {
    VirtualShadowPageKey Key;
    uint32_t PhysicalPage = 0;
    // Index into the immutable, frame-local caster list.
    uint32_t FirstCaster = 0;
    uint32_t CasterCount = 0;
};

struct VirtualShadowPageStatistics {
    uint32_t Requested = 0;
    uint32_t Resident = 0;
    uint32_t Reused = 0;
    uint32_t Empty = 0;
    uint32_t Evicted = 0;
    uint32_t Unmapped = 0;
};

// CPU-owned residency metadata. The graph-owned atlas is the only GPU resource;
// the immutable table is uploaded with the receiver uniform snapshot. Prepare
// never claims dirty pages as valid cache entries until Commit succeeds.
class VirtualShadowMapCache final {
  public:
    void Prepare(const VirtualShadowLayout& layout, std::span<const VirtualShadowCaster> casters, uint64_t frameId);
    void Commit() noexcept;
    void Invalidate() noexcept;
    [[nodiscard]] const std::array<uint32_t, kVirtualShadowTableEntries>& Table() const noexcept {
        return mTable;
    }
    [[nodiscard]] std::span<const VirtualShadowPageUpdate> Updates() const noexcept {
        return mUpdates;
    }
    [[nodiscard]] std::span<const uint32_t> Casters(const VirtualShadowPageUpdate& page) const noexcept {
        return std::span(mCasterIndices).subspan(page.FirstCaster, page.CasterCount);
    }
    [[nodiscard]] const VirtualShadowPageStatistics& Statistics() const noexcept {
        return mStatistics;
    }

  private:
    struct KeyHash {
        size_t operator()(const VirtualShadowPageKey& key) const noexcept;
    };
    struct PhysicalPage {
        VirtualShadowPageKey Key;
        uint64_t Signature = 0;
        uint64_t PendingSignature = 0;
        uint64_t LastUsed = 0;
        bool Occupied = false;
        bool Valid = false;
    };
    ShadowMatrix mWorldToLight{};
    uint32_t mResolution = 0;
    std::vector<PhysicalPage> mPages;
    std::unordered_map<VirtualShadowPageKey, uint32_t, KeyHash> mResidency;
    std::array<uint32_t, kVirtualShadowTableEntries> mTable{};
    std::vector<VirtualShadowPageUpdate> mUpdates;
    std::vector<uint32_t> mCasterIndices;
    std::vector<ShadowBounds> mLightBounds;
    VirtualShadowPageStatistics mStatistics;
};

} // namespace Fast::Renderer3ds
