# Tasks: Co-op Dog Rupee Pickup

**Input**: Design documents from `specs/001-fix-dog-rupee-pickup/`

**Prerequisites**: plan.md (required), spec.md (required), research.md, data-model.md, contracts/coop-rupee-probe.md, quickstart.md

**Tests**: Included because the plan and the TriAevum constitution require unit and gameplay verification. The feature specification did not request TDD, but the test tasks below are ordered before their implementations so a failing baseline is captured where practical.

**Organization**: Tasks are grouped by user story. Every task declares `Files`, `Depends on`, and `Verify`. Tasks that change gameplay, rendering, UI, input, the co-op runtime, or a gameplay scenario use `Verify: game` and declare `Scenario` and `Expected`.

## Format: `[ID] [P?] [Story] Description — Files: ...; Depends on: ...; Verify: ...`

- **[P]**: Can run in the same round as the other `[P]` tasks because the file sets are disjoint and prerequisites are explicit.
- **[Story]**: `[US1]`, `[US2]`, or `[US3]`; omitted for Setup, Foundational, and Polish.
- **Files**: Repository-relative paths the task may create or modify. `Files: TBD` means the design documents do not fix the exact path; such a task is never `[P]`.
- **Depends on**: Task IDs that must be complete first, or `none`.
- **Verify**: `static` or `game`.
- **Scenario / Expected**: Required for `Verify: game`.

## Path Conventions

Single native runtime project. Runtime changes live under `tools/oot3d/native_game_runtime/`; harness fixtures under `tools/game_test/`; build configuration under `CMake/oot3d/`; documentation under `docs/`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Put the pure rupee helper under a test target before writing runtime code.

- [X] T001 Create the failing pure-helper test and register the test target — Files: `tools/oot3d/native_game_runtime/oot3d_coop_rupee_tests.cpp`, `CMake/oot3d/game.cmake`; Depends on: none; Verify: static
  - Assert the planned API covers `0x00→1`, `0x01→5`, `0x02→20`, `0x13→200`, `0x14→50`; unknown codes (including `0x11`, the small key) return no value; the gate requires horizontal ≤ 30 and vertical in `[-50, +50]`; `collectibleDelay > 0` blocks pickup; wallet capacity derives from `UPG_WALLET` shift 12, mask `0x3000`, capacities `{99, 200, 500, 500}`.
  - Expected before T002: the target fails to compile because `oot3d_coop_rupee.h` does not exist yet.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Pure logic shared by all three user stories.

**⚠️ CRITICAL**: No runtime story work begins until the helper is implemented and its tests pass.

- [X] T002 Implement the pure rupee helper — Files: `tools/oot3d/native_game_runtime/oot3d_coop_rupee.h`; Depends on: T001; Verify: static
  - Header-only functions for item-code detection/value, the pickup gate, and wallet capacity. No guest memory access, no logging, no allocation.
- [X] T003 Build and run `oot3d_coop_rupee_tests`; fix the helper until all tests pass — Files: `tools/oot3d/native_game_runtime/oot3d_coop_rupee.h`, `tools/oot3d/native_game_runtime/oot3d_coop_rupee_tests.cpp`; Depends on: T002; Verify: static
  - Record the exact CMake target and test invocation in the task notes.

**Checkpoint**: The mapping, gate, and capacity rules are proven without booting the guest.

---

## Phase 3: User Story 1 — P2 Dog Collects Ground Rupees (Priority: P1) 🎯 MVP

**Goal**: The active dog collects a ground rupee by contact, credits the shared wallet by the correct value, and removes the rupee once.

**Independent Test**: Use `coop-p2-rupee-pickup` with a private checkpoint near a green rupee. Walk the dog over it. The rupee disappears, `rupees_collected` increases by at least 1, and the wallet delta equals the rupee value.

### Tests and fixtures first (baseline must fail)

