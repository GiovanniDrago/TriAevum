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

The timeline must select the existing save before pressing A. On the
file-select screen the default cursor can sit on an empty slot; pressing A
there opens name entry and never reaches gameplay. The shared timeline includes
`circle_y: 1.0` at frame 520 to move to the first save. If the seed is in a
different slot, change the direction/frame count and record the slot. A
name-entry stop is a setup failure; do not report it as a gameplay result.

The harness copies the seed into `artifacts/game-tests/<run>/savedata`; the
guest never writes the user's original files.

## Rules

- Never commit `.sav`, `.oot3dsav`, savedata directories, or save-derived
  screenshots.
- Do not assume a particular title-screen timing. Prefer `--load-state`.
- If neither a state nor a valid seed is available, return `BLOCKED`.
- Do not run two scenarios against the same save directory concurrently.
