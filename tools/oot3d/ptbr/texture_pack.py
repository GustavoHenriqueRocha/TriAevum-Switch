"""High-resolution texture pack (Azahar format: tex1_WxH_HASH_fmt_mip0.png) for the menus, maps and area titles.

    python -I texture_pack.py exportar          # native textures from the PT-BR romfs -> TRABALHO/pack/nativas
    (upscale nativas/*.png 4x, e.g. realesrgan-ncnn-vulkan, into TRABALHO/pack/ampliadas, same name)
    python -I texture_pack.py montar [fator]    # -> TRABALHO/pack/textures (fator 2 by default)

Item icons go separately, in TRABALHO/pack/textures_itens, to compare with and without: the runtime's
scan is recursive, so just move the folder into textures/ (itens/) to enable them.

The name carries the hash of the native bytes (CityHash64, tool texture_hash.cpp), so the pack only applies to
this romfs: rebuild after changing menus or titles.
"""
import json, struct, subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROMFS_PTBR, TRABALHO
from ctxb import CTXB_HEADER_SIZE, cabecalho, decodificar
from romfs import listar, localizar
from titles import entradas_zar

PACK = TRABALHO / 'pack'
HASH = PACK / 'hash_textura'
IDIOMAS = ('06_EU_SPANISH', '07_US_SPANISH')


def _hash(dados):
    arq = PACK / 'tmp.bin'; arq.write_bytes(dados)
    return subprocess.run([str(HASH), str(arq)], check=True, capture_output=True, text=True).stdout.strip()


def _ler(f, o, t):
    f.seek(o); return f.read(t)


def texturas():
    """(readable name, ctxb) of the Spanish menus (where the PT-BR is) and of the eusp/ussp area titles."""
    with open(ROMFS_PTBR, 'rb') as f:
        for cam in listar(ROMFS_PTBR):
            partes = cam.split('/')
            if len(partes) == 3 and partes[0] == 'menu' and partes[1] in IDIOMAS and cam.endswith('.ctxb'):
                yield f'{partes[1][:2]}_{partes[2][:-5]}', _ler(f, *localizar(ROMFS_PTBR, cam))
            elif cam.startswith('menu/map/') and cam.endswith('.ctxb'):
                yield 'mapa_' + cam[9:-5].replace('/', '_'), _ler(f, *localizar(ROMFS_PTBR, cam))
            elif cam.startswith('scene/') and cam.endswith('.zar'):
                z = _ler(f, *localizar(ROMFS_PTBR, cam))
                for nome, oz, tz in entradas_zar(z):
                    base = nome.split('\\')[-1][:-5]
                    if base.endswith(('_eusp', '_ussp')):
                        yield f'titulo_{base}', z[oz:oz + tz]


def pasta(nome):
    """Destination folder in the pack: item icons are kept separate (optional)."""
    return 'textures_itens' if 'icon_item' in nome else 'textures'


def exportar():
    if not HASH.exists():
        raise SystemExit(f'compile first: g++ -std=c++20 -O2 -I runtime/three_ds_recomp/include '
                         f'tools/oot3d/ptbr/texture_hash.cpp -o {HASH}')
    (PACK / 'nativas').mkdir(parents=True, exist_ok=True)
    mapa = {}
    for nome, ctxb in texturas():
        w, h, fmt = cabecalho(ctxb)
        bits = {0: 32, 1: 24, 2: 16, 3: 16, 4: 16, 5: 16, 6: 16, 7: 8, 8: 8, 9: 8, 10: 4, 11: 4, 12: 4, 13: 8}[fmt]
        base = ctxb[CTXB_HEADER_SIZE:CTXB_HEADER_SIZE + w * h * bits // 8]
        arquivo = f'tex1_{w}x{h}_{_hash(base)}_{fmt}_mip0.png'
        if arquivo in mapa.values():
            continue                                   # same texture in another language/scene
        decodificar(ctxb).save(PACK / 'nativas' / f'{nome}.png')
        mapa[nome] = arquivo
    (PACK / 'tmp.bin').unlink(missing_ok=True)
    (PACK / 'mapa.json').write_text(json.dumps(mapa, indent=1))
    print(len(mapa), 'textures in', PACK / 'nativas')


def montar(fator=2):
    from PIL import Image
    mapa = json.loads((PACK / 'mapa.json').read_text())
    for nome, arquivo in mapa.items():
        nativa = Image.open(PACK / 'nativas' / f'{nome}.png').convert('RGBA')
        grande = Image.open(PACK / 'ampliadas' / f'{nome}.png').convert('RGBA')
        alvo = (nativa.width * fator, nativa.height * fator)
        im = grande.resize(alvo, Image.LANCZOS) if grande.size != alvo else grande
        # Alpha from the upscaled native: the upscaler blurs transparent edges of labels and icons.
        im.putalpha(nativa.getchannel('A').resize(alvo, Image.BICUBIC))
        destino = PACK / pasta(nome); destino.mkdir(exist_ok=True)
        im.save(destino / arquivo, optimize=True)
    print(len(mapa), 'textures in', PACK / 'textures', 'and', PACK / 'textures_itens')


if __name__ == '__main__':
    acao = sys.argv[1] if len(sys.argv) > 1 else ''
    if acao == 'exportar': exportar()
    elif acao == 'montar': montar(int(sys.argv[2]) if len(sys.argv) > 2 else 2)
    else: raise SystemExit(__doc__)
