import json
import math
import struct
import tempfile
import unittest
from pathlib import Path

from scene_lights import (CATALOG_SCHEMA, GENERATOR_REVISION, build_catalog, category, content_hash, is_halo,
                          prepare_scene_lights, room_emitters, room_fingerprint, texture_colour)

RGBA8 = (0x6752, 0x1401)
MATERIAL_SIZE = 0x15C


def cmb(materials, textures, shapes, bones=((0, -1),)):
    """A version-6 CMB: materials [(texture index, additive)], textures
    [(name, (format, type), data)], shapes [(material, positions, triangles)]
    with float positions and u16 indices, identity bones [(index, parent)]."""
    def chunk(magic, body):
        return magic + struct.pack("<I", 8 + len(body)) + body

    mats = bytearray(struct.pack("<I", len(materials)))
    for texture, additive in materials:
        material = bytearray(MATERIAL_SIZE)
        struct.pack_into("<hhh", material, 0x10, texture, -1, -1)
        struct.pack_into("<h", material, 0x10 + 0x18, -1)
        struct.pack_into("<h", material, 0x10 + 0x30, -1)
        if additive:
            struct.pack_into("<I", material, 0x138, 1)
            struct.pack_into("<H", material, 0x13E, 1)
        mats += material
    tex_entries, texture_data = b"", b""
    for name, (fmt, kind), data in textures:
        tex_entries += struct.pack("<IHHHHHHI16s", len(data), 1, 0, 8, 8, fmt, kind, len(texture_data),
                                   name.encode("ascii"))
        texture_data += data
    indices, positions = b"", b""
    meshes, sepds = b"", []
    for index, (material, points, triangles) in enumerate(shapes):
        meshes += struct.pack("<HBB", index, material, 0)
        first = len(indices) // 2
        base = len(positions) // 12
        flat = [base + i for tri in triangles for i in tri]
        indices += struct.pack(f"<{len(flat)}H", *flat)
        positions += b"".join(struct.pack("<3f", *p) for p in points)
        sepds.append((first, len(flat)))
    # SKLM: header, mshs, shp with its sepd/prms/prm chunks.
    mshs = chunk(b"mshs", struct.pack("<II", len(shapes), 0) + meshes)
    shp_head_size = 0x10 + 2 * len(shapes)
    shp_head_size += -shp_head_size % 4
    bodies, offsets = b"", []
    for first, count in sepds:
        sepd = bytearray(0x10C)
        sepd[0:4] = b"sepd"
        struct.pack_into("<HH", sepd, 8, 1, 1)
        struct.pack_into("<IfHH", sepd, 0x24, 0, 1.0, 0x1406, 0)
        struct.pack_into("<H", sepd, 0x108, len(sepd))
        # One primitive bound rigidly to bone 0 (its table right after it).
        prms = bytearray(0x1C)
        prms[0:4] = b"prms"
        struct.pack_into("<IHHII", prms, 8, 1, 0, 1, 0x18, len(prms))
        prm = bytearray(0x18)
        prm[0:4] = b"prm "
        struct.pack_into("<IIHHHH", prm, 8, 1, 0, 0x1403, 0, count, first)
        offsets.append(shp_head_size + len(bodies))
        bodies += bytes(sepd) + bytes(prms) + bytes(prm)
    shp = bytearray(shp_head_size)
    shp[0:4] = b"shp "
    struct.pack_into("<II", shp, 4, shp_head_size + len(bodies), len(shapes))
    for i, offset in enumerate(offsets):
        struct.pack_into("<H", shp, 0x10 + 2 * i, offset)
    shp = bytes(shp) + bodies
    sklm_head = 0x10
    sklm = b"sklm" + struct.pack("<III", sklm_head + len(mshs) + len(shp), sklm_head, sklm_head + len(mshs))
    sklm += mshs + shp
    vatr = b"vatr" + struct.pack("<II", 0x2C + len(positions), 0) + struct.pack("<II", len(positions), 0x2C)
    vatr += b"\0" * (0x2C - len(vatr)) + positions

    skl = chunk(b"skl ", struct.pack("<II", len(bones), 0) + b"".join(
        struct.pack("<hh9f", index, parent, 1, 1, 1, 0, 0, 0, 0, 0, 0) for index, parent in bones))
    parts = [("skl", skl), ("mats", chunk(b"mats", bytes(mats))), ("tex", chunk(b"tex ", struct.pack("<I", len(textures)) +
                                                                     tex_entries)),
             ("sklm", sklm), ("vatr", vatr), ("indices", indices), ("texdata", texture_data)]
    offset, layout, body = 0x44, {}, b""
    for name, data in parts:
        layout[name] = offset
        body += data + b"\0" * (-len(data) % 4)
        offset = 0x44 + len(body)
    header = bytearray(0x44)
    header[0:4] = b"cmb "
    struct.pack_into("<II", header, 4, 0x44 + len(body), 6)
    struct.pack_into("<9I", header, 0x20, len(indices) // 2, layout["skl"], layout["mats"], layout["tex"], layout["sklm"], 0,
                     layout["vatr"], layout["indices"], layout["texdata"])
    return bytes(header) + body


