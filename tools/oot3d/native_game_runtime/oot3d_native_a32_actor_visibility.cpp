#include "oot3d_native_a32_actor_visibility.h"

#include "fast/oot3d/perspective_fov_policy.h"
#include "oot3d_native_a32_memory.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace Oot3dNativeGame {
namespace {

using oot3d::recomp::a32::GuestState;
using oot3d::recomp::a32::MemoryBus;

constexpr uint32_t kActorProjectedPosition = 0xECU;
constexpr uint32_t kActorProjectedW = 0xF8U;
constexpr uint32_t kActorUncullForward = 0xFCU;
constexpr uint32_t kActorUncullScale = 0x100U;
constexpr uint32_t kActorUncullDownward = 0x104U;
constexpr uint32_t kActorCullingState = 0x120U;
constexpr uint32_t kObjMure2DistanceScalar = 0x1DCU;
// En_Viewer hides behind culling during cutscene camera pans (SoH skips it).
constexpr uint16_t kActorEnViewer = 0x002AU; // ACTOR_EN_VIEWER, actor_object_semantics.h
// The Kokiri SoH stops fading: En_Ko has its own fade, En_Md (Mido) and
// En_Sa (Saria) share Actor_UpdateAlphaByDistance.
constexpr uint16_t kActorEnSa = 0x0146U;
constexpr uint16_t kActorEnMd = 0x016DU;

// The view-projection the title culls actors with (play+0x5BB4, row-major).
constexpr uint32_t kPlayCullingViewProjection = 0x5BB4U;

// Obj_Mure's spawn distances (sZClip: 1600, 1600, 1000, 1000, 1000) and the
// 40 its retirement adds, each read from guest memory at every use. SoH
// scales both the spawn and the retirement distance by 5.
constexpr uint32_t kObjMureDistanceTable = 0x005354B8U;
constexpr uint32_t kObjMureRetireMargin = 0x003CC430U;
constexpr std::array kObjMureNativeDistances{1600.0F, 1600.0F, 1000.0F, 1000.0F, 1000.0F, 40.0F};

// Both the culling walk and En_Wood02 keep play + 0x2000 in a register.
constexpr uint32_t kPlayHighHalf = 0x2000U;

constexpr std::array kHookPcs{
    kEnKoFadeDistanceA, kEnKoFadeDistanceB, kAlphaByDistanceCompare, kEnWood02SpawnZoneResult, kObjMure2ActiveEntry,
    kObjMure2SpawnEntry, kObjMureSpawnEntry, kObjMureActiveEntry, kActorCullingEntry,
    kActorCullingResultHook,
};

bool ReadFloat(MemoryBus& memory, uint32_t address, float& value) {
    uint32_t bits = 0U;
    if (!memory.Read32(address, &bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool ReadCullingSample(MemoryBus& memory, uint32_t actor, ActorCullingSample& sample) {
    return ReadFloat(memory, actor + kActorProjectedPosition + 0U, sample.X) &&
           ReadFloat(memory, actor + kActorProjectedPosition + 4U, sample.Y) &&
           ReadFloat(memory, actor + kActorProjectedPosition + 8U, sample.Z) &&
           ReadFloat(memory, actor + kActorProjectedW, sample.W) &&
           ReadFloat(memory, actor + kActorUncullForward, sample.Forward) &&
           ReadFloat(memory, actor + kActorUncullScale, sample.Scale) &&
           ReadFloat(memory, actor + kActorUncullDownward, sample.Downward);
}

// How much wider and taller the presented view is than the culling
// projection. The x and y rows of the culling view-projection scale by the
// inverse half-extent tangents (their spatial norms, as the view is rigid);
// the presented view is that frustum after ResolvePerspectiveFov, as the
// frustum hook applies it to the rendered camera.
ActorCullingVolume PresentedVolume(MemoryBus& memory, uint32_t play,
                                   const NativeActorVisibilityState& visibility) {
    ActorCullingVolume volume;
    volume.DistanceMultiplier =
        visibility.ExtendedDrawDistance ? kExtendedActorDistanceMultiplier : 1.0F;
    if (play == 0U) return volume;
    std::array<float, 8> rows{};
    for (uint32_t i = 0; i < rows.size(); ++i)
        if (!ReadFloat(memory, play + kPlayCullingViewProjection + i * 4U, rows[i])) return volume;
    const float scaleX = std::sqrt(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);
    const float scaleY = std::sqrt(rows[4] * rows[4] + rows[5] * rows[5] + rows[6] * rows[6]);
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 1.0e-6F || scaleY <= 1.0e-6F)
        return volume;
    const auto resolved = Fast::Oot3d::ResolvePerspectiveFov({
        {-1.0F / scaleX, 1.0F / scaleX, -1.0F / scaleY, 1.0F / scaleY, 1.0F, 2.0F},
        visibility.FovMultiplier,
        visibility.Presentation,
        true,
    });
    volume.Horizontal = resolved.HorizontalScale;
    volume.Vertical = resolved.VerticalScale;
    return volume;
}

bool Extends(const ActorCullingVolume& volume) {
    return volume.DistanceMultiplier > 1.0F || volume.Horizontal > 1.0F || volume.Vertical > 1.0F;
}

void ApplyActorCulling(GuestState& state, MemoryBus& memory, NativeActorVisibilityState& visibility) {
    const uint32_t actor = state.r[4];
    if (state.r[0] != 0U) {
        ++visibility.NativeVisible;
        return;
    }
    ++visibility.NativeCulled;
    const auto volume = visibility.CullingVolumeValid
        ? visibility.CullingVolume
        : PresentedVolume(memory, state.r[7] - kPlayHighHalf, visibility);
    if (visibility.Diagnostics && visibility.NativeCulled % 4096U == 1U)
        std::fprintf(stderr, "[actor-visibility] presented=%.4f,%.4f visible=%llu culled=%llu extended=%llu\n",
                     volume.Horizontal, volume.Vertical,
                     static_cast<unsigned long long>(visibility.NativeVisible),
                     static_cast<unsigned long long>(visibility.NativeCulled),
                     static_cast<unsigned long long>(visibility.Extended));
    if (!Extends(volume)) return;
    uint16_t id = 0U;
    ActorCullingSample sample;
    uint8_t nativeState = 0U;
    if (!memory.Read16(actor, &id) || id == kActorEnViewer || !ReadCullingSample(memory, actor, sample) ||
        !memory.Read8(actor + kActorCullingState, &nativeState))
        return;
    const auto classified = ClassifyExtendedActorCulling(sample, volume);
    const auto next = static_cast<uint8_t>(classified);
    // A pass is the title's own: state 0 and a true result. The fade that
    // follows ramps in from state 2 and fills at once from state 1. A
    // failure inside the extended depth is no longer a far one.
    if (next != nativeState && !memory.Write8(actor + kActorCullingState, next)) return;
    if (classified == ActorCullingClass::Inside) {
        state.r[0] = 1U;
        ++visibility.Extended;
    }
}

// Writes the Obj_Mure distances its readers are about to use: native, or
// SoH's 5x. Known values only, so another title revision is left alone.
void AssertObjMureDistances(NativeActorVisibilityState& visibility) {
    if (visibility.HostMemory == nullptr) return;
    auto& memory = *visibility.HostMemory;
    const float scale = visibility.ExtendedDrawDistance ? kExtendedActorDistanceMultiplier : 1.0F;
    const auto address = [](size_t i) {
        return i + 1U < kObjMureNativeDistances.size()
            ? kObjMureDistanceTable + static_cast<uint32_t>(i) * 4U
            : kObjMureRetireMargin;
    };
    for (size_t i = 0; i < kObjMureNativeDistances.size(); ++i) {
        uint32_t bits = 0U;
        const float native = kObjMureNativeDistances[i];
        const auto wanted = std::bit_cast<uint32_t>(native * scale);
        if (!memory.Read32(address(i), &bits)) return;
        if (bits == wanted) continue;
        if (bits != std::bit_cast<uint32_t>(native) &&
            bits != std::bit_cast<uint32_t>(native * kExtendedActorDistanceMultiplier)) {
            if (!visibility.ReportedUnknownConstants)
                std::fprintf(stderr, "[actor-visibility] Obj_Mure distance %08x at %08x is not the EUR value\n",
                             bits, address(i));
            visibility.ReportedUnknownConstants = true;
            return;
        }
        (void)memory.WriteHost<uint32_t>(address(i), wanted);
    }
}

void ApplyObjMure2(GuestState& state, MemoryBus& memory, const NativeActorVisibilityState& visibility) {
    const uint32_t actor = state.r[0];
    float scalar = 0.0F;
    float x = 0.0F;
    float z = 0.0F;
    if (!ReadFloat(memory, actor + kObjMure2DistanceScalar, scalar) ||
        !ReadFloat(memory, actor + kActorProjectedPosition + 0U, x) ||
        !ReadFloat(memory, actor + kActorProjectedPosition + 8U, z))
        return;
    // The update stores 1 or 4 before every action; anything else is ours.
    if (scalar != 1.0F && scalar != 4.0F) return;
    const auto volume = PresentedVolume(memory, state.r[1], visibility);
    const float adjusted =
        ObjMure2DistanceScalar(scalar, x, z, volume.Horizontal, visibility.ExtendedDrawDistance);
    if (adjusted != scalar && std::isfinite(adjusted))
        (void)memory.Write32(actor + kObjMure2DistanceScalar, std::bit_cast<uint32_t>(adjusted));
}

void ApplyEnWood02(GuestState& state, MemoryBus& memory, const NativeActorVisibilityState& visibility) {
    // The title reciprocates the stored inverse W and multiplies by that, so
    // its lateral test seldom passes and offspring follow the forward/3
    // fallback; that is left native. Extended, offspring spawn in SoH's
    // extended volume with the correct normalisation.
    if (!visibility.ExtendedDrawDistance || state.r[0] != 0U) return;
    const uint32_t spawner = state.r[4];
    ActorCullingSample sample;
    float inverseW = 0.0F;
    if (!ReadCullingSample(memory, spawner, sample) || !ReadFloat(memory, spawner + kActorProjectedW, inverseW) ||
        !(inverseW > 0.0F))
        return;
    sample.W = 1.0F / inverseW;
    if (ActorInsideExtendedCullingVolume(sample, PresentedVolume(memory, state.r[11] - kPlayHighHalf, visibility)))
        state.r[0] = 1U;
}

} // namespace

std::span<const uint32_t> NativeActorVisibilityHookPcs() noexcept {
    return kHookPcs;
}

bool ApplyNativeActorVisibilityHook(
    uint32_t pc, GuestState& state, MemoryBus& memory,
    NativeActorVisibilityState& visibility) {
    switch (pc) {
    case kActorCullingEntry:
        // The culling matrix is fixed for the walk: resolve its view once.
        visibility.CullingVolume = PresentedVolume(memory, state.r[0], visibility);
        visibility.CullingVolumeValid = true;
        return true;
    case kActorCullingResultHook:
        ApplyActorCulling(state, memory, visibility);
        return true;
    case kObjMureSpawnEntry:
    case kObjMureActiveEntry:
        AssertObjMureDistances(visibility);
        return true;
    case kObjMure2SpawnEntry:
    case kObjMure2ActiveEntry:
        ApplyObjMure2(state, memory, visibility);
        return true;
    case kEnWood02SpawnZoneResult:
        ApplyEnWood02(state, memory, visibility);
        return true;
    case kEnKoFadeDistanceA:
    case kEnKoFadeDistanceB:
        // Distance 0 is inside any appear distance: the Kokiri fade in with
        // the title's own smoothing and stay.
        if (visibility.ExtendedDrawDistance) state.vfp[0] = 0U;
        return true;
    case kAlphaByDistanceCompare: {
        // Distance 0 takes the near branch: the title raises the alpha to
        // 255 with its own smoothing.
        uint16_t id = 0U;
        if (visibility.ExtendedDrawDistance && memory.Read16(state.r[4], &id) &&
            (id == kActorEnMd || id == kActorEnSa))
            state.vfp[0] = 0U;
        return true;
    }
    default:
        return false;
    }
}

} // namespace Oot3dNativeGame