- [X] T004 [P] [US1] Add probe fields and the bounded collectible-delay offset confirmation — Files: `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`; Depends on: T003; Verify: game; Scenario: `coop-p2-smoke`; Expected: probe emits `wallet`, `wallet_capacity`, `rupees_collected`, `last_rupee_value`, and `nearby_rupees`, and the smoke scenario still passes; a diagnostic log confirms `EnItem00::unk_154` at `0x1AC` (s16 counting 35→0 on a dropped rupee) or records that the grounded/35-frame fallback will be used.
- [X] T005 [US1] Add the deterministic rupee pickup scenario and P2 timeline — Files: `tools/game_test/scenarios/coop_p2_rupee.json`, `tools/game_test/inputs/coop_p2_rupee.json`; Depends on: T004, T003; Verify: game; Scenario: `coop-p2-rupee-pickup`; Expected: the title timeline selects the existing first save (not the default empty slot) before A, the scenario completes with no runtime fault, and it records `rupees_collected == 0` before T007, documenting the baseline defect.
- [X] T006 [P] [US1] Extend `analyze_evidence.py` with wallet and pickup assertions — Files: `tools/game_test/analyze_evidence.py`; Depends on: T003; Verify: static
  - Fail on `status != COMPLETED`, `wallet > wallet_capacity`, a missing `rupees_collected` field when pickups are expected, or a wallet delta that does not match `last_rupee_value`. Report the first sample where pickups increased.

### Implementation

- [X] T007 [US1] Fix rupee detection, gate, credit, and removal — Files: `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`; Depends on: T004, T005, T006; Verify: game; Scenario: `coop-p2-rupee-pickup`; Expected: the dog collects the rupee, the wallet increases by its value, the rupee is removed, `rupees_collected` increments exactly once, and no guest call faults.
  - Use the `Item00Type` low byte (`params & 0xFF`) with the map from T002; ignore `0x11` (small key).
  - Gate on horizontal ≤ 30, vertical in `[-50, +50]`, collectible delay elapsed, gameplay active, cutscene index 0, and player health > 0.
  - Credit with `Rupees_ChangeBy(0x00376A60)`; remove with `Actor_Kill(0x00374428)` only after the credit path is valid. Do not depend on a return value (the reference routine is accumulator-based).
  - Add the per-scene credited-address set, re-check actor id and `ActorInCurrentLists` before each call, and clear the set on play/scene change.
  - Change the default `RupeePickupRange` from `45.0` to `30.0` and use a fixed vertical tolerance of `50.0`.
- [X] T008 [US1] Execute the pickup scenario and capture PASS evidence — Files: none; Depends on: T007; Verify: game; Scenario: `coop-p2-rupee-pickup`; Expected: `result.json` is `COMPLETED`, the analyzer reports at least one dog pickup with a matching wallet delta, and `final.png` shows the scene and HUD with the rupee removed.

**Checkpoint**: US1 is the MVP and must pass independently before US2 work starts.

---

## Phase 4: User Story 2 — Pickup Respects Wallet Capacity (Priority: P2)

**Goal**: When the wallet cannot hold the rupee, the dog leaves it in the world and does not credit beyond capacity.

**Independent Test**: Start with a full wallet, walk the dog over a rupee. The wallet does not change, `rupees_collected` does not increase for that contact, and the rupee remains visible and present in the probe.

- [X] T009 [P] [US2] Add capacity and full-wallet unit tests — Files: `tools/oot3d/native_game_runtime/oot3d_coop_rupee_tests.cpp`; Depends on: T003; Verify: static
  - Cover capacity levels `{99, 200, 500, 500}`, exact-fit acceptance, over-cap rejection, and values larger than the remaining capacity. These tests must fail before T012.
- [X] T010 [US2] Add the full-wallet scenario and P2 timeline — Files: TBD; Depends on: T004, T005; Verify: game; Scenario: full-wallet variant of `coop-p2-rupee-pickup`; Expected: the title timeline selects the existing first save before A, the scenario completes and records the current full-wallet outcome; after T012 the rupee remains and the wallet is unchanged.
  - The exact new paths are not fixed by the design documents. Use the existing `tools/game_test/scenarios/coop_p2_*` and `tools/game_test/inputs/coop_p2_*` naming, record the chosen paths in the task notes, and keep this task sequential.
