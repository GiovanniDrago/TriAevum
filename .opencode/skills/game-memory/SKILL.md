---
name: game-memory
description: Inspect TriAevum runtime JSON, logs, savestate evidence, and coop_probe.jsonl to verify actor, model, animation, position, and failure state.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## Evidence sources

- `runtime.json` from `--output`;
- `run.log` from the harness;
- `coop_probe.jsonl` from `TRIAEVUM_COOP_PROBE`;
- optional savestates and `oot3d_native_a32_savestate_diff`.

## Probe fields

Each probe line uses:

```text
schema: triaevum.coop.probe.v1
host_frame, guest_frame, play, scene, mode
dog_actor, dog_usable, desired_animation
dog_pos, dog_velocity, dog_health
dog_models_initialized, dog_model_context
```

Use the probe to distinguish:

- dog never spawned: `dog_actor` stays `0`;
- spawned then disappeared: non-zero followed by zero;
- unusable actor: non-zero but `dog_usable` false;
- model problem: `dog_models_initialized == 0` or `dog_model_context == 0`;
- frozen animation: `desired_animation` stays constant while movement occurs;
- position/physics issue: inspect `dog_pos` and `dog_velocity` over frames.

## Analysis command

```sh
python3 tools/game_test/analyze_evidence.py \
  --result artifacts/game-tests/<run>/result.json
```

The report includes spawn transitions, last position, animation, model state,
exit code, and artifact paths.

## Runtime JSON

Check at least:

- process/exit status;
- frame and guest-frame counts;
- memory faults and whole-AOT memory faults;
- input diagnostics;
- screenshot existence;
- any savestate failure count.

## Rules

- Never claim PASS from a memory fingerprint alone.
- State the exact field and frame for every memory conclusion.
- If the probe is missing, report `BLOCKED` or `FAIL`, not an inferred success.
