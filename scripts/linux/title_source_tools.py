#!/usr/bin/env python3
"""Extract, index and locally refresh TriAevum's translated title source.

The source archive is shipped with the official Linux release.  It contains
generated whole-AOT C++, not a hand-written decompilation; the index makes the
12k+ translated functions searchable by name and guest address.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import zipfile
from pathlib import Path

MANIFEST = "TITLE_SOURCE_MANIFEST.json"
ARCHIVE = Path("source/titles/oot3d-linux-translated.zip")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_manifest(root: Path) -> dict:
    manifest = json.loads((root / MANIFEST).read_text(encoding="utf-8"))
    if manifest.get("format") != "triaevum_translated_title_source_v1":
        raise SystemExit(f"unsupported translated source manifest: {root / MANIFEST}")
    return manifest


def verify_source(root: Path) -> dict:
    manifest = load_manifest(root)
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise SystemExit("translated source manifest has no file inventory")
    for name, expected in files.items():
        path = root / name
        if not path.is_file():
            raise SystemExit(f"translated source is incomplete: {path}")
        actual = sha256_file(path)
        if actual != str(expected).lower():
            raise SystemExit(f"translated source hash mismatch: {name}")
    return manifest


def extract(release: Path, output: Path) -> Path:
    archive = release / ARCHIVE
    if not archive.is_file():
        raise SystemExit(f"release does not contain the translated source archive: {archive}")
    output = output.expanduser().resolve()
    if (output / MANIFEST).is_file():
        verify_source(output)
        print(output)
        return output
    if output.exists():
        raise SystemExit(f"output exists but is not a translated source tree: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    staging = output.with_name(f".{output.name}.extracting-{os.getpid()}")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir()
    try:
        with zipfile.ZipFile(archive) as source:
            source.extractall(staging)
        verify_source(staging)
        staging.replace(output)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    print(output)
    return output


def index(source: Path) -> Path:
    import re

    source = source.expanduser().resolve()
    verify_source(source)
    pattern = re.compile(
        r"^Oot3dWholeAotFlow\s+(Execute_[A-Za-z0-9_]+)_([0-9A-Fa-f]{8})\s*\("
    )
    rows: dict[tuple[int, str], tuple[int, str, str, int]] = {}
    for path in sorted(source.glob("*.cpp")):
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for index, line in enumerate(lines):
            match = pattern.match(line)
            if not match:
                continue
            # Declarations end in ");"; definitions have a body on the following
            # lines.  A definition's signature always contains ") {".
            if not any(") {" in lines[n] for n in range(index + 1, min(index + 5, len(lines)))):
                continue
            address = int(match.group(2), 16)
            name = match.group(1)
            rows[(address, name)] = (address, name, path.name, index + 1)
    output = source / "function-index.tsv"
    with output.open("w", encoding="utf-8") as stream:
        stream.write("address\tname\tfile\tline\n")
        for address, name, file, line in sorted(rows.values()):
            stream.write(f"0x{address:08X}\t{name}\t{file}\t{line}\n")
    print(f"{len(rows)} translated functions indexed in {output}")
    return output


def refresh_manifest(source: Path) -> None:
    source = source.expanduser().resolve()
    manifest = load_manifest(source)
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise SystemExit("translated source manifest has no file inventory")
    for name in files:
        path = source / name
        if not path.is_file():
            raise SystemExit(f"translated source is incomplete: {path}")
        files[name] = sha256_file(path)
    manifest["local_modifications"] = True
    (source / MANIFEST).write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"updated {source / MANIFEST}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    extract_parser = sub.add_parser("extract", help="extract the release's translated source")
    extract_parser.add_argument("--release", type=Path, required=True)
    extract_parser.add_argument("--output", type=Path, required=True)

    index_parser = sub.add_parser("index", help="write function-index.tsv for a source tree")
    index_parser.add_argument("--source", type=Path, required=True)

    refresh_parser = sub.add_parser(
        "refresh-manifest", help="update file hashes after local source edits"
    )
    refresh_parser.add_argument("--source", type=Path, required=True)

    args = parser.parse_args(argv)
    if args.command == "extract":
        extract(args.release, args.output)
    elif args.command == "index":
        index(args.source)
    else:
        refresh_manifest(args.source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
