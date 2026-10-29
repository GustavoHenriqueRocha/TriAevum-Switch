"""Converte o texto PT-BR do SoH (formato N64) para payloads do eu.qm do OoT3D."""
import struct
from collections import Counter
from msgs import soh, qm, n64_tokens, t_tokens, N2T

ACENTOS = {0x80:'À',0x81:'Á',0x83:'Ã',0x86:'É',0x87:'Ê',0x89:'Í',0x90:'à',0x91:'á',0x92:'â',0x93:'ã',0x94:'ç',
           0x96:'é',0x97:'ê',0x99:'í',0x9a:'ô',0x9b:'õ',0x9c:'ó',0x9e:'ú'}
# botões do N64 → ícone 7f24 do 3DS
BOTOES = {0x9f:0x06,0xa0:0x07,0xa1:0x1c,0xa2:0x0a,0xa3:0x0b,0xa4:0x0a,0xa5:0x17,0xa6:0x1e,0xa7:0x1c,0xa8:0x1d,
          0xa9:0x21,0xaa:0x00,0xab:0x01}
ARGS_DO_3DS = {0x0e, 0x03, 0x11, 0x0d}   # som, próximo texto, fundo: argumento vem da mensagem do 3DS
avisos = Counter()


def converter(pt_bytes, en_bytes):
    en_args = {}
    for t in t_tokens(en_bytes or b''):
        if t[0] == 'c':
            en_args.setdefault(t[1], []).append(t[2])
    usados = Counter()
    out = bytearray()
    for t in n64_tokens(pt_bytes):
        if t[0] == 't':
            c = t[1]
            if c in ACENTOS:
                out += ACENTOS[c].encode()
            elif c in BOTOES:
                out += bytes([0x7f, 0x24, BOTOES[c]])
            elif 0x20 <= c < 0x7f:
                out.append(c)
            else:
                avisos[f'byte {c:#04x}'] += 1
            continue
        _, c, arg = t
        if c == 0x02:
            break
        if c not in N2T:
            avisos[f'código {c:#04x}'] += 1
            continue
        k = N2T[c]
        if k in ARGS_DO_3DS:
            lista = en_args.get(k, [])
            if usados[k] >= len(lista):
                avisos[f'sem argumento 3DS p/ 7f{k:02x}'] += 1
                continue
            arg = lista[usados[k]]
        elif k == 0x1d:
            arg = b'\x00' if arg == b'\x40' else arg
        elif k == 0x1e:
            arg = b''                       # deslocamento em pixels do N64 não existe no 3DS
        elif k == 0x1a:
            arg = b'\xff' * 4
        elif k == 0x1b:
            arg = b'\xff' * 6
        usados[k] += 1
        out += bytes([0x7f, k]) + arg
        if k == 0x01:
            out += b'\x7f\x1c'              # o 3DS sempre começa a caixa nova com quebra de linha
        if k == 0x03:
            break                           # "próximo texto" encerra a mensagem
    return bytes(out) + b'\x7f\x00'


def montar_qm(qm_bytes, pt):
    """Novo eu.qm: slots do espanhol (6 e 7 do runtime) com o PT-BR; resto igual."""
    n = struct.unpack_from('<I', qm_bytes, 8)[0]
    recs = [list(struct.unpack_from('<24I', qm_bytes, 0x10 + i*96)) for i in range(n)]
    primeiro = min(r[w] for r in recs for w in range(4, 22, 2) if r[w])
    blob = bytearray(qm_bytes[:primeiro]); vistos = {}
    def guardar(b):
        if not b: return 0, 0
        if b in vistos: return vistos[b], len(b)
        while len(blob) % 4: blob.append(0)
        vistos[b] = len(blob); blob.extend(b); return vistos[b], len(b)
    trocados = 0
    for i, r in enumerate(recs):
        mid = r[0]
        pares = [(r[w], r[w+1]) for w in range(4, 22, 2)]
        payloads = [qm_bytes[o:o+s] if s else b'' for o, s in pares]
        if mid in pt:
            novo = converter(pt[mid][2], payloads[1])
            payloads[6] = payloads[7] = novo; trocados += 1
        for s, b in enumerate(payloads):
            r[4+2*s], r[5+2*s] = guardar(b)
        struct.pack_into('<24I', blob, 0x10 + i*96, *r)
    return bytes(blob), trocados
