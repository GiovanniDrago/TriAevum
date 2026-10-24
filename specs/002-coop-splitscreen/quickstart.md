# Quickstart: Verifying Co-op Dynamic Split Screen

**Feature**: `002-coop-splitscreen`
**Date**: 2026-10-23

This guide proves the feature end-to-end. It keeps ROMs, saves, savestates, screenshots, logs, and build outputs outside the repository.

## Prerequisites

- A legal, user-supplied decrypted OOT3D ROM and the official TriAevum release inputs.
- A private gameplay save or savestate where the P2 dog can be spawned and the dog is usable. Prefer a `TRIAEVUM_TEST_STATE` checkpoint with an initialized dog; the normal save with P2 Start also works.
- A build directory outside the repository, for example:

```sh
export TRIAEVUM_BUILD_DIR="$PWD/../triaevum-coop-build"
export TRIAEVUM_TEST_PACKAGE="/path/to/private/package"
export TRIAEVUM_TEST_PROFILE="/path/to/private/package/TriAevum.launch.json"
export TRIAEVUM_TEST_SAVE_DATA="/path/to/private/savedata"
```

## 1. Build and unit tests

Configure/build the affected targets:

```sh
cmake --build "$TRIAEVUM_BUILD_DIR" --target oot3d_native_game oot3d_coop_splitscreen_tests
```

Run the pure split-controller tests:

```sh
"$TRIAEVUM_BUILD_DIR/oot3d_coop_splitscreen_tests"
```

Expected: projection/framing boundaries, the 0.5 s split/merge timers, no-dog/P2-inactive/cutscene/pause/scene-change force-single rules, dog camera composition, and invalid-actor fallback all pass.

## 2. Static checks

```sh
git diff --check
python3 -m py_compile tools/game_test/run_case.py tools/game_test/analyze_evidence.py
bash -n scripts/build-and-run-linux.sh
```

## 3. Runtime scenario — near, far, merge, summon

Run the split scenario:

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_splitscreen.json
```

The harness captures the final frame. For a split-mode screenshot, use a
split-hold variant (`coop_p2_splitscreen_hold.json`) that ends while the dog is
away, or run the runtime directly with `--screenshot-sequence`. The probe is
the authoritative transition evidence in either case.

Analyze:

```sh
python3 tools/game_test/analyze_evidence.py \
  --result artifacts/game-tests/<run>/result.json \
  --require-dog \
  --expect-split \
  --min-splits 1 \
  --min-merges 1 \
  --expect-ui-single
```

Expected evidence:

- `result.json`: `COMPLETED`, no timeout, no memory fault.
- `coop_probe.jsonl`:
  - first phase `split_active: false`, `split_panels: 0`;
  - after separation `split_active: true`, `split_panels: 2`, `split_count >= 1`;
  - after reunion/summon `split_active: false`, `merge_count >= 1`;
  - `ui_passes: 1` in every sample;
  - `link_framed`/`dog_framed` explain the transition.
- Screenshot while split: two scene panels (Link top, dog bottom), one full-screen HUD, no duplicated minimap or buttons.
- Frame timing: target 60 FPS in the supported split scene; record the split-mode frame time/percentage in the report.

## 4. Runtime scenarios — safety cases

Run these cases with the same private save/state:

1. **No dog**: do not press P2 Start. `split_active` must stay `false` for the whole run.
2. **P2 inactive**: join and then disconnect P2. The scene must be single after the dog is no longer active.
3. **Cutscene/pause**: trigger a cutscene or pause while split. The scene must be single; after resuming separated, it must split again.
4. **Scene change**: change scene while split. The scene must be single during the transition and re-evaluate after gameplay resumes.
5. **Summon**: while split, press P2 Start near Link. The scene must merge; `merge_count` increases.

Record the scenario name, start/end mode, transition counts, screenshot paths, and any blocked input.

## 5. Performance check

Read `runtime.json` from the split run:

- frame count and split-mode frame time;
- no guest stall or memory fault;
- target 60 FPS.

If the split scene is below target, verify the secondary-panel effects/quality fallback and repeat; if it still cannot hold the target, falling back to single is the safe outcome. A performance failure is not a visual PASS.

## 6. Troubleshooting

| Symptom | Check |
| --- | --- |
| Never splits | `split_active`, `link_framed`, `dog_framed`; the safe margin and 0.5 s grace; the dog is active and P2 is controlling it. |
| Splits but UI is duplicated | The `Ui` composition draws are being replayed with the scene; fix domain classification before visual acceptance. |
| Split shows the same camera twice | The dog view/projection uniform override is missing or the panel camera is not applied. |
| Dog panel is blank/black | The dog viewport/camera is invalid or the scissor clips everything; the runtime must fall back to single. |
| No dog | Use the normal save with P2 Start or a private `TRIAEVUM_TEST_STATE` with the dog initialized. The save-seed + test-warp path can fail to load `OBJECT_DOG`. |
| Screenshot is black/startup | The capture path returned an early frame; use the screenshot sequence or a later frame and do not claim a visual PASS from a black image. |
| FPS below target | Measure the split passes; reduce the secondary panel scale/effects, never the UI, and repeat. |

## 7. Evidence retention

Keep the run directory under `artifacts/game-tests/<run>/` (ignored by git) and quote its path in the final report. Do not attach the ROM, save, savestate, or personal paths. A `game-tester` report must state PASS, FAIL, or BLOCKED with the exact evidence paths.
