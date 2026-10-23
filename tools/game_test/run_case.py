#!/usr/bin/env python3
"""Run one deterministic TriAevum gameplay verification scenario.

The harness only orchestrates the existing runtime CLI. It never edits game
files, never commits, and keeps ROMs, saves, screenshots and logs outside the
repository by default.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Any


ENV_PATTERN = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def expand_env(value: Any) -> Any:
    if isinstance(value, str):
        return ENV_PATTERN.sub(lambda match: os.environ.get(match.group(1), match.group(0)), value)
    if isinstance(value, list):
        return [expand_env(item) for item in value]
    if isinstance(value, dict):
        return {key: expand_env(item) for key, item in value.items()}
    return value


def resolve_path(value: str | None, root: Path) -> Path | None:
    if not value or "${" in value:
        return None
    path = Path(value).expanduser()
    if not path.is_absolute():
        path = root / path
    return path.resolve()


def load_scenario(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        scenario = json.load(stream)
    if not isinstance(scenario, dict):
        raise SystemExit("scenario must be a JSON object")
    return expand_env(scenario)


def copy_save_data(source: Path | None, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if source is None:
        return
    if not source.is_dir():
        raise SystemExit(f"save-data seed is not a directory: {source}")
    shutil.copytree(source, destination, dirs_exist_ok=True)


def convert_bmp(path: Path) -> Path | None:
    if not path.is_file() or path.suffix.lower() != ".bmp":
        return None
    try:
        from PIL import Image
    except Exception:
        return None
    target = path.with_suffix(".png")
    try:
        with Image.open(path) as image:
            image.save(target, "PNG")
    except Exception:
        return None
    return target


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--package")
    parser.add_argument("--profile")
    parser.add_argument("--state")
    parser.add_argument("--save-data")
    parser.add_argument("--allow-headless", action="store_true")
    args = parser.parse_args()

    root = repo_root()
    scenario_path = args.scenario.resolve()
    scenario = load_scenario(scenario_path)
    for key in ("package", "profile", "state", "save_data"):
        override = getattr(args, key)
        if override is not None:
            scenario[key] = override

    name = str(scenario.get("name") or scenario_path.stem)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = (args.output_dir or root / "artifacts" / "game-tests" / f"{stamp}-{name}").resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    package = resolve_path(scenario.get("package"), root)
    profile = resolve_path(scenario.get("profile"), root)
    state = resolve_path(scenario.get("state"), root)
    save_seed = resolve_path(scenario.get("save_data"), root)
    p1_timeline = resolve_path(scenario.get("p1_timeline"), root)
    p2_input = resolve_path(scenario.get("p2_input"), root)
    binary = package / "TriAevum" if package else None

    if not package or not binary or not binary.is_file():
        raise SystemExit("scenario package must contain TriAevum; set TRIAEVUM_TEST_PACKAGE")
    if not profile or not profile.is_file():
        raise SystemExit("scenario profile must be a launch profile; set TRIAEVUM_TEST_PROFILE")
    if state and not state.is_file():
        raise SystemExit(f"load-state file not found: {state}")
    if p1_timeline and not p1_timeline.is_file():
        raise SystemExit(f"P1 input timeline not found: {p1_timeline}")
    if p2_input and not p2_input.is_file():
        raise SystemExit(f"P2 input file not found: {p2_input}")
    if not args.allow_headless and not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        raise SystemExit("no DISPLAY/WAYLAND_DISPLAY; pass --allow-headless only if the runtime supports it")

    frames = int(scenario.get("frames", 600))
    max_seconds = float(scenario.get("max_seconds", 90))
    width = int(scenario.get("width", 1280))
    height = int(scenario.get("height", 720))
    screenshot = output_dir / "final.bmp"
    runtime_json = output_dir / "runtime.json"
    probe_path = output_dir / "coop_probe.jsonl"
    run_log = output_dir / "run.log"
    save_data_dir = output_dir / "savedata"
    copy_save_data(save_seed, save_data_dir)

    command: list[str] = [
        str(binary),
        "--launch-profile",
        str(profile),
        "--save-data",
        str(save_data_dir),
        "--output",
        str(runtime_json),
        "--screenshot",
        str(screenshot),
        "--frames",
        str(frames),
        "--max-seconds",
        str(max_seconds),
        "--width",
        str(width),
        "--height",
        str(height),
    ]
    if state:
        command.extend(["--load-state", str(state)])
    if p1_timeline:
        command.extend(["--input-timeline", str(p1_timeline)])

    environment = os.environ.copy()
    environment["TRIAEVUM_COOP_PROBE"] = str(probe_path)
    environment["TRIAEVUM_COOP_PROBE_INTERVAL"] = str(
        int(scenario.get("probe_interval", 15))
    )
    if p2_input:
        environment["TRIAEVUM_COOP_TEST_INPUT"] = str(p2_input)
    if scenario.get("coop_warp"):
        environment["TRIAEVUM_COOP_WARP"] = str(scenario["coop_warp"])

    started = time.monotonic()
    timed_out = False
    returncode: int | None = None
    with run_log.open("w", encoding="utf-8") as log:
        log.write("COMMAND: " + json.dumps(command) + "\n")
        log.flush()
        process = subprocess.Popen(
            command,
            cwd=str(package),
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=max_seconds + 30.0)
        except subprocess.TimeoutExpired:
            timed_out = True
            try:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5.0)
            except Exception:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except Exception:
                    pass
            returncode = process.returncode

    duration = time.monotonic() - started
    png = convert_bmp(screenshot)
    probe_lines: list[dict[str, Any]] = []
    if probe_path.is_file():
        for line in probe_path.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                probe_lines.append(value)

    result = {
        "schema": "triaevum.game_test.result.v1",
        "name": name,
        "scenario": str(scenario_path),
        "command": command,
        "output_dir": str(output_dir),
        "started_at": dt.datetime.now().isoformat(),
        "duration_seconds": round(duration, 3),
        "timed_out": timed_out,
        "exit_code": returncode,
        "status": "TIMEOUT" if timed_out else ("COMPLETED" if returncode == 0 else "FAILED"),
        "artifacts": {
            "log": str(run_log),
            "runtime_json": str(runtime_json) if runtime_json.is_file() else None,
            "screenshot": str(png or screenshot) if (png or screenshot).is_file() else None,
            "probe": str(probe_path) if probe_path.is_file() else None,
        },
        "probe_samples": len(probe_lines),
        "expected": scenario.get("expected", []),
    }
    (output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "COMPLETED" else 1


if __name__ == "__main__":
    sys.exit(main())
