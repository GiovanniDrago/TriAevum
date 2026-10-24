# Phase 0 Research: Co-op Dynamic Split Screen

**Feature**: `002-coop-splitscreen`
**Date**: 2026-10-23
**Status**: Complete for planning; renderer spikes are implementation-time verifications

## Method

- Inspected the PICA visual-frame replay, composition tracker, presentation scheduler, and Vulkan draw bridge.
- Checked the existing per-draw override path, vertex transform uniform layout, and composition-domain classification.
- Compared three approaches for a second camera: a second guest render pass, a renderer-only replay with camera overrides, and a stereo/view-family reuse.
- Checked the coop dog runtime for actor state, camera-relative movement, and probe plumbing.

## R1 — How to render a second scene view without duplicating game simulation

**Decision**: Replay the existing scene draw list a second time with a different camera, using the renderer's per-draw override path. Do not run a second guest frame or a second scene simulation.

**Evidence**:

- `Oot3dPicaVisualDrawView` (`tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.h:45-56`) carries optional vertex uniforms, fragment uniforms, viewport, blend constant, light binding, and vertex bindings.
- `Oot3dPicaVisualDrawView::Overrides()` (`oot3d_native_pica_visual_frame.cpp:1838`) converts them into `Oot3dPicaVulkanDrawOverrides`.
- `SubmitOot3dPicaVulkanDrawPlan` (`oot3d_native_pica_vulkan_bridge.cpp:165-184`) uses the overridden vertex uniform state, viewport, and light binding instead of the plan defaults.
- `PicaVertexTransformLayout` (`runtime/three_ds_recomp/include/fast/renderer3ds/pica_shader_hooks.h:279-310`) exposes separate `ProjectionFirstUniform`, `ViewFirstUniform`, and `ModelFirstUniform` rows for `ModelViewProjection3x4`. Changing the view/projection rows while keeping the model rows changes the camera without changing geometry.
- The visual frame already uses `VertexUniforms` overrides for interpolation (`oot3d_native_pica_visual_frame.cpp:1334-1510`), so the override path is not speculative.

**Rationale**: The second view is a camera change over the same scene content. A second guest render would duplicate draw submission, potentially audio and game logic, and would risk the UI being drawn twice. The override path is the narrowest way to render the same scene twice.

**Alternatives considered**:

- Second guest frame / second render pass: rejected — duplicates simulation/UI and is much riskier for guest memory safety.
- Reusing the stereo view family: `PicaViewFamily` only supports `Mono` or `StereoLeft/Right` (`runtime/three_ds_recomp/include/fast/renderer3ds/pica_view_family.h:12-41`), not arbitrary split roles. Stereo also targets one presentation layout, not two independent panels.
- Renderer-only camera change without uniform overrides: rejected — the PICA draws contain camera-dependent uniforms, so the second view would be wrong.

## R2 — Separating scene draws from UI draws

**Decision**: Use the existing composition domain (`Scene` vs `Ui`). Render `Scene` draws in both panels; render `Ui` and unclassified draws once, full-screen.

**Evidence**:

- `Oot3dPicaCompositionDomain` is already used by the semantic trace (`oot3d_native_pica_semantic_trace.cpp:50-58`) with `Scene`, `Ui`, and `Unknown`.
- `Oot3dNativePicaCompositionTracker` classifies exact UI owners and scene draws (`oot3d_native_pica_composition.cpp:14-45`).
- `Oot3dPicaVulkanDrawPlan` carries `CompositionDomain` per draw (`oot3d_native_pica_vulkan_plan.h:86-89`).

**Rationale**: The feature explicitly keeps the UI single and splits only the scene. The composition tracker already encodes the title's UI ownership; reusing it avoids a second, heuristic UI classifier.

**Alternatives considered**:

- Heuristic by draw order/size: rejected — fragile across scenes and HUD states.
- Duplicate everything and draw a second UI: rejected — violates the single-UI requirement and doubles HUD/message work.

**Implementation check**: run the semantic trace on a normal gameplay frame and count `Scene`/`Ui`/`Unknown`. If `Unknown` contains scene content, refine classification; do not silently split it. If it contains genuine unknown content, keep it in the single full-screen pass.

## R3 — Panel targets and composition

**Decision**: Render both scene panels directly into the canonical top target through two disjoint viewports/scissors, clear depth once before the panels, then execute the single UI pass over the full target.

