"""Install pinned TopScreen textures and font coverage, never executable patches."""

from __future__ import annotations

import hashlib
import http.client
import struct
import zipfile
from pathlib import Path
from typing import Callable

from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import (
    SINGLESCREEN_ROMFS_PREFIXES, build_texture_pack,
)
from tools.oot3d.decomp_support.scripts.prepare_qbf_font_pack import prepare_font_pack

try:
    from .common import atomic_write_bytes, atomic_write_json, load_json_object, sha256_file
    from .verified_download import download_verified
    from .release_platform import host_platform
except ImportError:
    from common import atomic_write_bytes, atomic_write_json, load_json_object, sha256_file
    from verified_download import download_verified
    from release_platform import host_platform


# 2: SingleScreen 3.0 UI atlas, native localized labels (2.1.1 ones retired).
# 3: SingleScreen 3.0 HD UI atlas.
IMPORT_VERSION = 3
FONT_IMPORT_VERSION = 2
ARCHIVE_NAME = "topscreen211.zip"
LINK_ANIMATIONS = "zelda_link_ext.zar"
# The author's HD custom_ui01, shipped in the archive as an emulator texture
# replacement. It also draws elements the native atlas lacks (SELECT/START,
# ZL/ZR labels). TriAevum uses it as the 3.0 atlas, keyed by its native
# content, so it needs no separate texture pack.
UI_ATLAS_HD = "ui_atlas_hd.png"
UI_ATLAS_HD_MEMBER = ("SingleScreen_3.0/EUR/Emulator/load/textures/0004000000033600/"
                      "tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png")


def acquire_archive(root: Path, data_root: Path, contract: dict,
                    report: Callable[[str, str], None]) -> Path:
    cache = data_root / "downloads" / ARCHIVE_NAME
    for candidate in (root / ARCHIVE_NAME, root / "mods" / ARCHIVE_NAME, cache):
        if candidate.is_file():
            if (candidate.stat().st_size != contract["bytes"] or
                    sha256_file(candidate) != contract["sha256"]):
                raise ValueError(f"TopScreen 2.1.1 archive failed verification: {candidate}")
            return candidate

    report("topscreen", "Downloading the official TopScreen 2.1.1 texture source...")
    try:
        return download_verified(contract["url"], cache,
            size=contract["bytes"], sha256=contract["sha256"],
            progress=lambda count, size: report("topscreen", f"Downloading TopScreen textures: {count * 100 // size}%"),
            retry=lambda attempt, error: report("topscreen", f"Retrying TopScreen download ({attempt}/3): {error}"))
    except (OSError, EOFError, http.client.HTTPException, ValueError) as exc:
        raise ValueError(
            f"Could not prepare TopScreen textures: {exc}. Retry with internet access, "
            f"or place the official {ARCHIVE_NAME} beside {host_platform().forge}.") from exc


def find_singlescreen_archive(root: Path, data_root: Path, contract: dict) -> Path | None:
    """The user's SingleScreen 3.0 archive, verified; it is never downloaded."""
    name = contract["name"]
    for candidate in (root / name, root / "mods" / name, data_root / "downloads" / name):
        if candidate.is_file():
            if (candidate.stat().st_size != contract["bytes"] or
                    sha256_file(candidate) != contract["sha256"]):
                raise ValueError(f"SingleScreen 3.0 archive failed verification: {candidate}")
            return candidate
    return None


def extract_link_animations(archive: Path) -> bytes:
    """Link's Roll Dash animations, data only."""
    with zipfile.ZipFile(archive) as source:
        names = set(source.namelist())
        for prefix in SINGLESCREEN_ROMFS_PREFIXES:
            member = f"{prefix}actor/{LINK_ANIMATIONS}"
            if member in names:
                data = source.read(member)
                if data[:4] != b"ZAR\x01":
                    raise ValueError("SingleScreen 3.0 Link animations are not a ZAR archive")
                return data
    raise ValueError("SingleScreen 3.0 archive has no EUR Link animations")


def extract_ui_atlas_hd(archive: Path) -> bytes:
    """The HD custom_ui01: a PNG with the native atlas's 2:1 aspect, data only."""
    with zipfile.ZipFile(archive) as source:
        if UI_ATLAS_HD_MEMBER not in source.namelist():
            raise ValueError("SingleScreen 3.0 archive has no HD UI atlas")
        data = source.read(UI_ATLAS_HD_MEMBER)
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError("SingleScreen 3.0 HD UI atlas is not a PNG")
    width, height = struct.unpack(">II", data[16:24])
    if width < 512 or width % 512 != 0 or width != height * 2:
        raise ValueError(f"SingleScreen 3.0 HD UI atlas has unexpected dimensions {width}x{height}")
    return data


