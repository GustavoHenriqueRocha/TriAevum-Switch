"""TopScreen 3.0 import into the existing O3TU v2 profile-texture contract."""

import hashlib
import os
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools.oot3d.decomp_support.scripts import (
    build_topscreen_texture_override_pack as builder,
)


def fnv1a64(data: bytes) -> int:
    return builder.fnv1a64(data)


def parse_o3tu(pack: bytes) -> dict:
    """Python mirror of TopScreenTextureOverridePack::LoadBytes."""
    magic, version, count, profile_count = struct.unpack_from("<4sIII", pack)
    assert magic == b"O3TU" and version == 2
    cursor = 16
    entries = []
    for _ in range(count):
        original, replacement, size = struct.unpack_from("<QQI", pack, cursor)
        cursor += 20
        payload = pack[cursor:cursor + size]
        assert fnv1a64(payload) == replacement
        entries.append((original, payload))
        cursor += size
    textures = {}
    for _ in range(profile_count):
        name_size, width, height, native_format, size, digest = (
            struct.unpack_from("<IHHB3xIQ", pack, cursor))
        cursor += 24
        name = pack[cursor:cursor + name_size].decode("utf-8")
        cursor += name_size
        payload = pack[cursor:cursor + size]
        assert fnv1a64(payload) == digest and name not in textures
        textures[name] = (width, height, native_format, payload)
        cursor += size
    assert cursor == len(pack)
    return {"entries": entries, "textures": textures}