**Evidence**:

- `Oot3dPicaViewportState` exposes `HalfWidth`, `HalfHeight`, `CornerX`, `CornerY`, `DepthRange`, and `NearPlane` (`tools/oot3d/native_pica_frontend/oot3d_native_pica_draw_state.h:170-178`).
- `Oot3dPicaVulkanDrawOverrides` carries a viewport override, and the bridge applies it when constructing the draw (`oot3d_native_pica_vulkan_bridge.cpp:177-180`).
- `PicaDrawView` carries the framebuffer color/depth physical addresses and dimensions (`runtime/three_ds_recomp/include/fast/renderer3ds/pica_render_backend.h:100-135`), so both panels can share the canonical top target.
- The two panels occupy disjoint screen regions. A depth value written in the top panel's pixels cannot affect the bottom panel's pixels; one depth clear before both passes is sufficient.
- The top-screen snapshot/transfer is a separate operation and runs after both panels and the UI pass.

**Rationale**: This avoids a second target allocation, a composite/copy, and the associated target-identity/ordering work. The viewport restricts rasterization to each panel; the shared depth buffer is safe because the panels are pixel-disjoint.

**Alternatives considered**:

- Two offscreen targets plus a composite: rejected for the first version — extra target allocation, a copy/quad path, and target-identity/order complexity.
- A scissor-limited depth clear before each panel: rejected — unnecessary when the panel pixels are disjoint; one frame clear is enough.
- A second guest render: rejected earlier (duplicates simulation/UI).

**Implementation spike**: verify panel viewport/scissor clipping for full-screen scene effects, confirm depth behavior for both panels, and prove the top-screen transfer captures both panels + one UI.

## R4 — Split/merge rule

**Decision**: Project Link and the dog into the single Link camera using the scene view frustum and a safe screen margin. Split after 0.5 s outside; merge after 0.5 s inside. Gate on active dog, gameplay, no cutscene/pause/loading, and valid actors.

**Evidence**:

- `NativeA32SceneViewProbe` captures the current frustum and `Eye`/`At` (`oot3d_native_a32_scene_view.h:14-29`, `oot3d_native_a32_window_frame.cpp:2485-2552`).
- The coop runtime already validates the dog actor and reads Link/camera state (`oot3d_coop_dog.cpp`).
- The spec defines "together" as both characters framed in the shared view, with a 0.5 s grace period.

**Rationale**: A framing test matches the user's "when they are close the camera stays one, when one moves away it splits" and avoids arbitrary world-distance thresholds that behave badly across scene scales and vertical geometry.

**Alternatives considered**:

- Fixed world-distance threshold: rejected — a character can be far but still visible in a wide view, or near but outside the frame behind the camera.
- Immediate split/merge with no grace period: rejected — flickers at the boundary.

## R5 — Dog camera

**Decision**: A deterministic third-person follow camera derived from the dog's world position and yaw: eye behind and above the dog, looking at the dog's head; same near/far and vertical field of view as the Link camera; projection rebuilt for the panel aspect.

**Evidence**:

- The dog's world position and yaw are already read by the coop runtime (`kActorWorldPos`, `kActorWorldRot`).
- The scene view exposes the current near/far and frustum extents (`NativeA32SceneViewSnapshot`).
- The dog movement is camera-relative today (`ReadCameraYaw` reads the PlayState camera), so split mode needs a dog camera yaw for movement.

**Rationale**: The dog needs its own independent view; a fixed follow boom is deterministic, cheap, and easy to test. It does not require a second guest camera.

**Alternatives considered**:

- Use Link's camera offset applied to the dog: rejected — it does not follow the dog's facing and can put the dog off-centre.
- Full collision/occlusion camera: deferred — the first version uses the boom and validates near-plane behavior; advanced camera collision is a follow-up if needed.

## R6 — UI and top-screen composition

**Decision**: The UI draw sequence remains single and full-screen. The split panels are scene-only; the UI is rendered once after compositing. No per-player HUD, minimap, or dog-specific UI is added.

**Evidence**:

- `CompositionDomain::Ui` is already tracked.
- The top-screen UI is a separate overlay path in the runtime (`PublishOverlay`, top-screen runtime).
- The spec requires one UI instance.

