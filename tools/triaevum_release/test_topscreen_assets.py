import hashlib
import io
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from topscreen_assets import (UI_ATLAS_HD_MEMBER, acquire_archive, extract_ui_atlas_hd,
                              prepare_topscreen_assets, topscreen_link_animations, topscreen_ui_atlas_hd)
from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import (
    LANGUAGES, LAYOUT_PROBE, TEXTURES, PROFILE_TEXTURES, build_texture_pack,
)


class TopScreenAssetsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.data = self.root / "data"
        self.archive_bytes = b"official archive fixture"
        self.contract = {"sha256": hashlib.sha256(self.archive_bytes).hexdigest(),
                         "bytes": len(self.archive_bytes), "url": "https://example.test/archive"}
        self.recipe = {"inputs": {"romfs": {"sha256": "a" * 64}},
                       "optional_inputs": {"topscreen_2_1_1_archive": self.contract}}

    def prepare(self):
        return prepare_topscreen_assets(root=self.root, data_root=self.data,
                                       recipe=self.recipe, romfs=self.root / "romfs.bin")

    @staticmethod
    def fake_font_pack(*, archive, romfs, output):
        output.write_bytes(b"validated font fixture")
        return {"fonts": [{"romfs_path": "message/eu/ltn16.qbf"}]}

    def test_local_import_then_verified_cache_without_archive_or_network(self):
        archive = self.root / "topscreen211.zip"
        archive.write_bytes(self.archive_bytes)
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build, \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack) as fonts, \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = prepare_topscreen_assets(root=self.root, data_root=self.data,
                                           recipe=self.recipe, romfs=self.root / "romfs.bin")
            build.assert_called_once_with(archive=archive, original_romfs_image=self.root / "romfs.bin",
                                          singlescreen_archive=None)
            archive.unlink()
            self.assertEqual(prepare_topscreen_assets(root=self.root, data_root=self.data,
                             recipe=self.recipe, romfs=self.root / "romfs.bin"), pack)
            self.assertEqual(build.call_count, 1)
            self.assertEqual(fonts.call_count, 1)
            network.assert_not_called()

    def test_old_texture_receipt_upgrades_only_fonts_using_cached_download(self):
        cache = self.data / "downloads/topscreen211.zip"
        cache.parent.mkdir(parents=True)
        cache.write_bytes(self.archive_bytes)
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build, \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack) as fonts, \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = self.prepare()
            old_receipt = (pack.parent / "import.json").read_bytes()
            (pack.parent / "font_coverage.zip").unlink()
            (pack.parent / "fonts_import.json").unlink()
            self.assertEqual(self.prepare(), pack)
            self.assertEqual(build.call_count, 1)
            self.assertEqual(fonts.call_count, 2)
            self.assertEqual((pack.parent / "import.json").read_bytes(), old_receipt)
            network.assert_not_called()

    def test_corrupt_font_and_new_import_version_are_reprepared(self):
        (self.root / "topscreen211.zip").write_bytes(self.archive_bytes)
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build, \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack) as fonts:
            pack = self.prepare()
            (pack.parent / "font_coverage.zip").write_bytes(b"corrupt")
            self.prepare()
            with patch("topscreen_assets.FONT_IMPORT_VERSION", 3):
                self.prepare()
            self.assertEqual(build.call_count, 1)
            self.assertEqual(fonts.call_count, 3)
            receipt = json.loads((pack.parent / "fonts_import.json").read_text())
            self.assertEqual(receipt["source"]["font_import_version"], 3)
            self.assertTrue(receipt["runtime_enabled"])
            self.assertEqual(receipt["minimum_output_height"], 480)

    def test_font_failure_is_not_cached_as_success_and_can_be_retried(self):
        (self.root / "topscreen211.zip").write_bytes(self.archive_bytes)
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build:
            with patch("topscreen_assets.prepare_font_pack", side_effect=ValueError("invalid QBF")):
                with self.assertRaisesRegex(ValueError, "invalid QBF"):
                    self.prepare()
            self.assertFalse(list(self.data.rglob("fonts_import.json")))
            with patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack):
                self.prepare()
            self.assertEqual(build.call_count, 1)

    @staticmethod
    def png_header(width, height):
        return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + b"IHDR" +
                struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0) + b"fixture")

    def make_singlescreen_archive(self, path, *, animations=b"ZAR\x01 animation fixture", atlas=None):
        prefix = "SingleScreen_3.0/EUR/Emulator/load/mods/0004000000033600/romfs/"
        with zipfile.ZipFile(path, "w") as output:
            output.writestr(prefix + "actor/zelda_link_ext.zar", animations)
            output.writestr(prefix + "../code.ips", b"DO NOT IMPORT EXECUTABLE PATCH")
            output.writestr(UI_ATLAS_HD_MEMBER, atlas if atlas is not None else self.png_header(4096, 2048))
        data = path.read_bytes()
        return {"name": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def test_singlescreen_archive_beside_forge_installs_atlas_and_animations(self):
        (self.root / "topscreen211.zip").write_bytes(self.archive_bytes)
        singlescreen = self.root / "singlescreen_30_25aeb.zip"
        self.recipe["optional_inputs"]["singlescreen_3_0_archive"] = \
            self.make_singlescreen_archive(singlescreen)
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture") as build, \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack), \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = self.prepare()
            self.assertEqual(build.call_args.kwargs["singlescreen_archive"], singlescreen)
            animations = topscreen_link_animations(pack)
            self.assertEqual(animations.read_bytes(), b"ZAR\x01 animation fixture")
            receipt = json.loads((pack.parent / "import.json").read_text())
            self.assertEqual(receipt["link_animations_sha256"],
                             hashlib.sha256(animations.read_bytes()).hexdigest())
            atlas = topscreen_ui_atlas_hd(pack)
            self.assertEqual(atlas.read_bytes(), self.png_header(4096, 2048))
            self.assertEqual(receipt["ui_atlas_hd_sha256"], hashlib.sha256(atlas.read_bytes()).hexdigest())
            # The verified import is reused; a changed animation file is re-imported.
            self.assertEqual(self.prepare(), pack)
            self.assertEqual(build.call_count, 1)
            animations.write_bytes(b"ZAR\x01 tampered")
            self.assertEqual(self.prepare(), pack)
            self.assertEqual(build.call_count, 2)
            self.assertEqual(animations.read_bytes(), b"ZAR\x01 animation fixture")
            atlas.write_bytes(b"tampered")
            self.assertEqual(self.prepare(), pack)
            self.assertEqual(build.call_count, 3)
            self.assertEqual(atlas.read_bytes(), self.png_header(4096, 2048))
            network.assert_not_called()

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

    def test_missing_singlescreen_archive_is_optional_and_never_downloaded(self):
        (self.root / "topscreen211.zip").write_bytes(self.archive_bytes)
        self.recipe["optional_inputs"]["singlescreen_3_0_archive"] = {
            "name": "singlescreen_30_25aeb.zip", "bytes": 1, "sha256": "0" * 64}
        messages = []
        with patch("topscreen_assets.build_texture_pack", return_value=b"O3TU fixture"), \
                patch("topscreen_assets.prepare_font_pack", side_effect=self.fake_font_pack), \
                patch("verified_download.urllib.request.urlopen") as network:
            pack = prepare_topscreen_assets(root=self.root, data_root=self.data, recipe=self.recipe,
                                            romfs=self.root / "romfs.bin",
                                            report=lambda _, text: messages.append(text))
            self.assertIsNone(topscreen_link_animations(pack))
            self.assertIsNone(topscreen_ui_atlas_hd(pack))
            self.assertTrue(any("singlescreen_30_25aeb.zip" in text for text in messages))
            network.assert_not_called()

    def test_corrupt_singlescreen_archive_rejected(self):
        (self.root / "topscreen211.zip").write_bytes(self.archive_bytes)
        singlescreen = self.root / "singlescreen_30_25aeb.zip"
        contract = self.make_singlescreen_archive(singlescreen)
        singlescreen.write_bytes(b"tampered")
        self.recipe["optional_inputs"]["singlescreen_3_0_archive"] = contract
        with patch("topscreen_assets.build_texture_pack") as build:
            with self.assertRaisesRegex(ValueError, "SingleScreen 3.0 archive failed verification"):
                self.prepare()
            build.assert_not_called()

    def test_corrupt_local_archive_rejected_before_import(self):
        (self.root / "topscreen211.zip").write_bytes(b"bad")
        with patch("topscreen_assets.build_texture_pack") as build:
            with self.assertRaisesRegex(ValueError, "failed verification"):
                prepare_topscreen_assets(root=self.root, data_root=self.data,
                                        recipe=self.recipe, romfs=self.root / "romfs.bin")
            build.assert_not_called()

    def test_download_verified_atomically_and_oversize_rejected(self):
        for payload, valid in ((self.archive_bytes + b"extra", False), (self.archive_bytes, True)):
            response = io.BytesIO(payload)
            response.geturl = lambda: "https://example.test/archive"
            with patch("verified_download.urllib.request.urlopen", return_value=response):
                if valid:
                    path = acquire_archive(self.root, self.data, self.contract, lambda *_: None)
                    self.assertEqual(path.read_bytes(), self.archive_bytes)
                else:
                    with self.assertRaisesRegex(ValueError, "size or time limit"):
                        acquire_archive(self.root, self.data, self.contract, lambda *_: None)
                    self.assertFalse((self.data / "downloads/topscreen211.zip").exists())
            self.assertFalse(list(self.data.rglob("*.partial")))

    def test_other_title_does_not_acquire_oot3d_assets(self):
        with patch("verified_download.urllib.request.urlopen") as network:
            self.assertIsNone(prepare_topscreen_assets(root=self.root, data_root=self.data,
                              recipe={}, romfs=self.root / "other.bin"))
            network.assert_not_called()

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
            output.writestr(prefix + "actor/zelda_link_ext.zar", b"ZAR\x01")
            output.writestr("SingleScreen_3.0/EUR/Emulator/load/mods/0004000000033600/code.ips",
                            b"DO NOT IMPORT EXECUTABLE PATCH")
        pack = build_texture_pack(archive=archive, original_romfs=original,
                                  singlescreen_archive=singlescreen)
        self.assertEqual(struct.unpack_from("<4sIII", pack), (b"O3TU", 2, 0, 3))
        self.assertIn(b"oot3d/topscreen/3.0/ui_atlas", pack)
        self.assertNotIn(b"DO NOT IMPORT", pack)


if __name__ == "__main__":
    unittest.main()
