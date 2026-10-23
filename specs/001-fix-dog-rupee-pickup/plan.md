# Implementation Plan: Co-op Dog Rupee Pickup

**Branch**: `coop-local` (feature branch `001-fix-dog-rupee-pickup` was not created — no `before_specify` hook) | **Date**: 2026-10-23 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/001-fix-dog-rupee-pickup/spec.md`

**Note**: This template is filled in by the `/speckit.plan` command; its definition describes the execution workflow.

## Summary

The P2 dog currently walks through ground rupees without collecting them. Research shows the runtime already attempts collection, but it identifies rupees with the wrong encoding: it compares `En_Item00` params against `ITEM_RUPEE_*` item IDs (`0x84`–`0x88`), while the actor stores a 26-value `Item00Type` code in the low byte (`0x00` green, `0x01` blue, `0x02` red, `0x13` orange/gold, `0x14` purple). The value lookup therefore always yields zero and no wallet credit or removal happens.

The fix will:

1. Map `En_Item00` low-byte item codes to the correct rupee values.
2. Gate pickup with the same proximity rules as Link (horizontal ≤ 30, vertical ±50) and the actor's own collectible delay.
3. Credit the shared wallet through the game's existing rupee path (`Rupees_ChangeBy`, the function Link's pickup ends in) and remove the actor only after a valid credit path.
4. Add a wallet-capacity guard so a full wallet never consumes a rupee (documented safety-first deviation; see research).
5. Extend the co-op runtime probe with wallet/pickup counters and add a deterministic gameplay scenario plus unit tests for the mapping and gate.

## Technical Context

**Language/Version**: C++20 (`CMAKE_CXX_STANDARD 20`), Python 3.10+ for the gameplay harness

**Primary Dependencies**: TriAevum `NativeA32Process` guest runtime, `NativeA32Memory`, nlohmann/json, SDL2 controller input, `tools/game_test/run_case.py` harness

**Storage**: Guest SaveContext at fixed address `0x00587958`; wallet balance at `+0x48` (s16), wallet upgrade bits at `+0x8C + 0x2C` (u32). No new host persistence, no save format change.

**Testing**: C++ executable tests in the existing `Require(...)` style; Python syntax checks; `git diff --check`; deterministic runtime scenario through `tools/game_test/run_case.py` and `analyze_evidence.py`; `game-tester` subagent report for `Verify: game`.

**Target Platform**: Linux native desktop runtime (the supported developer workflow in `docs/GAME_TESTING.md`)

**Project Type**: native game runtime / desktop application

**Performance Goals**: Hold 60 FPS. The pickup scan is `O(12 actor lists × ≤512 actors)` once per host frame and must not allocate or log in the steady state.

**Constraints**: Guest pointer, actor-list membership, and model/actor lifecycle validation before any guest call; bounded synchronous invocations; no ROM, save, savestate, log, screenshot, or machine path in the repository; no change to Link's pickup path.

**Scale/Scope**: Five rupee denominations (1/5/20/50/200), 12 actor lists, optional P2 companion; one runtime file plus tests and test fixtures.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status | Evidence |
| --- | --- | --- | --- |
| I. Source-Only Reproducibility | No ROM, assets, saves, logs, or build outputs added; verification inputs stay external | **PASS** | Only source, tests, scenarios, and docs are touched. Harness artifacts remain under `artifacts/game-tests/` (ignored). |
| II. Upstream Compatibility And Licensing | No rewriting `main`; work stays on the personal `coop-local` branch; no licence changes | **PASS** | Current branch is `coop-local`; no history rewrite and no third-party code added. |
| III. Guest-Memory Safety | Validate dog/rupee actor identity and list membership; bounded invokes; fail closed | **PASS** | Pickup only after `id == 0x15`, `ActorInCurrentLists`, readable position/params, and a dedup set. Guest calls are single `Rupees_ChangeBy` / `Actor_Kill` invocations with the existing `kDirectCallReturn` policy. Invalid state results in no wallet change and no removal. |
| IV. Verification Before Merge | Build the affected target, run syntax checks, exercise the runtime with a legal ROM, record evidence | **PASS (planned)** | Plan includes the native game target, a dedicated C++ test target, `py_compile`/`git diff --check`, and a `game-tester` rupee scenario. Runtime evidence is required, not compile-only. |
| V. Small, Reviewable Changes | Single behaviour fix, no unrelated refactor, existing interfaces preserved | **PASS** | Changes are confined to the co-op dog runtime, one new helper/test, the probe, the harness scenario, and docs. Link's path is untouched. |

**Post-Design Re-check**: same status. No violations or complexity exceptions are required.

## Project Structure

### Documentation (this feature)

```text
specs/001-fix-dog-rupee-pickup/
├── plan.md                       # This file
├── research.md                   # Phase 0 output
├── data-model.md                 # Phase 1 output
├── quickstart.md                 # Phase 1 output
├── contracts/
│   └── coop-rupee-probe.md       # Probe/analyzer contract
├── checklists/
│   └── requirements.md           # Spec quality checklist
└── tasks.md                      # Phase 2 output (/speckit.tasks, not created here)
```

### Source Code (repository root)

```text
tools/oot3d/native_game_runtime/
├── oot3d_coop_dog.cpp            # Fix rupee detection, gate, credit, removal
├── oot3d_coop_dog.h              # Small config/constant additions if needed
├── oot3d_coop_rupee.h            # NEW: pure item-code → value and gate helpers
├── oot3d_coop_rupee_tests.cpp    # NEW: unit tests for the helpers
├── oot3d_native_a32_window_setup.cpp  # Only if probe/config plumbing is required
└── ...

