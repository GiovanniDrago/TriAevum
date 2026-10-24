# Tasks: Co-op Dynamic Split Screen

**Input**: Design documents from `specs/002-coop-splitscreen/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/coop-splitscreen-probe.md, quickstart.md

**Tests**: Included because the plan and the TriAevum constitution require unit, renderer, and gameplay verification. T001 is the test-first baseline; the US2/US3 tests extend coverage after the foundational controller exists.

**Organization**: Tasks are grouped by user story. Every task declares `Files`, `Depends on`, and `Verify`. Tasks that change gameplay, rendering, UI, input, the coop runtime, or a gameplay scenario use `Verify: game` and declare `Scenario` and `Expected`.

## Format: `[ID] [P?] [Story] Description — Files: ...; Depends on: ...; Verify: ...`

- **[P]**: Can run in the same round as the other `[P]` tasks because the file sets are disjoint and prerequisites are explicit.
- **[Story]**: `[US1]`, `[US2]`, or `[US3]`; omitted for Setup, Foundational, and Polish.
- **Files**: Repository-relative paths the task may create or modify.
- **Depends on**: Task IDs that must be complete first, or `none`.
- **Verify**: `static` or `game`.
- **Scenario / Expected**: Required for `Verify: game`.

## Path Conventions

Single native runtime project. Runtime/renderer changes live under `tools/oot3d/native_game_runtime/`; PICA targets under `CMake/oot3d/`; harness fixtures under `tools/game_test/`; docs under `docs/`.

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Put the split controller and the renderer split path under failing tests before implementation.

- [X] T001 Create the failing split-controller and split-renderer tests and register their targets — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen_tests.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_split_tests.cpp`, `CMake/oot3d/game.cmake`; Depends on: none; Verify: static
  - Controller assertions: projected framing with safe margin `0.9`; split after 500 ms outside; merge after 500 ms inside; no dog / P2 inactive / cutscene / pause / loading / invalid actor force single; scene change and teleport reset; dog camera with distance `180`, height `90`, target height `30`; panel aspect; invalid camera fallback.
  - Renderer assertions: `Scene` draws execute twice with different view/projection uniforms and disjoint viewports; `Ui` and `Unknown` execute once; panel viewports cover top/bottom halves; top transfer occurs after both panels and the single UI; invalid panel falls back to single.
  - Register `oot3d_coop_splitscreen_tests` (header-only controller; include `tools/oot3d/native_game_runtime`) and `oot3d_native_pica_split_tests` (link `oot3d_native_pica_vulkan_plan` / `oot3d_native_pica_vulkan_bridge`).
  - Expected before T002/T004: the targets fail to build or run because the new headers/helpers do not exist yet.

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Split state/camera logic, renderer split execution, and the runtime plumbing shared by all stories.

**⚠️ CRITICAL**: No user story work begins until the controller and renderer split path are tested.

- [X] T002 Implement the header-only split controller — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen.h`; Depends on: T001; Verify: static
  - Define `SplitMode`, `SplitReason`, `SplitScreenState`, `CharacterFrame`, `SplitCamera`, `ViewPanel`, `CoopSplitRenderPlan`, and the pure projection/framing/timer/camera functions. No guest memory access, no allocation in the update path.
- [X] T003 Build and run the controller tests; fix the controller — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen.h`, `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen_tests.cpp`; Depends on: T002; Verify: static
- [X] T004 [P] Implement the renderer split draw-view helpers — Files: `tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp`; Depends on: T002; Verify: static
  - Classify `Scene` / `Ui` / `Unknown`; build Link and dog panel draw views from the current sample; patch projection/view uniforms, viewport, optional scissor, and light-binding world-to-view; preserve existing interpolation overrides; no guest calls.
- [X] T005 Add the scissor override plumbing — Files: `tools/oot3d/native_game_runtime/oot3d_native_pica_vulkan_plan.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_vulkan_bridge.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp`; Depends on: T004; Verify: static
  - Carry an optional panel scissor through `Oot3dPicaVulkanDrawOverrides` and apply it when the bridge constructs the draw. Default remains the plan scissor.
