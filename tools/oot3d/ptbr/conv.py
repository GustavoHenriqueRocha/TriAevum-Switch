"""Converts the SoH PT-BR text (N64 format) into OoT3D eu.qm payloads."""
import struct
from collections import Counter
from msgs import soh, qm, n64_tokens, t_tokens, N2T

ACCENTS = {0x80:'À',0x81:'Á',0x83:'Ã',0x86:'É',0x87:'Ê',0x89:'Í',0x90:'à',0x91:'á',0x92:'â',0x93:'ã',0x94:'ç',
           0x96:'é',0x97:'ê',0x99:'í',0x9a:'ô',0x9b:'õ',0x9c:'ó',0x9e:'ú'}
# N64 buttons → 3DS 7f24 icon
BUTTONS = {0x9f:0x06,0xa0:0x07,0xa1:0x1c,0xa2:0x0a,0xa3:0x0b,0xa4:0x0a,0xa5:0x17,0xa6:0x1e,0xa7:0x1c,0xa8:0x1d,
          0xa9:0x21,0xaa:0x00,0xab:0x01}
# sound, next text, background, fade and 7f06 (N64 0x0a has no argument, the 3DS code has one): the argument comes
# from the 3DS message, so each code carries exactly the bytes the game reads (msgs.T_ARGS)
ARGS_FROM_3DS = {0x0e, 0x03, 0x11, 0x0d, 0x06}
conversion_warnings = Counter()


def convert(pt_bytes, en_bytes):
    en_args = {}
    for t in t_tokens(en_bytes or b''):
        if t[0] == 'c':
            en_args.setdefault(t[1], []).append(t[2])
    used = Counter()
    out = bytearray()
    for t in n64_tokens(pt_bytes):
        if t[0] == 't':
            c = t[1]
            if c in ACCENTS:
                out += ACCENTS[c].encode()
            elif c in BUTTONS:
                out += bytes([0x7f, 0x24, BUTTONS[c]])
            elif 0x20 <= c < 0x7f:
                out.append(c)
            else:
                conversion_warnings[f'byte {c:#04x}'] += 1
            continue
        _, c, arg = t
        if c == 0x02:
            break
        if c not in N2T:
            conversion_warnings[f'code {c:#04x}'] += 1
            continue
        k = N2T[c]
        if k in ARGS_FROM_3DS:
            args_list = en_args.get(k, [])
            if used[k] >= len(args_list):
                conversion_warnings[f'no 3DS argument for 7f{k:02x}'] += 1
                continue
            arg = args_list[used[k]]
        elif k == 0x1d:
            arg = b'\x00' if arg == b'\x40' else arg
        elif k == 0x1e:
            arg = b''                       # N64 pixel offset does not exist on 3DS
        elif k == 0x1a:
            arg = b'\xff' * 4
        elif k == 0x1b:
            arg = b'\xff' * 6
        used[k] += 1
        out += bytes([0x7f, k]) + arg
        if k == 0x01:
            out += b'\x7f\x1c'              # the 3DS always starts a new box with a line break
        if k == 0x03:
            break                           # "next text" ends the message
    return bytes(out) + b'\x7f\x00'


def assemble_qm(qm_bytes, pt):
    """New eu.qm: Spanish slots (runtime 6 and 7) with PT-BR; the rest unchanged."""
    n = struct.unpack_from('<I', qm_bytes, 8)[0]
    recs = [list(struct.unpack_from('<24I', qm_bytes, 0x10 + i*96)) for i in range(n)]
    first = min(r[w] for r in recs for w in range(4, 22, 2) if r[w])
    blob = bytearray(qm_bytes[:first]); seen = {}
    def store(b):
        if not b: return 0, 0
        if b in seen: return seen[b], len(b)
        while len(blob) % 4: blob.append(0)
        seen[b] = len(blob); blob.extend(b); return seen[b], len(b)
    swapped = 0
    for i, r in enumerate(recs):
        mid = r[0]
        pairs = [(r[w], r[w+1]) for w in range(4, 22, 2)]
        payloads = [qm_bytes[o:o+s] if s else b'' for o, s in pairs]
        if mid in pt:
            new_payload = convert(pt[mid][2], payloads[1])
            payloads[6] = payloads[7] = new_payload; swapped += 1
        for s, b in enumerate(payloads):
            r[4+2*s], r[5+2*s] = store(b)
        struct.pack_into('<24I', blob, 0x10 + i*96, *r)
    return bytes(blob), swapped
