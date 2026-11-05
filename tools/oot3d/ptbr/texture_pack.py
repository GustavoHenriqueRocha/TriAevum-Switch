"""High-resolution texture pack (Azahar format: tex1_WxH_HASH_fmt_mip0.png) for the menus, maps and area titles.

    python -I texture_pack.py export            # native textures from the PT-BR romfs -> WORK_DIR/pack/native
    (upscale native/*.png 4x, e.g. realesrgan-ncnn-vulkan, into WORK_DIR/pack/upscaled, same name)
    python -I texture_pack.py assemble [factor]    # -> WORK_DIR/pack/textures (factor 2 by default)

Item icons go separately, in WORK_DIR/pack/textures_items, to compare with and without: the runtime's
scan is recursive, so just move the folder into textures/ (items/) to enable them.

The name carries the hash of the native bytes (CityHash64, tool texture_hash.cpp), so the pack only applies to
this romfs: rebuild after changing menus or titles.
"""
import json, struct, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROMFS_PTBR, WORK_DIR
from ctxb import CTXB_HEADER_SIZE, header, decode
from romfs import list_files, locate
from titles import zar_entries

PACK = WORK_DIR / 'pack'
HASH = PACK / 'texture_hash'
LANGUAGES = ('06_EU_SPANISH', '07_US_SPANISH')


def _hash(data):
    tmp_file = PACK / 'tmp.bin'; tmp_file.write_bytes(data)
    return subprocess.run([str(HASH), str(tmp_file)], check=True, capture_output=True, text=True).stdout.strip()


def _read(f, o, t):
    f.seek(o); return f.read(t)


def textures():
    """(readable name, ctxb) of the Spanish menus (where the PT-BR is) and of the eusp/ussp area titles."""
    with open(ROMFS_PTBR, 'rb') as f:
        for entry_path in list_files(ROMFS_PTBR):
            parts = entry_path.split('/')
            if len(parts) == 3 and parts[0] == 'menu' and parts[1] in LANGUAGES and entry_path.endswith('.ctxb'):
                yield f'{parts[1][:2]}_{parts[2][:-5]}', _read(f, *locate(ROMFS_PTBR, entry_path))
            elif entry_path.startswith('menu/map/') and entry_path.endswith('.ctxb'):
                yield 'map_' + entry_path[9:-5].replace('/', '_'), _read(f, *locate(ROMFS_PTBR, entry_path))
            elif entry_path.startswith('scene/') and entry_path.endswith('.zar'):
                z = _read(f, *locate(ROMFS_PTBR, entry_path))
                for name, oz, tz in zar_entries(z):
                    base = name.split('\\')[-1][:-5]
                    if base.endswith(('_eusp', '_ussp')):
                        yield f'title_{base}', z[oz:oz + tz]


def folder(name):
    """Destination folder in the pack: item icons are kept separate (optional)."""
    return 'textures_items' if 'icon_item' in name else 'textures'


def export():
    if not HASH.exists():
        raise SystemExit(f'compile first: g++ -std=c++20 -O2 -I runtime/three_ds_recomp/include '
                         f'tools/oot3d/ptbr/texture_hash.cpp -o {HASH}')
    (PACK / 'native').mkdir(parents=True, exist_ok=True)
    mapping = {}
    for name, ctxb in textures():
        w, h, fmt = header(ctxb)
        bits = {0: 32, 1: 24, 2: 16, 3: 16, 4: 16, 5: 16, 6: 16, 7: 8, 8: 8, 9: 8, 10: 4, 11: 4, 12: 4, 13: 8}[fmt]
        base = ctxb[CTXB_HEADER_SIZE:CTXB_HEADER_SIZE + w * h * bits // 8]
        file_name = f'tex1_{w}x{h}_{_hash(base)}_{fmt}_mip0.png'
        if file_name in mapping.values():
            continue                                   # same texture in another language/scene
        decode(ctxb).save(PACK / 'native' / f'{name}.png')
        mapping[name] = file_name
    (PACK / 'tmp.bin').unlink(missing_ok=True)
    (PACK / 'map.json').write_text(json.dumps(mapping, indent=1))
    print(len(mapping), 'textures in', PACK / 'native')


def assemble(factor=2):
    from PIL import Image
    mapping = json.loads((PACK / 'map.json').read_text())
    for name, file_name in mapping.items():
        native = Image.open(PACK / 'native' / f'{name}.png').convert('RGBA')
        large = Image.open(PACK / 'upscaled' / f'{name}.png').convert('RGBA')
        target = (native.width * factor, native.height * factor)
        im = large.resize(target, Image.LANCZOS) if large.size != target else large
        # Alpha from the upscaled native: the upscaler blurs transparent edges of labels and icons.
        im.putalpha(native.getchannel('A').resize(target, Image.BICUBIC))
        dest = PACK / folder(name); dest.mkdir(exist_ok=True)
        im.save(dest / file_name, optimize=True)
    print(len(mapping), 'textures in', PACK / 'textures', 'and', PACK / 'textures_items')


if __name__ == '__main__':
    action = sys.argv[1] if len(sys.argv) > 1 else ''
    if action == 'export': export()
    elif action == 'assemble': assemble(int(sys.argv[2]) if len(sys.argv) > 2 else 2)
    else: raise SystemExit(__doc__)