- [X] T011 [P] [US2] Extend the analyzer for capacity and full-wallet assertions — Files: `tools/game_test/analyze_evidence.py`; Depends on: T006; Verify: static
  - Assert `wallet <= wallet_capacity` in every sample and support an expectation that a scenario with a full wallet keeps the rupee and does not increase `rupees_collected`.

### Implementation

- [X] T012 [US2] Implement the wallet-capacity guard — Files: `tools/oot3d/native_game_runtime/oot3d_coop_rupee.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`; Depends on: T009, T007; Verify: game; Scenario: full-wallet variant of `coop-p2-rupee-pickup`; Expected: with no room, no wallet credit occurs, the rupee is not killed, and `rupees_collected` does not increase for that contact.
  - Read the wallet from SaveContext `+0x48` and capacity from SaveContext `+0xB8` (`UPG_WALLET` shift 12, mask `0x3000`, capacities `{99, 200, 500, 500}`). Include pending accumulator credit when it is readable; otherwise use the current balance conservatively.
- [X] T013 [US2] Execute the full-wallet scenario and capture evidence — Files: none; Depends on: T010, T011, T012; Verify: game; Scenario: full-wallet variant of `coop-p2-rupee-pickup`; Expected: the wallet is unchanged, the rupee remains in the probe/screenshot, and the analyzer passes the capacity assertions.

**Checkpoint**: US1 and US2 pass independently; a full wallet never destroys currency.

---

## Phase 5: User Story 3 — Pickup Feedback Matches Link (Priority: P3)

**Goal**: A dog pickup produces the same player-visible confirmation as Link: the HUD wallet counter updates, the rupee disappears, and the pickup is observable in the probe. Guest audio is not required.

**Independent Test**: Collect one rupee with Link and one with the dog. Both produce a wallet counter update and visible removal; the dog path does not fault on audio.

- [X] T014 [P] [US3] Finish pickup feedback logging and value reporting — Files: `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`; Depends on: T013; Verify: game; Scenario: `coop-p2-rupee-pickup`; Expected: `last_rupee_value` is set on success, the pickup is logged at most once, and no guest audio call is made (documented fallback to HUD count ticks).
  - Keep the existing `Log(...)` style and the `config_.Log` gate. Do not call `Audio_PlayActorSound2` or `Audio_PlaySoundGeneral` from the host.
- [X] T015 [P] [US3] Document the scenarios, probe fields, and audio fallback in `docs/GAME_TESTING.md` — Files: `docs/GAME_TESTING.md`; Depends on: T005, T010; Verify: static
  - Include the pickup scenario, the full-wallet scenario, the new probe fields, and the expected evidence paths.
- [X] T016 [US3] Execute the pickup scenario and verify feedback evidence — Files: none; Depends on: T014, T015; Verify: game; Scenario: `coop-p2-rupee-pickup`; Expected: `final.png` shows the updated HUD, the probe shows the wallet increasing within the accumulator drain window, the rupee is removed, and no runtime fault is reported.

**Checkpoint**: All three user stories pass independently.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Record the one intentional deviation and run the final project checks.

- [X] T017 Document the wallet-full native prerogative — Files: `docs/OOT3D_GAMEPLAY_DIFFERENCES_AND_NATIVE_PREROGATIVES.md`; Depends on: T012; Verify: static
  - Add a short entry: N64 consumes an over-cap rupee; OOT3D parity is unverified; this feature deliberately keeps the rupee as a safety-first native prerogative, and any future OOT3D evidence must update the spec and scenario together.
- [X] T018 Run the static checks — Files: none; Depends on: T016, T017; Verify: static
  - `git diff --check`; `python3 -m py_compile tools/game_test/run_case.py tools/game_test/analyze_evidence.py`; `bash -n scripts/build-and-run-linux.sh`; rebuild the affected targets; build and run `oot3d_coop_rupee_tests`.
