# Phase 0 Research: Co-op Dog Rupee Pickup

**Feature**: `001-fix-dog-rupee-pickup`
**Date**: 2026-10-23
**Status**: Complete

## Method

- Inspected the current co-op runtime, guest layout constants, and actor-list traversal.
- Compared the runtime's rupee detection against the repository's own `ACTOR_EN_ITEM00` scene records.
- Consulted the N64 OOT decompilation for the structural pickup contract (`Item00Type`, `EnItem00_Update`, `Rupees_ChangeBy`, wallet accumulator).
- Checked the repository's OOT3D UI layout for the exact SaveContext wallet and upgrade offsets.
- Treated N64 behavior as structural evidence, not as proof of OOT3D constants, per `docs/OOT3D_GAMEPLAY_DIFFERENCES_AND_NATIVE_PREROGATIVES.md`.

## R1 — Why the dog passes through rupees (root cause)

**Decision**: The current code never matches a rupee because it compares the wrong field encoding.

**Evidence**:

- `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp:1272-1281` matches `params & 0xFF` against `0x84U..0x88U` and maps those to 1/5/20/50/200.
- `0x84..0x88` are `ITEM_RUPEE_GREEN..ITEM_RUPEE_GOLD` item IDs (`tools/oot3d/decomp_support/include/oot3d/actor_object_semantics.h:1182`), not `En_Item00` params.
- The repository's scene data stores `ACTOR_EN_ITEM00` with low-byte item codes. Examples from `tools/oot3d/decomp_support/src/assets/scenes/spot04_info.c:1397-1403`:
  - `0x1201` → low byte `0x01` (blue rupee)
  - `0x1101` → low byte `0x01` (blue rupee)
  - `0x1C03` → low byte `0x03` (recovery heart)
- Across all checked-in scene files the observed low bytes are `0x00`, `0x01`, `0x02`, `0x03`, `0x06`, `0x11`. The `0x84+` IDs never appear.
- The reference `Item00Type` table (`zeldaret/oot`, `include/z_en_item00.h`) defines:
  - `0x00 ITEM00_RUPEE_GREEN`
  - `0x01 ITEM00_RUPEE_BLUE`
  - `0x02 ITEM00_RUPEE_RED`
  - `0x13 ITEM00_RUPEE_ORANGE`
  - `0x14 ITEM00_RUPEE_PURPLE`
  - `0x11 ITEM00_SMALL_KEY` (which explains the dungeon placements)
- Therefore `amount` is always zero for a real rupee and the actor is never credited or removed.

**Rationale**: This is a pure detection bug, not a distance or movement bug. At 200 world units/s sprint and a 30-unit radius, the dog moves far less than the radius per frame, so tunneling is not the cause.

**Alternatives considered**: Increasing `rupee_pickup_range` (rejected — the lookup returns zero before distance matters); matching by actor category only (rejected — would collect hearts, keys, and ammo).

## R2 — Wallet-full behavior in the reference game

**Decision**: Keep the rupee in the world when the wallet cannot hold its value, and document this as a safety-first deviation pending OOT3D evidence.

**Evidence**:

- The N64 reference `Rupees_ChangeBy` only adds to `gSaveContext.rupeeAccumulator`; the wallet is drained one unit per frame in `Interface_Update`.
- When the wallet reaches capacity, `Interface_Update` zeroes the accumulator and does not credit the remainder. The rupee actor has already been marked collected, so N64 effectively destroys the over-cap amount.
- OOT3D's `Rupees_ChangeBy` is a 20-byte function in the repository inventory (`tools/oot3d/native_a32_runtime/upstream/analysis/codebin_function_inventory.csv:6935`), consistent with the same accumulator-only implementation. OOT3D parity for the full-wallet case is not proven.

**Rationale**: The project's native-prerogative policy allows a deliberate safety improvement when OOT3D behavior is unverified, provided it is documented. Losing a rupee is a negative player outcome that is not required by the user request.

**Alternatives considered**: Always consume (exact N64 behavior, but unverified for OOT3D and worse for players); show a custom message (rejected — new UI scope).

**Follow-up**: If OOT3D is proven to consume the over-cap rupee, change the guard in a separate, evidence-backed task.

## R3 — Pickup gate (when a rupee is collectible)

**Decision**: Mirror Link's gate: horizontal distance ≤ 30, vertical distance in `[-50, +50]`, the actor's collectible delay elapsed, gameplay active, and no game-over/cutscene state.

