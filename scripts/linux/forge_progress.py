#!/usr/bin/env python3
"""Render the frozen Forge worker's JSON-line protocol as concise progress."""

from __future__ import annotations

import json
import sys


def main() -> int:
    for line in sys.stdin:
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            print(line, end="", flush=True)
            continue
        if not isinstance(event, dict):
            print(line, end="", flush=True)
            continue
        kind = event.get("forge_worker_event")
        if kind == "stage":
            payload = event.get("payload")
            if isinstance(payload, list) and len(payload) >= 2:
                print(f"[{payload[0]}] {payload[1]}", flush=True)
            else:
                print(line, end="", flush=True)
        elif kind == "complete":
            print("Installation complete.", flush=True)
        elif kind == "error":
            print(f"ERROR: {event.get('payload')}", flush=True)
            return 1
        else:
            print(line, end="", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
