---
name: game-input
description: Drive TriAevum P1 and P2 with deterministic input timelines, including P2 dog spawn, movement, jump, bark, sprint, and summon.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## P1: runtime input timeline

The runtime accepts:

```text
--input-timeline FILE
```

Schema:

```json
{
  "schema": "oot3d.native_game.input_timeline.v1",
  "frame_origin": "run",
  "segments": [
    {"start_frame": 100, "end_frame_exclusive": 110, "buttons": ["start"]},
    {"start_frame": 160, "end_frame_exclusive": 260, "circle_y": 1.0}
  ]
}
```

- `frame_origin`: `guest` or `run`; use `run` for a test from process start.
- Buttons: `a`, `b`, `x`, `y`, `l`, `r`, `zl`, `zr`, `start`, `select`,
  `up`, `down`, `left`, `right`.
- Circle axes: `circle_x`, `circle_y` in `[-1, 1]`.
- Touch: `{"touch": {"pressed": true, "x": 96, "y": 221}}`.
- Existing examples live in
  `tools/oot3d/native_game_runtime/input_timelines/`.

## P2: coop test input

Set:

```text
TRIAEVUM_COOP_TEST_INPUT=tools/game_test/inputs/coop_p2_smoke.json
```

Schema:

```json
{
  "schema": "triaevum.coop.test_input.v1",
  "frame_origin": "run",
  "segments": [
    {"start_frame": 20, "end_frame_exclusive": 24, "buttons": ["start"]},
    {"start_frame": 60, "end_frame_exclusive": 160, "move_y": 1.0},
    {"start_frame": 180, "end_frame_exclusive": 184, "buttons": ["a"]},
    {"start_frame": 240, "end_frame_exclusive": 244, "buttons": ["b"]},
    {"start_frame": 300, "end_frame_exclusive": 304, "buttons": ["dpad_down"]}
  ]
}
```

Supported fields:

- `move_x`, `move_y`: camera-relative stick values in `[-1, 1]`;
- `sprint`: boolean, equivalent to ZL;
- `buttons`: `start`, `a`, `b`, `zl`, `dpad_down`.

This input is test-only and bypasses SDL for P2, so it does not depend on a
physical controller or virtual gamepad.

## Rules

- P1 and P2 timelines must be coordinated by frame.
- Start P2 only after the game has reached gameplay (prefer `--load-state`).
- Keep button segments short so the edge-triggered action is registered once.
- Do not use a test input file in a normal player session.