**Evidence** (N64 reference `src/code/z_en_item00.c`):

- `EnItem00_Update` returns early while `this->unk_154 > 0` (the 35-frame spawn delay used by dropped rupees).
- Collection requires `xzDistToPlayer <= 30.0f` and `yDistToPlayer` in `[-50.0f, +50.0f]`.
- Collection is skipped while `play->gameOverCtx.state != GAMEOVER_INACTIVE`.
- Scene-placed rupees have `unk_154 == 0`; dropped rupees start with `unk_154 = 35`.

**OOT3D adaptation**:

- The OOT3D `Actor` base is `0x1A4` bytes (`zeldaret/oot3d` `z_en_item00.hpp`). Applying the N64 `EnItem00` post-actor field order gives the expected OOT3D offsets: `actionFunc 0x1A4`, `collectibleFlag 0x1A8`, `getItemId 0x1AA`, `unk_154 0x1AC`, `unk_156 0x1AE`, `unk_158 0x1B0`, `despawnTimer 0x1B2`, `scale 0x1B4`.
- The expected `unk_154` offset (`0x1AC`) MUST be confirmed with a bounded runtime probe: read it as `s16` while a dropped rupee is present and observe it counting down from 35 to 0. If the offset differs, record the observed offset in `data-model.md` before merging.
- Fallback if the offset cannot be confirmed: only collect when the actor is grounded (`bgCheckFlags & 1`) and `gravity == 0`, and require 35 guest frames since first observation for actors that spawned with gravity. This never collects a falling drop early, at the cost of a short delay on scripted airborne spawns.
- `BeforeGuest` already returns unless `play mode == 3` (`oot3d_coop_dog.cpp:720`), which covers pause/menu/loading. Add a cutscene guard using `SaveContext.cutsceneIndex` (`+0x08`) and a player-health guard (`SaveContext+0x44 != 0`) for the game-over path.

**Alternatives considered**: Keep the existing 45/60 tolerances (rejected — looser than Link and can grab rupees on adjacent ledges); collect immediately regardless of state (rejected — violates FR-005).

## R4 — How to credit the wallet and remove the rupee

**Decision**: Call `Rupees_ChangeBy(value)` (`0x00376A60`) through `InvokeFunction`, then call `Actor_Kill(rupee)` (`0x00374428`) only after the credit path is valid. Do not call `Item_Give` from the host.

**Evidence**:

- The N64 `EnItem00_Update` calls `Item_Give(play, ITEM_RUPEE_*)`; `Item_Give`'s rupee branch calls `Rupees_ChangeBy(value)`.
- `Rupees_ChangeBy` is a small, side-effect-only guest routine already used by the current co-op code.
- `Item_Give` is a 3888-byte general item routine that can spawn actors and touch audio/inventory (`codebin_function_inventory.csv:6936`). Invoking it from the host is riskier than the narrow `Rupees_ChangeBy`.
- `Audio_PlayActorSound2` already faults when invoked from the host in this runtime (`oot3d_coop_dog.cpp:1180-1183`), so guest audio calls are not a safe path.
- The HUD wallet counter drains the accumulator with the normal rupee-count ticks and flash, providing feedback without an extra host-to-guest audio call.

**Rationale**: Smallest safe call that produces the required wallet and HUD behavior. It also keeps the change reviewable and avoids reimplementing wallet logic. Because the reference routine is accumulator-based and effectively void, the implementation must gate on the capacity check rather than on a return code.

**Alternatives considered**:
- `Item_Give`: closer to Link's call site, but larger host invocation, may trigger item-get/actor-spawn behavior, and does not itself play the pickup ding.
- Direct SaveContext writes: rejected — bypasses accumulator/HUD semantics and risks save corruption.
- Guest `Audio_PlaySoundGeneral`: rejected as a hard dependency; if the initial pickup ding is required, use the host `NativeAudioService::PlaySoundGeneral`, not a guest call.

## R5 — Wallet and capacity offsets

**Decision**: Read wallet balance and capacity from the existing OOT3D SaveContext layout; do not add new persistence.

**Evidence**:

