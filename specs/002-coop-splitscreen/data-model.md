# Data Model: Co-op Dynamic Split Screen

**Feature**: `002-coop-splitscreen`
**Date**: 2026-10-23

This feature has no persisted data. It derives a per-frame presentation state from the guest camera, the Link actor, the dog actor, and the current PICA composition. The model below is the internal contract between the coop runtime, the split controller, the renderer, and the probe.

## 1. Split Screen State

| Field | Type | Meaning |
| --- | --- | --- |
| `Mode` | `Single` / `Split` | Current scene presentation |
| `OutsideTimerMs` | `uint32_t` | Time one character has been outside the shared frame while `Single` |
| `InsideTimerMs` | `uint32_t` | Time both characters have been inside the shared frame while `Split` |
| `LinkFramed` | `bool` | Link is inside the shared frame with the safe margin |
| `DogFramed` | `bool` | Dog is inside the shared frame with the safe margin |
| `SplitCount` | `uint32_t` | Monotonic split transitions in the run |
| `MergeCount` | `uint32_t` | Monotonic merge transitions in the run |
| `Reason` | enum | `DogActive`, `NoDog`, `P2Inactive`, `Cutscene`, `Pause`, `Loading`, `InvalidActor`, `InvalidCamera` |
| `LastUpdateFrame` | `uint64_t` | Guest or presentation frame of the last evaluation |

### State transitions

```text
Unavailable / no dog / P2 inactive / cutscene / pause / loading / invalid
  → Single (force, reset timers)

Single
  → [both framed] reset OutsideTimerMs; stay Single
  → [one outside] OutsideTimerMs += dt
  → [OutsideTimerMs >= 500 ms] Split (SplitCount++, reset timers)

Split
  → [both framed] InsideTimerMs += dt
  → [one outside] reset InsideTimerMs; stay Split
  → [InsideTimerMs >= 500 ms] Single (MergeCount++, reset timers)

Scene change / teleport / play-state change
  → Single (force, reset timers, clear camera history)

P2 summon / Start beside Link
  → Single when the dog is valid and framed; otherwise re-evaluate normally
```

## 2. Character Frame

| Field | Type | Meaning |
| --- | --- | --- |
| `ActorAddress` | `uint32_t` | Guest actor address, validated before use |
| `WorldPosition` | `float[3]` | Actor world position |
| `WorldHeight` | `float` | Height offset used to project the character's body centre |
| `Projected` | `float[3]` | NDC point in the Link camera |
| `InsideSafeMargin` | `bool` | Projected point (with a body margin) is inside the safe frame and in front of the near plane |
| `Valid` | `bool` | Actor id/list membership/position read succeeded |

The Link frame uses the player actor; the dog frame uses the coop dog actor. The safe margin is a configurable normalized border (default `0.9`, i.e. the inner 90% of the view) so a character at the extreme edge is considered outside.

## 3. Split Camera

| Field | Type | Meaning |
| --- | --- | --- |
| `Eye` | `float[3]` | Camera position |
| `At` | `float[3]` | Look-at point |
| `Up` | `float[3]` | Up vector; world up for the first version |
| `VerticalFov` | `float` | Same vertical field of view as the source scene view |
| `NearPlane` | `float` | Same near plane as the source scene view |
| `FarPlane` | `float` | Same far plane as the source scene view |
| `Aspect` | `float` | Panel width / panel height |
| `View` | `float[3][4]` | World-to-view 3x4 rows |
| `Projection` | `float[4][4]` | Panel projection |

### Link camera

The existing scene view camera. Its projection is rebuilt for the panel aspect while preserving the source vertical extents and near/far planes.

### Dog camera

A deterministic follow camera:

```text
yaw       = dog world yaw
forward   = (sin(yaw), 0, cos(yaw))
eye       = dogPosition
          - forward * DogCameraDistance
          + worldUp * DogCameraHeight
at        = dogPosition + worldUp * DogCameraTargetHeight
up        = worldUp
verticalFov, near, far = source scene view
aspect    = panel width / panel height
```

Defaults to be tuned in `coop_controls.json`: distance `180`, height `90`, target height `30`, in world units. The camera is not a second guest camera and does not run a second update.

## 4. View Panel