- [X] T006 Implement split execution in the presentation scheduler — Files: `tools/oot3d/native_game_runtime/oot3d_native_pica_presentation_scheduler.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_presentation_scheduler.cpp`; Depends on: T004, T005; Verify: static
  - Add a split execution path: execute each `Scene` draw once for Link and once for the dog; execute `Ui`/`Unknown` once; execute fills/transfers once in original order; keep the top transfer after both panels and the UI; fall back to single when the plan is invalid.
- [X] T007 Build and run the renderer split tests; fix the split execution — Files: `tools/oot3d/native_game_runtime/oot3d_native_pica_split_tests.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_presentation_scheduler.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_presentation_scheduler.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_vulkan_plan.h`, `tools/oot3d/native_game_runtime/oot3d_native_pica_vulkan_bridge.cpp`; Depends on: T006; Verify: static
- [X] T008 [P] Expose dog state and split tuning config — Files: `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`; Depends on: T002; Verify: static
  - Add `DogActor()`, `DogUsable()`, `DogPosition()`, and `DogYaw()` getters; parse and clamp `splitscreen_enabled`, `splitscreen_grace_ms` (500), `splitscreen_safe_margin` (0.9), `splitscreen_layout` (`horizontal`), `dog_camera_distance` (180), `dog_camera_height` (90), `dog_camera_target_height` (30).
- [X] T009 Wire the split controller into the window/render boundary — Files: `tools/oot3d/native_game_runtime/oot3d_native_a32_window_session.h`, `tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`; Depends on: T006, T008; Verify: game; Scenario: `coop-p2-smoke`; Expected: the smoke scenario still completes; the split plan is evaluated from the scene view/players; the scene stays single while the dog and Link are framed together; no guest stall or memory fault.
- [X] T010 Emit split probe fields and analyzer assertions — Files: `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`, `tools/game_test/analyze_evidence.py`; Depends on: T009; Verify: game; Scenario: `coop-p2-smoke`; Expected: probe adds `split_active`, `split_reason`, `split_timer_ms`, `link_framed`, `dog_framed`, `split_count`, `merge_count`, `split_panels`, `ui_passes`; analyzer parses them and passes the smoke evidence.
- [X] T011 Add the split scenario and input timelines — Files: `tools/game_test/scenarios/coop_p2_splitscreen.json`, `tools/game_test/inputs/coop_p2_splitscreen.json`, `tools/game_test/scenarios/coop_p2_splitscreen_hold.json`, `tools/game_test/inputs/coop_p2_splitscreen_hold.json`; Depends on: T009, T010; Verify: game; Scenario: `coop-p2-splitscreen`; Expected: the scenario covers near -> far -> merge -> summon; `split_panels` 0/2/0; `split_count` and `merge_count` at least 1; `ui_passes` 1 in every sample.

**Checkpoint**: The controller, the renderer split path, the probe, and the scenario exist and are testable.

---

## Phase 3: User Story 1 — Automatic Split When The Dog Leaves The Shared View (Priority: P1) 🎯 MVP

**Goal**: The scene automatically splits when Link and the dog leave each other's shared view, and merges when they reunite.

**Independent Test**: With the active dog, keep both close and confirm one view; move the dog away and confirm two panels; bring it back and confirm one view. Repeat by moving Link.

### Implementation