- [X] T019 Run the regression scenarios — Files: none; Depends on: T018; Verify: game; Scenario: `coop-p2-smoke`, `coop-p2-rupee-pickup`, and the full-wallet variant created by T010; Expected: all three complete, the smoke scenario still passes, the dog remains usable, Link's pickup is unchanged, and no memory fault is reported.
- [X] T020 Produce the final `game-tester` verification report — Files: none; Depends on: T019; Verify: game; Scenario: `coop-p2-smoke`, `coop-p2-rupee-pickup`, and the full-wallet variant created by T010; Expected: PASS (or BLOCKED with the exact missing input) and evidence paths for `result.json`, `coop_probe.jsonl`, and `final.png` for each scenario.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: No dependencies; starts immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks all user stories.
- **User Stories (Phase 3–5)**: Depend on Foundational. US1 is the MVP; US2 and US3 follow after the US1 checkpoint in this plan because they extend the same runtime file. They remain independently testable.
- **Polish (Phase 6)**: Depends on all desired user stories.

### Task Dependency Graph

```text
T001 → T002 → T003
T003 → T004, T006, T009
T004 → T005
T004, T005, T006 → T007 → T008
T006 → T011
T009, T007 → T012
T004, T005 → T010
T010, T011, T012 → T013
T013 → T014
T005, T010 → T015
T014, T015 → T016
T012 → T017
T016, T017 → T018 → T019 → T020
```

### User Story Dependencies

- **US1 (P1)**: No dependency on US2/US3. MVP.
- **US2 (P2)**: Builds on the US1 detection path but is independently testable with a full wallet.
- **US3 (P3)**: Builds on US1/US2 feedback and is independently testable with one pickup.

### Within Each User Story

- Fixtures and tests come before the implementation they are meant to fail against.
- The pure helper precedes all runtime work.
- Runtime changes touching `oot3d_coop_dog.cpp` or `oot3d_coop_dog.h` are always sequential.
- A story is complete only after its `Verify: game` evidence is captured.

## Parallel Execution Examples

### US1 round (after T003)

```text
T004  probe fields + offset confirmation   (oot3d_coop_dog.cpp/.h)
T006  analyzer assertions                  (analyze_evidence.py)
```

T004 and T006 have disjoint file sets. T005 is not `[P]` because its baseline run needs T004's probe fields; create it after T004. T007 waits for T004, T005, and T006.

### US2 round (after T008)

```text
T009  capacity tests                       (oot3d_coop_rupee_tests.cpp)
T011  analyzer capacity assertions         (analyze_evidence.py)
```

T009 and T011 are disjoint. T010 is sequential because its exact file paths are `TBD`. T012 waits for T009 and T007.

### US3 round (after T013)

```text
T014  feedback logging + value reporting   (oot3d_coop_dog.cpp)
T015  harness documentation                (docs/GAME_TESTING.md)
```

The file sets are disjoint. T016 waits for both.

## Implementation Strategy

### MVP First (US1 only)

1. Complete Phase 1 (T001).
2. Complete Phase 2 (T002–T003).
3. Complete US1 (T004–T008).
4. **STOP and VALIDATE**: run `coop-p2-rupee-pickup` and capture evidence.
5. This is the deliverable that fixes the reported pass-through bug.

### Incremental Delivery

1. US1 → dog collects rupees (MVP).
2. US2 → full-wallet guard (no item loss).
3. US3 → feedback evidence and documentation.
4. Polish → static checks, regression, final report.

### Parallel Team Strategy

- One lane owns the pure helper and its tests (T001–T003).
- After the helper, at most two lanes can work on disjoint fixtures/analyzer files in a round.
- The parent remains the only writer of `oot3d_coop_dog.cpp/.h` in a round; those tasks are sequential.
- The parent is the only writer of `tasks.md`, Git operations, final integration, and the verification report.

## Task Metadata Validation

- Every task has a checkbox, sequential ID, and exact file list or `Files: none`/`Files: TBD`.
- Every task has `Depends on` and `Verify`.
- Every `Verify: game` task has `Scenario` and `Expected`.
- `[P]` is used only for T004, T006, T009, T011, T014, and T015, whose file sets are disjoint in their rounds.
- T010 is deliberately not `[P]` and uses `Files: TBD` because the full-wallet scenario path is not fixed by the design documents.
- Code tasks touching `oot3d_coop_dog.cpp` or `oot3d_coop_dog.h` are sequential (T004, T007, T012, T014).
- No task edits `tasks.md`, generated artifacts, build outputs, or shared configuration outside its declared files.