def topscreen_ui_atlas_hd(pack: Path | None) -> Path | None:
    """The imported HD UI atlas beside a prepared texture pack."""
    if pack is None:
        return None
    candidate = pack.parent / UI_ATLAS_HD
    return candidate if candidate.is_file() else None


def topscreen_link_animations(pack: Path | None) -> Path | None:
    """The imported zelda_link_ext.zar beside a prepared texture pack."""
    if pack is None:
        return None
    candidate = pack.parent / LINK_ANIMATIONS
    return candidate if candidate.is_file() else None


def prepare_topscreen_assets(*, root: Path, data_root: Path, recipe: dict,
                            romfs: Path,
                            report: Callable[[str, str], None] = lambda *_: None) -> Path | None:
    optional = recipe.get("optional_inputs", {})
    contract = optional.get("topscreen_2_1_1_archive")
    if contract is None:
        return None  # Other title/revision recipes do not inherit OOT3D UI assets.
    singlescreen_contract = optional.get("singlescreen_3_0_archive")
    singlescreen = (find_singlescreen_archive(root, data_root, singlescreen_contract)
                    if singlescreen_contract is not None else None)
    if singlescreen_contract is not None and singlescreen is None:
        report("topscreen", f"SingleScreen 3.0 extras skipped: place {singlescreen_contract['name']} "
                            f"beside {host_platform().forge} to install them.")
    identity = {"import_version": IMPORT_VERSION, "archive_sha256": contract["sha256"],
                "romfs_sha256": recipe["inputs"]["romfs"]["sha256"],
                "singlescreen_sha256": singlescreen_contract["sha256"] if singlescreen else None}
    key = hashlib.sha256(repr(sorted(identity.items())).encode("ascii")).hexdigest()
    directory = data_root / "mods" / "topscreen" / key
    pack = directory / "atlas_overrides.o3tu"
    animations = directory / LINK_ANIMATIONS
    ui_atlas_hd = directory / UI_ATLAS_HD
    font_pack = directory / "font_coverage.zip"
    font_receipt_path = directory / "fonts_import.json"
    receipt_path = directory / "import.json"
    textures_valid = False
    if pack.is_file() and receipt_path.is_file():
        receipt = load_json_object(receipt_path)
        textures_valid = (receipt.get("source") == identity and
                          receipt.get("pack_sha256") == sha256_file(pack) and
                          (singlescreen is None or
                           (animations.is_file() and
                            receipt.get("link_animations_sha256") == sha256_file(animations) and
                            ui_atlas_hd.is_file() and
                            receipt.get("ui_atlas_hd_sha256") == sha256_file(ui_atlas_hd))))
    font_identity = {**identity, "font_import_version": FONT_IMPORT_VERSION}
    fonts_valid = False
    if font_pack.is_file() and font_receipt_path.is_file():
        receipt = load_json_object(font_receipt_path)
        fonts_valid = (receipt.get("source") == font_identity and
                       receipt.get("pack_sha256") == sha256_file(font_pack))
    if textures_valid and fonts_valid:
        return pack

    archive = acquire_archive(root, data_root, contract, report)
    directory.mkdir(parents=True, exist_ok=True)
    if not textures_valid:
        report("topscreen", "Importing native TopScreen UI textures...")
        payload = build_texture_pack(archive=archive, original_romfs_image=romfs,
                                     singlescreen_archive=singlescreen)
        atomic_write_bytes(pack, payload)
        receipt = {"source": identity, "pack_sha256": sha256_file(pack),
                   "attribution": "TopScreen / Single Screen Experience by M-1",
                   "source_url": "https://gamebanana.com/mods/695893"}
        if singlescreen is not None:
            report("topscreen", "Importing SingleScreen 3.0 Link animations...")
            atomic_write_bytes(animations, extract_link_animations(singlescreen))
            receipt["link_animations_sha256"] = sha256_file(animations)
            report("topscreen", "Importing the SingleScreen 3.0 HD UI atlas...")
            atomic_write_bytes(ui_atlas_hd, extract_ui_atlas_hd(singlescreen))
            receipt["ui_atlas_hd_sha256"] = sha256_file(ui_atlas_hd)
        atomic_write_json(receipt_path, receipt)
    if not fonts_valid:
        report("topscreen", "Preparing TopScreen font coverage...")
        manifest = prepare_font_pack(archive=archive, romfs=romfs, output=font_pack)
        atomic_write_json(font_receipt_path, {
            "source": font_identity, "pack_sha256": sha256_file(font_pack),
            "runtime_enabled": True,
            "minimum_output_height": 480,
            "status": "native_atlas_coverage_ready",
            "font_count": len(manifest["fonts"]),
            "attribution": "TopScreen / Single Screen Experience by M-1 / rlgcarrot",
            "source_url": "https://gamebanana.com/mods/695893"})
    return pack
