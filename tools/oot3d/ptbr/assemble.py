"""Generates romfs-ptbr.bin: PT-BR texts in place of Spanish (eu.qm slots 6/7) and redrawn menus.

    python -I assemble.py          # uses paths.py; needs Pillow and etcpak

Texts: SoH (SolidOne and LordeJabiraca) converted from the N64 format + traducao.json for what only exists on 3DS.
The new eu.qm fits in the original's space, so the romfs is patched in place (no offset changes).
"""
import shutil, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROMFS_ORIGINAL, ROMFS_PTBR, TRABALHO
from conv import converter
from importer import carregar
from msgs import qm, soh
from romfs import localizar


def montar_qm():
    d, _ = qm(); pt = soh(); novas, problemas = carregar()
    if problemas:
        raise SystemExit(f'traducao.json has problems: {problemas[:5]}')
    n = struct.unpack_from('<I', d, 8)[0]
    recs = [list(struct.unpack_from('<24I', d, 0x10 + i * 96)) for i in range(n)]
    primeiro = min(r[w] for r in recs for w in range(4, 22, 2) if r[w])
    blob = bytearray(d[:primeiro]); vistos = {}

    def guardar(b):
        if not b: return 0, 0
        if b in vistos: return vistos[b], len(b)
        while len(blob) % 4: blob.append(0)
        vistos[b] = len(blob); blob.extend(b); return vistos[b], len(b)

    for i, r in enumerate(recs):
        pl = [d[o:o + s] if s else b'' for o, s in ((r[w], r[w + 1]) for w in range(4, 22, 2))]
        mid = r[0]
        novo = converter(pt[mid][2], pl[1]) if mid in pt else novas.get(mid)
        if novo:
            pl[6] = pl[7] = novo                   # 06_EU_SPANISH and 07_US_SPANISH
        for s, b in enumerate(pl):
            r[4 + 2 * s], r[5 + 2 * s] = guardar(b)
        struct.pack_into('<24I', blob, 0x10 + i * 96, *r)
    if len(blob) > len(d):
        raise SystemExit(f'PT-BR eu.qm ended up larger than the original ({len(blob)} > {len(d)})')
    return bytes(blob), len(d)


def main():
    if not ROMFS_PTBR.exists():
        shutil.copyfile(ROMFS_ORIGINAL, ROMFS_PTBR)
    q, limite = montar_qm()
    o, tam = localizar(ROMFS_PTBR, 'message/eu/eu.qm'); assert tam == limite
    with open(ROMFS_PTBR, 'r+b') as f:
        f.seek(o); f.write(q + b'\0' * (tam - len(q)))
    (TRABALHO / 'eu-ptbr.qm').write_bytes(q)
    print('eu.qm PT-BR:', len(q), 'bytes')
    from ctxb import ler, codificar_ctxb
    from menu_batch import BOTOES_MAPAS, LOTE, MAPAS
    from menus_ptbr import gravar_na_romfs, hud, selos
    from swap import trocar
    imagens = {'hud_all00': hud()}
    for n, (rot, op) in LOTE.items():
        imagens[n] = trocar(n, rot, **op)[0]
    imagens['menu_hint_movie_parts00'] = selos(imagens['menu_hint_movie_parts00'])
    for n, (rot, op) in MAPAS.items():
        imagens[n] = trocar(n, BOTOES_MAPAS[n], inicial=trocar(n, rot, **op)[0])[0]
    for n, im in imagens.items():
        gravar_na_romfs(ROMFS_PTBR, n, codificar_ctxb(ler('06_EU_SPANISH', n), im))
        im.save(TRABALHO / 'menus-pt' / f'{n}.png')
    print(len(imagens), 'menu textures')
    from titles import gravar as gravar_titulos
    print(gravar_titulos(ROMFS_PTBR, TRABALHO / 'menus-pt'), 'area titles')


if __name__ == '__main__':
    main()