- [X] T012 [US1] Finish the dog camera and split-mode dog movement — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.h`, `tools/oot3d/native_game_runtime/oot3d_coop_dog.cpp`; Depends on: T011; Verify: game; Scenario: `coop-p2-splitscreen`; Expected: the Link panel preserves the Link camera; the dog panel follows the dog; P2 camera-relative movement uses the dog camera yaw only while split; no control cross-talk.
- [ ] T013 [US1] Run the split and hold scenarios; capture PASS evidence — Files: none; Depends on: T012; Verify: game; Scenario: `coop-p2-splitscreen`, `coop-p2-splitscreen-hold`; Expected: `split_active` false -> true -> false; `split_count` and `merge_count` at least 1; `split_panels` 0/2/0; `ui_passes` 1; the hold screenshot shows two panels and one HUD; no runtime fault.

**Checkpoint**: US1 is the MVP and must pass independently.

---

## Phase 4: User Story 2 — Single View When The Dog Is Absent Or Summoned Close (Priority: P2)

**Goal**: No dog or P2 inactivity keeps one view; summoning the dog beside Link merges a split.

**Independent Test**: With no dog, confirm one view for a whole run. While split, press P2 Start beside Link and confirm a merge. Disconnect P2 and confirm a merge.

- [X] T014 [P] [US2] Add no-dog, P2-inactive, and summon-merge controller tests — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen_tests.cpp`; Depends on: T003; Verify: static
  - Quote the constraints: no dog/P2 inactive forces `Single`; summon beside Link forces/merges to `Single`; a dog lost/dismissed/disconnected resets to `Single`; no split without a valid dog.
- [ ] T015 [P] [US2] Add the no-dog and summon scenarios — Files: `tools/game_test/scenarios/coop_p2_splitscreen_no_dog.json`, `tools/game_test/inputs/coop_p2_splitscreen_no_dog.json`, `tools/game_test/scenarios/coop_p2_splitscreen_summon.json`, `tools/game_test/inputs/coop_p2_splitscreen_summon.json`; Depends on: T011; Verify: game; Scenario: `coop-p2-splitscreen-no-dog`, `coop-p2-splitscreen-summon`; Expected: no-dog run has zero split frames; the summon run merges when the dog lands beside Link.
- [ ] T016 [US2] Run the no-dog and summon scenarios — Files: none; Depends on: T014, T015; Verify: game; Scenario: `coop-p2-splitscreen-no-dog`, `coop-p2-splitscreen-summon`; Expected: `split_active` stays false with no dog; `merge_count` increases on summon; `ui_passes` 1; no dog/P2-inactive stale split.

**Checkpoint**: US1 and US2 pass independently.

---

## Phase 5: User Story 3 — Stable And Safe Transitions (Priority: P3)

**Goal**: No flicker at the boundary; single view in menus/cutscenes/loading/scene changes; invalid views fall back safely; the split scene holds the target frame rate.

**Independent Test**: Hover at the boundary for 30 s and confirm no rapid oscillation. Enter a cutscene, pause, and a new scene while split and confirm single view. Measure the split scene frame time.

- [X] T017 [P] [US3] Add boundary, cutscene, pause, loading, scene-change, and invalid-actor controller tests — Files: `tools/oot3d/native_game_runtime/oot3d_coop_splitscreen_tests.cpp`; Depends on: T003; Verify: static
  - Quote the constraints: symmetric 500 ms grace; no split outside gameplay; scene change/teleport resets timers; invalid dog/camera/panel forces `Single`.
- [ ] T018 [P] [US3] Add the safety scenario — Files: `tools/game_test/scenarios/coop_p2_splitscreen_safety.json`, `tools/game_test/inputs/coop_p2_splitscreen_safety.json`; Depends on: T011; Verify: game; Scenario: `coop-p2-splitscreen-safety`; Expected: cutscene/pause/scene change show one scene view; after resuming separated, the split re-appears; no stale split.
- [ ] T019 [P] [US3] Add split frame-time measurement and the safe performance fallback — Files: `tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp`, `tools/oot3d/native_game_runtime/oot3d_native_pica_presentation_scheduler.cpp`, `tools/game_test/analyze_evidence.py`; Depends on: T013; Verify: game; Scenario: `coop-p2-splitscreen`; Expected: split-mode frame time is recorded; the scene holds 60 FPS or reduces secondary effects; if it still misses the target, the runtime falls back to single and records the reason.
- [ ] T020 [US3] Run the safety and performance scenarios — Files: none; Depends on: T018, T019; Verify: game; Scenario: `coop-p2-splitscreen-safety`, `coop-p2-splitscreen`; Expected: zero split frames during cutscene/pause/loading; no boundary flicker; 60 FPS or documented safe fallback; no memory fault.

