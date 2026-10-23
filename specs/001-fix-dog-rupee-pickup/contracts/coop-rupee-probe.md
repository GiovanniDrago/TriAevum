# Contract: Co-op Rupee Probe and Test Interface

**Feature**: `001-fix-dog-rupee-pickup`
**Version**: `triaevum.coop.probe.v1` (backward-compatible extension)
**Date**: 2026-10-23

This contract covers the observable interfaces used to verify the fix. It does not expose new user-facing APIs.

## 1. Probe record

`MaybeWriteProbe` continues to append one JSON object per sampled guest frame to `coop_probe.jsonl`. Existing consumers must keep working; the new fields are additive.

### Existing fields (unchanged)

```json
{
  "schema": "triaevum.coop.probe.v1",
  "host_frame": 123,
  "guest_frame": 456,
  "play": 134217728,
  "dog_actor": 136314880,
  "dog_usable": true,
  "desired_animation": 0,
  "scene": 85,
  "mode": 3
}
```

### New fields

| Field | Type | Required | Semantics |
| --- | --- | --- | --- |
| `wallet` | integer | yes | Current SaveContext rupee balance (`0x00587958 + 0x48`) |
| `wallet_capacity` | integer | yes | Derived from `UPG_WALLET`; one of `99`, `200`, `500` |
| `rupees_collected` | integer | yes | Monotonic successful dog pickup count for the run |
| `last_rupee_value` | integer | yes | Value of the last successful pickup; `0` before the first |
| `nearby_rupees` | integer | yes | Eligible rupee actors inside the gate in this sample |

### Example

```json
{
  "schema": "triaevum.coop.probe.v1",
  "host_frame": 180,
  "guest_frame": 360,
  "play": 134217728,
  "dog_actor": 136314880,
  "dog_usable": true,
  "desired_animation": 0,
  "scene": 85,
  "mode": 3,
  "dog_pos": [100.0, 0.0, -200.0],
  "dog_velocity": [0.0, 0.0, 120.0],
  "dog_health": 4,
  "dog_models_initialized": 1,
  "dog_model_context": 134742016,
  "wallet": 42,
  "wallet_capacity": 99,
  "rupees_collected": 1,
  "last_rupee_value": 5,
  "nearby_rupees": 0
}
```

## 2. Analyzer expectations

`tools/game_test/analyze_evidence.py` MUST fail when:

- `status != COMPLETED`;
- `rupees_collected` never increases while the scenario expects a pickup;
- the wallet delta does not match `last_rupee_value` for the sampled sequence;
- `wallet > wallet_capacity` in any sample;
- the dog is not usable at the end (existing `--require-dog` behavior).

The analyzer SHOULD report the first sample where `rupees_collected` increased and the wallet before/after values.

## 3. Scenario and input contract

New files:

```text
tools/game_test/scenarios/coop_p2_rupee.json
tools/game_test/inputs/coop_p2_rupee.json
```

The scenario uses the existing harness schema and the existing P2 input schema (`triaevum.coop.test_input.v1`). No schema change is required.

Expected scenario shape:

```json
{
  "name": "coop-p2-rupee-pickup",
  "package": "${TRIAEVUM_TEST_PACKAGE}",
  "profile": "${TRIAEVUM_TEST_PROFILE}",
  "state": "${TRIAEVUM_TEST_STATE}",
  "save_data": "${TRIAEVUM_TEST_SAVE_DATA}",
  "p1_timeline": "",
  "p2_input": "tools/game_test/inputs/coop_p2_rupee.json",
  "frames": 600,
  "max_seconds": 120,
  "width": 1280,
  "height": 720,
  "probe_interval": 5,
  "expected": [
    "The game reaches gameplay without a runtime fault.",
    "P2 Start creates the dog.",
    "The dog walks over a ground rupee.",
    "The rupee is removed and the wallet increases by its value.",
    "The probe records rupees_collected >= 1 and a matching wallet delta."
  ]
}
```

The exact start frame for the walk depends on the private save/state; the input timeline must be adjusted outside the repository when the seed changes, and the scenario description records the scene and rupee denomination used.

## 4. Runtime configuration contract

`data/config/coop_controls.json` keeps the existing keys. The only behavioural change is the default value:

| Key | Old default | New default | Notes |
| --- | --- | --- | --- |
| `rupee_pickup_range` | `45.0` | `30.0` | Horizontal gate; matches Link's practical reach. Existing user files may still override it. |

The vertical gate is a fixed runtime constant of `50.0` to match Link and is not exposed as a new config key.

## 5. Compatibility

- Probe consumers that ignore unknown fields continue to work.
- Saves, savestates, and the `triaevum.coop.test_input.v1` schema are unchanged.
- No new command-line flags are introduced; verification uses the existing environment variables and `run_case.py`.
