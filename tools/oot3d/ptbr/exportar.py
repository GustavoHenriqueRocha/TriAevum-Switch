"""Exporta as mensagens do 3DS sem PT-BR, com os códigos como ⟦hex⟧, e um glossário EN→PT."""
import json, re
from collections import Counter, defaultdict
from msgs import soh, qm, t_tokens
from conv import converter
from caminhos import TRABALHO

def texto(b):
    """payload do 3DS → texto legível; \\n = 7f1c; outros códigos = ⟦7fXX..⟧."""
    out, buf = [], bytearray()
    def solta():
        if buf: out.append(buf.decode('utf-8', 'replace')); buf.clear()
    for t in t_tokens(b):
        if t[0] == 't': buf.append(t[1]); continue
        solta()
        if t[1] == 0x1c: out.append('\n')
        elif t[1] == 0x00: break
        else: out.append('⟦' + bytes([0x7f, t[1]]).hex() + t[2].hex() + '⟧')
    solta(); return ''.join(out)

def vermelhos(s):
    return re.findall(r'⟦7f1d41⟧(.*?)⟦7f1d00⟧', s, re.S)

if __name__ == '__main__':
    pt = soh(); _, q = qm()
    glos = defaultdict(Counter)
    for mid in pt:
        if mid not in q: continue
        en = texto(q[mid][1][0]); p = texto(converter(pt[mid][2], q[mid][1][0]))
        a, b = vermelhos(en), vermelhos(p)
        if len(a) == len(b):
            for x, y in zip(a, b):
                x, y = ' '.join(x.split()), ' '.join(y.split())
                if x and y: glos[x][y] += 1
    glossario = {k: v.most_common(1)[0][0] for k, v in sorted(glos.items())}
    pend = [{'id': f'{m:#06x}', 'en': texto(q[m][1][0])} for m in sorted(set(q) - set(pt))]
    S = str(TRABALHO) + '/'
    json.dump(pend, open(S + 'pendentes.json', 'w'), ensure_ascii=False, indent=1)
    json.dump(glossario, open(S + 'glossario.json', 'w'), ensure_ascii=False, indent=1)
    print(len(pend), 'pendentes;', len(glossario), 'termos no glossário;', sum(len(p['en']) for p in pend), 'caracteres')
