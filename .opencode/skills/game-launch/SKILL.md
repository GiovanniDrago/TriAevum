---
name: game-launch
description: Validate and launch the TriAevum native game package with a bounded timeout, isolated save data, and captured logs.
license: GPL-3.0-only
compatibility: opencode
metadata:
  audience: game-tester
  workflow: runtime-verification
---
## Prerequisites

- `TRIAEVUM_TEST_PACKAGE` points to a directory containing `TriAevum`.
- `TRIAEVUM_TEST_PROFILE` points to `TriAevum.launch.json`.
- `DISPLAY` or `WAYLAND_DISPLAY` is available.
- The package has already been built and imported with a legal user ROM.

Check:

```sh
test -x "$TRIAEVUM_TEST_PACKAGE/TriAevum"
test -f "$TRIAEVUM_TEST_PROFILE"
```

## Preferred launcher

Use the harness so the launch is bounded and artifacts are isolated:

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_smoke.json \
  --output-dir artifacts/game-tests/manual-launch
```

## Direct launch contract

The runtime supports:

```text
--launch-profile FILE
--save-data DIRECTORY
--load-state FILE
--input-timeline FILE
--output FILE
--screenshot FILE
--frames N
--max-seconds N
--width N --height N
```

Do not run the game without a timeout. Do not write into the user's normal
`data/savedata` directory. Do not add personal package, ROM, or save paths to
tracked files.

## Expected artifacts

```text
artifacts/game-tests/<run>/run.log
artifacts/game-tests/<run>/runtime.json
artifacts/game-tests/<run>/final.png
artifacts/game-tests/<run>/coop_probe.jsonl
artifacts/game-tests/<run>/result.json
```

If the process exits non-zero or times out, preserve the log and report
`FAILED` or `TIMEOUT`; do not continue to a visual PASS claim.
