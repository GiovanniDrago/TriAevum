# Contract: Co-op Split-Screen Probe and Render Plan

**Feature**: `002-coop-splitscreen`
**Version**: `triaevum.coop.probe.v1` (backward-compatible extension)
**Date**: 2026-10-23

This contract defines the observable probe fields, analyzer expectations, configuration defaults, and the internal split render plan used by the runtime and renderer tests. It does not add a user-facing API.

## 1. Probe record

`MaybeWriteProbe` continues to append one JSON object per sampled guest frame to `coop_probe.jsonl`. Existing fields are unchanged; the split fields are additive.

### New fields

| Field | Type | Required | Semantics |
| --- | --- | --- | --- |
| `split_active` | boolean | yes | The scene was split for this sample |
| `split_reason` | string | yes | `dog_active`, `no_dog`, `p2_inactive`, `cutscene`, `pause`, `loading`, `invalid_actor`, `invalid_camera`, `reset`, `disabled` |
| `split_timer_ms` | integer | yes | Active outside/inside timer, 0..500 |
| `link_framed` | boolean | yes | Link is inside the shared-view safe margin |
| `dog_framed` | boolean | yes | Dog is inside the shared-view safe margin |
| `split_count` | integer | yes | Monotonic split transitions in the run |
| `merge_count` | integer | yes | Monotonic merge transitions in the run |
| `split_panels` | integer | yes | `0` for single, `2` for split |
| `ui_passes` | integer | yes | Always `1` for this feature |
| `dog_camera_eye` | number[3] | when split | Dog camera eye |
| `dog_camera_at` | number[3] | when split | Dog camera look-at |

### Example

```json
{
  "schema": "triaevum.coop.probe.v1",
  "host_frame": 900,
  "guest_frame": 420,
  "scene": 85,
  "mode": 3,
  "dog_actor": 160575888,
  "dog_usable": true,
  "split_active": true,
  "split_reason": "dog_active",
  "split_timer_ms": 0,
  "link_framed": true,
  "dog_framed": false,
  "split_count": 1,
  "merge_count": 0,
  "split_panels": 2,
  "ui_passes": 1,
  "dog_camera_eye": [38.0, 210.0, -58.0],
  "dog_camera_at": [20.0, 200.0, -22.0]
}
```

## 2. Analyzer expectations

`tools/game_test/analyze_evidence.py` MUST support:

- `--expect-split`: fail unless `split_active` is true in at least one sample and `split_panels == 2` in those samples.
- `--expect-single`: fail if any sample has `split_active` true.
- `--min-splits N`: fail unless `split_count` increases by at least `N`.
- `--min-merges N`: fail unless `merge_count` increases by at least `N`.
- `--expect-ui-single`: fail if `ui_passes != 1` in any sample.
- Existing `--require-dog`, `--expect-pickups`, `--expect-wallet-delta`, `--expect-rupee-kept` remain valid.

The analyzer MUST report the first split sample, the first merge sample, and the transition counts in `analysis.json`.

## 3. Config contract

Optional `coop_controls.json` keys and defaults:

| Key | Default | Notes |
| --- | --- | --- |
| `splitscreen_enabled` | `true` | Developer/config switch; no in-game toggle |
| `splitscreen_grace_ms` | `500` | Split and merge grace |
| `splitscreen_safe_margin` | `0.9` | Inner normalized frame |
| `splitscreen_layout` | `"horizontal"` | `horizontal` = top/bottom |
| `dog_camera_distance` | `180.0` | Follow distance |
| `dog_camera_height` | `90.0` | Follow height |
| `dog_camera_target_height` | `30.0` | Look-at height |

Invalid values clamp to safe ranges; a missing config keeps these defaults.

## 4. Scenario contract

New files:

```text
tools/game_test/scenarios/coop_p2_splitscreen.json
tools/game_test/inputs/coop_p2_splitscreen.json
tools/game_test/scenarios/coop_p2_splitscreen_hold.json
tools/game_test/inputs/coop_p2_splitscreen_hold.json
```

`coop_p2_splitscreen` covers the full near → far → merge → summon transition.
`coop_p2_splitscreen_hold` ends while the dog is away so the harness's final
screenshot shows the two panels; the probe still records the transition.

The scenario uses the existing harness schema and the normal title-to-save timeline (with the first-save selection step). P2 input phases:

1. Start after gameplay begins.
2. Walk away from Link for long enough to exceed the shared frame and the 0.5 s grace.
3. Hold still briefly (split stable).
4. Walk back toward Link (merge).
5. Press Start to summon the dog beside Link (merge confirmation).

Expected:

```text
split_active: false -> true -> false
split_count >= 1
merge_count >= 1
split_panels: 0/2/0
ui_passes: 1 in every sample
screenshot: two scene panels while split, one HUD
```

The private save/state and P2 timing must be supplied through the documented harness environment; no save or state is committed.

## 5. Internal render plan contract

The renderer consumes a plan equivalent to:

```cpp
enum class CoopSplitMode { Single, Split };

struct CoopSplitPanel {
  CoopSplitSource Source;       // Link / Dog
  PanelRect Rect;               // top/bottom half
  SplitCamera Camera;
  PanelViewport Viewport;        // top/bottom half of the canonical target
};

struct CoopSplitRenderPlan {
  CoopSplitMode Mode = CoopSplitMode::Single;
  std::array<CoopSplitPanel, 2> Panels{};
  bool UiSingle = true;
  bool TopTransferAfterSplit = true;
  CoopSplitReason Reason = CoopSplitReason::NoDog;
};
```

Renderer invariants:

1. `Scene` draws execute once per panel in split mode, with the panel's view/projection uniforms, viewport, and light binding.
2. `Ui` draws execute exactly once, full-screen, after both panel viewports.
3. `Unknown` draws execute exactly once in the full-screen pass; they are not duplicated.
4. The top-screen snapshot/transfer runs after both panel viewports and the single UI pass.
5. In single mode the draw list and UI pass are the existing path, with no split overrides; in split mode the canonical top target is shared by both panels and the UI.
6. If either panel camera/target is invalid, the plan falls back to `Single`.

## 6. Compatibility

- Only additive probe fields; existing consumers keep working.
- Save/savestate formats are unchanged.
- No new command-line flags; verification uses the existing harness plus the new scenario.
- Split is inactive by default when the dog is absent or P2 is inactive; the single-view path remains the existing behavior.
