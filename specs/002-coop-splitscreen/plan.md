# Implementation Plan: Co-op Dynamic Split Screen

**Branch**: `coop-local` (feature branch `002-coop-splitscreen` was not created — no `before_specify` hook) | **Date**: 2026-10-23 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `specs/002-coop-splitscreen/spec.md`

## Summary

The feature adds an automatic single/split scene presentation while the P2 dog is active. Both characters stay in one view while they are framed together; when one leaves the shared view for 0.5 s, the scene renders twice (one Link view, one dog view) into two disjoint viewports of the top target. The UI remains a single full-screen overlay. P2 Start/summon beside Link, dog loss, scene changes, cutscenes, pause, and invalid state all merge back to one view.

The renderer already separates PICA draws into composition domains (`Scene` and `Ui`), exposes a per-draw view with uniform/viewport/light overrides, and records separate projection/view/model uniform rows in the vertex transform layout. The plan reuses those structures to replay the scene draws with a second camera while keeping the UI draw sequence single.

## Technical Context

**Language/Version**: C++20

**Primary Dependencies**: NativeA32 guest runtime, PICA visual-frame replay and composition tracker, Vulkan/NRI PICA backend, SDL2 input, nlohmann/json

**Storage**: No new persistence. Optional tuning keys in the existing `coop_controls.json`; no save or savestate format change.

**Testing**: C++ unit tests for the split controller/projection/camera, renderer tests for scene-duplication and UI-single-pass, probe assertions, and the existing `tools/game_test/run_case.py` + `game-tester` runtime verification.

**Target Platform**: Linux native desktop runtime (Vulkan/NRI path; the supported developer workflow in `docs/GAME_TESTING.md`)

**Project Type**: native game runtime / desktop application

**Performance Goals**: 60 FPS in the supported split test scene; the second scene view must not stall the guest or duplicate game simulation. If the full-resolution second view cannot hold 60 FPS, reduce the secondary panel's effects/quality while keeping the UI full resolution; if it still cannot hold the target, fall back to single and record the reason.

**Constraints**: Single UI instance; only scene cameras split; no second guest update/render pass that would duplicate game logic, audio, or UI; validate actor state and fall back to single view on invalid input; no ROM, save, screenshot, or build output in the repository.

**Scale/Scope**: Two scene panels plus one UI pass per presented frame; two cameras (Link and dog); all dog-capable scenes within the currently loaded scene/room.

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

| Principle | Gate | Status | Evidence |
| --- | --- | --- | --- |
| I. Source-Only Reproducibility | No ROM, assets, saves, logs, or build outputs added | **PASS** | Only runtime/renderer source, tests, config defaults, harness fixtures, and docs are touched. Game-test artifacts remain outside the repository. |
| II. Upstream Compatibility And Licensing | No rewriting `main`; work stays on `coop-local`; no licence changes | **PASS** | Current branch is `coop-local`; no history rewrite and no third-party code added. |
| III. Guest-Memory Safety | Validate dog/Link actor identity, presence, and camera state; fail closed | **PASS** | The split controller reads only validated actor positions and the existing scene-view snapshot. Invalid or uninitialized actors force a single view. No guest render call is made from the host. |
| IV. Verification Before Merge | Build, unit/renderer tests, and runtime evidence with a legal ROM | **PASS (planned)** | The plan includes split-controller tests, renderer duplicate/single-pass tests, probe assertions, and a `game-tester` scenario with screenshots, probe, and frame timing. |
| V. Small, Reviewable Changes | Feature-sized change with a clear reviewable boundary | **PASS with justification** | The feature necessarily touches the renderer, the split controller, the probe, and the harness. Complexity is isolated behind a split-view contract; existing single-camera paths remain the default when the dog is absent or the split is inactive. |

**Post-Design Re-check**: same status. The renderer work is the only large addition; it is justified by the user-visible split-screen requirement and is isolated from the single-view path.

## Project Structure

### Documentation (this feature)

```text
specs/002-coop-splitscreen/
├── plan.md                    # This file
├── research.md                # Phase 0 output
├── data-model.md              # Phase 1 output
├── quickstart.md              # Phase 1 output
├── contracts/
│   └── coop-splitscreen-probe.md
├── checklists/
│   └── requirements.md
└── tasks.md                   # Phase 2 output (/speckit.tasks, not created here)
```

