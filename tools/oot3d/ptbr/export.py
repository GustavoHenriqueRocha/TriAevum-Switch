"""Exports the 3DS messages without PT-BR, with codes as ⟦hex⟧, and an EN→PT glossary."""
import json, re
from collections import Counter, defaultdict
from msgs import soh, qm, t_tokens
from conv import convert
from paths import WORK_DIR

def text(b):
    """3DS payload → readable text; \\n = 7f1c; other codes = ⟦7fXX..⟧."""
    out, buf = [], bytearray()
    def flush():
        if buf: out.append(buf.decode('utf-8', 'replace')); buf.clear()
    for t in t_tokens(b):
        if t[0] == 't': buf.append(t[1]); continue
        flush()
        if t[1] == 0x1c: out.append('\n')
        elif t[1] == 0x00: break
        else: out.append('⟦' + bytes([0x7f, t[1]]).hex() + t[2].hex() + '⟧')
    flush(); return ''.join(out)

def red_spans(s):
    return re.findall(r'⟦7f1d41⟧(.*?)⟦7f1d00⟧', s, re.S)

if __name__ == '__main__':
    pt = soh(); _, q = qm()
    glossary_counts = defaultdict(Counter)
    for mid in pt:
        if mid not in q: continue
        en = text(q[mid][1][0]); p = text(convert(pt[mid][2], q[mid][1][0]))
        a, b = red_spans(en), red_spans(p)
        if len(a) == len(b):
            for x, y in zip(a, b):
                x, y = ' '.join(x.split()), ' '.join(y.split())
                if x and y: glossary_counts[x][y] += 1
    glossary = {k: v.most_common(1)[0][0] for k, v in sorted(glossary_counts.items())}
    pending = [{'id': f'{m:#06x}', 'en': text(q[m][1][0])} for m in sorted(set(q) - set(pt))]
    S = str(WORK_DIR) + '/'
    json.dump(pending, open(S + 'pending.json', 'w'), ensure_ascii=False, indent=1)
    json.dump(glossary, open(S + 'glossary.json', 'w'), ensure_ascii=False, indent=1)
    print(len(pending), 'pending;', len(glossary), 'glossary terms;', sum(len(p['en']) for p in pending), 'characters')
