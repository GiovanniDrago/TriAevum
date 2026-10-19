from __future__ import annotations

import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from singlescreen_archive import (
    DOWNLOAD_31,
    ArchiveContract,
    archive_contracts,
    download_archive,
    import_archive,
    stored_archive,
)

RECIPES = Path(__file__).with_name("supported_revisions.json")
PAYLOAD = b"singlescreen fixture archive"
OTHER = b"another fixture archive of another size"


def contract(kind: str, name: str, data: bytes) -> ArchiveContract:
    return ArchiveContract(kind, name, len(data), hashlib.sha256(data).hexdigest())


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()


class SingleScreenArchiveTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.data = self.root / "data"
        self.contracts = (contract("singlescreen_3_1_archive", "ss31.zip", PAYLOAD),
                          contract("singlescreen_3_0_archive", "ss30.zip", OTHER))

    def test_shipped_recipes_accept_31_first(self) -> None:
        contracts = archive_contracts(RECIPES)
        self.assertEqual([item.version for item in contracts], ["3.1", "3.0"])
        self.assertEqual(contracts[0].name, "singlescreen_31_989c3.zip")
        self.assertEqual(contracts[0].bytes, 28384023)

    def test_selected_archive_is_copied_into_downloads(self) -> None:
        source = self.root / "picked.zip"
        source.write_bytes(PAYLOAD)
        kept, path = import_archive(source, self.data, self.contracts)
        self.assertEqual(kept.version, "3.1")
        self.assertEqual(path, self.data / "downloads" / "ss31.zip")
        self.assertEqual(path.read_bytes(), PAYLOAD)
        self.assertEqual(stored_archive(self.root / "package", self.data, self.contracts), kept)

    def test_other_files_are_rejected_and_nothing_is_kept(self) -> None:
        source = self.root / "picked.zip"
        source.write_bytes(PAYLOAD[:-1] + b"X")
        with self.assertRaisesRegex(ValueError, "not the official SingleScreen archive"):
            import_archive(source, self.data, self.contracts)
        self.assertFalse((self.data / "downloads").exists())
        self.assertIsNone(stored_archive(self.root / "package", self.data, self.contracts))

    def test_download_keeps_only_a_verified_31_archive(self) -> None:
        requests = []

        def opener(request, *, timeout, context):
            requests.append((request.full_url, timeout, context))
            return FakeResponse(PAYLOAD)

        progress = []
        kept, path = download_archive(self.data, self.contracts, opener=opener,
                                      progress=lambda done, total: progress.append((done, total)))
        self.assertEqual(requests[0][0], DOWNLOAD_31)
        self.assertEqual(kept.version, "3.1")
        self.assertEqual(path.read_bytes(), PAYLOAD)
        self.assertEqual(progress[-1], (len(PAYLOAD), len(PAYLOAD)))

    def test_a_different_release_fails_and_leaves_no_partial_file(self) -> None:
        for body, message in ((PAYLOAD + b"newer", "larger than SingleScreen 3.1"),
                              (PAYLOAD[:-1] + b"X", "not the SingleScreen 3.1 archive")):
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    download_archive(self.data, self.contracts,
                                     opener=lambda *_args, **_kwargs: FakeResponse(body))
                self.assertEqual(list((self.data / "downloads").iterdir()), [])

    def test_recipes_must_agree_on_an_archive(self) -> None:
        recipes = self.root / "recipes.json"
        recipes.write_text(json.dumps({"recipes": [
            {"optional_inputs": {"singlescreen_3_1_archive": {"name": "a.zip", "bytes": 1, "sha256": "a" * 64}}},
            {"optional_inputs": {"singlescreen_3_1_archive": {"name": "b.zip", "bytes": 1, "sha256": "a" * 64}}},
        ]}), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "disagree"):
            archive_contracts(recipes)


if __name__ == "__main__":
    unittest.main()
