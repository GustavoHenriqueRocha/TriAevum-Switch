"""Decodes/encodes the menu CTXB textures (PICA200: RGBA8, RGB565, RGBA4, ETC1, ETC1A4)."""
import struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import (
    CTXB_HEADER_SIZE, PICA_FORMATS, decode_etc1a4, decode_rgba4_tiled, encode_rgba4_tiled, _tiled_pixel,
    read_level3_romfs_file)
from PIL import Image

from paths import ROMFS_ORIGINAL as ROM   # always the original
TEXT_TEXTURES = ("hud_all00 hud_menu_title00 icon_item_menu00 menu_equip_parts00 menu_file_select_parts00 "
             "menu_file_select_parts01 menu_hint_movie_parts00 menu_item_parts00 menu_item_parts01 menu_map_parts00 "
             "menu_map_parts01 menu_okarina_parts00 menu_option_parts00 menu_top_parts00 name_entry00_parts00 "
             "name_entry00_parts01").split()


def header(data):
    w, h, f, t = struct.unpack_from('<HHHH', data, 0x2C)
    return w, h, PICA_FORMATS.get((f, t))


def _tiled(payload, w, h, bpp, px):
    out = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = _tiled_pixel(x, y, w)
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = px(payload[i * bpp:(i + 1) * bpp])
    return out


def decode(data):
    w, h, fmt = header(data); p = data[CTXB_HEADER_SIZE:]
    if fmt == 4: rgba = decode_rgba4_tiled(p, w, h)
    elif fmt == 13: rgba = decode_etc1a4(p, w, h)
    elif fmt == 12:   # ETC1 without alpha: same blocks as ETC1A4 without the 8 alpha bytes (opaque)
        rgba = decode_etc1a4(b''.join(b'\xff' * 8 + p[i:i + 8] for i in range(0, w * h // 2, 8)), w, h)
    elif fmt == 0: rgba = _tiled(p, w, h, 4, lambda b: bytes((b[3], b[2], b[1], b[0])))
    elif fmt == 5: rgba = _tiled(p, w, h, 2, lambda b: bytes((b[1], b[1], b[1], b[0])))   # LA8: byte 0 = alpha
    elif fmt == 3:
        def px(b):
            v = b[0] | b[1] << 8
            return bytes((((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
        rgba = _tiled(p, w, h, 2, px)
    elif fmt == 2:
        def px(b):
            v = b[0] | b[1] << 8
            return bytes((((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31,
                          255 if v & 1 else 0))
        rgba = _tiled(p, w, h, 2, px)
    else: raise ValueError(f'format {fmt} not handled')
    return Image.frombytes('RGBA', (w, h), bytes(rgba))


def read(language, name):
    return read_level3_romfs_file(ROM, f'menu/{language}/{name}.ctxb')


if __name__ == '__main__':
    dest = Path(sys.argv[1]); dest.mkdir(parents=True, exist_ok=True)
    for n in TEXT_TEXTURES:
        im = decode(read('06_EU_SPANISH', n)); im.save(dest / f'{n}.png')
        background = Image.new('RGBA', im.size, (40, 40, 60, 255)); background.alpha_composite(im)
        background.convert('RGB').save(dest / f'{n}.ver.png')
        print(n, im.size)


def encode_etc1a4(rgba, w, h):
    """Linear RGBA → PICA ETC1A4 payload (4x4 blocks in 8x8 tiles, little-endian ETC1 word)."""
    import etcpak
    blocks = etcpak.compress_etc1_rgb(bytes(rgba), w, h)          # blocks in row order, 8 bytes each
    bw = w // 4
    out = bytearray()
    for ty in range(0, h, 8):
        for tx in range(0, w, 8):
            for bx, by in ((0, 0), (4, 0), (0, 4), (4, 4)):
                x0, y0 = tx + bx, ty + by
                alpha = 0
                for x in range(4):
                    for y in range(4):
                        a = rgba[((y0 + y) * w + x0 + x) * 4 + 3]
                        alpha |= ((a * 15 + 127) // 255) << (4 * (x * 4 + y))
                i = (y0 // 4) * bw + x0 // 4
                word = int.from_bytes(blocks[i * 8:i * 8 + 8], ETC_BYTE_ORDER)
                out += struct.pack('<QQ', alpha, word)
    return bytes(out)


ETC_BYTE_ORDER = 'big'


def encode_ctxb(original, im):
    """Same texture, same format and size as the original, with the new image."""
    from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import encode_rgba8_tiled
    w, h, fmt = header(original); assert im.size == (w, h)
    rgba = im.convert('RGBA').tobytes()
    if fmt == 4: p = encode_rgba4_tiled(rgba, w, h)
    elif fmt == 13: p = encode_etc1a4(rgba, w, h)
    elif fmt == 0: p = encode_rgba8_tiled(rgba, w, h)
    elif fmt == 5:
        la = im.convert('LA').tobytes(); p = bytearray(w * h * 2)
        for y in range(h):
            for x in range(w):
                i = _tiled_pixel(x, y, w); j = (y * w + x) * 2
                p[i * 2], p[i * 2 + 1] = la[j + 1], la[j]
        p = bytes(p)
    else: raise ValueError(f'encoding of format {fmt} not implemented')
    assert len(p) == len(original) - CTXB_HEADER_SIZE
    return original[:CTXB_HEADER_SIZE] + p