def cube(centre, size):
    x, y, z = centre
    h = size / 2
    points = [(x + dx * h, y + dy * h, z + dz * h) for dx in (-1, 1) for dy in (-1, 1) for dz in (-1, 1)]
    triangles = [(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1),
                 (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)]
    return points, triangles


def room(model: bytes) -> bytes:
    return b"ZSI\x01" + b"room".ljust(12, b"\0") + bytes(range(256)) * 2 + model


ORANGE = bytes([255, 0x20, 0x80, 0xFF]) * 64  # A, B, G, R
GREY = bytes([255, 0x80, 0x80, 0x80]) * 64


class SceneLightsTests(unittest.TestCase):
    def test_glow_cards_with_a_round_halo_are_lamps(self):
        # A 16x16 RGBA8 radial glow (stored in 8x8 Morton tiles) is a halo; a flat one is not.
        def stored(value):
            texels = [b"\0"] * 256
            for y in range(16):
                for x in range(16):
                    tile = (y // 8) * 2 + x // 8
                    m = sum((((x % 8) >> i) & 1) << (2 * i) | (((y % 8) >> i) & 1) << (2 * i + 1) for i in range(3))
                    level = round(255 * value(x, y))
                    texels[tile * 64 + m] = bytes([255, level, level, level])
            return b"".join(texels)
        radial = stored(lambda x, y: max(0.0, 1.0 - (math.hypot(x - 7.5, y - 7.5) / 6.5) ** 2))
        flat = stored(lambda x, y: 0.8)
        self.assertTrue(is_halo(*RGBA8, radial, 16, 16))
        self.assertFalse(is_halo(*RGBA8, flat, 16, 16))
        self.assertEqual(category("anything", True, halo=True), "lamp")
        self.assertEqual(category("anything", False, halo=True), None)
        self.assertEqual(category("shop_any_lant_0", False), "lamp")

    def test_categories_follow_the_first_texture(self):
        self.assertEqual(category("gnn_torch_03", False), "torch")
        self.assertEqual(category("spot01_mado", False), "window-lamp")
        self.assertEqual(category("dg05_kabe_al", True), "fire-lava")
        self.assertIsNone(category("dg05_kabe_al", False))
        self.assertIsNone(category("ganon_kabe", False))

    def test_texture_colour_reads_pica_rgba8_as_abgr(self):
        r, g, b = texture_colour(*RGBA8, ORANGE)
        self.assertAlmostEqual(r, 1.0)
        self.assertAlmostEqual(g, 0x80 / 255.0, places=3)
        self.assertAlmostEqual(b, 0x20 / 255.0, places=3)

    def test_a_torch_stand_lights_above_its_top_in_its_colour(self):
        stand, triangles = cube((100.0, 50.0, -200.0), 40.0)
        wall, wall_triangles = cube((0.0, 0.0, 0.0), 300.0)
        model = cmb([(0, False), (1, False)], [("gnn_torch_03", RGBA8, ORANGE), ("ganon_kabe", RGBA8, GREY)],
                    [(0, stand, triangles), (1, wall, wall_triangles)])
        emitters = room_emitters(room(model))
        self.assertEqual(len(emitters), 1)
        torch = emitters[0]
        self.assertEqual(torch["category"], "torch")
        self.assertEqual(torch["position"], [100.0, 95.0, -200.0])
        self.assertEqual(torch["colour"], [1.0, 0.502, 0.125])
        self.assertEqual(torch["texture"], "gnn_torch_03")

    def test_malformed_or_nonfinite_models_add_no_lights(self):
        stand, triangles = cube((0.0, 0.0, 0.0), 40.0)
        good = cmb([(0, False)], [("gnn_torch_03", RGBA8, ORANGE)], [(0, stand, triangles)])
        # The shape's position stream offset far outside the file.
        broken = bytearray(good)
        sepd = broken.index(b"sepd")
        struct.pack_into("<I", broken, sepd + 0x24, 0x7FFFFFFF)
        self.assertEqual(room_emitters(room(bytes(broken))), [])
        # A NaN position scale.
        nan = bytearray(good)
        struct.pack_into("<f", nan, sepd + 0x28, float("nan"))
        self.assertEqual(room_emitters(room(bytes(nan))), [])
        # Bones parenting each other (a cycle).
        cyclic = cmb([(0, False)], [("gnn_torch_03", RGBA8, ORANGE)], [(0, stand, triangles)],
                     bones=[(0, 1), (1, 0)])
        self.assertEqual(room_emitters(room(cyclic)), [])
        # Indices beyond the model's index buffer.
        overrun = bytearray(good)
        struct.pack_into("<I", overrun, 0x20, 3)
        self.assertEqual(room_emitters(room(bytes(overrun))), [])
        # A good model beside a broken one still lights.
        self.assertEqual(len(room_emitters(room(bytes(broken) + good))), 1)

    def test_bones_place_rigid_primitives(self):
        stand, triangles = cube((0.0, 0.0, 0.0), 40.0)
        model = bytearray(cmb([(0, False)], [("gnn_torch_03", RGBA8, ORANGE)], [(0, stand, triangles)]))
        # One bone translated by (1000, 0, -500), rotated a quarter turn about Y.
        skl = model.index(b"skl ")
        struct.pack_into("<hh9f", model, skl + 0x10, 0, -1, 1, 1, 1, 0, 1.5707963267948966, 0, 1000, 0, -500)
        emitters = room_emitters(room(bytes(model)))
        self.assertEqual(emitters[0]["position"], [1000.0, 45.0, -500.0])

    def test_catalog_names_scene_room_variant_and_fingerprint(self):
        stand, triangles = cube((0.0, 0.0, 0.0), 40.0)
        data = room(cmb([(0, False)], [("gnn_torch_03", RGBA8, ORANGE)], [(0, stand, triangles)]))
        with tempfile.TemporaryDirectory() as directory:
            romfs = Path(directory)
            (romfs / "scene").mkdir()
            (romfs / "scene/ganon_1_info.zsi").write_bytes(data)
            (romfs / "scene/ganon_1_dd_info.zsi").write_bytes(data[:20] + b"\xff" + data[21:])
            (romfs / "scene/unknown_0_info.zsi").write_bytes(data)
            (romfs / "scene/ganon_info.zsi").write_bytes(data)
            payload, summary = build_catalog(romfs)
        catalog = json.loads(payload)
        self.assertEqual(catalog["schema"], CATALOG_SCHEMA)
        self.assertEqual([(r["scene"], r["room"], r["masterQuest"]) for r in catalog["rooms"]],
                         [(10, 1, False), (10, 1, True)])
        self.assertEqual(catalog["rooms"][0]["fingerprint"], f"{content_hash(data[16:144]):016x}")
        self.assertNotEqual(catalog["rooms"][0]["fingerprint"], catalog["rooms"][1]["fingerprint"])
        self.assertEqual(room_fingerprint(data), catalog["rooms"][0]["fingerprint"])
        self.assertEqual(summary, {"rooms": 2, "emitters": {"torch": 2}})

    def test_prepare_reuses_the_catalog_while_its_receipt_matches(self):
        stand, triangles = cube((0.0, 0.0, 0.0), 40.0)
        data = room(cmb([(0, False)], [("gnn_torch_03", RGBA8, ORANGE)], [(0, stand, triangles)]))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "romfs/scene").mkdir(parents=True)
            (root / "romfs/scene/ganon_1_info.zsi").write_bytes(data)
            recipe = {"inputs": {"romfs": {"sha256": "ab" * 32}}}
            reports = []
            first = prepare_scene_lights(data_root=root / "data", recipe=recipe, romfs=root / "romfs",
                                         report=lambda *args: reports.append(args))
            second = prepare_scene_lights(data_root=root / "data", recipe=recipe, romfs=root / "romfs",
                                          report=lambda *args: reports.append(args))
            self.assertEqual(first, second)
            self.assertEqual(len(reports), 1)
            receipt = json.loads((first.parent / "receipt.json").read_text())
            self.assertEqual(receipt["source"]["generator_revision"], GENERATOR_REVISION)
            first.write_text("changed")
            third = prepare_scene_lights(data_root=root / "data", recipe=recipe, romfs=root / "romfs",
                                         report=lambda *args: reports.append(args))
            self.assertEqual(json.loads(third.read_text())["schema"], CATALOG_SCHEMA)
            self.assertEqual(len(reports), 2)
            self.assertIsNone(prepare_scene_lights(data_root=root / "data", recipe={}, romfs=root / "romfs"))


if __name__ == "__main__":
    unittest.main()
