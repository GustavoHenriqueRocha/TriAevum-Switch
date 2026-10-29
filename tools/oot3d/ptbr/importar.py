"""Traz traducao.json de volta para payloads do 3DS, conferindo códigos e fonte."""
import json, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from msgs import soh, qm
from exportar import texto
from conv import converter

from caminhos import TRABALHO as S, ROMFS_PTBR
COD = re.compile(r'⟦([0-9a-f]+)⟧')


def para_bytes(s):
    out = bytearray(); pos = 0
    for m in COD.finditer(s):
        out += s[pos:m.start()].replace('\n', '\x00NL\x00').encode('utf-8').replace(b'\x00NL\x00', b'\x7f\x1c')
        out += bytes.fromhex(m.group(1)); pos = m.end()
    out += s[pos:].replace('\n', '\x00NL\x00').encode('utf-8').replace(b'\x00NL\x00', b'\x7f\x1c')
    return bytes(out) + b'\x7f\x00'


def glifos():
    from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import read_level3_romfs_file
    from tools.oot3d.decomp_support.scripts.qbf_font import parse_qbf
    f = parse_qbf(read_level3_romfs_file(ROMFS_PTBR, 'message/eu/ltn16.qbf'))
    return {c.code for c in f.characters}


def carregar():
    """→ ({id: payload}, problemas)"""
    _, q = qm(); trad = json.load(open(S / 'traducao.json', encoding='utf-8')); fonte = glifos()
    pront, prob = {}, []
    for k, pt in trad.items():
        mid = int(k, 16)
        if mid not in q: prob.append((k, 'id não existe')); continue
        en = texto(q[mid][1][0])
        if COD.findall(en) != COD.findall(pt): prob.append((k, 'códigos diferentes')); continue
        fora = {c for c in COD.sub('', pt) if c != '\n' and ord(c) not in fonte}
        if fora: prob.append((k, 'sem glifo: ' + ''.join(sorted(fora)))); continue
        pront[mid] = para_bytes(pt)
    return pront, prob


if __name__ == '__main__':
    p, prob = carregar(); print(len(p), 'prontas;', len(prob), 'problemas', prob[:10])
