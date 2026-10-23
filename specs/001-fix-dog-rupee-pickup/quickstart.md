# Quickstart: Verifying Co-op Dog Rupee Pickup

**Feature**: `001-fix-dog-rupee-pickup`
**Date**: 2026-10-23

This guide proves the fix end-to-end. It never copies a ROM, save, savestate, screenshot, log, or build directory into the repository.

## Prerequisites

- A legal, user-supplied decrypted OoT3D ROM and the official TriAevum release inputs (see `README.md` and `scripts/build-and-run-linux.sh`).
- A private gameplay checkpoint or save seed (`TRIAEVUM_TEST_STATE` or `TRIAEVUM_TEST_SAVE_DATA`) that starts near a ground rupee. The checkpoint itself stays outside the repository. When using a save seed through the title/file-select flow, the timeline must select the existing save before pressing A; the default empty slot opens name entry instead of gameplay. `title_to_existing_save.json` includes `circle_y: 1.0` at frame 520 for the first save.
- A build directory outside the repository, for example:

```sh
export TRIAEVUM_BUILD_DIR="$PWD/../triaevum-linux-build"
export TRIAEVUM_TEST_PACKAGE="/path/to/private/package"
export TRIAEVUM_TEST_PROFILE="/path/to/private/package/TriAevum.launch.json"
export TRIAEVUM_TEST_STATE="/path/to/private/checkpoint.oot3dsav"
```

## 1. Build the runtime and the unit tests

First configure (the helper installs the release/title inputs and keeps them under `XDG` data):

```sh
scripts/build-and-run-linux.sh --no-launch /path/to/decrypted-game.cci
```

For iteration on this feature, rebuild only the affected targets:

```sh
cmake --build "$TRIAEVUM_BUILD_DIR" --target oot3d_native_game oot3d_coop_rupee_tests
```

Run the pure unit tests:

```sh
"$TRIAEVUM_BUILD_DIR/oot3d_coop_rupee_tests"
```

Expected: every rupee code maps to the right value, unknown codes are rejected, the 30-unit horizontal and ±50 vertical boundaries behave, and the collectible-delay gate rejects early pickups.

## 2. Static checks

```sh
git diff --check
python3 -m py_compile tools/game_test/run_case.py tools/game_test/analyze_evidence.py
bash -n scripts/build-and-run-linux.sh
```

## 3. Runtime scenario — rupee pickup

Run the deterministic scenario:

```sh
python3 tools/game_test/run_case.py \
  --scenario tools/game_test/scenarios/coop_p2_rupee.json
```

Analyze the evidence:

```sh
python3 tools/game_test/analyze_evidence.py \
  --result artifacts/game-tests/<run>/result.json \
  --require-dog
```

Expected evidence:

- `result.json` has `status: COMPLETED`, no timeout, no memory fault.
- `coop_probe.jsonl` shows `dog_usable: true` and `rupees_collected` increasing by at least 1.
- The wallet delta equals `last_rupee_value` (allow for the accumulator draining over a few frames).
- `wallet <= wallet_capacity` in every sample.
- `final.png` shows the expected scene and HUD; the collected rupee is no longer visible at the pickup position.

Record in the verification report: scene name, rupee denomination, start/end wallet values, `rupees_collected`, and the evidence directory.

## 4. Runtime scenario — full wallet (guard)

Use a private save seed with the wallet filled to capacity, or fill it in the scenario setup, then walk the dog over a rupee. Expected:

- `rupees_collected` does **not** increase for that rupee.
- `wallet` is unchanged.
- The rupee remains visible in `final.png` and is still present in the probe's `nearby_rupees` while the dog stands on it.

If the OOT3D reference is later proven to consume the rupee in this case, update the spec and this scenario together; do not change the guard silently.

## 5. Regression checks

- Link alone collects a rupee normally (single-player path untouched).
- P2 dog movement, jump, bark, sprint, and summon still work (`coop_p2_smoke.json`).
- No new warnings in the affected build target.
- No ROM, save, screenshot, log, or build output appears in `git status`.

## 6. Evidence retention

Keep the run directory under `artifacts/game-tests/<run>/` (ignored by git) and quote its path in the final report. Do not attach the ROM, save, or savestate. The `game-tester` subagent report must state PASS, FAIL, or BLOCKED with the evidence paths.

## 7. Troubleshooting

| Symptom | Check |
| --- | --- |
| Dog passes through a rupee | Confirm the probe's `nearby_rupees` count; if 0, the gate or the item-code mapping is still wrong. |
| Wallet does not change | Confirm `rupees_collected` increased; if it did but the wallet did not, check the accumulator/HUD path and whether the rupee was already at capacity. |
| Rupee stays after a successful credit | `Actor_Kill` may have failed; the dedup set should still prevent a second credit. Capture the `Actor_Kill` error path in the log. |
| Scenario cannot find a rupee | The private checkpoint/seed is scene-specific; adjust the P2 input timeline outside the repository and record the denomination used. |
| Run stops at name entry | The file-select cursor was on an empty slot. Select the existing save before A; `title_to_existing_save.json` does this with `circle_y: 1.0` at frame 520. |
| Dog never spawns, log says `OBJECT_DOG object context is not ready yet` | The loaded scene has no resident dog object. Use a private `TRIAEVUM_TEST_STATE` checkpoint where the dog already exists/initialized, or a package/save where `OBJECT_DOG` is loaded. |
| No audio | Expected fallback: guest audio is not invoked from the host. HUD count ticks provide feedback. See `research.md`, R4. |