**Checkpoint**: All three user stories pass independently.

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: Documentation, final checks, and the verification report.

- [X] T021 Document the split scenario, probe fields, UI-single rule, and performance fallback — Files: `docs/GAME_TESTING.md`; Depends on: T013, T016, T020; Verify: static
- [X] T022 Run the static checks — Files: none; Depends on: T021; Verify: static
  - `git diff --check`; `python3 -m py_compile tools/game_test/run_case.py tools/game_test/analyze_evidence.py`; `bash -n scripts/build-and-run-linux.sh`; build the affected targets; run `oot3d_coop_splitscreen_tests` and `oot3d_native_pica_split_tests`.
- [ ] T023 Run the regression scenarios — Files: none; Depends on: T022; Verify: game; Scenario: `coop-p2-smoke`, `coop-p2-splitscreen`, `coop-p2-splitscreen-no-dog`; Expected: the smoke scenario still passes, the dog remains usable, the split transitions still work, and the no-dog run stays single.
- [ ] T024 Produce the final game-tester report — Files: none; Depends on: T023; Verify: game; Scenario: all split/no-dog/summon/safety/hold scenarios; Expected: PASS with split/merge counts, `ui_passes` 1, two-panel and single-HUD screenshots, frame timing, and exact evidence paths; BLOCKED with the missing input if the dog or state is unavailable.

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: Starts immediately.
- **Foundational (Phase 2)**: Depends on Setup; blocks all user stories.
- **User Stories (Phase 3–5)**: Depend on Foundational. US1 is the MVP; US2 and US3 add the safety/merge cases.
- **Polish (Phase 6)**: Depends on all desired user stories.

### Task Dependency Graph

```text
T001 → T002 → T003
T002 → T004, T008
T004 → T005 → T006 → T007
T006, T008 → T009 → T010 → T011
T011 → T012 → T013
T003 → T014
T011 → T015
T014, T015 → T016
T003 → T017
T011 → T018
T013 → T019
T018, T019 → T020
T013, T016, T020 → T021 → T022 → T023 → T024
```

### User Story Dependencies

- **US1 (P1)**: Depends on the controller/renderer/probe/scenario foundation; MVP.
- **US2 (P2)**: Builds on the same foundation; no-dog/summon tests are independent of US1's visual acceptance.
- **US3 (P3)**: Builds on the foundation and the split scenario; safety and performance are independently testable.

### Within Each User Story

- Tests come before the implementation they cover.
- Controller tests before the controller implementation; renderer tests before the renderer split path; scene/UI invariants before visual acceptance.
- Runtime changes touching the same file are sequential.

## Parallel Execution Examples

### Foundational round (after T002)

```text
T004  renderer split draw-view helpers   (oot3d_native_pica_visual_frame.h/.cpp)
T008  dog state + tuning config          (oot3d_coop_dog.h/.cpp)
```

Disjoint files; both depend on T002. T005 waits for T004; T009 waits for T006 and T008.

### US2 round (after T003/T011)

```text
T014  no-dog/summon controller tests     (oot3d_coop_splitscreen_tests.cpp)
T015  no-dog/summon scenarios            (new scenario/input files)
```

Disjoint files; both prerequisites are complete. T016 waits for both.

### US3 round (after T013/T011)

```text
T017  safety controller tests            (oot3d_coop_splitscreen_tests.cpp)
T018  safety scenario                    (new scenario/input files)
T019  frame-time measurement/fallback    (window frame/scheduler/analyzer)
```

Disjoint files; T020 waits for T018 and T019.

## Implementation Strategy

### MVP First (US1 only)

1. Complete T001–T011 (setup, controller, renderer split path, probe, scenario).
2. Complete T012–T013 (dog camera/movement, split/merge evidence).
3. **STOP and VALIDATE**: run the split and hold scenarios; confirm two panels, one HUD, split/merge counts.
4. This is the first usable splitscreen increment.

### Incremental Delivery

