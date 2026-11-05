"""Generates romfs-ptbr.bin: PT-BR texts in place of Spanish (eu.qm slots 6/7) and redrawn menus.

    python -I assemble.py          # uses paths.py; needs Pillow and etcpak

Texts: SoH (SolidOne and LordeJabiraca) converted from the N64 format + translation.json for what only exists on 3DS.
The new eu.qm fits in the original's space, so the romfs is patched in place (no offset changes).
"""
import shutil, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from paths import ROMFS_ORIGINAL, ROMFS_PTBR, WORK_DIR
from conv import convert
from importer import load
from msgs import qm, soh
from romfs import locate


def assemble_qm():
    d, _ = qm(); pt = soh(); new_texts, problems = load()
    if problems:
        raise SystemExit(f'translation.json has problems: {problems[:5]}')
    n = struct.unpack_from('<I', d, 8)[0]
    recs = [list(struct.unpack_from('<24I', d, 0x10 + i * 96)) for i in range(n)]
    first = min(r[w] for r in recs for w in range(4, 22, 2) if r[w])
    blob = bytearray(d[:first]); seen = {}

    def store(b):
        if not b: return 0, 0
        if b in seen: return seen[b], len(b)
        while len(blob) % 4: blob.append(0)
        seen[b] = len(blob); blob.extend(b); return seen[b], len(b)

    for i, r in enumerate(recs):
        pl = [d[o:o + s] if s else b'' for o, s in ((r[w], r[w + 1]) for w in range(4, 22, 2))]
        mid = r[0]
        new_payload = convert(pt[mid][2], pl[1]) if mid in pt else new_texts.get(mid)
        if new_payload:
            pl[6] = pl[7] = new_payload                   # 06_EU_SPANISH and 07_US_SPANISH
        for s, b in enumerate(pl):
            r[4 + 2 * s], r[5 + 2 * s] = store(b)
        struct.pack_into('<24I', blob, 0x10 + i * 96, *r)
    if len(blob) > len(d):
        raise SystemExit(f'PT-BR eu.qm ended up larger than the original ({len(blob)} > {len(d)})')
    return bytes(blob), len(d)


def main():
    if not ROMFS_PTBR.exists():
        shutil.copyfile(ROMFS_ORIGINAL, ROMFS_PTBR)
    q, limit = assemble_qm()
    o, size = locate(ROMFS_PTBR, 'message/eu/eu.qm'); assert size == limit
    with open(ROMFS_PTBR, 'r+b') as f:
        f.seek(o); f.write(q + b'\0' * (size - len(q)))
    (WORK_DIR / 'eu-ptbr.qm').write_bytes(q)
    print('eu.qm PT-BR:', len(q), 'bytes')
    from ctxb import read, encode_ctxb
    from menu_batch import MAP_BUTTONS, BATCH, MAPS
    from menus_ptbr import write_to_romfs, hud, badges
    from swap import swap
    images = {'hud_all00': hud()}
    for n, (labels, opts) in BATCH.items():
        images[n] = swap(n, labels, **opts)[0]
    images['menu_hint_movie_parts00'] = badges(images['menu_hint_movie_parts00'])
    for n, (labels, opts) in MAPS.items():
        images[n] = swap(n, MAP_BUTTONS[n], initial=swap(n, labels, **opts)[0])[0]
    for n, im in images.items():
        write_to_romfs(ROMFS_PTBR, n, encode_ctxb(read('06_EU_SPANISH', n), im))
        im.save(WORK_DIR / 'menus-pt' / f'{n}.png')
    print(len(images), 'menu textures')
    from titles import write as write_titles
    print(write_titles(ROMFS_PTBR, WORK_DIR / 'menus-pt'), 'area titles')


if __name__ == '__main__':
    main()