| Field | Type | Meaning |
| --- | --- | --- |
| `Source` | enum | `Link` / `Dog` |
| `Rect` | `{x, y, width, height}` | Destination rectangle in the top target |
| `Camera` | `SplitCamera` | Panel camera |
| `Viewport` | `{cornerX, cornerY, halfWidth, halfHeight, depthRange}` | Panel viewport in the canonical top target |
| `Scissor` | optional rect | Optional panel clip for scene effects |
| `SceneDrawCount` | `uint32_t` | Scene draws executed in the panel |
| `Valid` | `bool` | Camera/target are usable |

Default layout: horizontal split, Link on top, dog on the bottom. Each panel occupies the full width and half the top-target height. The panel render targets use the panel dimensions; the UI uses the full top-target dimensions.

## 5. Split Render Plan

| Field | Type | Meaning |
| --- | --- | --- |
| `Mode` | `Single` / `Split` | Renderer execution mode |
| `Panels` | `ViewPanel[2]` | Present only in split mode |
| `UiSingle` | `bool` | Always true in this feature |
| `TopTransferAfterSplit` | `bool` | The top-screen snapshot/transfer runs after both panel viewports + the single UI |
| `UnknownDomainPolicy` | enum | `Single` (default): unclassified draws stay in the full-screen UI pass |
| `Reason` | enum | Diagnostic reason from `SplitScreenState` |

## 6. Probe Fields

Additive `triaevum.coop.probe.v1` fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `split_active` | bool | The scene was split for the sampled frame |
| `split_reason` | string | `dog_active`, `no_dog`, `p2_inactive`, `cutscene`, `pause`, `loading`, `invalid_actor`, `invalid_camera`, `reset`, `disabled` |
| `split_timer_ms` | int | Active outside/inside timer in milliseconds |
| `link_framed` | bool | Link framed in the shared view |
| `dog_framed` | bool | Dog framed in the shared view |
| `split_count` | int | Monotonic split transitions |
| `merge_count` | int | Monotonic merge transitions |
| `dog_camera_eye` | float[3] (optional) | Dog camera eye when split |
| `dog_camera_at` | float[3] (optional) | Dog camera at when split |

The probe remains additive; consumers that ignore unknown fields continue to work.

## 7. Configuration Defaults

Optional `coop_controls.json` keys, all with safe defaults:

| Key | Default | Meaning |
| --- | --- | --- |
| `splitscreen_enabled` | `true` | Developer/config switch; no user-facing toggle |
| `splitscreen_grace_ms` | `500` | Split/merge grace period |
| `splitscreen_safe_margin` | `0.9` | Inner normalized frame for "framed together" |
| `splitscreen_layout` | `"horizontal"` | `horizontal` (top/bottom) or `vertical` (left/right) |
| `dog_camera_distance` | `180.0` | Follow distance |
| `dog_camera_height` | `90.0` | Follow height |
| `dog_camera_target_height` | `30.0` | Look-at height above the dog |

## 8. Validation Rules

| Rule | Requirement |
| --- | --- |
| Dog must be active, usable, and in the current actor lists | FR-001, FR-008, FR-018 |
| Link must be a valid player actor with a readable position | FR-002, FR-018 |
| Scene view/frustum must be available for framing | FR-002, FR-018 |
| One character outside the safe frame starts the outside timer | FR-003 |
| Both inside the safe frame starts the inside timer | FR-004 |
| Grace period is applied symmetrically | FR-013, SC-006 |
| Both panels have positive dimensions and disjoint viewport rectangles | FR-005, FR-019 |
| UI draws execute exactly once, full-screen | FR-006, FR-007, SC-007 |
| Scene draws execute once per panel with that panel's camera and viewport | FR-005, SC-010 |
| `Ui` and `Unknown` draws never duplicate | FR-006, FR-007 |
| Cutscene/pause/loading/invalid force Single and reset timers | FR-011, FR-012, FR-018 |
| Scene change/teleport clears camera history and re-evaluates | FR-012, SC-011 |
| Split is never active without a valid dog/P2 | FR-008, FR-018 |

## 9. Invariants

- The UI is drawn exactly once per presented frame, in both modes.
- The guest simulation runs exactly once per frame, regardless of split mode.
- Each scene panel uses its own camera; panel cameras never share a viewport or depth target.
- No split is shown during non-gameplay states.
- No stale split survives a scene change, teleport, or dog loss.
- A failed second view falls back to single, never a blank panel.
- The single-view path is byte-for-byte the existing path when no dog/split is active.
