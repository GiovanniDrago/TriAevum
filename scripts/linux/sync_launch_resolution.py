#!/usr/bin/env python3
"""Keep the development launch profile's window size in sync with F1 settings.

The release installer writes a fixed 1280x720 launch profile.  F1 persists the
requested output resolution in ``config/TriAevum.json``; this helper copies that
value into the profile so a resolution change survives the next launch.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--launch-profile", type=Path, required=True)
    args = parser.parse_args(argv)

    config_path = args.config.expanduser().resolve()
    profile_path = args.launch_profile.expanduser().resolve()
    if not config_path.is_file() or not profile_path.is_file():
        print("resolution sync skipped: config or launch profile is missing")
        return 0
    try:
        config = json.loads(config_path.read_text(encoding="utf-8"))
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
        output = config["Graphics"]["Output"]
        width = int(output["Width"])
        height = int(output["Height"])
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
        print(f"resolution sync skipped: {exc}")
        return 0
    if width < 320 or height < 240:
        print("resolution sync skipped: invalid configured output size")
        return 0
    arguments = profile.get("arguments")
    if not isinstance(arguments, list):
        print("resolution sync skipped: launch profile has no arguments")
        return 0

    def replace(option: str, value: str) -> None:
        if option in arguments:
            index = arguments.index(option)
            if index + 1 < len(arguments):
                arguments[index + 1] = value
            else:
                arguments.append(value)
        else:
            arguments.extend((option, value))

    replace("--width", str(width))
    replace("--height", str(height))
    temporary = profile_path.with_name(profile_path.name + ".partial")
    try:
        temporary.write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
        temporary.replace(profile_path)
    finally:
        temporary.unlink(missing_ok=True)
    print(f"launch resolution: {width}x{height}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