### Source Code (repository root)

```text
tools/oot3d/native_game_runtime/
├── oot3d_coop_dog.cpp/.h                 # expose dog state/position; split probe fields
├── oot3d_coop_splitscreen.h               # NEW: header-only split state, framing, dog camera, timers
├── oot3d_coop_splitscreen_tests.cpp       # NEW: pure controller/projection/camera tests
├── oot3d_native_a32_window_frame.cpp      # evaluate/publish split state at the render boundary
├── oot3d_native_a32_window_session.h      # own the split controller and camera snapshot
├── oot3d_native_pica_visual_frame.cpp/.h  # per-view draw overrides and scene/UI split helpers
├── oot3d_native_pica_presentation_scheduler.cpp # execute split panels + single UI pass
├── oot3d_native_pica_vulkan_bridge.cpp    # camera/view uniform override plumbing as needed
├── oot3d_native_a32_scene_view.h/.cpp     # publish the exact scene frustum for framing (if needed)
└── ...

runtime/three_ds_recomp/
├── include/fast/renderer3ds/pica_view_family.h   # existing view-family contract (reference)
├── include/fast/renderer3ds/pica_shader_hooks.h  # existing projection/view/model uniform layout
└── src/fast/...                                  # only if the split override needs a shared helper

CMake/oot3d/game.cmake                     # add the split controller/test target
tools/game_test/
├── scenarios/coop_p2_splitscreen.json     # NEW: near -> far -> merge -> summon
├── inputs/coop_p2_splitscreen.json        # NEW: P2 movement/summon timeline
├── scenarios/coop_p2_splitscreen_hold.json # NEW: ends split for the visual
├── inputs/coop_p2_splitscreen_hold.json    # NEW: separation-only timeline
├── analyze_evidence.py                    # split-state assertions
└── ...

docs/GAME_TESTING.md                       # split scenario, probe fields, evidence
```

**Structure Decision**: Single native runtime project. The split decision/camera logic is a pure, unit-testable module (`oot3d_coop_splitscreen.*`). The renderer consumes a small split-view plan and keeps the single-view path untouched. The coop dog runtime exposes only the dog's validated position/active state and the new probe fields.

## Design Decisions (summary)

1. **Split the scene by composition domain.** The PICA composition tracker already classifies draws as `Scene` or `Ui`. Scene draws are rendered twice with different cameras/viewports; `Ui` and unclassified draws remain a single full-screen pass. This satisfies "only the scene cameras split" and keeps the HUD single.
2. **Replay the existing scene draw list with a second camera.** `Oot3dPicaVisualDrawView` already supports per-draw vertex-uniform, viewport, and light-binding overrides, and `PicaVertexTransformLayout` exposes separate projection/view/model uniform rows. For the dog panel, replace the view/projection rows and the light binding's world-to-view; keep model rows and geometry. Do not run a second guest frame or scene simulation.
3. **One top target with two panel viewports + one final UI pass.** Render the scene draws into the top and bottom halves of the canonical top target through per-panel viewports; clear depth once before the panels, then render the `Ui` draws once over the full target. The panel pixels are disjoint, so the shared depth buffer has no cross-panel conflict, and the top-screen snapshot/transfer still runs after the complete split frame.
4. **Framing-based split decision with 0.5 s hysteresis.** Project Link and the dog into the single Link camera with a safe margin. If one has been outside for 0.5 s, split; if both have been inside for 0.5 s, merge. This matches "framed together" rather than a fixed distance and prevents flicker.
5. **Deterministic dog follow camera.** The dog view uses the dog's position/yaw with a configurable follow distance/height, the same near/far and vertical field of view as the Link view, and the panel aspect. In split mode, the dog's camera-relative movement uses the dog camera's yaw; in single mode it keeps using Link's camera.
6. **Force single outside normal gameplay.** No dog/P2 inactive, menus, cutscenes, loading, pause, scene changes, or invalid actors force a single view and reset the split timers. After gameplay resumes, the state is re-evaluated.
7. **Single UI and single game simulation.** The UI draw sequence and all game logic execute once. Splitting only duplicates the scene draw replay, not the guest update, audio, or UI.
8. **Performance fallback.** If the two full-resolution scene passes cannot hold 60 FPS in the test scene, reduce the secondary panel's effects/quality first; the UI stays full resolution. If it still cannot hold the target, fall back to single and record the reason. A failure to render a valid second view falls back to single, never a blank panel.

