---
name: game-save-load
description: Load a private TriAevum savestate or save-data seed without modifying the user's save files.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## Preferred: savestate

Use a private checkpoint supplied through `TRIAEVUM_TEST_STATE`:

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_smoke.json
```

The scenario passes:

```text
--load-state "$TRIAEVUM_TEST_STATE"
```

Savestates are deterministic and avoid title/file-select timing. They are not
committed to the repository.

## Save file seed

If the scenario must load a real save:

- put the original save files in a private directory;
- set `TRIAEVUM_TEST_SAVE_DATA` to that directory;
- use the existing title timeline:

```text
tools/oot3d/native_game_runtime/input_timelines/title_to_existing_save.json
```

The harness copies the seed into `artifacts/game-tests/<run>/savedata`; the
guest never writes the user's original files.

## Rules

- Never commit `.sav`, `.oot3dsav`, savedata directories, or save-derived
  screenshots.
- Do not assume a particular title-screen timing. Prefer `--load-state`.
- If neither a state nor a valid seed is available, return `BLOCKED`.
- Do not run two scenarios against the same save directory concurrently.
