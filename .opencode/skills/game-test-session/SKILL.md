---
name: game-test-session
description: End-to-end TriAevum gameplay verification using the repository harness, screenshots, scripted P1/P2 input, and runtime memory probes.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## What this skill does

Coordinates a complete gameplay verification run:

1. validate the package and launch profile;
2. choose a deterministic save or savestate;
3. run P1 and P2 scripted input;
4. capture the framebuffer;
5. inspect runtime JSON, logs, and `coop_probe.jsonl`;
6. report evidence to the parent agent.

## Required environment

```text
TRIAEVUM_TEST_PACKAGE   package directory containing TriAevum
TRIAEVUM_TEST_PROFILE   TriAevum launch profile
TRIAEVUM_TEST_STATE     optional private .oot3dsav checkpoint
TRIAEVUM_TEST_SAVE_DATA optional save-data seed directory
DISPLAY or WAYLAND_DISPLAY
```

## Standard command

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_smoke.json
```

Then:

```sh
python3 tools/game_test/analyze_evidence.py \
  --result artifacts/game-tests/<run>/result.json
```

## Rules

- One game instance at a time unless the parent explicitly provides isolated
  packages, save directories, displays, and GPU caches.
- Never modify the user's save data. The harness copies a seed into a temporary
  run directory.
- Never edit source, tasks.md, or Git state.
- A compile or a log line is not a visual pass. Read the screenshot.
- If any required input is missing, return `BLOCKED` with the exact missing
  variable or file.