- SaveContext base `0x00587958` (`oot3d_dev_warp.cpp`, `guest_ui_layout.h`).
- `save_context::kRupees = 0x48` (`guest_ui_layout.h:62`), s16.
- `save_context::kInventory = 0x8C` (`guest_ui_layout.h:73`); `Inventory.upgrades` is at `+0x2C` (`guest_ui_layout.h:39`), therefore SaveContext `+0xB8`.
- `UPG_WALLET = 4` (`actor_object_semantics.h:979`); shift `12`, mask `0x3000` (`ui_inventory_content.cpp:18-23`).
- Capacity table `{99, 200, 500, 500}` (`ui_inventory_content.cpp:25-33`).
- Pending credit is tracked by `Rupees_ChangeBy`'s accumulator; the capacity guard should include it when known, or conservatively use the current wallet balance.

**Validation**: The probe writes `wallet` and `wallet_capacity`; a test fills the wallet via a private save seed and confirms the guard.

## R6 — Double-credit and removal safety

**Decision**: Use a per-scene `std::unordered_set<uint32_t>` of credited rupee actors, re-validate list membership and actor id immediately before each guest call, and clear the set on play/scene change.

**Evidence**:

- Actor lists are traversed by `CollectRupees` and by the guest in the same frame.
- `Actor_Kill` sets the actor's update function to null, so the guest should not process it again, but the host must not rely on that if the call fails.
- The existing `ActorInCurrentLists` helper already provides the membership check used by dog validation.

**Rationale**: Prevents duplicate wallet credit if `Actor_Kill` is rejected or the actor remains reachable for one frame, and satisfies FR-003/FR-006.

**Alternatives considered**: Immediate kill without dedup (rejected — a failed kill can double-credit); mutating `params` to a sentinel (rejected — visible to the guest and not reversible).

## R7 — Observable evidence

**Decision**: Extend `triaevum.coop.probe.v1` with backward-compatible fields and add a dedicated scenario.

**Evidence**:

- `MaybeWriteProbe` already emits `dog_actor`, `dog_usable`, `dog_pos`, `scene`, and `mode` (`oot3d_coop_dog.cpp:539-589`).
- `analyze_evidence.py` already reads the probe and supports `--require-dog`.
- The harness supports scripted P2 input and framebuffer capture (`docs/GAME_TESTING.md`).

**New fields**: `wallet`, `wallet_capacity`, `rupees_collected`, `last_rupee_value`, `nearby_rupees`.

**Scenario**: place the dog near a known rupee, walk over it, and assert `rupees_collected >= 1`, `wallet` delta equals the denomination, and no runtime fault. A second scenario fills the wallet and verifies the rupee remains.

**Alternatives considered**: Rely only on screenshots (rejected — not deterministic enough for wallet accounting); parse `run.log` (rejected — logs are optional and may be localized).

## R8 — OOT3D authority and remaining verification

**Decision**: Keep N64 as the structural reference and verify OOT3D constants at runtime before claiming parity.

**Remaining verification items**:

1. Confirm the OOT3D `EnItem00::unk_154` offset (`0x1AC` expected) and the 35-frame drop delay.
2. Confirm that OOT3D `Rupees_ChangeBy` has the accumulator semantics and that the HUD drains it.
3. Confirm the rupee value map for `0x13` (orange → 200 via `ITEM_RUPEE_GOLD`) and `0x14` (purple → 50). The scene files only exercise green/blue/red; large rupees will be verified with a private save/state or a controlled spawn.
4. Confirm that `Actor_Kill` on the rupee does not drop a duplicate collectible or leave a dangling list node.

These are runtime checks, not design blockers. If any fails, the fallback behavior is documented in the task and no parity claim is made.

## Resolved Unknowns

| Unknown | Resolution |
| --- | --- |
| Why the dog ignores rupees | Wrong params encoding (`0x84..0x88` vs `Item00Type` low byte) |
| Correct rupee codes | `0x00=1`, `0x01=5`, `0x02=20`, `0x13=200`, `0x14=50` |
| Link's pickup distance | Horizontal 30, vertical ±50 |
| How to credit currency | `Rupees_ChangeBy` (existing guest routine), HUD drains accumulator |
| How to avoid lost rupees when full | Capacity guard from SaveContext upgrade bits; leave the rupee |
| How to prove it | Probe fields + deterministic scenario + `game-tester` evidence |
| Which files change | `oot3d_coop_dog.cpp`, new `oot3d_coop_rupee.h`, tests, probe, scenario, docs |

No `NEEDS CLARIFICATION` items remain in the spec or plan.