**Rationale**: The user explicitly allows one UI for now and asks only for the scene camera to split.

## R7 — Movement and controls in split mode

**Decision**: In single mode, the dog keeps using Link's camera yaw as today. In split mode, the dog uses the dog camera yaw for camera-relative movement. P1 keeps Link's existing camera and controls.

**Evidence**:

- `CoopDogRuntime::ReadCameraYaw` reads the PlayState camera; this is the current single-view behavior.
- The split state knows the dog camera yaw, which is derived from the dog's yaw.

**Rationale**: If the dog view shows a different direction but movement remains relative to Link's camera, P2 controls would feel inverted or disconnected. This is a small, local behavior change only in split mode.

## R8 — Performance and failure policy

**Decision**: Measure the two scene passes. If the supported test scene cannot hold 60 FPS, reduce the secondary panel's effects/quality while keeping the UI full resolution; if it still cannot hold the target, fall back to single and record the reason. If a panel/camera is invalid, fall back to the single view; never show a blank or half-broken split.

**Evidence**:

- The existing scheduler and backend expose per-frame timing statistics.
- The renderer already has separate scene and UI composition domains, so the UI can remain full resolution if the scene panel scale changes.

**Rationale**: A split screen doubles scene rendering cost; the feature must remain responsive. A deterministic fallback preserves the single-view experience.

## R9 — Verification strategy

**Decision**:

1. Pure unit tests for projection/framing, timers, gating, dog camera, and scene-change resets.
2. Renderer tests with a synthetic draw list: Scene draws execute twice with different view/projection uniforms and disjoint viewports; Ui/Unknown execute once; the panel viewport placement and top-transfer ordering are asserted.
3. Probe fields (`split_active`, `split_timer_ms`, `link_framed`, `dog_framed`, `split_count`, `merge_count`) plus analyzer flags.
4. Runtime scenarios: near (single), separate (split), reunite (merge), P2 summon beside Link (merge), no dog (single), cutscene/pause (single), scene change (reset).
5. `game-tester` evidence: screenshot with two panels and one HUD, probe transitions, frame timing.

**Evidence**: The existing game-test harness, scene-view probe, and composition-domain trace already support these checks. The coop dog runtime already exposes the dog actor/position and the probe stream.

**Known runtime input**: The save-seed + test-warp path can fail to load `OBJECT_DOG` in some scenes; the normal save with P2 Start is known to work, and a private `TRIAEVUM_TEST_STATE` with the dog already initialized is the preferred automated input.

## Resolved Unknowns

| Unknown | Resolution |
| --- | --- |
| Can a second camera be rendered without a second guest frame? | Yes: replay the scene draws with per-draw view/projection uniform overrides and a second target. |
| How is the UI kept single? | Use the existing `CompositionDomain::Ui`; render it once after the scene panels. |
| How is "near" defined? | Project both characters into the Link view; split after 0.5 s outside the safe frame, merge after 0.5 s inside. |
| Where does the dog camera come from? | Deterministic follow camera from the dog's position/yaw, same near/far and vertical FOV, panel aspect. |
| How does P2 movement work in split? | Dog camera yaw; Link camera yaw remains the single-mode behavior. |
| What happens when the dog is absent/summoned? | No dog/inactive P2 = single; summon beside Link = merge; dog loss = merge. |
| What about menus/cutscenes/scene changes? | Force single and reset the timers; re-evaluate when gameplay resumes. |
| How is performance protected? | 60 FPS gate; secondary panel effects/quality fallback; if still below target, fall back to single; invalid view falls back to single. |

## Implementation-Time Spikes (not planning blockers)

- Composition coverage in a real gameplay frame (`Scene`/`Ui`/`Unknown` counts).
- Top-transfer ordering after the two panel viewports and the single UI pass.
- Interpolated/replayed frame behavior with the split overrides.
- Room loading when the dog leaves the currently loaded area; the first version falls back to single rather than loading a second room.
- Dog follow camera near-plane/occlusion behavior in tight spaces.
- Actual GPU cost of two scene passes and the need for a secondary render-scale fallback.

## No unresolved specifications

The spec has no `[NEEDS CLARIFICATION]` markers. The presentation defaults (0.5 s grace period, horizontal top/bottom panels, dog follow distance/height) are documented in the spec/plan and can be tuned without changing the feature behavior.
