#!/usr/bin/env python3
"""Summarize a TriAevum gameplay test result and its runtime probe."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-dog", action="store_true")
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
    if result.get("status") != "COMPLETED":
        return 1
    if args.require_dog and not (
        summary["dog_seen"]
        and summary["dog_usable"]
        and summary["dog_persisted_at_end"]
    ):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
