#!/usr/bin/env python3
"""Generate a high-density QBF coverage pack for the native OoT3D message font.

The runtime already knows how to replace the native 16x16 A4 font atlas with a
uniform-density coverage atlas (NativeHdFontRuntime).  This helper builds that
coverage from a normal desktop TTF while preserving every native character code,
metric and glyph alias.  The result is a private user-generated
``font_coverage.zip`` beside the TopScreen texture pack.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
import sys
import zipfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import (  # noqa: E402
    read_level3_romfs_file,
)
from tools.oot3d.decomp_support.scripts.qbf_font import (  # noqa: E402
    QbfFont,
    parse_qbf,
    pair_font_coverage,
)

MANIFEST = "manifest.json"
DEFAULT_FONTS = (
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/cantarell/Cantarell-VF.otf",
    "/usr/share/fonts/Adwaita/AdwaitaSans-Regular.ttf",
)
GENERATOR_VERSION = 2
NATIVE_FONTS = (
    ("message/eu/ltn16.qbf", "EUR"),
    ("message/us/ltn16.qbf", "USA"),
)
PREVIEW_TEXT = "The quick brown fox jumps. 0123456789 Éàçñ Œœ — „“ €"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_romfs(data_root: Path) -> Path:
    active_path = data_root / "active-title.json"
    if not active_path.is_file():
        raise SystemExit(f"no active title file: {active_path}")
    active = json.loads(active_path.read_text(encoding="utf-8"))
    directory = Path(active["directory"])
    if not directory.is_absolute():
        directory = (data_root / directory).resolve()
    index = json.loads((directory / "content.tap").read_text(encoding="utf-8"))
    descriptor = index["inputs"]["romfs"]
    path = Path(descriptor["path"])
    if not path.is_absolute():
        path = (directory / path).resolve()
    if not path.is_file():
        raise SystemExit(f"active RomFS is missing: {path}")
    return path


def resolve_pack_directory(launch_profile: Path) -> Path:
    profile = json.loads(launch_profile.read_text(encoding="utf-8"))
    arguments = profile.get("arguments", [])
    try:
        index = arguments.index("--topscreen-texture-overrides")
    except ValueError as exc:
        raise SystemExit("launch profile has no TopScreen texture pack") from exc
    value = str(arguments[index + 1]).replace("${runtime_dir}", str(launch_profile.parent))
    pack = Path(value)
    if not pack.is_file():
        raise SystemExit(f"TopScreen texture pack is missing: {pack}")
    return pack.parent


def auto_font() -> Path:
    for candidate in DEFAULT_FONTS:
        path = Path(candidate)
        if path.is_file():
            return path
    raise SystemExit("no usable TrueType font found; pass --font PATH")


def glyph_bitmap(native: QbfFont, index: int) -> tuple[bytes, tuple[int, int, int, int] | None]:
    data = native.glyph_coverage(index)
    xs: list[int] = []
    ys: list[int] = []
    for y in range(native.height):
        for x in range(native.width):
            if data[y * native.width + x]:
                xs.append(x)
                ys.append(y)
    if not xs:
        return data, None
    return data, (min(xs), min(ys), max(xs), max(ys))


def pack_a4(alpha: Image.Image) -> bytes:
    pixels = alpha.tobytes()
    output = bytearray(len(pixels) // 2)
    for index in range(0, len(pixels), 2):
        high = min(15, int(round(pixels[index] / 17.0)))
        low = min(15, int(round(pixels[index + 1] / 17.0)))
        output[index // 2] = (high << 4) | low
    return bytes(output)


def render_glyph(
    font: ImageFont.FreeTypeFont,
    character: str,
    size: int,
    baseline: int,
    width: int,
    height: int,
    native_bbox: tuple[int, int, int, int] | None,
    density: int,
    notdef: bytes,
) -> bytes:
    if character in (" ", "\xa0", "\xad") or native_bbox is None:
        return bytes(width * height // 2)
    mask = font.getmask(character, mode="L")
    if bytes(mask) == notdef:
        return bytes(width * height // 2)
    bounds = font.getbbox(character, anchor="ls")
    if bounds is None:
        return bytes(width * height // 2)
    left, _, right, _ = bounds
    glyph_width = max(1, right - left)
    native_center = (native_bbox[0] + native_bbox[2] + 1) * density / 2.0
    x = int(round(native_center - glyph_width / 2.0 - left))
    image = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(image)
    draw.text((x, baseline), character, font=font, fill=255, anchor="ls")
    return pack_a4(image)


def build_coverage(
    native_data: bytes,
    native: QbfFont,
    font_path: Path,
    density: int,
    size: int,
    baseline: int,
) -> tuple[bytes, dict]:
    width = native.width * density
    height = native.height * density
    font = ImageFont.truetype(str(font_path), size)
    notdef = bytes(font.getmask("\ue000", mode="L"))
    glyph_chars: dict[int, str] = {}
    for entry in native.characters:
        glyph_chars.setdefault(entry.glyph_index, chr(entry.code))
    bitmaps: list[bytes] = []
    for index in range(native.glyph_count):
        _, bbox = glyph_bitmap(native, index)
        character = glyph_chars.get(index, "")
        bitmaps.append(
            render_glyph(font, character, size, baseline, width, height, bbox, density, notdef)
        )
    directory = bytearray()
    for entry in native.characters:
        directory.extend(struct.pack("<HH", entry.code, entry.glyph_index))
        directory.extend(entry.metrics)
    header = (
        b"QBF1"
        + struct.pack("<HHHH", len(native.characters), native.glyph_count,
                      native.fallback_code, 0)
        + struct.pack("4B", 4, width, height, 2)
    )
    coverage = header + bytes(directory) + b"".join(bitmaps)
    replacement = parse_qbf(coverage)
    contract = pair_font_coverage(native, replacement)
    return coverage, contract


def draw_preview(native: QbfFont, coverage: QbfFont, density: int, output: Path) -> None:
    text = PREVIEW_TEXT
    cell_w, cell_h = native.width, native.height
    hd_w, hd_h = coverage.width, coverage.height
    native_by_code = {entry.code: entry for entry in native.characters}
    coverage_by_code = {entry.code: entry for entry in coverage.characters}
    native_image = Image.new("L", (cell_w * len(text), cell_h), 0)
    hd_image = Image.new("L", (hd_w * len(text), hd_h), 0)
    for index, character in enumerate(text):
        entry = native_by_code.get(ord(character))
        if entry is not None:
            data = native.glyph_coverage(entry.glyph_index)
            tile = Image.frombytes("L", (cell_w, cell_h), data)
            native_image.paste(tile, (index * cell_w, 0))
        entry = coverage_by_code.get(ord(character))
        if entry is not None:
            data = coverage.glyph_coverage(entry.glyph_index)
            tile = Image.frombytes("L", (hd_w, hd_h), data)
            hd_image.paste(tile, (index * hd_w, 0))
    native_scaled = native_image.resize(
        (native_image.width * density, native_image.height * density), Image.Resampling.NEAREST
    )
    width = max(native_scaled.width, hd_image.width)
    preview = Image.new("L", (width, native_scaled.height + hd_image.height + 8), 0)
    preview.paste(native_scaled, (0, 0))
    preview.paste(hd_image, (0, native_scaled.height + 8))
    preview.save(output)
    print(f"preview: {output}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--launch-profile", type=Path, required=True)
    parser.add_argument("--romfs", type=Path)
    parser.add_argument("--output", type=Path, help="pack directory or font_coverage.zip")
    parser.add_argument("--font", type=Path, help="TTF/OTF to render; default: system sans")
    parser.add_argument("--density", type=int, default=4, help="integer coverage density (default: 4)")
    parser.add_argument("--size", type=int, help="pixel size (default: density * 14)")
    parser.add_argument("--baseline", type=int, help="baseline in HD pixels (default: density * 13)")
    parser.add_argument("--preview", type=Path, help="write a native-vs-HD preview PNG")
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing non-built-in font_coverage.zip")
    parser.add_argument("--remove", action="store_true",
                        help="remove a previously generated built-in pack")
    args = parser.parse_args(argv)

    if args.output:
        output = args.output.expanduser().resolve()
    else:
        output = resolve_pack_directory(args.launch_profile.expanduser().resolve())
    pack = output if output.suffix.lower() == ".zip" else output / "font_coverage.zip"
    receipt_path = pack.with_name("builtin-font.json")
    if args.remove:
        if pack.is_file() and receipt_path.is_file():
            pack.unlink()
            receipt_path.unlink()
            print(f"removed built-in HD font pack: {pack}")
        else:
            print("no built-in HD font pack to remove")
        return 0

    if args.density < 1 or args.density > 8:
        raise SystemExit("density must be between 1 and 8")
    size = args.size or args.density * 14
    baseline = args.baseline or args.density * 13
    data_root = args.data_root.expanduser().resolve()
    romfs = args.romfs.expanduser().resolve() if args.romfs else resolve_romfs(data_root)

    force = args.force or args.font is not None
    current_hash = sha256_file(pack) if pack.is_file() else None
    archive_receipt = pack.with_name("fonts_import.json")
    if pack.is_file() and not force and archive_receipt.is_file():
        try:
            archive = json.loads(archive_receipt.read_text(encoding="utf-8"))
            if archive.get("pack_sha256") == current_hash:
                print(f"keeping existing SingleScreen HD font pack: {pack}")
                return 0
        except (OSError, ValueError, json.JSONDecodeError):
            pass
    existing_font: Path | None = None
    if receipt_path.is_file():
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            value = receipt.get("font_file")
            if isinstance(value, str) and value:
                existing_font = Path(value)
        except (OSError, ValueError, json.JSONDecodeError):
            existing_font = None
    if pack.is_file() and not force and not args.preview:
        if not receipt_path.is_file():
            print(f"keeping existing user font pack: {pack}")
            return 0
        try:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            if (
                receipt.get("generator_version") == GENERATOR_VERSION
                and receipt.get("pack_sha256") == current_hash
                and existing_font is not None
                and existing_font.is_file()
                and receipt.get("density") == args.density
                and receipt.get("pixel_size") == size
                and receipt.get("baseline") == baseline
            ):
                print(f"keeping existing built-in HD font pack: {pack}")
                return 0
        except (OSError, ValueError, json.JSONDecodeError):
            pass

    font_path = args.font
    if font_path is None:
        font_path = existing_font if existing_font is not None and existing_font.is_file() else auto_font()
    font_path = font_path.expanduser().resolve()
    if not font_path.is_file():
        raise SystemExit(f"font not found: {font_path}")

    assets: dict[str, bytes] = {}
    fonts = []
    first_native = first_coverage = None
    for romfs_path, region in NATIVE_FONTS:
        try:
            native_data = read_level3_romfs_file(romfs, romfs_path)
        except FileNotFoundError:
            continue
        native = parse_qbf(native_data)
        coverage_data, contract = build_coverage(
            native_data, native, font_path, args.density, size, baseline
        )
        coverage = parse_qbf(coverage_data)
        native_name = f"native/{sha256_bytes(native_data)}.qbf"
        coverage_name = f"coverage/{sha256_bytes(coverage_data)}.qbf"
        assets[native_name] = native_data
        assets[coverage_name] = coverage_data
        fonts.append(
            {
                "romfs_path": romfs_path,
                "region": region,
                "coverage_file": coverage_name,
                "native_file": native_name,
                **contract,
            }
        )
        if first_native is None:
            first_native, first_coverage = native, coverage
    if not fonts:
        raise SystemExit("the prepared RomFS contains no supported ltn16.qbf font")

    manifest = {
        "format": "oot3d_qbf_coverage_v1",
        "runtime_enabled": True,
        "minimum_output_height": 480,
        "status": "native_atlas_coverage_ready",
        "source": "builtin_ttf_coverage",
        "generator_version": GENERATOR_VERSION,
        "font_file": str(font_path),
        "font_sha256": sha256_file(font_path),
        "density": args.density,
        "pixel_size": size,
        "baseline": baseline,
        "fonts": fonts,
        "attribution": f"Locally generated from {font_path.name}",
    }
    pack.parent.mkdir(parents=True, exist_ok=True)
    temporary = pack.with_name(pack.name + ".partial")
    try:
        with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as target:
            for name, payload in sorted(assets.items()):
                target.writestr(zipfile.ZipInfo(name), payload)
            target.writestr(
                zipfile.ZipInfo(MANIFEST),
                json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8"),
            )
        temporary.replace(pack)
    finally:
        temporary.unlink(missing_ok=True)
    receipt = pack.with_name("builtin-font.json")
    receipt_manifest = dict(manifest, pack_sha256=sha256_file(pack))
    receipt.write_text(
        json.dumps(receipt_manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if args.preview and first_native is not None and first_coverage is not None:
        draw_preview(first_native, first_coverage, args.density, args.preview)
    print(f"wrote {pack} ({pack.stat().st_size} bytes, density {args.density}, {len(fonts)} font(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
