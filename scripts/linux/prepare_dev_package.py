#!/usr/bin/env python3
"""Stage an official Linux release around a source-built TriAevum runtime.

The public source checkout contains the runtime, renderer and host services but
not the translated title logic.  This helper copies the official release
package, replaces its runtime and native game module with the source build,
optionally installs a locally rebuilt title, and rebinds the release catalog to
the new binaries.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from tools.triaevum_release.common import atomic_write_json, sha256_file  # noqa: E402
from tools.triaevum_release.linux_precompiled_catalog import create_catalog  # noqa: E402
from tools.triaevum_release.release_platform import LINUX  # noqa: E402


def load_catalog(root: Path) -> dict:
    path = root / "recipes/precompiled-titles.json"
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("format") != "triaevum_precompiled_titles_v1":
        raise SystemExit(f"unsupported precompiled title catalog: {path}")
    titles = payload.get("titles")
    if not isinstance(titles, list) or not titles:
        raise SystemExit(f"precompiled title catalog is empty: {path}")
    return payload


def title_plugin_path(reference: Path) -> Path:
    catalog = load_catalog(reference)
    path = catalog["titles"][0].get("plugin", {}).get("path")
    if not isinstance(path, str) or not path.endswith(LINUX.title_module):
        raise SystemExit("release catalog does not name a Linux title plugin")
    return Path(path)


def stage(reference: Path, package: Path, *, refresh: bool) -> None:
    if package.exists() and refresh:
        shutil.rmtree(package)
    if not package.exists():
        shutil.copytree(reference, package, symlinks=True)
    for name in ("TriAevum", "TriAevumForge"):
        if not (package / name).is_file():
            raise SystemExit(f"staged package is missing {name}: {package}")
    (package / "forge").mkdir(exist_ok=True)


def replace_runtime(package: Path, build: Path) -> None:
    for source, destination in (
        (build / LINUX.runtime, package / LINUX.runtime),
        (build / Path(LINUX.native_module).name, package / LINUX.native_module),
        (build / LINUX.title_module, package / LINUX.title_module),
    ):
        if not source.is_file():
            raise SystemExit(f"source build is missing {source}")
        shutil.copy2(source, destination)


def install_title(package: Path, plugin: Path, relative: Path) -> None:
    destination = package / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(plugin, destination)


def bind_catalog(reference: Path, package: Path, plugin: Path, source_commit: str) -> dict:
    catalog = load_catalog(reference)
    title = catalog["titles"][0]
    shader_compiler = None
    shader_dependencies: list[Path] = []
    preparation = title.get("renderer_shader_preparation")
    if isinstance(preparation, dict):
        shader_compiler = package / preparation["compiler"]["path"]
        shader_dependencies = [
            package / record["path"] for record in preparation.get("dependencies", [])
        ]
    return create_catalog(
        reference,
        package,
        plugin,
        source_commit=source_commit,
        shader_compiler=shader_compiler,
        shader_dependencies=shader_dependencies,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True,
                        help="extracted official Linux release directory")
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--source-commit", required=True,
                        help="full 40-character source commit")
    parser.add_argument("--title-plugin", type=Path,
                        help="locally rebuilt title plugin; omit to use the release title")
    parser.add_argument("--refresh-release", action="store_true",
                        help="replace the staged package with a fresh release copy")
    args = parser.parse_args(argv)

    reference = args.release.expanduser().resolve()
    build = args.build_dir.expanduser().resolve()
    package = args.package.expanduser().resolve()
    relative = title_plugin_path(reference)
    stage(reference, package, refresh=args.refresh_release)
    replace_runtime(package, build)
    if args.title_plugin is not None:
        install_title(package, args.title_plugin.expanduser().resolve(), relative)
    else:
        install_title(package, reference / relative, relative)
    plugin = package / relative
    if not plugin.is_file():
        raise SystemExit(f"staged title plugin is missing: {plugin}")
    catalog = bind_catalog(reference, package, plugin, args.source_commit)
    receipt = {
        "format": "triaevum_linux_dev_package_v1",
        "release": str(reference),
        "package": str(package),
        "source_commit": args.source_commit,
        "runtime": str(package / LINUX.runtime),
        "runtime_sha256": sha256_file(package / LINUX.runtime),
        "native_module": str(package / LINUX.native_module),
        "native_module_sha256": sha256_file(package / LINUX.native_module),
        "title_plugin": str(plugin),
        "title_plugin_sha256": sha256_file(plugin),
        "title_plugin_locally_built": args.title_plugin is not None,
        "catalog_title_sha256": catalog["titles"][0]["plugin"]["sha256"],
    }
    atomic_write_json(package / "dev-package.json", receipt)
    print(json.dumps(receipt, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