1. US1 → automatic split/merge.
2. US2 → no-dog/P2-inactive/summon merge.
3. US3 → safety, no flicker, performance fallback.
4. Polish → docs, regression, final report.

### Parallel Team Strategy

- One lane owns the pure controller/tests; another owns the renderer split path.
- After the controller and renderer are integrated, fixtures/analyzer and runtime scenarios can use disjoint lanes.
- The parent remains the only writer of `oot3d_native_a32_window_frame.cpp` and `oot3d_coop_dog.cpp` in a round; those tasks are sequential.
- The parent is the only writer of `tasks.md`, Git operations, and final verification.

## Task Metadata Validation

- Every task has a checkbox, sequential ID, and exact file list or `Files: none`.
- Every task has `Depends on` and `Verify`.
- Every `Verify: game` task declares `Scenario` and `Expected`.
- `[P]` is used only for T004, T008, T014, T015, T017, T018, and T019, whose file sets are disjoint in their rounds.
- No task edits `tasks.md`, generated artifacts, build outputs, or shared configuration outside its declared files.
- The test-first baseline is T001 → T002/T004/T006. The US2/US3 controller tests (T014/T017) are additional edge-case coverage over the foundational controller, and the runtime scenarios (T011/T015/T018) precede their corresponding runs.

## Notes

- The first implementation task after T001 should be the renderer spike embedded in T007: prove `Scene` twice / `Ui` once, disjoint viewports, and top-transfer ordering before visual acceptance.
- Use a private save/state with an initialized dog for runtime runs. The save-seed + test-warp path can miss `OBJECT_DOG`; the normal save with P2 Start is known to work.
- The harness captures the final frame; use the hold scenario for the split screenshot or the runtime's screenshot sequence. A black/startup image is not a visual PASS.
- The UI must never be duplicated; if `ui_passes != 1`, stop and fix composition classification before claiming the split is complete.

---

## Execution Notes (parent, 2026-10-23)

### Completed and statically verified

- **T001–T003**: split controller and tests; **55 checks pass** (framing, 500 ms split/merge, no-dog/P2/cutscene/pause/loading/invalid, reset/summon, dog camera, panel aspect, hysteresis).
- **T004**: split draw-view helper; **23 renderer checks pass** (Scene ×2, Ui/Unknown ×1, disjoint viewports, scissor, uniform patching, model preservation, single/invalid fallback).
- **T005**: scissor override carried from the split draw view through the bridge.
- **T006**: `Oot3dPicaPresentationScheduler::ExecuteSplit` implemented; Scene draws replay per panel, Ui/Unknown and fills/transfers stay single/ordered.
- **T007**: split renderer target builds and passes.
- **T008**: dog actor/usable/position/yaw getters and split tuning config; `oot3d_native_game` builds.
- **T009/T010/T011/T012 (code)**: window/render plumbing, split probe fields, analyzer flags, full/hold scenarios, and split-mode dog yaw movement are implemented and the game target builds.
- Static checks: `git diff --check`, `py_compile`, build targets, unit/renderer tests.

### Runtime blocker (T013+)

The automated save-seed path still stops at **guest frame ~85/scene 107** (name-entry/file-select) for the normal save. Five selection variants (none, +1, -1, D-pad up, D-pad down) were tried; none reached gameplay with the dog. The manual run from the user's session did reach gameplay, so this is a harness save-selection/seed limitation, not the split renderer.

No split-mode screenshot/probe has been produced yet. Do not mark T009/T010/T011/T012 complete for runtime purposes, and do not mark T013+ complete until a `game-tester` report shows:

- `split_active` false → true → false;
- `split_panels` 0/2/0 and `ui_passes` 1;
- two-panel screenshot with one HUD;
- `split_count`/`merge_count` at least 1;
- no runtime fault and 60 FPS or the documented safe fallback.

**Required input**: a private `TRIAEVUM_TEST_STATE` with the dog already initialized, or a manual run of the new binary with the normal save and P2 Start (the user confirmed this path works). The split scenario/inputs are ready once that input exists.
