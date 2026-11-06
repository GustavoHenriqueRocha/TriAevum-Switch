"""Brings translation.json back into 3DS payloads, checking codes and font."""
import json, re, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from msgs import soh, qm, grammar_error
from export import text
from conv import convert

from paths import WORK_DIR as S, ROMFS_PTBR
COD = re.compile(r'⟦([0-9a-f]+)⟧')


def to_bytes(s):
    out = bytearray(); pos = 0
    for m in COD.finditer(s):
        out += s[pos:m.start()].replace('\n', '\x00NL\x00').encode('utf-8').replace(b'\x00NL\x00', b'\x7f\x1c')
        out += bytes.fromhex(m.group(1)); pos = m.end()
    out += s[pos:].replace('\n', '\x00NL\x00').encode('utf-8').replace(b'\x00NL\x00', b'\x7f\x1c')
    return bytes(out) + b'\x7f\x00'


def glyphs():
    from tools.oot3d.decomp_support.scripts.build_topscreen_texture_override_pack import read_level3_romfs_file
    from tools.oot3d.decomp_support.scripts.qbf_font import parse_qbf
    f = parse_qbf(read_level3_romfs_file(ROMFS_PTBR, 'message/eu/ltn16.qbf'))
    return {c.code for c in f.characters}


def load():
    """→ ({id: payload}, problemas)"""
    _, q = qm(); translation = json.load(open(S / 'translation.json', encoding='utf-8')); glyph_set = glyphs()
    ready, problems = {}, []
    for k, pt in translation.items():
        mid = int(k, 16)
        if mid not in q: problems.append((k, 'id does not exist')); continue
        en = text(q[mid][1][0])
        if COD.findall(en) != COD.findall(pt): problems.append((k, 'codes differ')); continue
        missing = {c for c in COD.sub('', pt) if c != '\n' and ord(c) not in glyph_set}
        if missing: problems.append((k, 'no glyph: ' + ''.join(sorted(missing)))); continue
        payload = to_bytes(pt)
        error = grammar_error(payload)
        if error: problems.append((k, 'the game cannot parse it: ' + error)); continue
        ready[mid] = payload
    return ready, problems


if __name__ == '__main__':
    p, problems = load(); print(len(p), 'ready;', len(problems), 'problems', problems[:10])