def detile_rgba8(payload: bytes, width: int, height: int) -> bytes:
    """Python mirror of the C++ format-0 decode (ABGR bytes, Morton tiles)."""
    output = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            pixel = ((y // 8) * (width // 8) + x // 8) * 64 + builder.morton8(
                x % 8, y % 8)
            alpha, blue, green, red = payload[pixel * 4:pixel * 4 + 4]
            output[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes(
                (red, green, blue, alpha))
    return bytes(output)


def solid_etc1a4_block(alpha: int) -> bytes:
    # Individual mode, both sub-blocks colour 0x8/0x4/0x2, table 0, all
    # indices 0 => +2 modifier: RGB (138, 70, 36).
    color = (0x8 << 60) | (0x8 << 56) | (0x4 << 52) | (0x4 << 48) | (
        0x2 << 44) | (0x2 << 40)
    nibbles = 0
    for index in range(16):
        nibbles |= (alpha & 15) << (4 * index)
    return struct.pack("<QQ", nibbles, color)


def ui_atlas_fixture(width: int = 512, height: int = 256) -> bytes:
    """Opaque only inside 8x8 tiles touched by the 3.0 source regions."""
    opaque_tiles = set()
    sources = [region[1] for region in builder.TOPSCREEN_30_MENU_ATLAS_REGIONS]
    for _, regions in builder.TOPSCREEN_30_NATIVE_TEXTURES:
        sources += [region[1] for region in regions]
    for x, y, w, h in sources:
        for tile_y in range(y // 8, (y + h - 1) // 8 + 1):
            for tile_x in range(x // 8, (x + w - 1) // 8 + 1):
                opaque_tiles.add((tile_x, tile_y))
    payload = bytearray()
    for tile_y in range(height // 8):
        for tile_x in range(width // 8):
            block = solid_etc1a4_block(15 if (tile_x, tile_y) in opaque_tiles
                                       else 0)
            payload += block * 4
    header = bytearray(builder.CTXB_HEADER_SIZE)
    header[:4] = b"ctxb"
    struct.pack_into("<HHHH", header, 0x2C, width, height, 0x675B, 0x0000)
    return bytes(header + payload)


def original_menu_ctxb(width: int, height: int) -> bytes:
    """Original-style RGBA4 menu CTXB: transparent except texel (0, 0)."""
    header = bytearray(builder.CTXB_HEADER_SIZE)
    header[:4] = b"ctxb"
    struct.pack_into("<HHHH", header, 0x2C, width, height, 0x6752, 0x8033)
    payload = bytearray(width * height * 2)
    struct.pack_into("<H", payload, 0, 0x123F)  # pixel (0,0) is tiled texel 0
    return bytes(header + payload)


def write_original_menus(romfs: Path, languages=builder.LANGUAGES) -> None:
    for language in languages:
        directory = romfs / "menu" / language
        directory.mkdir(parents=True, exist_ok=True)
        (directory / builder.TOPSCREEN_211_PAUSE_TOP_PAGE).write_bytes(
            original_menu_ctxb(512, 256))
        (directory / builder.TOPSCREEN_211_CURSOR).write_bytes(
            original_menu_ctxb(128, 128))


def quarter_turn_display(stored: bytes, width: int, rect, sprite_size):
    """Mirror of BuildQuad with uv_quarter_turn (oot3d_n64_ui_renderer.cpp).

    Destination (dx, dy) of the sprite_size quad samples the rect texel
    (u0 + dy, v1 - dx): top-left (u0,v1), bottom-left (u1,v1), top-right
    (u0,v0).
    """
    x, y, w, h = rect
    sprite_w, sprite_h = sprite_size
    assert (w, h) == (sprite_h, sprite_w)
    return [[stored[((y + h - 1 - dx) * width + x + dy) * 4:
                    ((y + h - 1 - dx) * width + x + dy) * 4 + 4]
             for dx in range(sprite_w)] for dy in range(sprite_h)]


class TopScreen30CodecTests(unittest.TestCase):
    def test_rgba4_round_trip_is_bit_exact(self):
        payload = bytes(range(256)) * (16 * 16 * 2 // 256)
        rgba = builder.decode_rgba4_tiled(payload, 16, 16)
        self.assertEqual(builder.encode_rgba4_tiled(rgba, 16, 16), payload)
        # Little-endian RGBA4: 0x123F at texel 0 is R=1 G=2 B=3 A=15.
        single = bytearray(8 * 8 * 2)
        struct.pack_into("<H", single, 0, 0x123F)
        self.assertEqual(builder.decode_rgba4_tiled(bytes(single), 8, 8)[:4],
                         bytes((17, 34, 51, 255)))

    def test_quarter_turn_matches_renderer_corners(self):
        sprite = bytearray(4 * 3 * 4)
        for index in range(12):
            sprite[index * 4:index * 4 + 4] = bytes((index, 0, 0, 255))
        stored = builder.compose_topscreen_30_native_texture(
            bytes(8 * 8 * 2), 8, 8, bytes(sprite), 4, 3,
            (("glyph", (0, 0, 4, 3), (1, 2, 3, 4), (0, 0), True),), "test")
        display = quarter_turn_display(
            builder.decode_rgba4_tiled(stored, 8, 8), 8, (1, 2, 3, 4), (4, 3))
        expected = [[builder.decode_rgba4_tiled(
            builder.encode_rgba4_tiled(bytes(sprite[(dy * 4 + dx) * 4:
                                                    (dy * 4 + dx) * 4 + 4]) *
                                       64, 8, 8), 8, 8)[:4]
                     for dx in range(4)] for dy in range(3)]
        self.assertEqual([[bytes(t) for t in row] for row in display],
                         [[bytes(t) for t in row] for row in expected])

    def test_native_texture_rejects_non_transparent_211_rect(self):
        payload = bytearray(8 * 8 * 2)
        struct.pack_into("<H", payload, builder._tiled_pixel(5, 5, 8) * 2,
                         0x0001)
        with self.assertRaisesRegex(ValueError, "not transparent"):
            builder.compose_topscreen_30_native_texture(
                bytes(payload), 8, 8, bytes(4 * 4 * 4), 4, 4,
                (("x", (0, 0, 2, 2), (4, 4, 2, 2), (0, 0), False),), "test")

class TopScreen30CodecTests(unittest.TestCase):
    def test_etc1a4_differential_flip_block(self):
        # Differential mode, flip, base (16,8,4) delta (+1,-1,0), tables 1/2.
        color = ((16 << 59) | (1 << 56) | (8 << 51) | (7 << 48) | (4 << 43) |
                 (1 << 37) | (2 << 34) | (1 << 33) | (1 << 32))
        color |= 1 << 0           # texel (0,0): lsb -> large (+17)
        color |= 1 << (16 + 3)    # texel (0,3): msb -> -small (-9)
        payload = struct.pack("<QQ", 0xFEDCBA9876543210, color) * 4
        rgba = builder.decode_etc1a4(payload, 8, 8)

        def texel(x, y):
            return tuple(rgba[(y * 8 + x) * 4:(y * 8 + x) * 4 + 4])

        expand = lambda value: (value << 3) | (value >> 2)
        first = (expand(16), expand(8), expand(4))
        second = (expand(17), expand(7), expand(4))
        self.assertEqual(texel(0, 0), tuple(c + 17 for c in first) + (0,))
        self.assertEqual(texel(1, 0), tuple(c + 5 for c in first) + (0x44,))
        self.assertEqual(texel(0, 3), tuple(c - 9 for c in second) + (0x33,))

    def test_rgba8_tiling_matches_native_decoder_layout(self):
        image = bytes(range(256)) * (16 * 16 * 4 // 256)
        encoded = builder.encode_rgba8_tiled(image, 16, 16)
        self.assertEqual(detile_rgba8(encoded, 16, 16), image)
        # Pixel (1, 0) is texel 1 and (0, 1) texel 2 of the first tile.
        self.assertEqual(encoded[4:8], bytes(reversed(image[4:8])))
        self.assertEqual(encoded[8:12], bytes(reversed(image[64:68])))


class TopScreen30PackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.atlas = ui_atlas_fixture()
        self.romfs = self.root / "romfs"
        (self.romfs / "message/us").mkdir(parents=True)
        (self.romfs / "message/us/ltn16.qbf").write_bytes(b"QBF1")
        write_original_menus(self.romfs)

    def write_archive(self, region="USA", target="Emulator/load/mods"):
        title = builder.TOPSCREEN_30_TITLE_IDS[region]
        base = f"SingleScreen_3.0/{region}/{target}/{title}/"
        archive = self.root / f"singlescreen_30_{region}.zip"
        with zipfile.ZipFile(archive, "w") as output:
            output.writestr(base + "romfs/menu/custom_ui01.ctxb", self.atlas)
            output.writestr(base + "exefs/code.ips", b"PATCH DO NOT IMPORT")
            output.writestr(base + "romfs/stage_grid.bin", b"NOT A TEXTURE")
        return archive

    def contract(self, archive):
        data = archive.read_bytes()
        return patch.multiple(
            builder,
            TOPSCREEN_30_ARCHIVE_BYTES=len(data),
            TOPSCREEN_30_ARCHIVE_SHA256=hashlib.sha256(data).hexdigest(),
            TOPSCREEN_30_UI_ATLAS_SHA256=hashlib.sha256(self.atlas).hexdigest())

    def test_builds_v2_pack_with_211_layout_and_30_atlas(self):
        archive = self.write_archive()
        with self.contract(archive):
            pack = builder.build_texture_pack(
                archive=archive, original_romfs=self.romfs)
        self.assertEqual(pack[4:8], bytes.fromhex("02000000"))
        self.assertNotIn(b"DO NOT IMPORT", pack)
        self.assertNotIn(b"NOT A TEXTURE", pack)
        parsed = parse_o3tu(pack)
        self.assertEqual(len(parsed["entries"]), 2)
        textures = parsed["textures"]
        self.assertEqual(set(textures), {
            "oot3d/topscreen/2.1.1/menu_atlas",
            "oot3d/topscreen/2.1.1/font_atlas",
            "oot3d/topscreen/3.0/ui_atlas"})
        raw = self.atlas[builder.CTXB_HEADER_SIZE:]
        self.assertEqual(textures["oot3d/topscreen/3.0/ui_atlas"],
                         (512, 256, 13, raw))
        self.assertEqual(textures["oot3d/topscreen/2.1.1/font_atlas"],
                         (512, 256, 13, raw))
        width, height, native_format, payload = textures[
            "oot3d/topscreen/2.1.1/menu_atlas"]
        self.assertEqual((width, height, native_format), (512, 512, 0))
        menu = detile_rgba8(payload, 512, 512)
        alpha = lambda x, y: menu[(y * 512 + x) * 4 + 3]
        # Exact 2.1.1 rectangles sampled by the unchanged C++ consumers.
        for x, y, w, h in ((434, 410, 64, 16), (492, 104, 16, 16),
                           (458, 2, 40, 40)):
            self.assertTrue(all(alpha(x + dx, y + dy) == 255
                                for dx in range(w) for dy in range(h)))
            self.assertEqual(menu[(y * 512 + x) * 4:(y * 512 + x) * 4 + 3],
                             bytes((138, 70, 36)))
        self.assertEqual(alpha(0, 0), 0)
        self.assertEqual(alpha(300, 300), 0)
        self.assertEqual(alpha(434 - 2, 410), 0)  # outside the 1-texel margin

    def test_native_menu_entries_paint_211_rectangles(self):
        archive = self.write_archive()
        with self.contract(archive):
            pack = builder.build_texture_pack(
                archive=archive, original_romfs=self.romfs)
        entries = dict(parse_o3tu(pack)["entries"])
        for width, height, regions in (
                (512, 256, builder.TOPSCREEN_30_PAUSE_TOP_PAGE_REGIONS),
                (128, 128, builder.TOPSCREEN_30_CURSOR_REGIONS)):
            original = original_menu_ctxb(width, height)
            payload = entries[fnv1a64(original[builder.CTXB_HEADER_SIZE:])]
            self.assertEqual(len(payload), width * height * 2)
            rgba = builder.decode_rgba4_tiled(payload, width, height)
            alpha = lambda x, y: rgba[(y * width + x) * 4 + 3]
            # Original texels outside the 2.1.1 rectangles are kept.
            self.assertEqual(rgba[:4], bytes((17, 34, 51, 255)))
            painted = set()
            for _, (_, _, sw, sh), (dx, dy, _, _), (ox, oy), turn in regions:
                w, h = (sh, sw) if turn else (sw, sh)
                painted |= {(dx + ox + c, dy + oy + r)
                            for c in range(w) for r in range(h)}
            for y in range(height):
                for x in range(width):
                    if (x, y) != (0, 0):
                        self.assertEqual(alpha(x, y),
                                         255 if (x, y) in painted else 0)
        top = builder.decode_rgba4_tiled(entries[fnv1a64(original_menu_ctxb(
            512, 256)[builder.CTXB_HEADER_SIZE:])], 512, 256)
        top_alpha = lambda x, y: top[(y * 512 + x) * 4 + 3]
        # D-pad cross: the full 30x30 rect sampled by TouchCluster quad 5.
        self.assertTrue(all(top_alpha(432 + c, 218 + r) == 255
                            for c in range(30) for r in range(30)))
        # Circle shifted one texel left; 432..433 above the cross stay clear.
        self.assertEqual(top_alpha(431, 201), 255)
        self.assertEqual(top_alpha(432, 201), 0)
        # ZR/ZL labels centred in their 17x11 rects.
        self.assertEqual((top_alpha(440, 190), top_alpha(441, 190)), (0, 255))
        self.assertEqual((top_alpha(440, 201), top_alpha(456, 211)), (0, 255))

    def test_original_romfs_without_menu_textures_is_rejected(self):
        archive = self.write_archive()
        bare = self.root / "bare"
        (bare / "message/us").mkdir(parents=True)
        (bare / "message/us/ltn16.qbf").write_bytes(b"QBF1")
        with self.contract(archive):
            with self.assertRaisesRegex(ValueError, "menu_top_parts00"):
                builder.build_texture_pack(archive=archive,
                                           original_romfs=bare)

    def test_region_follows_romfs_and_hardware_target_is_accepted(self):
        archive = self.write_archive("EUR", "3DS Hardware/luma/titles")
        eur = self.root / "eur"
        (eur / "message/eu").mkdir(parents=True)
        (eur / "message/eu/ltn16.qbf").write_bytes(b"QBF1")
        write_original_menus(eur, builder.LANGUAGES[2:4])
        with self.contract(archive):
            self.assertEqual(builder.detect_romfs_region(original_romfs=eur),
                             "EUR")
            builder.build_texture_pack(archive=archive, original_romfs=eur)
            with self.assertRaisesRegex(ValueError, "USA UI atlas"):
                builder.build_texture_pack(
                    archive=archive, original_romfs=self.romfs)

    def test_unverified_archive_or_atlas_is_rejected(self):
        archive = self.write_archive()
        with self.assertRaisesRegex(ValueError, "failed verification"):
            builder.build_texture_pack(archive=archive,
                                       original_romfs=self.romfs)
        data = archive.read_bytes()
        with patch.multiple(
                builder, TOPSCREEN_30_ARCHIVE_BYTES=len(data),
                TOPSCREEN_30_ARCHIVE_SHA256=hashlib.sha256(data).hexdigest()):
            with self.assertRaisesRegex(ValueError, "unexpected TopScreen 3.0"):
                builder.build_texture_pack(archive=archive,
                                           original_romfs=self.romfs)

    def test_ambiguous_romfs_region_requires_explicit_region(self):
        archive = self.write_archive()
        (self.romfs / "message/eu").mkdir(parents=True)
        (self.romfs / "message/eu/ltn16.qbf").write_bytes(b"QBF1")
        with self.contract(archive):
            with self.assertRaisesRegex(ValueError, "--region"):
                builder.build_texture_pack(archive=archive,
                                           original_romfs=self.romfs)
            builder.build_texture_pack(archive=archive,
                                       original_romfs=self.romfs,
                                       region="USA")


@unittest.skipUnless(
    os.environ.get("OOT3D_TOPSCREEN_30_ARCHIVE") and
    os.environ.get("OOT3D_ORIGINAL_ROMFS_IMAGE"),
    "set OOT3D_TOPSCREEN_30_ARCHIVE and OOT3D_ORIGINAL_ROMFS_IMAGE")
class TopScreen30RealArchiveTests(unittest.TestCase):
    def test_real_archive_sprites_land_on_211_rectangles(self):
        archive = Path(os.environ["OOT3D_TOPSCREEN_30_ARCHIVE"])
        romfs = Path(os.environ["OOT3D_ORIGINAL_ROMFS_IMAGE"])
        pack = builder.build_texture_pack(archive=archive,
                                          original_romfs_image=romfs)
        parsed = parse_o3tu(pack)
        textures = parsed["textures"]
        _, _, _, payload = textures["oot3d/topscreen/2.1.1/menu_atlas"]
        menu = detile_rgba8(payload, 512, 512)
        for x, y, w, h in ((434, 410, 64, 16), (492, 104, 16, 16),
                           (458, 2, 40, 40)):
            opaque = sum(menu[((y + dy) * 512 + x + dx) * 4 + 3] > 128
                         for dx in range(w) for dy in range(h))
            self.assertGreater(opaque, w * h // 5)
        # 2.1.1 also had eight unique localized menu replacements.
        entries = dict(parsed["entries"])
        self.assertEqual(len(entries), 8)
        original = builder.read_level3_romfs_file(
            romfs, "menu/01_US_ENGLISH/menu_top_parts00.ctxb")
        top = builder.decode_rgba4_tiled(
            entries[fnv1a64(original[builder.CTXB_HEADER_SIZE:])], 512, 256)
        for x, y, w, h in ((432, 218, 30, 30), (386, 201, 48, 48),
                           (440, 190, 17, 11), (440, 201, 17, 11),
                           (486, 230, 26, 26)):
            opaque = sum(top[((y + dy) * 512 + x + dx) * 4 + 3] > 128
                         for dx in range(w) for dy in range(h))
            self.assertGreater(opaque, w * h // 5)


if __name__ == "__main__":
    unittest.main()
