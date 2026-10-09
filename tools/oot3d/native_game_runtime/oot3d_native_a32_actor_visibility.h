#pragma once

// Applies the actor visibility policy to the EUR title's actor culling and
// distance checks at existing A32 block entries. Gameplay state is changed
// only where the title itself decides visibility or spawn distance.

#include "oot3d_actor_visibility_policy.h"
#include "fast/oot3d/scene_presentation_policy.h"
#include "recomp/a32_runtime.h"

#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

class NativeA32Memory;

// Actor_UpdateCulling (0x00461904, r0 = play) walks every actor list. Its
// result block: r4 = actor, r0 = native result (0 culled), r1 = the previous
// culling state, r7 = play + 0x2000; actor+0x120 already holds the new state.
inline constexpr uint32_t kActorCullingEntry = 0x00461904U;
inline constexpr uint32_t kActorCullingResultHook = 0x00461AC4U;
// Obj_Mure spawn and active actions, the only readers of its distance table.
inline constexpr uint32_t kObjMureSpawnEntry = 0x003BC5DCU;
inline constexpr uint32_t kObjMureActiveEntry = 0x003CC298U;
// Obj_Mure2 spawn and active actions: r0 = actor, r1 = play; the update has
// just stored the native distance scalar (1, or 4 in cutscenes) at +0x1DC.
inline constexpr uint32_t kObjMure2SpawnEntry = 0x003B30ACU;
inline constexpr uint32_t kObjMure2ActiveEntry = 0x003AEF48U;
// En_Wood02 offspring: r0 = the main spawn-zone test, r4 = the spawner, whose
// +0xEC..+0xF4 hold the candidate's projection and +0xF8 its inverse W;
// r11 = play + 0x2000.
inline constexpr uint32_t kEnWood02SpawnZoneResult = 0x0034E854U;
// En_Ko fade branches: s0 = the distance compared with the appear distance.
inline constexpr uint32_t kEnKoFadeDistanceA = 0x001B5FE0U;
inline constexpr uint32_t kEnKoFadeDistanceB = 0x001B60E8U;
// Actor_UpdateAlphaByDistance (0x00341C28), Mido's and Saria's fade, after its
// distance: r4 = actor, s0 = distance, s16 = radius. Mido's house and Saria's
// cutscene action set full alpha without reaching it.
inline constexpr uint32_t kAlphaByDistanceCompare = 0x00341CD8U;

struct NativeActorVisibilityState {
    // Snapshot of the settings and presentation, taken between host frames.
    bool ExtendedDrawDistance = false;
    // The presented view: the frustum hook widens every perspective the
    // GPU draws with, but the title builds its culling matrix separately.
    Fast::Oot3d::ScenePresentationPolicy Presentation{};
    float FovMultiplier = 1.0F;
    // The presented volume of the current culling walk, from its play state.
    ActorCullingVolume CullingVolume{};
    bool CullingVolumeValid = false;
    // Host-owned writes to the title's read-only distance constants.
    NativeA32Memory* HostMemory = nullptr;
    bool Diagnostics = false;
    bool ReportedUnknownConstants = false;
    uint64_t NativeVisible = 0U;
    uint64_t NativeCulled = 0U;
    uint64_t Extended = 0U;
};

[[nodiscard]] std::span<const uint32_t> NativeActorVisibilityHookPcs() noexcept;

// Returns true when the pc belongs to this policy.
bool ApplyNativeActorVisibilityHook(
    uint32_t pc, oot3d::recomp::a32::GuestState& state,
    oot3d::recomp::a32::MemoryBus& memory,
    NativeActorVisibilityState& visibility);

} // namespace Oot3dNativeGame
