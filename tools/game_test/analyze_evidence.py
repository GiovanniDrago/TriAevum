#!/usr/bin/env python3
"""Summarize a TriAevum gameplay test result and its runtime probe."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, Iterable


def load_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return {}
    return value if isinstance(value, dict) else {}


def load_probe(path: Path) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    if not path.is_file():
        return samples
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            samples.append(value)
    return samples


def probe_int(sample: dict[str, Any], key: str) -> int | None:
    """Return an integer probe field, or None when it is absent or unusable."""
    value = sample.get(key)
    if value is None or isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value) if math.isfinite(value) and value.is_integer() else None
    if isinstance(value, str):
        try:
            return int(value, 10)
        except ValueError:
            return None
    return None


def probe_bool(sample: dict[str, Any], key: str) -> bool | None:
    """Return a boolean probe field, or None when it is absent or unusable."""
    value = sample.get(key)
    return value if isinstance(value, bool) else None


def probe_series(samples: Iterable[dict[str, Any]], key: str) -> list[int | None]:
    return [probe_int(sample, key) for sample in samples]


def probe_bool_series(
    samples: Iterable[dict[str, Any]], key: str
) -> list[bool | None]:
    return [probe_bool(sample, key) for sample in samples]


def first_observed(series: list[int | None]) -> int | None:
    return next((value for value in series if value is not None), None)


def last_observed(series: list[int | None]) -> int | None:
    return next((value for value in reversed(series) if value is not None), None)


def first_increase_index(series: list[int | None]) -> int | None:
    """Index of the first sample strictly above the previously observed value."""
    previous: int | None = None
    for index, value in enumerate(series):
        if value is None:
            continue
        if previous is not None and value > previous:
            return index
        previous = value
    return None


def distinct_observed(series: list[int | None]) -> list[int]:
    seen: list[int] = []
    for value in series:
        if value is not None and value not in seen:
            seen.append(value)
    return seen


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-dog", action="store_true")
    parser.add_argument("--expect-pickups", action="store_true")
    parser.add_argument("--expect-wallet-delta", type=int)
    parser.add_argument("--expect-rupee-kept", action="store_true")
    parser.add_argument("--expect-split", action="store_true")
    parser.add_argument("--expect-single", action="store_true")
    parser.add_argument("--min-splits", type=int)
    parser.add_argument("--min-merges", type=int)
    parser.add_argument("--expect-ui-single", action="store_true")
    args = parser.parse_args()

    result = load_json(args.result)
    output_dir = Path(result.get("output_dir", args.result.parent))
    artifacts = result.get("artifacts", {})
    probe = load_probe(Path(artifacts["probe"])) if artifacts.get("probe") else []
    runtime = load_json(Path(artifacts["runtime_json"])) if artifacts.get("runtime_json") else {}

    dog_samples = [sample for sample in probe if sample.get("dog_actor")]
    usable_samples = [sample for sample in dog_samples if sample.get("dog_usable")]
    spawn_transitions = 0
    previous = 0
    for sample in probe:
        current = int(sample.get("dog_actor", 0) or 0)
        if previous == 0 and current != 0:
            spawn_transitions += 1
        previous = current

    last = probe[-1] if probe else {}
    wallet_series = probe_series(probe, "wallet")
    capacity_series = probe_series(probe, "wallet_capacity")
    pickups_series = probe_series(probe, "rupees_collected")
    last_value_series = probe_series(probe, "last_rupee_value")
    nearby_series = probe_series(probe, "nearby_rupees")
    split_series = probe_bool_series(probe, "split_active")
    split_panel_series = probe_series(probe, "split_panels")
    ui_series = probe_series(probe, "ui_passes")
    split_count_series = probe_series(probe, "split_count")
    merge_count_series = probe_series(probe, "merge_count")
    link_framed_series = probe_bool_series(probe, "link_framed")
    dog_framed_series = probe_bool_series(probe, "dog_framed")

    wallet_first = first_observed(wallet_series)
    wallet_last = last_observed(wallet_series)
    wallet_delta = (
        wallet_last - wallet_first
        if wallet_first is not None and wallet_last is not None
        else None
    )
    pickups_first = first_observed(pickups_series)
    pickups_last = last_observed(pickups_series)
    pickups_total_delta = (
        pickups_last - pickups_first
        if pickups_first is not None and pickups_last is not None
        else None
    )
    first_pickup_sample = first_increase_index(pickups_series)
    capacity_violations = [
        index
        for index, (wallet, capacity) in enumerate(
            zip(wallet_series, capacity_series)
        )
        if wallet is not None and capacity is not None and wallet > capacity
    ]

    split_active_count = sum(1 for value in split_series if value is True)
    split_first_sample = next(
        (index for index, value in enumerate(split_series) if value is True), None
    )
    split_last_sample = next(
        (
            index
            for index in range(len(split_series) - 1, -1, -1)
            if split_series[index] is True
        ),
        None,
    )
    split_panel_violations = [
        index
        for index, active in enumerate(split_series)
        if active is True
        and index < len(split_panel_series)
        and split_panel_series[index] != 2
    ]
    ui_pass_violations = [
        index
        for index, value in enumerate(ui_series)
        if value is not None and value != 1
    ]
    split_count_max = max(
        (value for value in split_count_series if value is not None), default=0
    )
    merge_count_max = max(
        (value for value in merge_count_series if value is not None), default=0
    )
    split_panels_seen = distinct_observed(split_panel_series)
    last_split_reason = next(
        (
            probe[index].get("split_reason")
            for index in range(len(probe) - 1, -1, -1)
            if isinstance(probe[index].get("split_reason"), str)
        ),
        None,
    )

    failures: list[str] = []
    if capacity_violations:
        failures.append(
            f"wallet > wallet_capacity in {len(capacity_violations)} sample(s); "
            f"first at sample {capacity_violations[0]}"
        )
    if args.expect_pickups:
        if pickups_first is None:
            failures.append("rupees_collected is absent from every probe sample")
        elif first_pickup_sample is None:
            failures.append("rupees_collected never increases")
    if args.expect_wallet_delta is not None:
        if wallet_delta is None:
            failures.append("wallet balance is absent from the probe")
        elif wallet_delta != args.expect_wallet_delta:
            failures.append(
                f"wallet delta {wallet_delta} does not equal "
                f"--expect-wallet-delta {args.expect_wallet_delta}"
            )
    if split_panel_violations:
        failures.append(
            "split_active is true with split_panels != 2; first at sample "
            f"{split_panel_violations[0]}"
        )
    if args.expect_split and split_active_count == 0:
        failures.append("split_active is never true")
    if args.expect_single and split_active_count != 0:
        failures.append(
            f"split_active is true in {split_active_count} sample(s); first at "
            f"sample {split_first_sample}"
        )
    if args.min_splits is not None and split_count_max < args.min_splits:
        failures.append(
            f"split_count {split_count_max} is below --min-splits {args.min_splits}"
        )
    if args.min_merges is not None and merge_count_max < args.min_merges:
        failures.append(
            f"merge_count {merge_count_max} is below --min-merges {args.min_merges}"
        )
    if args.expect_ui_single:
        if not any(value is not None for value in ui_series):
            failures.append("ui_passes is absent from every probe sample")
        elif ui_pass_violations:
            failures.append(
                f"ui_passes != 1 in {len(ui_pass_violations)} sample(s); first at "
                f"sample {ui_pass_violations[0]}"
            )
    if args.expect_rupee_kept:
        if pickups_total_delta is None:
            failures.append("rupees_collected is absent from the probe")
        elif pickups_total_delta != 0:
            failures.append(
                "rupees_collected increased although the rupee should have been kept"
            )
        if wallet_delta is None:
            failures.append("wallet balance is absent from the probe")
        elif wallet_delta != 0:
            failures.append(
                "wallet changed although the rupee should have been kept"
            )

    summary = {
        "schema": "triaevum.game_test.analysis.v1",
        "result": str(args.result),
        "status": result.get("status", "UNKNOWN"),
        "exit_code": result.get("exit_code"),
        "timed_out": result.get("timed_out", False),
        "probe_samples": len(probe),
        "dog_seen": bool(dog_samples),
        "dog_usable": bool(usable_samples),
        "dog_persisted_at_end": bool(last.get("dog_actor")) and bool(last.get("dog_usable")),
        "dog_spawn_transitions": spawn_transitions,
        "last_dog_actor": last.get("dog_actor"),
        "last_dog_pos": last.get("dog_pos"),
        "last_dog_velocity": last.get("dog_velocity"),
        "last_animation": last.get("desired_animation"),
        "last_models_initialized": last.get("dog_models_initialized"),
        "last_model_context": last.get("dog_model_context"),
        "first_pickup_sample": first_pickup_sample,
        "first_pickup_host_frame": (
            probe[first_pickup_sample].get("host_frame")
            if first_pickup_sample is not None
            else None
        ),
        "wallet_first": wallet_first,
        "wallet_last": wallet_last,
        "wallet_delta": wallet_delta,
        "pickups_first": pickups_first,
        "pickups_last": pickups_last,
        "pickups_total_delta": pickups_total_delta,
        "wallet_capacity_seen": distinct_observed(capacity_series),
        "nearby_rupees_last": last_observed(nearby_series),
        "last_rupee_value": last_observed(last_value_series),
        "wallet_capacity_violations": len(capacity_violations),
        "split_active_count": split_active_count,
        "split_first_sample": split_first_sample,
        "split_first_host_frame": (
            probe[split_first_sample].get("host_frame")
            if split_first_sample is not None
            else None
        ),
        "split_last_sample": split_last_sample,
        "split_count": split_count_max,
        "merge_count": merge_count_max,
        "split_panels_seen": split_panels_seen,
        "last_split_reason": last_split_reason,
        "link_framed_any": any(value is True for value in link_framed_series),
        "dog_framed_any": any(value is True for value in dog_framed_series),
        "ui_pass_violations": len(ui_pass_violations),
        "failures": failures,
        "runtime": {
            key: runtime.get(key)
            for key in (
                "frames",
                "run_frames",
                "process",
                "memory_faults",
                "whole_aot_memory_faults",
            )
            if key in runtime
        },
        "screenshot": artifacts.get("screenshot"),
        "expected": result.get("expected", []),
    }
    target = args.output or output_dir / "analysis.json"
    target.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    for failure in failures:
        print(f"analysis: {failure}", file=sys.stderr)
    if result.get("status") != "COMPLETED":
        return 1
    if args.require_dog and not (
        summary["dog_seen"]
        and summary["dog_usable"]
        and summary["dog_persisted_at_end"]
    ):
        return 1
    if failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