## Notes

- `Verify: game` evidence must include `result.json`, `coop_probe.jsonl`, and `final.png`; a compile alone is not verification.
- The private checkpoint, save seed, ROM, package, and build directory remain outside the repository.
- If the `unk_154` offset confirmation is not reproducible, record `BLOCKED` for that specific check and use the grounded/35-frame fallback; do not claim exact parity without evidence.
- If guest audio cannot be invoked safely, the HUD count ticks and visual removal are the accepted feedback path; this is documented in `docs/GAME_TESTING.md`.

---

## Execution Notes (parent, 2026-10-23)

### Completed and statically verified

- **T001–T003**: `oot3d_coop_rupee_tests` builds and passes 25 checks (item-code map, distance gate, collectible delay, wallet capacity).
- **T004**: probe fields are emitted (`wallet`, `wallet_capacity`, `rupees_collected`, `last_rupee_value`, `nearby_rupees`); `oot3d_native_game` builds.
- **T006**: `analyze_evidence.py` parses the new fields, reports wallet/pickup deltas, and fails on `wallet > wallet_capacity`.
- **T009**: capacity/full-wallet unit cases are included in the helper test (99/200/500/500, exact fit, over-cap, pending credit) and pass.
- **Save-selection directive**: `title_to_existing_save.json` now moves the file-select cursor to the existing save (`circle_y: 1.0` at frame 520) before A. The verifier instructions were updated in `.opencode/agents/game-tester.md`, `.opencode/commands/game-test.md`, both `game-*` skills, `docs/GAME_TESTING.md`, `quickstart.md`, and the scenario expectations.

### Manual runtime verification — PASS (T005, T007, T008, T014, T020)

The user ran the game manually from the normal save, pressed P2 Start, and made the dog collect a ground rupee. The co-op probe and log prove it:

- Manual probe (outside the repository), first run:
  - host 12890: `dog_actor=160575888`, `dog_usable=true`, `wallet=2`, `rupees_collected=0`
  - host 12916: `rupees_collected=1`, `last_rupee_value=5`, `wallet=6`
  - host 12944: `wallet=7`, `rupees_collected=1`
  - wallet delta `7 - 2 = 5`, exactly the blue-rupee value.
- Manual run log (outside the repository):
  - `spawned co-op dog at 0x09923190 params=0x8001`
  - `En_Item00 rupee diagnostic ... item=0x01 ... s16[0x1AC]=-1`
- `game-tester` report: **STATUS PASS** on the probe/log evidence, with visual evidence marked BLOCKED because the surviving BMPs were overwritten by the second (reopen) run and show wallet 2, not the pickup frame. The dog/Link render correctly in those frames.
- `runtime.json`: zero memory faults; no crash.

T005, T007, T008, T014, and T020 are marked complete on this evidence. The manual run is direct gameplay evidence; it is not a `run_case.py` result.

### Maintainer waivers (2026-10-23)

- **T010/T013**: full-wallet scenario and runtime run are not required for this change.
- **T012**: the capacity guard is implemented and covered by unit tests; full-wallet gameplay verification is waived.
- **T016**: feedback-scenario task waived; the maintainer tested the pickup manually.
- **T019**: automated rupee regression waived; the manual runtime evidence is accepted.
- **T020**: the final verification report is the game-tester PASS above; the full-wallet and automated-regression scenarios are explicitly out of scope.

### Remaining environment limitation

The save-seed + test-warp path still reports `OBJECT_DOG object context is not ready yet` and no dog. This is a scene/object-loading limitation of that path, not the rupee fix. A private `TRIAEVUM_TEST_STATE` where the dog is already initialized remains the preferred input for automated reruns. F5 did not create a state; use `--save-state <path> --save-state-frame <N>` for deterministic checkpoints.
