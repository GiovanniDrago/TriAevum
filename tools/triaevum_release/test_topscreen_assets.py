import hashlib
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from topscreen_assets import (BUNDLED_DIRECTORY, LINK_ANIMATIONS, UI_ATLAS, UI_ATLAS_HD_MEMBER,
                              extract_ui_atlas_hd, prepare_topscreen_assets, topscreen_link_animations,
                              topscreen_ui_atlas_hd)
from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import (
    LANGUAGES, LAYOUT_PROBE, TEXTURES, PROFILE_TEXTURES, build_texture_pack,
)

ARCHIVE = "singlescreen_30_25aeb.zip"
REPOSITORY = Path(__file__).resolve().parents[2]
SHIPPED = REPOSITORY / "runtime/triaevum_resources/topscreen/singlescreen-3.0"


class TopScreenAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        bundled = self.root / BUNDLED_DIRECTORY
        bundled.mkdir(parents=True)
        for name in (UI_ATLAS, LINK_ANIMATIONS):
            (bundled / name).write_bytes((SHIPPED / name).read_bytes())
        staging = self.root / "staging"
        staging.mkdir()
        self.contract = self.make_singlescreen_archive(staging / ARCHIVE)
        self.archive_bytes = (staging / ARCHIVE).read_bytes()
        self.recipe = {"inputs": {"romfs": {"sha256": "a" * 64}},
                       "optional_inputs": {"singlescreen_3_0_archive": self.contract}}

    def prepare(self, report=lambda *_: None):
        return prepare_topscreen_assets(root=self.root, data_root=self.data, recipe=self.recipe,
                                        romfs=self.root / "romfs.bin", report=report)

    def place_archive(self):
        (self.root / ARCHIVE).write_bytes(self.archive_bytes)
        return self.root / ARCHIVE

    @staticmethod
    def fake_font_pack(*, archive, romfs, output):
        output.write_bytes(b"validated font fixture")
        return {"fonts": [{"romfs_path": "message/eu/ltn16.qbf"}]}

    @staticmethod
    def png_header(width, height):
        return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" +
                struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0) + b"fixture")

    def make_singlescreen_archive(self, path, *, atlas=None):
        with zipfile.ZipFile(path, "w") as output:
            output.writestr("SingleScreen_3.0/EUR/Emulator/load/mods/0004000000033600/code.ips",
                            b"DO NOT IMPORT EXECUTABLE PATCH")
            output.writestr(UI_ATLAS_HD_MEMBER, atlas if atlas is not None else self.png_header(4096, 2048))
        data = path.read_bytes()
        return {"name": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def test_shipped_assets_install_without_archive_or_network(self):
        with patch("topscreen_assets.prepare_font_pack") as fonts, \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = self.prepare()
            fonts.assert_not_called()
            network.assert_not_called()
        payload = pack.read_bytes()
        self.assertEqual(struct.unpack_from("<4sIII", payload), (b"O3TU", 2, 0, 1))
        self.assertIn(b"oot3d/topscreen/3.0/ui_atlas", payload)
        self.assertEqual(topscreen_link_animations(pack).read_bytes(),
                         (SHIPPED / LINK_ANIMATIONS).read_bytes())
        # The high-resolution graphics stay out without the user's archive.
        self.assertIsNone(topscreen_ui_atlas_hd(pack))
        self.assertFalse((pack.parent / "font_coverage.zip").exists())
        receipt = json.loads((pack.parent / "import.json").read_text())
        self.assertIsNone(receipt["source"]["hd_archive_sha256"])
        with patch("topscreen_assets.build_texture_pack") as build:
            self.assertEqual(self.prepare(), pack)
            build.assert_not_called()

    def test_modified_or_missing_shipped_asset_is_a_package_error(self):
        (self.root / BUNDLED_DIRECTORY / UI_ATLAS).write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "package is incomplete or modified"):
            self.prepare()
        (self.root / BUNDLED_DIRECTORY / UI_ATLAS).unlink()
        with self.assertRaisesRegex(ValueError, "custom_ui01.ctxb"):
            self.prepare()

    def test_tampered_installed_animations_are_reinstalled(self):
        pack = self.prepare()
        topscreen_link_animations(pack).write_bytes(b"ZAR\x01 tampered")
        self.assertEqual(self.prepare(), pack)
        self.assertEqual(topscreen_link_animations(pack).read_bytes(),
                         (SHIPPED / LINK_ANIMATIONS).read_bytes())

    def test_local_archive_adds_the_hd_atlas_and_fonts(self):
        archive = self.place_archive()
        messages = []
        with patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack) as fonts, \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = self.prepare(report=lambda _, text: messages.append(text))
            network.assert_not_called()
        self.assertEqual(fonts.call_args.kwargs["archive"], archive)
        self.assertEqual(topscreen_ui_atlas_hd(pack).read_bytes(), self.png_header(4096, 2048))
        receipt = json.loads((pack.parent / "fonts_import.json").read_text())
        self.assertEqual(receipt["source"]["hd_archive_sha256"], self.contract["sha256"])
        self.assertTrue(any("local archive" in text for text in messages))

    def test_corrupt_font_and_new_import_version_are_reprepared(self):
        self.place_archive()
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build, \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack) as fonts:
            pack = self.prepare()
            (pack.parent / "font_coverage.zip").write_bytes(b"corrupt")
            self.prepare()
            with patch("topscreen_assets.FONT_IMPORT_VERSION", 99):
                self.prepare()
            self.assertEqual(build.call_count, 1)
            self.assertEqual(fonts.call_count, 3)
            receipt = json.loads((pack.parent / "fonts_import.json").read_text())
            self.assertEqual(receipt["source"]["font_import_version"], 99)
            self.assertTrue(receipt["runtime_enabled"])

    def test_font_failure_is_not_cached_as_success_and_can_be_retried(self):
        self.place_archive()
        with patch("topscreen_assets.prepare_font_pack", side_effect=ValueError("invalid QBF")):
            with self.assertRaisesRegex(ValueError, "invalid QBF"):
                self.prepare()
        self.assertFalse(list(self.data.rglob("fonts_import.json")))
        with patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack):
            self.prepare()

    def test_corrupt_local_archive_rejected(self):
        (self.root / ARCHIVE).write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "SingleScreen 3.0 archive failed verification"):
            self.prepare()

    def test_hd_ui_atlas_must_be_a_two_to_one_png(self):
        for name, atlas in (("square", self.png_header(2048, 2048)), ("odd", self.png_header(4000, 2000)),
                            ("text", b"not a png at all, padded to the IHDR length")):
            path = self.root / f"{name}.zip"
            self.make_singlescreen_archive(path, atlas=atlas)
            with self.subTest(name), self.assertRaises(ValueError):
                extract_ui_atlas_hd(path)
        path = self.root / "native.zip"
        self.make_singlescreen_archive(path, atlas=self.png_header(512, 256))
        self.assertEqual(extract_ui_atlas_hd(path), self.png_header(512, 256))

    def test_other_title_does_not_install_oot3d_assets(self):
        self.assertIsNone(prepare_topscreen_assets(root=self.root, data_root=self.data,
                                                   recipe={}, romfs=self.root / "other.bin"))
        self.assertFalse(self.data.exists())

    def test_builder_imports_only_native_textures_not_executable_mod(self):
        original = self.root / "romfs"
        archive = self.root / "fixture.zip"
        header = bytearray(0x48)
        header[:4] = b"ctxb"
        struct.pack_into("<HHHH", header, 0x2c, 8, 8, 0x6752, 0x1401)
        with zipfile.ZipFile(archive, "w") as output:
            for language in LANGUAGES:
                for name in TEXTURES:
                    relative = f"menu/{language}/{name}"
                    path = original / relative
                    path.parent.mkdir(parents=True, exist_ok=True)
                    path.write_bytes(header + bytes(256))
                    output.writestr("TopScreenMod/romfs/" + relative, header + bytes([1]) * 256)
            for name, _ in PROFILE_TEXTURES:
                output.writestr("TopScreenMod/romfs/menu/" + name, header + bytes([2]) * 256)
            # The localized 2.1.1 labels identify the layout but are not imported.
            output.writestr("TopScreenMod/romfs/" + LAYOUT_PROBE, header + bytes([1]) * 256)
            output.writestr("TopScreenMod/code.ips", b"DO NOT IMPORT EXECUTABLE PATCH")
            output.writestr("../../escape", b"not extracted")
        pack = build_texture_pack(archive=archive, original_romfs=original)
        self.assertEqual(struct.unpack_from("<4sIII", pack), (b"O3TU", 2, 0, 2))
        self.assertNotIn(b"DO NOT IMPORT", pack)
        self.assertFalse((self.root / "escape").exists())

        singlescreen = self.root / "singlescreen.zip"
        prefix = "SingleScreen_3.0/EUR/Emulator/load/mods/0004000000033600/romfs/"
        with zipfile.ZipFile(singlescreen, "w") as output:
            output.writestr(prefix + "menu/custom_ui01.ctxb", header + bytes([3]) * 256)
            output.writestr("SingleScreen_3.0/EUR/Emulator/load/mods/0004000000033600/code.ips",
                            b"DO NOT IMPORT EXECUTABLE PATCH")
        pack = build_texture_pack(archive=archive, original_romfs=original,
                                  singlescreen_archive=singlescreen)
        self.assertEqual(struct.unpack_from("<4sIII", pack), (b"O3TU", 2, 0, 3))
        self.assertIn(b"oot3d/topscreen/3.0/ui_atlas", pack)
        self.assertNotIn(b"DO NOT IMPORT", pack)

        # Forge's install: the shipped 3.0 atlas alone, no ROM and no archive.
        pack = build_texture_pack(singlescreen_ui_atlas=bytes(header + bytes([3]) * 256))
        self.assertEqual(struct.unpack_from("<4sIII", pack), (b"O3TU", 2, 0, 1))
        self.assertIn(b"oot3d/topscreen/3.0/ui_atlas", pack)
        self.assertNotIn(b"oot3d/topscreen/2.1.1/", pack)
        with self.assertRaisesRegex(ValueError, "TopScreen 2.1.1 or SingleScreen 3.0"):
            build_texture_pack(original_romfs=original)
        with self.assertRaisesRegex(ValueError, "at most one SingleScreen 3.0 source"):
            build_texture_pack(singlescreen_archive=singlescreen, singlescreen_ui_atlas=b"x")


if __name__ == "__main__":
    unittest.main()
