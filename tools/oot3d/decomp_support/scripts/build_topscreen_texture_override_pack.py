#!/usr/bin/env python3
"""Build the optional TopScreen O3TU v2 texture override pack.

Accepts the official TopScreen 2.1.1 archive (localized CTXB overrides plus
custom_menu00/custom_font00) or the verified TopScreen 3.0 archive
(custom_ui01 relocated into the 2.1.1 profile-texture layout).
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import zipfile
from pathlib import Path


LANGUAGES = (
    "00_JP_JAPANESE", "01_US_ENGLISH", "02_EU_ENGLISH",
    "03_EU_GERMAN", "04_EU_FRENCH", "05_US_FRENCH",
    "06_EU_SPANISH", "07_US_SPANISH", "08_EU_ITALIAN",
)
TEXTURES = ("menu_top_parts00.ctxb", "menu_cursor00.ctxb")
CTXB_HEADER_SIZE = 0x48
PROFILE_TEXTURES = (
    ("custom_menu00.ctxb", "oot3d/topscreen/2.1.1/menu_atlas"),
    ("custom_font00.ctxb", "oot3d/topscreen/2.1.1/font_atlas"),
)

PICA_FORMATS = {
    (0x6752, 0x1401): 0,
    (0x6754, 0x1401): 1,
    (0x6752, 0x8034): 2,
    (0x6754, 0x8363): 3,
    (0x6752, 0x8033): 4,
    (0x6758, 0x1401): 5,
    (0x6757, 0x1401): 7,
    (0x6756, 0x1401): 8,
    (0x6758, 0x6760): 9,
    (0x6757, 0x6761): 10,
    (0x6756, 0x6761): 11,
    (0x675A, 0x0000): 12,
    (0x675B, 0x0000): 13,
}
NCCH_ROMFS_SUPERBLOCK_SIZE = 0x1000
INVALID_ROMFS_OFFSET = 0xFFFFFFFF

# TopScreen / Single Screen Experience 3.0 (GameBanana 695893). 2.1.1 is no
# longer downloadable; 3.0 replaces the localized menu CTXBs and the separate
# custom_menu00/custom_font00 atlases with one 512x256 ETC1A4 atlas,
# romfs/menu/custom_ui01.ctxb (byte-identical for every region and target).
TOPSCREEN_30_ARCHIVE_BYTES = 28_883_512
TOPSCREEN_30_ARCHIVE_SHA256 = (
    "5668640fec8dc2b47b4516fd0b871c4d7d90ca1c2203acc2a22adb59aaab5993")
TOPSCREEN_30_UI_ATLAS_SHA256 = (
    "20290e417c56973cd19daa1f61a6c3963f075d61776db3d996954ee3296d07d8")
TOPSCREEN_30_ROOT = "SingleScreen_3.0"
TOPSCREEN_30_TITLE_IDS = {
    "EUR": "0004000000033600",
    "USA": "0004000000033500",
    "JPN": "0004000000033400",
}
TOPSCREEN_30_TARGET_ROMFS = (
    "Emulator/load/mods/{title_id}/romfs/",
    "3DS Hardware/luma/titles/{title_id}/romfs/",
)
TOPSCREEN_30_UI_ATLAS = "menu/custom_ui01.ctxb"
TOPSCREEN_30_UI_ATLAS_SEMANTIC = "oot3d/topscreen/3.0/ui_atlas"
TOPSCREEN_211_MENU_ATLAS_SEMANTIC = PROFILE_TEXTURES[0][1]
TOPSCREEN_211_FONT_ATLAS_SEMANTIC = PROFILE_TEXTURES[1][1]
TOPSCREEN_211_MENU_ATLAS_SIZE = (512, 512)
# Source rectangles are the 3.0 payload's own float rect table (Emulator
# code.ips, EUR and USA: 0x0060A6F8, 0x0060A708, 0x0060A718). Destinations are
# the 2.1.1 rectangles hard-coded by the native consumers:
# kSelectTemplate/kQuestionTemplate (oot3d_top_screen_items_hint.cpp) and the
# 2.1.1 rect 0x005E1894 cycle mark (oot3d_top_screen_dpad_presentation.cpp).
# Sizes are identical in both versions, so relocation is exact.
TOPSCREEN_30_MENU_ATLAS_REGIONS = (
    ("items_hint_select", (277, 222, 64, 16), (434, 410)),
    ("items_hint_question", (344, 222, 16, 16), (492, 104)),
    ("dpad_cycle_mark", (320, 3, 40, 40), (458, 2)),
)
# One texel of the 3.0 neighbourhood is carried along so bilinear sampling at
# region edges sees what the 3.0 payload itself samples there.
TOPSCREEN_30_REGION_MARGIN = 1
PICA_FORMAT_RGBA8 = 0
PICA_FORMAT_RGBA4 = 4
PICA_FORMAT_ETC1A4 = 13

# 2.1.1 also replaced the localized menu_top_parts00/menu_cursor00 CTXBs and
# painted HUD sprites into texels the original game leaves fully transparent.
# The native TopScreen consumers still sample those 2.1.1 rectangles from the
# native textures, so a 3.0 pack rebuilds the replacements: original payload
# plus the 3.0 sprite at each 2.1.1 rectangle, hash-matched per language.
#
# (label, 3.0 source rect, 2.1.1 sampled rect, placement inside it,
#  quarter_turn). Sources come from the 3.0 custom_ui01 rect tables (Emulator
# code.ips): the D-pad cross is the 3.0 remap 2.1.1 (434,219) -> (385,108)
# (literal pool 0x005E52D4..0x005E5310; rect 0x0060A598); circle 0x0060A568;
# ZR/ZL cells 0x0060A658/0x0060A648; arrow 0x0060A5D8; R/L 0x0060A6B8/A6A8.
# Each source is cropped to whole transparent borders around the opaque
# sprite so nothing is scaled:
# - circle: 3.0 48x48 (opaque 45x45) shifted 1 texel left so columns
#   432..433, shared with the D-pad rectangle, stay transparent;
# - ZR/ZL: opaque 15x9 inside the 3.0 16x16 cell, centred in the 17x11 label;
# - arrow: 3.0 28x26 (opaque 25x24) cropped to 26x26;
# - L/R: 3.0 37x28 (opaque 35x26) cropped to 36x28 and stored a quarter turn
#   as the 28x36 2.1.1 crop (UiPrimitive::uv_quarter_turn).
TOPSCREEN_211_PAUSE_TOP_PAGE = "menu_top_parts00.ctxb"
TOPSCREEN_211_CURSOR = "menu_cursor00.ctxb"
TOPSCREEN_30_PAUSE_TOP_PAGE_REGIONS = (
    # TouchCluster quad 5 (oot3d_top_screen_mod_profile_hud.cpp).
    ("dpad_cross", (385, 108, 30, 30), (432, 218, 30, 30), (0, 0), False),
    # TouchCluster quads 0..4: X/Y/I/II action circles.
    ("action_circle", (98, 3, 46, 48), (386, 201, 48, 48), (0, 0), False),
    # TouchLabels quad 0 (Item I circle) and quad 1 (Item II circle).
    ("item_i_label_zr", (47, 234, 16, 11), (440, 190, 17, 11), (1, 0), False),
    ("item_ii_label_zl", (245, 230, 16, 11), (440, 201, 17, 11), (1, 0),
     False),
    # OcarinaNavigation (right quad; the left quad mirrors it).
    ("ocarina_arrow", (47, 124, 26, 26), (486, 230, 26, 26), (0, 0), False),
)
TOPSCREEN_30_CURSOR_REGIONS = (
    # TabHint glyphs: R at crop X 72, L at crop X 100.
    ("tab_hint_r", (240, 117, 36, 28), (72, 89, 28, 36), (0, 0), True),
    ("tab_hint_l", (462, 108, 36, 28), (100, 89, 28, 36), (0, 0), True),
)
TOPSCREEN_30_NATIVE_TEXTURES = (
    (TOPSCREEN_211_PAUSE_TOP_PAGE, TOPSCREEN_30_PAUSE_TOP_PAGE_REGIONS),
    (TOPSCREEN_211_CURSOR, TOPSCREEN_30_CURSOR_REGIONS),
)
ETC1_MODIFIERS = (
    (2, 8), (5, 17), (9, 29), (13, 42),
    (18, 60), (24, 80), (33, 106), (47, 183),
)


def morton8(x: int, y: int) -> int:
    return ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
            ((x & 4) << 2) | ((y & 4) << 3))


def _etc1_block_rgb(word: int) -> list[tuple[int, int, int]]:
    """Return 16 RGB texels of one ETC1 block indexed by x * 4 + y."""
    differential = (word >> 33) & 1
    flip = (word >> 32) & 1
    tables = ((word >> 37) & 7, (word >> 34) & 7)
    if differential:
        def signed3(value: int) -> int:
            return value - 8 if value & 4 else value

        def expand5(value: int) -> int:
            return (value << 3) | (value >> 2)

        base = ((word >> 59) & 31, (word >> 51) & 31, (word >> 43) & 31)
        delta = (signed3((word >> 56) & 7), signed3((word >> 48) & 7),
                 signed3((word >> 40) & 7))
        colors = (tuple(expand5(c) for c in base),
                  tuple(expand5((c + d) & 31) for c, d in zip(base, delta)))
    else:
        colors = (
            tuple(((word >> shift) & 15) * 17 for shift in (60, 52, 44)),
            tuple(((word >> shift) & 15) * 17 for shift in (56, 48, 40)),
        )
    texels = []
    for x in range(4):
        for y in range(4):
            index = x * 4 + y
            second = (y >= 2) if flip else (x >= 2)
            small, large = ETC1_MODIFIERS[tables[second]]
            modifier = large if (word >> index) & 1 else small
            if (word >> (16 + index)) & 1:
                modifier = -modifier
            texels.append(tuple(
                min(255, max(0, c + modifier)) for c in colors[second]))
    return texels


def decode_etc1a4(payload: bytes, width: int, height: int) -> bytearray:
    """Decode a tiled PICA ETC1A4 payload to linear top-down RGBA8.

    Mirrors Oot3d::Renderer::DecodePicaTextureRgba8 (no vertical flip), so
    pixel coordinates match the UV rectangles used by the native consumers.
    """
    if width % 8 or height % 8:
        raise ValueError("ETC1A4 dimensions must be multiples of 8")
    if len(payload) != width * height:
        raise ValueError("ETC1A4 payload size mismatch")
    rgba = bytearray(width * height * 4)
    offset = 0
    for tile_y in range(0, height, 8):
        for tile_x in range(0, width, 8):
            for block_x, block_y in ((0, 0), (4, 0), (0, 4), (4, 4)):
                alpha, color = struct.unpack_from("<QQ", payload, offset)
                offset += 16
                texels = _etc1_block_rgb(color)
                for x in range(4):
                    for y in range(4):
                        index = x * 4 + y
                        destination = ((tile_y + block_y + y) * width +
                                       tile_x + block_x + x) * 4
                        rgba[destination:destination + 3] = bytes(
                            texels[index])
                        rgba[destination + 3] = (
                            (alpha >> (4 * index)) & 15) * 17
    return rgba


def encode_rgba8_tiled(rgba: bytes, width: int, height: int) -> bytes:
    """Encode linear RGBA8 as the PICA tiled RGBA8 (native format 0)."""
    if width % 8 or height % 8 or len(rgba) != width * height * 4:
        raise ValueError("invalid RGBA8 image")
    output = bytearray(width * height * 4)
    tiles_x = width // 8
    for y in range(height):
        for x in range(width):
            source = (y * width + x) * 4
            pixel = ((y // 8) * tiles_x + x // 8) * 64 + morton8(x % 8, y % 8)
            red, green, blue, alpha = rgba[source:source + 4]
            output[pixel * 4:pixel * 4 + 4] = bytes(
                (alpha, blue, green, red))
    return bytes(output)


def _tiled_pixel(x: int, y: int, width: int) -> int:
    return ((y // 8) * (width // 8) + x // 8) * 64 + morton8(x % 8, y % 8)


def decode_rgba4_tiled(payload: bytes, width: int, height: int) -> bytearray:
    """Decode PICA tiled RGBA4 (native format 4) to linear RGBA8."""
    if width % 8 or height % 8 or len(payload) != width * height * 2:
        raise ValueError("invalid RGBA4 image")
    rgba = bytearray(width * height * 4)
    for y in range(height):
        for x in range(width):
            value = struct.unpack_from(
                "<H", payload, _tiled_pixel(x, y, width) * 2)[0]
            target = (y * width + x) * 4
            rgba[target:target + 4] = bytes(
                ((value >> shift) & 15) * 17 for shift in (12, 8, 4, 0))
    return rgba


def encode_rgba4_tiled(rgba: bytes, width: int, height: int) -> bytes:
    """Encode linear RGBA8 as PICA tiled RGBA4 (rounded to 4 bits)."""
    if width % 8 or height % 8 or len(rgba) != width * height * 4:
        raise ValueError("invalid RGBA8 image")
    output = bytearray(width * height * 2)
    for y in range(height):
        for x in range(width):
            source = (y * width + x) * 4
            red, green, blue, alpha = (
                (channel * 15 + 127) // 255
                for channel in rgba[source:source + 4])
            struct.pack_into("<H", output, _tiled_pixel(x, y, width) * 2,
                             (red << 12) | (green << 8) | (blue << 4) | alpha)
    return bytes(output)


def compose_topscreen_30_native_texture(
        original_payload: bytes, width: int, height: int,
        ui_atlas_rgba: bytes, ui_width: int, ui_height: int,
        regions: tuple, label: str) -> bytes:
    """Paint 3.0 sprites into an original RGBA4 menu texture.

    Every 2.1.1 rectangle must be fully transparent in the original, so native
    draws never sample a changed texel; all other texels are kept bit-exact.
    """
    rgba = decode_rgba4_tiled(original_payload, width, height)
    # 2.1.1 rectangles may overlap (action circle and D-pad share columns
    # 432..433), so every check runs on the original before any paint.
    for name, (sx, sy, sw, sh), (dx, dy, dw, dh), (ox, oy), turn in regions:
        if (dx + dw > width or dy + dh > height or
                sx + sw > ui_width or sy + sh > ui_height):
            raise ValueError(f"TopScreen 3.0 region out of bounds: {name}")
        placed_w, placed_h = (sh, sw) if turn else (sw, sh)
        if ox + placed_w > dw or oy + placed_h > dh:
            raise ValueError(f"TopScreen 3.0 sprite exceeds its rect: {name}")
        if any(rgba[((dy + row) * width + dx + column) * 4 + 3]
               for row in range(dh) for column in range(dw)):
            raise ValueError(
                f"original {label} is not transparent at the 2.1.1 {name} "
                "rectangle")
    for name, (sx, sy, sw, sh), (dx, dy, dw, dh), (ox, oy), turn in regions:
        placed_w, placed_h = (sh, sw) if turn else (sw, sh)
        for row in range(placed_h):
            for column in range(placed_w):
                if turn:
                    # uv_quarter_turn shows stored texel (c, r) at display
                    # (placed_h - 1 - r, c) of the sw x sh sprite.
                    source_x, source_y = sx + placed_h - 1 - row, sy + column
                else:
                    source_x, source_y = sx + column, sy + row
                source = (source_y * ui_width + source_x) * 4
                target = ((dy + oy + row) * width + dx + ox + column) * 4
                rgba[target:target + 4] = ui_atlas_rgba[source:source + 4]
    return encode_rgba4_tiled(rgba, width, height)


def compose_topscreen_30_menu_atlas(ui_atlas_rgba: bytes, width: int,
                                    height: int) -> bytes:
    """Relocate the 3.0 Items-hint/D-pad sprites into the 2.1.1 layout."""
    atlas_width, atlas_height = TOPSCREEN_211_MENU_ATLAS_SIZE
    composed = bytearray(atlas_width * atlas_height * 4)
    margin = TOPSCREEN_30_REGION_MARGIN
    for label, (sx, sy, sw, sh), (dx, dy) in TOPSCREEN_30_MENU_ATLAS_REGIONS:
        if (sx + sw > width or sy + sh > height or
                dx + sw > atlas_width or dy + sh > atlas_height):
            raise ValueError(f"TopScreen 3.0 region out of bounds: {label}")
        for row in range(-margin, sh + margin):
            for column in range(-margin, sw + margin):
                source_x, source_y = sx + column, sy + row
                target_x, target_y = dx + column, dy + row
                if not (0 <= source_x < width and 0 <= source_y < height and
                        0 <= target_x < atlas_width and
                        0 <= target_y < atlas_height):
                    continue
                source = (source_y * width + source_x) * 4
                target = (target_y * atlas_width + target_x) * 4
                composed[target:target + 4] = ui_atlas_rgba[source:source + 4]
    return bytes(composed)


def fnv1a64(data: bytes) -> int:
    value = 0xCBF29CE484222325
    for byte in data:
        value ^= byte
        value = (value * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def read_level3_romfs_file(image: Path, relative_path: str) -> bytes:
    """Read one file from a raw-IVFC or Level-3 service-view RomFS."""
    with image.open("rb") as stream:
        stream.seek(0, 2)
        image_size = stream.tell()
        stream.seek(0)
        prefix = stream.read(4)
        level3_base = NCCH_ROMFS_SUPERBLOCK_SIZE if prefix == b"IVFC" else 0
        if level3_base + 0x28 > image_size:
            raise ValueError("truncated original RomFS image")
        stream.seek(level3_base)
        header_data = stream.read(0x28)
        header = struct.unpack("<10I", header_data)
        if header[0] != 0x28:
            raise ValueError("original RomFS has no Level-3 header")
        directory_metadata_offset = header[3]
        directory_metadata_size = header[4]
        file_metadata_offset = header[7]
        file_metadata_size = header[8]
        file_data_offset = header[9]

        def read_metadata(offset: int, table_offset: int, table_size: int,
                          fixed_size: int) -> bytes:
            if offset >= table_size or fixed_size > table_size - offset:
                raise ValueError("RomFS metadata offset is outside its table")
            stream.seek(level3_base + table_offset + offset)
            data = stream.read(fixed_size)
            if len(data) != fixed_size:
                raise ValueError("truncated RomFS metadata")
            return data

        def read_directory(offset: int) -> tuple[int, int, str]:
            fixed = read_metadata(
                offset, directory_metadata_offset, directory_metadata_size,
                24)
            _, sibling, child_directory, child_file, _, name_size = (
                struct.unpack("<6I", fixed))
            if name_size % 2 != 0 or 24 + name_size > (
                    directory_metadata_size - offset):
                raise ValueError("invalid RomFS directory name")
            name_data = stream.read(name_size)
            if len(name_data) != name_size:
                raise ValueError("truncated RomFS directory name")
            return sibling, child_directory, child_file, name_data.decode(
                "utf-16-le")

        def read_file(offset: int) -> tuple[int, int, int, str]:
            fixed = read_metadata(
                offset, file_metadata_offset, file_metadata_size, 32)
            _, sibling, data_offset, data_size, _, name_size = struct.unpack(
                "<IIQQII", fixed)
            if name_size % 2 != 0 or 32 + name_size > (
                    file_metadata_size - offset):
                raise ValueError("invalid RomFS file name")
            name_data = stream.read(name_size)
            if len(name_data) != name_size:
                raise ValueError("truncated RomFS file name")
            return sibling, data_offset, data_size, name_data.decode(
                "utf-16-le")

        parts = tuple(part for part in relative_path.split("/") if part)
        if not parts:
            raise ValueError("empty RomFS path")
        directory_offset = 0
        for component in parts[:-1]:
            _, child_directory, _, _ = read_directory(directory_offset)
            visited: set[int] = set()
            while child_directory != INVALID_ROMFS_OFFSET:
                if child_directory in visited:
                    raise ValueError("cyclic RomFS directory list")
                visited.add(child_directory)
                sibling, _, _, name = read_directory(child_directory)
                if name == component:
                    directory_offset = child_directory
                    break
                child_directory = sibling
            else:
                raise FileNotFoundError(relative_path)

        _, _, child_file, _ = read_directory(directory_offset)
        visited_files: set[int] = set()
        while child_file != INVALID_ROMFS_OFFSET:
            if child_file in visited_files:
                raise ValueError("cyclic RomFS file list")
            visited_files.add(child_file)
            sibling, data_offset, data_size, name = read_file(child_file)
            if name == parts[-1]:
                absolute_data_offset = level3_base + file_data_offset + data_offset
                if (absolute_data_offset > image_size or
                        data_size > image_size - absolute_data_offset):
                    raise ValueError("RomFS file payload is outside the image")
                stream.seek(absolute_data_offset)
                data = stream.read(data_size)
                if len(data) != data_size:
                    raise ValueError("truncated RomFS file payload")
                return data
            child_file = sibling
    raise FileNotFoundError(relative_path)


def build_texture_pack(*, archive: Path | None = None,
                       mod_romfs: Path | None = None,
                       original_romfs: Path | None = None,
                       original_romfs_image: Path | None = None,
                       region: str | None = None) -> bytes:
    if (archive is None) == (mod_romfs is None):
        raise ValueError("provide exactly one replacement source")
    if (original_romfs is None) == (original_romfs_image is None):
        raise ValueError("provide exactly one original RomFS source")
    if archive is not None and is_topscreen_30_archive(archive):
        verify_topscreen_30_archive(archive)
        if region is None:
            region = detect_romfs_region(
                original_romfs=original_romfs,
                original_romfs_image=original_romfs_image)
        return build_topscreen_30_pack(
            archive=archive, region=region, original_romfs=original_romfs,
            original_romfs_image=original_romfs_image)
    entries_by_original: dict[int, tuple[int, bytes, str]] = {}
    profile_textures: list[tuple[str, int, int, int, bytes]] = []

    def add_entry(original: bytes, replacement: bytes, label: str) -> None:
        if original[:CTXB_HEADER_SIZE] != replacement[:CTXB_HEADER_SIZE]:
            raise ValueError(f"CTXB header mismatch: {label}")
        original_payload = original[CTXB_HEADER_SIZE:]
        replacement_payload = replacement[CTXB_HEADER_SIZE:]
        if len(original_payload) != len(replacement_payload):
            raise ValueError(f"CTXB payload size mismatch: {label}")
        original_hash = fnv1a64(original_payload)
        replacement_hash = fnv1a64(replacement_payload)
        existing = entries_by_original.get(original_hash)
        if existing is not None:
            if (existing[0], existing[1]) != (replacement_hash,
                                               replacement_payload):
                raise ValueError(
                    "one original CTXB payload maps to conflicting "
                    f"replacements: {existing[2]} and {label}")
            return
        entries_by_original[original_hash] = (
            replacement_hash, replacement_payload, label)

    def parse_profile_texture(data: bytes, semantic: str) -> None:
        if len(data) < CTXB_HEADER_SIZE or data[:4] != b"ctxb":
            raise ValueError(f"invalid profile CTXB: {semantic}")
        width, height, raw_format, raw_type = struct.unpack_from(
            "<HHHH", data, 0x2C)
        native_format = PICA_FORMATS.get((raw_format, raw_type))
        if native_format is None:
            raise ValueError(
                f"unsupported profile CTXB format: {semantic}: "
                f"0x{raw_format:04x}/0x{raw_type:04x}")
        payload = data[CTXB_HEADER_SIZE:]
        if width == 0 or height == 0 or not payload:
            raise ValueError(f"empty profile CTXB: {semantic}")
        profile_textures.append(
            (semantic, width, height, native_format, payload))

    if archive is not None:
        archive_context = zipfile.ZipFile(archive)
        names = set(archive_context.namelist())

        def find_archive_romfs_prefix() -> str:
            candidates = (
                "TopScreenMod/romfs/",
                "TopScreen2.1.1/EUR/Emulator/load/mods/"
                "0004000000033600/romfs/",
                "TopScreen2.1.1/EUR/3DS Hardware/luma/titles/"
                "0004000000033600/romfs/",
            )
            for candidate in candidates:
                if candidate + "menu/01_US_ENGLISH/menu_cursor00.ctxb" in names:
                    return candidate
            raise ValueError("archive has no supported EUR TopScreen RomFS")

        archive_romfs_prefix = find_archive_romfs_prefix()
        read_replacement = lambda language, texture: archive_context.read(
            f"{archive_romfs_prefix}menu/{language}/{texture}")
        read_profile_texture = lambda texture: archive_context.read(
            f"{archive_romfs_prefix}menu/{texture}")
    else:
        archive_context = None
        read_replacement = lambda language, texture: (
            mod_romfs / "menu" / language / texture).read_bytes()
        read_profile_texture = lambda texture: (
            mod_romfs / "menu" / texture).read_bytes()
    try:
        for language in LANGUAGES:
            for texture in TEXTURES:
                replacement = read_replacement(language, texture)
                if original_romfs is not None:
                    original = (
                        original_romfs / "menu" / language /
                        texture).read_bytes()
                else:
                    original = read_level3_romfs_file(
                        original_romfs_image,
                        f"menu/{language}/{texture}")
                add_entry(original, replacement, f"{language}/{texture}")
        for texture, semantic in PROFILE_TEXTURES:
            parse_profile_texture(read_profile_texture(texture), semantic)
    finally:
        if archive_context is not None:
            archive_context.close()

    entries = [
        (original_hash, replacement_hash, payload)
        for original_hash, (replacement_hash, payload, _) in
        entries_by_original.items()
    ]
    return serialize_pack(entries, profile_textures)


def serialize_pack(
        entries: list[tuple[int, int, bytes]],
        profile_textures: list[tuple[str, int, int, int, bytes]]) -> bytes:
    """Serialize O3TU v2 exactly as TopScreenTextureOverridePack parses it."""
    output = bytearray(b"O3TU")
    output += struct.pack("<III", 2, len(entries), len(profile_textures))
    for original_hash, replacement_hash, payload in entries:
        output += struct.pack("<QQI", original_hash, replacement_hash, len(payload))
        output += payload
    for semantic, width, height, native_format, payload in profile_textures:
        encoded_semantic = semantic.encode("utf-8")
        output += struct.pack(
            "<IHHB3xIQ", len(encoded_semantic), width, height,
            native_format, len(payload), fnv1a64(payload))
        output += encoded_semantic
        output += payload
    return bytes(output)


def is_topscreen_30_archive(archive: Path) -> bool:
    with zipfile.ZipFile(archive) as source:
        return any(name.startswith(TOPSCREEN_30_ROOT + "/")
                   for name in source.namelist())


def verify_topscreen_30_archive(archive: Path) -> None:
    size = archive.stat().st_size
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    if (size != TOPSCREEN_30_ARCHIVE_BYTES or
            digest.hexdigest() != TOPSCREEN_30_ARCHIVE_SHA256):
        raise ValueError(
            "TopScreen 3.0 archive failed verification: expected "
            f"{TOPSCREEN_30_ARCHIVE_BYTES} bytes, SHA-256 "
            f"{TOPSCREEN_30_ARCHIVE_SHA256}; got {size} bytes, "
            f"{digest.hexdigest()}")


def detect_romfs_region(*, original_romfs: Path | None = None,
                        original_romfs_image: Path | None = None) -> str:
    """Identify EUR/USA from the original RomFS message directories."""
    def exists(relative_path: str) -> bool:
        if original_romfs is not None:
            return (original_romfs / relative_path).is_file()
        try:
            read_level3_romfs_file(original_romfs_image, relative_path)
        except FileNotFoundError:
            return False
        return True

    found = [region for region, path in (("USA", "message/us/ltn16.qbf"),
                                         ("EUR", "message/eu/ltn16.qbf"))
             if exists(path)]
    if len(found) != 1:
        raise ValueError(
            "cannot identify the RomFS region; pass --region EUR, USA or JPN")
    return found[0]


def read_original_menu_ctxb(language: str, texture: str, *,
                            original_romfs: Path | None,
                            original_romfs_image: Path | None) -> bytes:
    relative = f"menu/{language}/{texture}"
    if original_romfs is not None:
        path = original_romfs / relative
        if not path.is_file():
            raise FileNotFoundError(relative)
        return path.read_bytes()
    if original_romfs_image is None:
        raise ValueError("TopScreen 3.0 needs the original RomFS")
    return read_level3_romfs_file(original_romfs_image, relative)


def build_topscreen_30_native_entries(
        ui_rgba: bytes, ui_width: int, ui_height: int, *,
        original_romfs: Path | None,
        original_romfs_image: Path | None) -> list[tuple[int, int, bytes]]:
    """Hash-matched menu_top_parts00/menu_cursor00 replacements for 3.0."""
    entries: dict[int, tuple[int, bytes]] = {}
    for texture, regions in TOPSCREEN_30_NATIVE_TEXTURES:
        found = 0
        for language in LANGUAGES:
            try:
                original = read_original_menu_ctxb(
                    language, texture, original_romfs=original_romfs,
                    original_romfs_image=original_romfs_image)
            except FileNotFoundError:
                continue
            found += 1
            label = f"{language}/{texture}"
            if len(original) < CTXB_HEADER_SIZE or original[:4] != b"ctxb":
                raise ValueError(f"invalid original CTXB: {label}")
            width, height, raw_format, raw_type = struct.unpack_from(
                "<HHHH", original, 0x2C)
            payload = original[CTXB_HEADER_SIZE:]
            if (PICA_FORMATS.get((raw_format, raw_type)) != PICA_FORMAT_RGBA4
                    or len(payload) != width * height * 2):
                raise ValueError(f"original CTXB is not RGBA4: {label}")
            original_hash = fnv1a64(payload)
            if original_hash in entries:
                continue
            replacement = compose_topscreen_30_native_texture(
                payload, width, height, ui_rgba, ui_width, ui_height,
                regions, label)
            entries[original_hash] = (fnv1a64(replacement), replacement)
        if not found:
            raise ValueError(f"original RomFS has no menu/*/{texture}")
    return [(original, replacement_hash, payload)
            for original, (replacement_hash, payload) in entries.items()]


def build_topscreen_30_pack(*, archive: Path, region: str,
                            original_romfs: Path | None = None,
                            original_romfs_image: Path | None = None) -> bytes:
    """Build an O3TU v2 pack from the TopScreen 3.0 archive.

    3.0 ships no localized CTXB replacements; the hash-matched entries are
    rebuilt from the original RomFS: each distinct localized
    menu_top_parts00/menu_cursor00 payload with the 3.0 sprites painted at the
    2.1.1 rectangles the native consumers sample (D-pad cross, action circles,
    ZR/ZL labels, Ocarina arrows, L/R tab hints). Profile textures:
    - 2.1.1 ``menu_atlas``: 512x512 RGBA8 in the 2.1.1 layout, holding the
      exact 3.0 Items-hint and D-pad cycle sprites at the 2.1.1 coordinates the
      native consumers sample (unchanged C++).
    - 2.1.1 ``font_atlas``: custom_ui01 itself; 3.0 merged the custom_font00
      glyphs into it. The runtime only requires this semantic to exist.
    - ``oot3d/topscreen/3.0/ui_atlas``: custom_ui01 verbatim (512x256 ETC1A4).
    Executable patches, the Link model, stage_grid.bin and QBF fonts are never
    read.
    """
    title_id = TOPSCREEN_30_TITLE_IDS.get(region)
    if title_id is None:
        raise ValueError(f"unsupported TopScreen 3.0 region: {region}")
    with zipfile.ZipFile(archive) as source:
        names = set(source.namelist())
        data = None
        for target in TOPSCREEN_30_TARGET_ROMFS:
            name = (f"{TOPSCREEN_30_ROOT}/{region}/"
                    f"{target.format(title_id=title_id)}{TOPSCREEN_30_UI_ATLAS}")
            if name in names:
                data = source.read(name)
                break
    if data is None:
        raise ValueError(f"archive has no TopScreen 3.0 {region} UI atlas")
    if hashlib.sha256(data).hexdigest() != TOPSCREEN_30_UI_ATLAS_SHA256:
        raise ValueError("unexpected TopScreen 3.0 custom_ui01.ctxb")
    if len(data) < CTXB_HEADER_SIZE or data[:4] != b"ctxb":
        raise ValueError("invalid TopScreen 3.0 custom_ui01.ctxb")
    width, height, raw_format, raw_type = struct.unpack_from(
        "<HHHH", data, 0x2C)
    if PICA_FORMATS.get((raw_format, raw_type)) != PICA_FORMAT_ETC1A4:
        raise ValueError("TopScreen 3.0 UI atlas is not ETC1A4")
    payload = data[CTXB_HEADER_SIZE:]
    ui_rgba = decode_etc1a4(payload, width, height)
    menu_width, menu_height = TOPSCREEN_211_MENU_ATLAS_SIZE
    menu_payload = encode_rgba8_tiled(
        compose_topscreen_30_menu_atlas(ui_rgba, width, height),
        menu_width, menu_height)
    entries = build_topscreen_30_native_entries(
        ui_rgba, width, height, original_romfs=original_romfs,
        original_romfs_image=original_romfs_image)
    return serialize_pack(entries, [
        (TOPSCREEN_211_MENU_ATLAS_SEMANTIC, menu_width, menu_height,
         PICA_FORMAT_RGBA8, menu_payload),
        (TOPSCREEN_211_FONT_ATLAS_SEMANTIC, width, height,
         PICA_FORMAT_ETC1A4, payload),
        (TOPSCREEN_30_UI_ATLAS_SEMANTIC, width, height,
         PICA_FORMAT_ETC1A4, payload),
    ])


def main() -> int:
    parser = argparse.ArgumentParser()
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--archive", type=Path)
    source.add_argument("--mod-romfs", type=Path)
    originals = parser.add_mutually_exclusive_group(required=True)
    originals.add_argument("--original-romfs", type=Path)
    originals.add_argument("--original-romfs-image", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--region", choices=sorted(TOPSCREEN_30_TITLE_IDS),
        help="TopScreen 3.0 region variant (default: detected from RomFS)")
    args = parser.parse_args()
    output = build_texture_pack(
        archive=args.archive, mod_romfs=args.mod_romfs,
        original_romfs=args.original_romfs,
        original_romfs_image=args.original_romfs_image,
        region=args.region)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    print(f"wrote {len(output)} bytes: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
