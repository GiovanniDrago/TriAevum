# Data Model: Co-op Dog Rupee Pickup

**Feature**: `001-fix-dog-rupee-pickup`
**Date**: 2026-10-23

This feature adds no persisted data. It reads and writes existing guest state and adds derived probe fields. The model below describes the runtime entities used by the pickup logic and the evidence contract.

## 1. Rupee Actor (`En_Item00` rupee variant)

| Field | Source | Type | Notes |
| --- | --- | --- | --- |
| `actorAddress` | actor-list node | `u32` | Guest address; used as the dedup key |
| `actorId` | `actor + 0x00` | `u16` | Must be `0x0015` (`ACTOR_EN_ITEM00`) |
| `itemCode` | `actor + 0x1C` low byte | `u8` | `Item00Type`; `0x00/0x01/0x02/0x13/0x14` are rupees |
| `worldPos` | `actor + 0x28` | `f32[3]` | Used for the distance gate |
| `velocity` | `actor + 0x60` | `f32[3]` | Used to reject falling drops in the fallback gate |
| `gravity` | `actor + 0x70` | `f32` | Non-zero for dropped items |
| `bgCheckFlags` | `actor + 0x90` | `u16` | Bit 0 = grounded |
| `collectibleDelay` | `actor + 0x1AC` (expected) | `s16` | `> 0` means "not collectible yet" |
| `listMembership` | actor context | derived | Must be present in one of the 12 current lists at call time |

### Item code → value

| `itemCode` | Reference name | Value |
| --- | --- | --- |
| `0x00` | `ITEM00_RUPEE_GREEN` | 1 |
| `0x01` | `ITEM00_RUPEE_BLUE` | 5 |
| `0x02` | `ITEM00_RUPEE_RED` | 20 |
| `0x13` | `ITEM00_RUPEE_ORANGE` → `ITEM_RUPEE_GOLD` | 200 |
| `0x14` | `ITEM00_RUPEE_PURPLE` | 50 |
| other | heart, key, ammo, etc. | not a rupee → skip |

`0x11` is `ITEM00_SMALL_KEY`, not a rupee, and must be ignored. This is important because dungeon scenes contain many `0x11` records.

## 2. P2 Dog Companion

| Field | Source | Type | Notes |
| --- | --- | --- | --- |
| `actorAddress` | runtime | `u32` | Must be usable |
| `worldPos` | `actor + 0x28` | `f32[3]` | Current host-published position |
| `usable` | runtime | `bool` | Existing `IsDogUsable` check (id, draw, list membership) |
| `active` | runtime | `bool` | P2 is joined and the dog is present |

The pickup runs only after `BeforeGuest` has produced a valid dog position for the current frame.

## 3. Shared Wallet

| Field | Source | Type | Notes |
| --- | --- | --- | --- |
| `balance` | SaveContext `0x00587958 + 0x48` | `s16` | Current rupee count |
| `capacity` | SaveContext `+0xB8` upgrade bits | `u16` | `UPG_WALLET` level → `{99, 200, 500, 500}` |
| `pending` | rupee accumulator (if exposed) | `s16` | Credit queued by `Rupees_ChangeBy`; included in the guard when readable |

Capacity derivation:

```text
upgrades = u32 at 0x00587958 + 0x8C + 0x2C
level    = (upgrades >> 12) & 0x3     // UPG_WALLET shift/mask
capacity = {99, 200, 500, 500}[min(level, 3)]
```

## 4. Pickup Attempt (transient)

| Field | Type | Notes |
| --- | --- | --- |
| `rupeeAddress` | `u32` | Candidate actor |
| `value` | `u16` | Computed from `itemCode` |
| `distanceXZ` | `f32` | Must be ≤ 30.0 |
| `distanceY` | `f32` | Must be in `[-50.0, +50.0]` |
| `collectibleDelay` | `s16` | Must be ≤ 0 |
| `walletBefore` | `s16` | Snapshot before the guest call |
| `walletCapacity` | `u16` | Snapshot before the guest call |
| `outcome` | `enum` | `Credited`, `RejectedFullWallet`, `RejectedNotCollectible`, `RejectedInvalidActor` |

Only `Credited` triggers `Actor_Kill` and increments `rupees_collected`.

## 5. Probe Sample

Existing `triaevum.coop.probe.v1` fields are preserved. New fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `wallet` | `int` | SaveContext rupee balance |
| `wallet_capacity` | `int` | Derived capacity |
| `rupees_collected` | `int` | Monotonic count of successful dog pickups in the run |
| `last_rupee_value` | `int` | Value of the most recent successful pickup |
| `nearby_rupees` | `int` | Eligible rupee actors within the gate in the sampled frame |

## 6. State Transitions

### Scene-placed rupee (collectible immediately)

```text
Spawned (delay = 0)
  → Available
  → [dog within gate and wallet has room] → Credited → Removed
  → [dog within gate and wallet full]     → RejectedFullWallet → Available (expected next frames)
  → [actor invalid or not in lists]       → Skipped (no mutation)
```

### Dropped rupee (spawns airborne, delay = 35)

```text
Spawned (delay = 35, gravity != 0, airborne)
  → Falling (not collectible)
  → Settled (grounded)
  → delay counts down to 0
  → Available
  → Credited → Removed
```

The fallback gate (grounded + gravity == 0 + 35 observed frames) is used only if the `collectibleDelay` offset cannot be confirmed.

### Link and dog reach the same rupee

```text
Rupee Available
  → dog check runs in BeforeGuest → Credited → Actor_Kill
  → guest actor update sees no updater / actor removed → no second credit
```

If `Actor_Kill` is deferred or fails, the per-scene dedup set prevents a second credit. The rupee may remain visually for the rest of the frame; this is acceptable and does not credit twice.

### Scene change / play-state change

```text
Clear dedup set, pickup counters remain for the run, nearby cache recalculated
```

## 7. Validation Rules (mapped to requirements)

| Rule | Requirement |
| --- | --- |
| `actorId == 0x0015` and `itemCode` in the rupee set | FR-001, FR-002 |
| `value` from the table above; unknown → skip | FR-002 |
| Credit exactly once; dedup set + kill | FR-003, FR-006 |
| xz ≤ 30, y in ±50, no tunneling at sprint speed | FR-004 |
| `collectibleDelay <= 0` (or fallback gate) | FR-005 |
| Full wallet guard leaves the rupee | FR-007 |
| HUD updates via accumulator; no guest audio dependency | FR-008 |
| `play mode == 3`, cutscene index 0, player health > 0 | FR-009 |
| Re-read id/list membership immediately before guest calls | FR-010 |
| Do not kill if the credit call fails or is rejected | FR-011 |
| No scene-specific branch or config | FR-012 |
| Do not touch Link's pickup path | FR-013 |

## 8. Invariants

- A rupee address is credited at most once per scene visit.
- The wallet never exceeds `wallet_capacity`.
- No guest memory is written outside the existing wallet/actor fields.
- A failed or invalid pickup leaves the world and the wallet unchanged.
- The probe can reconstruct every successful pickup from `rupees_collected`, `last_rupee_value`, and the wallet samples.