CMake/oot3d/game.cmake            # Add the pure helper to the game target and a test target

tools/game_test/
├── scenarios/coop_p2_rupee.json  # NEW: deterministic rupee pickup scenario
├── inputs/coop_p2_rupee.json     # NEW: P2 walk-over timeline
├── analyze_evidence.py           # Extend probe assertions for wallet/pickups
└── ...

docs/GAME_TESTING.md              # Document the new scenario/probe fields
```

**Structure Decision**: Single native runtime project. The pure rupee mapping/gate logic is isolated in `oot3d_coop_rupee.h` so it can be compiled and tested without booting the guest runtime; the guest-facing orchestration stays in `oot3d_coop_dog.cpp`. Test fixtures live in the existing `tools/game_test/` harness.

## Design Decisions (summary)

1. **Correct the rupee encoding.** Use the `Item00Type` low-byte codes, not the `ITEM_RUPEE_*` item IDs. Mapping: `0x00 → 1`, `0x01 → 5`, `0x02 → 20`, `0x13 → 200` (orange maps to `ITEM_RUPEE_GOLD`), `0x14 → 50`. Unknown codes are skipped.
2. **Match Link's gate.** Horizontal distance ≤ 30, vertical distance in `[-50, +50]`, and the actor's own collectible delay (`unk_154`) must be elapsed. Do not collect falling/uncollected drops.
3. **Credit through the game's rupee path.** Invoke `Rupees_ChangeBy(value)` (`0x00376A60`) — the function Link's `Item_Give` path ends in — and capture the wallet balance before and after. The reference routine is accumulator-based and effectively void, so the decision must not depend on a return code.
4. **Remove only after a valid credit path.** Read wallet balance before and after, and the capacity guard, before calling `Actor_Kill`. Track credited actor addresses per scene to prevent double credit even if removal is deferred.
5. **Full-wallet guard.** Compute capacity from the wallet upgrade (`UPG_WALLET = 4`, shift 12, mask `0x3000`, capacities `{99, 200, 500, 500}`). If `wallet + pending + value > capacity`, leave the rupee in the world. This is a deliberate safety-first choice relative to N64 (see `research.md`, R2).
6. **Feedback.** Wallet credit drains through the existing accumulator and the HUD plays its normal rupee-count ticks/flash. Do not invoke guest audio from the host: `Audio_PlayActorSound2` is already known to fault in this path (see `oot3d_coop_dog.cpp:1180`). If the initial pickup ding is required later, use the host `NativeAudioService::PlaySoundGeneral` rather than a guest call.
7. **Observable evidence.** Extend the probe with `wallet`, `wallet_capacity`, `rupees_collected`, `last_rupee_value`, and `nearby_rupees`, and assert them in `analyze_evidence.py`.

## Phase Plan

### Phase 0 — Research (complete)

See [research.md](./research.md). Root cause confirmed from the repository's own scene data (`ACTOR_EN_ITEM00` params low bytes `0x00/0x01/0x02/0x03/0x06/0x11`) and from the reference `Item00Type` table; the current `0x84..0x88` comparison can never match.

### Phase 1 — Design (this output)

- [data-model.md](./data-model.md): rupee, wallet, dog, probe entities, validation rules, and state transitions.
- [contracts/coop-rupee-probe.md](./contracts/coop-rupee-probe.md): backward-compatible probe fields and analyzer expectations.
- [quickstart.md](./quickstart.md): build, unit, static, and runtime verification steps.

### Phase 2 — Tasks (`/speckit.tasks`, not created here)

Expected task themes:

1. Add `oot3d_coop_rupee.h` with the pure code→value map and gate predicate; unit-test all five denominations, unknown codes, radius boundaries, vertical boundaries, and collectible delay.
2. Rewrite `CoopDogRuntime::CollectRupees` to use the helper, validate membership, apply the capacity guard, credit via `Rupees_ChangeBy`, deduplicate, and kill.
3. Extend `MaybeWriteProbe` and the runtime config defaults (`RupeePickupRange` 45 → 30; vertical constant 50).
4. Add the deterministic scenario/input and analyzer assertions.
5. Document the scenario and probe in `docs/GAME_TESTING.md`.
6. Run the affected CMake targets, static checks, and the `game-tester` runtime verification.

## Complexity Tracking

No constitution violations or added architectural layers. The only new module is a small header of pure functions, added to keep guest-facing code reviewable and testable.

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --------- | ---------- | ------------------------------------ |
| None | — | — |
