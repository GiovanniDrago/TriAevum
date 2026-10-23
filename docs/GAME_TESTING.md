# Gameplay Verification

This directory documents the automated gameplay verification path used by the
`game-tester` OpenCode subagent. The harness launches the native runtime,
loads a private save/state, drives scripted P1 and P2 input, captures the
framebuffer, and reads semantic runtime probes.

No ROM, save, savestate, screenshot, log, or personal path belongs in this
repository.

## Components

| Path | Purpose |
| --- | --- |
| `.opencode/agents/game-tester.md` | Specialized subagent for gameplay evidence |
| `.opencode/commands/game-test.md` | Manual `/game-test` command |
| `.opencode/skills/game-*` | Reusable procedures for launch, input, capture, memory, report |
| `tools/game_test/run_case.py` | Bounded scenario runner |
| `tools/game_test/analyze_evidence.py` | Runtime/probe summary |
| `tools/game_test/scenarios/` | Versioned scenario definitions |
| `tools/game_test/inputs/` | Versioned P1/P2 input timelines |
| `artifacts/game-tests/` | Ignored local evidence output |

## Environment

```text
TRIAEVUM_TEST_PACKAGE    directory containing TriAevum and the launch profile
TRIAEVUM_TEST_PROFILE    TriAevum.launch.json
TRIAEVUM_TEST_STATE      optional private .oot3dsav checkpoint
TRIAEVUM_TEST_SAVE_DATA  optional save-data seed directory
DISPLAY / WAYLAND_DISPLAY
```

The harness creates a private `savedata` copy under the run directory. It never
writes the user's save data.

## Run

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_smoke.json
```

Analyze:

```sh
python3 tools/game_test/analyze_evidence.py \
  --result artifacts/game-tests/<run>/result.json --require-dog
```

Outputs:

```text
artifacts/game-tests/<run>/
├── result.json
├── run.log
├── runtime.json
├── final.bmp
├── final.png
├── coop_probe.jsonl
└── savedata/
```

`result.json` records the exact command, exit code, timeout state, duration,
artifact paths, probe sample count, and scenario expectations. It does not
declare a visual PASS by itself.

## Save loading

Preferred:

```text
--load-state "$TRIAEVUM_TEST_STATE"
```

The state must match the gameplay timing and be supplied outside the repository.
For real save files, set `TRIAEVUM_TEST_SAVE_DATA` and use
`title_to_existing_save.json`; the harness copies the seed into the run
directory.

When a save seed is loaded through the title/file-select flow, the P1 timeline
must move the file-select cursor onto the existing save before pressing A.
The default cursor can sit on an empty slot; pressing A there opens the
name-entry screen and never reaches gameplay. `title_to_existing_save.json`
therefore holds `circle_y: 1.0` at frame 520 to select the first save before
the A press at frame 600. If the seed lives in a different slot, change the
direction/frame count and record the slot in the scenario description. A run
that stops at name entry is a scenario-setup failure, not a gameplay result.

## P1 input

Use the runtime schema `oot3d.native_game.input_timeline.v1` with
`--input-timeline`. Existing examples are in
`tools/oot3d/native_game_runtime/input_timelines/`.

## P2 input

The coop runtime supports a test-only scripted input file selected by:

```text
TRIAEVUM_COOP_TEST_INPUT
```

Schema `triaevum.coop.test_input.v1`:

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

Supported fields: `move_x`, `move_y`, `sprint`, and `buttons` containing
`start`, `a`, `b`, `zl`, `dpad_down`. This bypasses SDL for P2 and is inactive
unless the environment variable is set.

## Screenshots

The runtime writes direct framebuffer captures through `--screenshot`; the
harness converts BMP to PNG when Pillow is available. The subagent must read the
image and report visible scene, Link, P2 dog, HUD, and anomalies. Desktop
screenshots are not used.

## Memory probe

Set `TRIAEVUM_COOP_PROBE` to a JSONL path (the harness does this) and optionally
`TRIAEVUM_COOP_PROBE_INTERVAL` (default 15 guest frames).

Each line contains:

```text
schema, host_frame, guest_frame, play, scene, mode
dog_actor, dog_usable, desired_animation
dog_pos, dog_velocity, dog_health
dog_models_initialized, dog_model_context
```

Use it to distinguish:

- never spawned: `dog_actor == 0`;
- spawned then disappeared: non-zero followed by zero;
- unusable actor: non-zero with `dog_usable == false`;
- model problem: `dog_models_initialized == 0` or `dog_model_context == 0`;
- frozen animation or physics: compare `desired_animation`, positions and
  velocity across frames.

`runtime.json` supplies process/frame/memory-fault and input diagnostics.
Savestate diffs remain available for state-level comparisons.

## Report

The `game-tester` returns:

```text
STATUS: PASS | FAIL | BLOCKED
SCENARIO
COMMANDS
ARTIFACTS
EXPECTED
OBSERVED
MEMORY
SCREENSHOTS
ERRORS
NEXT ACTION
```

`PASS` requires a completed run, a readable screenshot, and the expected
runtime/probe evidence. Missing inputs are `BLOCKED`, never `PASS`.

## Updating the skills

Edit the relevant `.opencode/skills/game-*/SKILL.md` and
`docs/GAME_TESTING.md` together. Keep commands copy-pasteable, keep paths
relative to the repository, and update the scenario/input files when the runtime
CLI or probe schema changes. Do not add machine-specific paths.

## Co-op rupee probe

The co-op runtime appends these additive fields to
`triaevum.coop.probe.v1`:

| Field | Meaning |
| --- | --- |
| `wallet` | SaveContext rupee balance (`0x00587958 + 0x48`) |
| `wallet_capacity` | Derived from `UPG_WALLET` (`99`, `200`, `500`) |
| `rupees_collected` | Monotonic successful dog pickup count |
| `last_rupee_value` | Denomination of the last successful pickup |
| `nearby_rupees` | Eligible rupee actors inside the pickup gate |

`analyze_evidence.py` supports `--expect-pickups` and
`--expect-wallet-delta N`; it always fails when `wallet > wallet_capacity` in a
sample. Use the rupee scenario at
`tools/game_test/scenarios/coop_p2_rupee.json` together with a save seed or
savestate that places the dog near a ground rupee.

The dog pickup uses the game's accumulator path (`Rupees_ChangeBy`); the HUD
wallet count and its normal count ticks are the feedback. Guest audio is not
invoked from the host because `Audio_PlayActorSound2` is known to fault in this
path; a host `NativeAudioService` request is the only sanctioned alternative if
the initial pickup ding is added later.

### Known verification blockers

- A save seed that starts in a scene without a resident `OBJECT_DOG` cannot
  spawn the dog: the log reports `OBJECT_DOG object context is not ready yet`.
  Use a private `TRIAEVUM_TEST_STATE` checkpoint where the dog already exists
  and is initialized.
- The bounded `--screenshot` path can return a black or startup frame in some
  staged packages. Treat a missing final-frame image as a capture defect, not
  as gameplay evidence.