## Phase Plan

### Phase 0 — Research (complete)

See [research.md](./research.md). Key findings:

- `Oot3dPicaCompositionDomain::Scene` vs `Ui` already exists.
- `Oot3dPicaVisualDrawView` carries optional vertex uniforms, viewport, and light binding overrides; the presentation scheduler applies them through `Oot3dPicaVulkanDrawOverrides`.
- `PicaVertexTransformLayout` exposes `ProjectionFirstUniform`, `ViewFirstUniform`, and `ModelFirstUniform`, so a camera override can change the view/projection without touching geometry.
- The current scene draw list comes from one guest render; the second view is a replay, not a second guest render.

### Phase 1 — Design (this output)

- [data-model.md](./data-model.md): split state, cameras, panels, probe fields, and transitions.
- [contracts/coop-splitscreen-probe.md](./contracts/coop-splitscreen-probe.md): probe and analyzer contract, config defaults.
- [quickstart.md](./quickstart.md): build, unit/renderer tests, runtime scenarios, expected evidence.

### Phase 2 — Tasks (`/speckit.tasks`, not created here)

Expected task themes:

1. Add `oot3d_coop_splitscreen.h` with pure projection/framing/camera/timer logic; unit-test near/far, margin, hysteresis, no-dog, cutscene, summon, scene-change, invalid actor.
2. Expose the dog's active actor/position and add split probe fields to the coop runtime; add analyzer flags.
3. Extend the PICA visual frame/scheduler with a split execution mode: classify Scene vs Ui, duplicate Scene draws with view/projection/light overrides, render into two disjoint panel viewports of the canonical top target, then execute Ui once.
4. Add the dog camera view/projection construction and the Link panel projection helper; verify panel aspect and near/far preservation.
5. Plumb the split controller into the window/render boundary and config defaults.
6. Add the split gameplay scenario/input, probe assertions, and performance measurement.
7. Document the feature and run the `game-tester` verification with near/far/summon/no-dog/cutscene cases.

## Risks And Open Questions (resolved in implementation spikes)

- **Composition coverage**: verify that a normal gameplay frame classifies all scene draws as `Scene` and all HUD/message draws as `Ui`; if `Unknown` is significant, keep it single/full-screen and refine the classification rather than guessing.
- **Top-transfer timing**: both panel viewports and the single UI pass must complete before the top-screen snapshot/transfer. The scheduler spike must prove the transfer still captures the complete split frame.
- **Interpolated frames**: the split view must remain stable on interpolated/replayed frames. Reuse the same split cameras/overrides; if the sampled draw list is not compatible, keep the previous split composition for that presentation frame.
- **Room loading**: the second view replays the currently loaded scene/room draw list. If the dog leaves the loaded area, the view falls back to the single view (or the loaded scene from the dog camera) rather than loading a second room. Loading a second room is out of scope for the first version.
- **Camera occlusion**: the dog follow camera uses a fixed boom without full collision/occlusion handling in the first version; near-plane clipping and safe fallback must be verified in the test scene.
- **Performance**: measure the split scene before deciding whether the secondary panel needs reduced effects/quality; if it still misses 60 FPS, fall back to single.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --------- | ---------- | ------------------------------------ |
| Second scene replay and panel compositor | A splitscreen view requires two cameras and two scene renderings | A UI-only or camera-shake mock does not show the dog's independent view; a second guest frame would duplicate game simulation, audio, and UI |
| Split controller in the render boundary | The exact framing and cameras depend on the current scene view and actor positions | A fixed world-distance rule does not match "framed together" and behaves poorly with scene geometry |
| Panel viewports in one target | The two panels need independent projections and disjoint screen regions | A second offscreen target/composite adds a target allocation, a copy, and ordering risk; disjoint pixels of the shared depth buffer do not conflict |

No principle violations require a waiver. The renderer addition is isolated behind the split-view contract and is inactive whenever the split is off.
