"""Install the SingleScreen 3.0 assets TriAevum ships, never executable patches.

SingleScreen 3.0 is part of TriAevum with the author's permission: its
low-resolution UI atlas (custom_ui01.ctxb) and Link animations
(zelda_link_ext.zar) are in the package. Its high-resolution graphics (the
HD UI atlas and the HD fonts) stay out of the package until their author
consents; Forge imports them only from a SingleScreen 3.0 archive the user
places beside it, and never downloads one.
"""

from __future__ import annotations

import hashlib
import shutil
import struct
import zipfile
from pathlib import Path
from typing import Callable

from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import build_texture_pack
from tools.oot3d.decomp_support.scripts.prepare_qbf_font_pack import prepare_font_pack

try:
    from .common import atomic_write_bytes, atomic_write_json, load_json_object, sha256_file
except ImportError:
    from common import atomic_write_bytes, atomic_write_json, load_json_object, sha256_file


# 2: SingleScreen 3.0 UI atlas, native localized labels (2.1.1 ones retired).
# 3: SingleScreen 3.0 HD UI atlas.
# 4: SingleScreen 3.0 is the only source; TopScreen 2.1.1 is retired.
# 5: the low-resolution assets come from the package.
IMPORT_VERSION = 5
# 3: the HD fonts come from SingleScreen 3.0.
FONT_IMPORT_VERSION = 3
UI_ATLAS = "custom_ui01.ctxb"
LINK_ANIMATIONS = "zelda_link_ext.zar"
# Package directory of the shipped low-resolution assets, and their identity.
BUNDLED_DIRECTORY = Path("resources/topscreen/singlescreen-3.0")
BUNDLED_ASSETS = {
    UI_ATLAS: (131144, "20290e417c56973cd19daa1f61a6c3963f075d61776db3d996954ee3296d07d8"),
    LINK_ANIMATIONS: (186404, "06d60e05c493e86677497eb935b49951d613249c43f896816ae898bb8b005902"),
}
# The HD graphics' author's custom_ui01, shipped in the archive as an emulator
# texture replacement. It also draws elements the native atlas lacks
# (SELECT/START, ZL/ZR labels). TriAevum uses it as the 3.0 atlas, keyed by its
# native content, so it needs no separate texture pack.
UI_ATLAS_HD = "ui_atlas_hd.png"
UI_ATLAS_HD_MEMBER = ("SingleScreen_3.0/EUR/Emulator/load/textures/0004000000033600/"
                      "tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png")
MOD_PAGE = "https://gamebanana.com/mods/695893"


def bundled_assets(root: Path) -> dict[str, Path]:
    """The package's SingleScreen 3.0 assets, verified."""
    assets = {}
    for name, (size, digest) in BUNDLED_ASSETS.items():
        path = root / BUNDLED_DIRECTORY / name
        if not path.is_file() or path.stat().st_size != size or sha256_file(path) != digest:
            raise ValueError(f"The TriAevum package is incomplete or modified: {BUNDLED_DIRECTORY / name}. "
                             "Extract the release again.")
        assets[name] = path
    return assets


def find_singlescreen_archive(root: Path, data_root: Path, contract: dict) -> Path | None:
    """The user's SingleScreen 3.0 archive for the HD graphics, verified; never downloaded."""
    name = contract["name"]
    for candidate in (root / name, root / "mods" / name, data_root / "downloads" / name):
        if candidate.is_file():
            if (candidate.stat().st_size != contract["bytes"] or
                    sha256_file(candidate) != contract["sha256"]):
                raise ValueError(f"SingleScreen 3.0 archive failed verification: {candidate}")
            return candidate
    return None


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
    """The installed zelda_link_ext.zar beside a prepared texture pack."""
    if pack is None:
        return None
    candidate = pack.parent / LINK_ANIMATIONS
    return candidate if candidate.is_file() else None


def prepare_topscreen_assets(*, root: Path, data_root: Path, recipe: dict,
                            romfs: Path,
                            report: Callable[[str, str], None] = lambda *_: None) -> Path | None:
    contract = recipe.get("optional_inputs", {}).get("singlescreen_3_0_archive")
    if contract is None:
        return None  # Other title/revision recipes do not inherit OOT3D UI assets.
    assets = bundled_assets(root)
    hd_archive = find_singlescreen_archive(root, data_root, contract)
    identity = {"import_version": IMPORT_VERSION,
                "ui_atlas_sha256": BUNDLED_ASSETS[UI_ATLAS][1],
                "link_animations_sha256": BUNDLED_ASSETS[LINK_ANIMATIONS][1],
                "hd_archive_sha256": contract["sha256"] if hd_archive else None,
                "romfs_sha256": recipe["inputs"]["romfs"]["sha256"]}
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
                          animations.is_file() and
                          sha256_file(animations) == BUNDLED_ASSETS[LINK_ANIMATIONS][1] and
                          (hd_archive is None or
                           (ui_atlas_hd.is_file() and
                            receipt.get("ui_atlas_hd_sha256") == sha256_file(ui_atlas_hd))))
    font_identity = {**identity, "font_import_version": FONT_IMPORT_VERSION}
    fonts_valid = hd_archive is None
    if not fonts_valid and font_pack.is_file() and font_receipt_path.is_file():
        receipt = load_json_object(font_receipt_path)
        fonts_valid = (receipt.get("source") == font_identity and
                       receipt.get("pack_sha256") == sha256_file(font_pack))
    if textures_valid and fonts_valid:
        return pack

    directory.mkdir(parents=True, exist_ok=True)
    if not textures_valid:
        report("topscreen", "Installing the SingleScreen 3.0 UI textures and Link animations...")
        atomic_write_bytes(pack, build_texture_pack(singlescreen_ui_atlas=assets[UI_ATLAS].read_bytes()))
        shutil.copyfile(assets[LINK_ANIMATIONS], animations)
        receipt = {"source": identity, "pack_sha256": sha256_file(pack),
                   "attribution": "TopScreen / Single Screen Experience by M-1",
                   "source_url": MOD_PAGE}
        if hd_archive is not None:
            report("topscreen", "Importing the SingleScreen 3.0 HD UI atlas from the local archive...")
            atomic_write_bytes(ui_atlas_hd, extract_ui_atlas_hd(hd_archive))
            receipt["ui_atlas_hd_sha256"] = sha256_file(ui_atlas_hd)
        atomic_write_json(receipt_path, receipt)
    if not fonts_valid:
        report("topscreen", "Preparing the SingleScreen 3.0 HD fonts from the local archive...")
        manifest = prepare_font_pack(archive=hd_archive, romfs=romfs, output=font_pack)
        atomic_write_json(font_receipt_path, {
            "source": font_identity, "pack_sha256": sha256_file(font_pack),
            "runtime_enabled": True,
            "minimum_output_height": 480,
            "status": "native_atlas_coverage_ready",
            "font_count": len(manifest["fonts"]),
            "attribution": "TopScreen / Single Screen Experience by M-1 / rlgcarrot",
            "source_url": MOD_PAGE})
    return pack
