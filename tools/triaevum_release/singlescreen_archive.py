"""Obtain the user's SingleScreen archive for the optional HD graphics.

The HD textures (by chubbycubby) and HD fonts are not in the package. On the
user's request Forge downloads the SingleScreen 3.1 archive from GameBanana, or
copies one the user selects, and keeps it only after the recipe's size and
SHA-256 match. topscreen_assets.py imports the graphics from that copy.
"""

from __future__ import annotations

import os
import shutil
import ssl
import sys
import tempfile
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

try:
    from .common import load_json_object, sha256_file
except ImportError:
    from common import load_json_object, sha256_file


MOD_PAGE = "https://gamebanana.com/mods/695893"
# GameBanana's file link for singlescreen_31_989c3.zip; it redirects to the file host.
DOWNLOAD_31 = "https://gamebanana.com/dl/1851953"
ARCHIVE_KINDS = ("singlescreen_3_1_archive", "singlescreen_3_0_archive")
VERSIONS = {"singlescreen_3_1_archive": "3.1", "singlescreen_3_0_archive": "3.0"}
# Frozen Linux builds carry the SDK's OpenSSL, whose default CA path may not
# exist on the host distribution.
LINUX_CA_BUNDLES = (
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",
    "/etc/ssl/cert.pem",
)


@dataclass(frozen=True)
class ArchiveContract:
    kind: str
    name: str
    bytes: int
    sha256: str

    @property
    def version(self) -> str:
        return VERSIONS[self.kind]


def archive_contracts(recipes: Path) -> tuple[ArchiveContract, ...]:
    """Every SingleScreen archive the supported recipes accept, newest first."""
    payload = load_json_object(recipes)
    found: dict[str, ArchiveContract] = {}
    for recipe in payload.get("recipes", []):
        optional = recipe.get("optional_inputs") if isinstance(recipe, dict) else None
        if not isinstance(optional, dict):
            continue
        for kind in ARCHIVE_KINDS:
            item = optional.get(kind)
            if isinstance(item, dict):
                contract = ArchiveContract(kind, str(item["name"]), int(item["bytes"]),
                                           str(item["sha256"]).lower())
                if found.setdefault(contract.sha256, contract) != contract:
                    raise ValueError(f"Recipes disagree about SingleScreen archive {contract.sha256}")
    return tuple(sorted(found.values(), key=lambda item: ARCHIVE_KINDS.index(item.kind)))


def stored_archive(root: Path, data_root: Path,
                   contracts: tuple[ArchiveContract, ...]) -> ArchiveContract | None:
    """The newest archive where the import looks for one, by size only (cheap)."""
    for contract in contracts:
        for directory in (root, root / "mods", downloads_directory(data_root)):
            path = directory / contract.name
            if path.is_file() and path.stat().st_size == contract.bytes:
                return contract
    return None


def downloads_directory(data_root: Path) -> Path:
    return data_root / "downloads"


def _matching(path: Path, contracts: tuple[ArchiveContract, ...]) -> ArchiveContract | None:
    size = path.stat().st_size
    candidates = [contract for contract in contracts if contract.bytes == size]
    if not candidates:
        return None
    digest = sha256_file(path)
    return next((contract for contract in candidates if contract.sha256 == digest), None)


def _keep(temporary: Path, data_root: Path, contract: ArchiveContract) -> Path:
    destination = downloads_directory(data_root) / contract.name
    os.replace(temporary, destination)
    return destination


def import_archive(source: Path, data_root: Path, contracts: tuple[ArchiveContract, ...]) -> tuple[ArchiveContract, Path]:
    """Copy a user-selected archive into the data folder after verification."""
    source = source.expanduser().resolve()
    if not source.is_file():
        raise ValueError(f"Not a file: {source}")
    contract = _matching(source, contracts)
    if contract is None:
        names = " or ".join(contract.name for contract in contracts)
        raise ValueError(f"This is not the official SingleScreen archive ({names}): {source.name}")
    directory = downloads_directory(data_root)
    directory.mkdir(parents=True, exist_ok=True)
    destination = directory / contract.name
    if destination.resolve() == source:
        return contract, destination
    descriptor, name = tempfile.mkstemp(prefix=".", suffix=".part", dir=directory)
    os.close(descriptor)
    temporary = Path(name)
    try:
        shutil.copyfile(source, temporary)
        if _matching(temporary, (contract,)) is None:
            raise ValueError("The copied archive changed while it was being copied")
        return contract, _keep(temporary, data_root, contract)
    finally:
        temporary.unlink(missing_ok=True)


def tls_context() -> ssl.SSLContext:
    context = ssl.create_default_context()
    if sys.platform.startswith("linux") and not context.get_ca_certs():
        bundle = next((path for path in LINUX_CA_BUNDLES if os.path.isfile(path)), None)
        if bundle is not None:
            context.load_verify_locations(cafile=bundle)
    return context


def download_archive(
    data_root: Path,
    contracts: tuple[ArchiveContract, ...],
    *,
    url: str = DOWNLOAD_31,
    progress: Callable[[int, int], None] = lambda _done, _total: None,
    opener: Callable[..., object] = urllib.request.urlopen,
) -> tuple[ArchiveContract, Path]:
    """Download the SingleScreen 3.1 archive and keep it only if it verifies."""
    contract = next((item for item in contracts if item.kind == "singlescreen_3_1_archive"), None)
    if contract is None:
        raise ValueError("No supported game revision accepts the SingleScreen 3.1 archive")
    directory = downloads_directory(data_root)
    directory.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix=".", suffix=".part", dir=directory)
    temporary = Path(name)
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "TriAevumForge"})
        done = 0
        with os.fdopen(descriptor, "wb") as output, opener(request, timeout=60, context=tls_context()) as response:
            while chunk := response.read(1024 * 1024):
                done += len(chunk)
                if done > contract.bytes:
                    raise ValueError("The download is larger than SingleScreen 3.1: the mod page "
                                     "may offer a newer release")
                output.write(chunk)
                progress(done, contract.bytes)
        if _matching(temporary, (contract,)) is None:
            raise ValueError("The download is not the SingleScreen 3.1 archive: the mod page "
                             "may offer a newer release")
        return contract, _keep(temporary, data_root, contract)
    finally:
        temporary.unlink(missing_ok=True)
