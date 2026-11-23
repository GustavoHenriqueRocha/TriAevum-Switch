"""Mensagens do Majora's Mask 3D: `message/eu/eu?.gmsg` (um arquivo por idioma) e a fonte `message/ltn16.gzf`.

GMSG (deduzido do EUR Rev 1; ver docs/pesquisas/2026-11-23-mm3d-ptbr.md):
    0x00  'GMSG', u32 versão (1), u32 n, u32 idioma (bit: E 0x10, G 0x20, F 0x40, S 0x80, I 0x100)
    0x10  n entradas de 20 bytes: u16 id, u16, u32, u16, u8, u8 (caixa/posição; não mexemos),
          u32 offset (desde o início do arquivo), u32 tamanho
    depois: os textos, em ordem, cada um alinhado a 4; o arquivo termina alinhado a 4.

Texto: UTF-8 com códigos 0x7f + (zero até par) + u16 código + argumentos:
    u16 (alinhado a 2) para os de ARGS; 0x32 (som) leva u32 alinhado a 4;
    0x20 (palavra que muda com o número) leva u16 e duas palavras UTF-8 terminadas em zero, cada uma até par.
    0x01 = quebra de linha; 0x00 = fim.
Forma de texto: \\n para 0x01; ⟦cc⟧, ⟦cc:hhhh⟧, ⟦32:hhhhhhhh⟧, ⟦20:hhhh:singular:plural⟧ (só as palavras do 0x20
se traduzem; o resto dos códigos fica igual).
"""
import re
import struct

# quantos u16 cada código leva (os demais, nenhum); deduzido até as 30760 mensagens dos 5 idiomas fecharem
ARGS = {0x11: 1, 0x15: 1, 0x1c: 1, 0x22: 1, 0x25: 1, 0x26: 1, 0x29: 1, 0x2a: 1, 0x2b: 1, 0x2d: 1, 0x2f: 1, 0x31: 1,
        0x3a: 1}
SOM, PLURAL, QUEBRA, FIM = 0x32, 0x20, 0x01, 0x00
CAB, ENTRADA = 0x10, 20
COD = re.compile(r'⟦([0-9a-f]{2})((?::[^:⟧]*)*)⟧')


class ErroGmsg(ValueError):
    pass


def ler(d):
    """→ (cabeçalho 16 bytes, [entrada como lista de 8 campos], [payload])"""
    if d[:4] != b'GMSG':
        raise ErroGmsg('não é GMSG')
    n = struct.unpack_from('<I', d, 8)[0]
    ents = [list(struct.unpack_from('<HHIHBBII', d, CAB + ENTRADA * i)) for i in range(n)]
    return d[:CAB], ents, [d[e[6]:e[6] + e[7]] for e in ents]


def montar(cab, ents, payloads):
    """Arquivo novo com os payloads em ordem, cada um alinhado a 4 (o mesmo arranjo do original)."""
    out = bytearray(cab) + bytes(ENTRADA * len(ents))
    for i, (e, p) in enumerate(zip(ents, payloads)):
        e = list(e)
        if p:
            while len(out) % 4: out.append(0)
            e[6], e[7] = len(out), len(p); out += p
        else:
            e[6] = e[7] = 0
        struct.pack_into('<HHIHBBII', out, CAB + ENTRADA * i, *e)
    while len(out) % 4: out.append(0)
    return bytes(out)


def _palavra(b, j):
    k = b.index(b'\0', j)
    s = b[j:k].decode('utf-8'); j = k + 1
    if j % 2:
        if b[j]: raise ErroGmsg('enchimento não zero')
        j += 1
    if ':' in s or '⟧' in s:
        raise ErroGmsg(f'palavra com separador: {s!r}')
    return s, j


def texto(b):
    """payload (começando alinhado a 4) → texto legível. Levanta ErroGmsg se não fechar certinho."""
    out, i, txt = [], 0, bytearray()
    while i < len(b):
        if b[i] != 0x7f:
            if b[i] < 0x20: raise ErroGmsg(f'byte {b[i]:#04x} no texto em {i}')
            txt.append(b[i]); i += 1; continue
        out.append(txt.decode('utf-8')); txt.clear()
        j = i + 1
        if j % 2:
            if b[j]: raise ErroGmsg('enchimento não zero')
            j += 1
        c = struct.unpack_from('<H', b, j)[0]; j += 2
        if c > 0xff: raise ErroGmsg(f'código {c:#x}')
        if c == FIM:
            if j != len(b): raise ErroGmsg('fim antes do tamanho')
            return ''.join(out)
        if c == QUEBRA:
            out.append('\n')
        elif c == SOM:
            while j % 4:
                if b[j]: raise ErroGmsg('enchimento não zero')
                j += 1
            out.append(f'⟦32:{struct.unpack_from("<I", b, j)[0]:08x}⟧'); j += 4
        elif c == PLURAL:
            a = struct.unpack_from('<H', b, j)[0]; j += 2
            um, j = _palavra(b, j)
            varios, j = _palavra(b, j)
            out.append(f'⟦20:{a:04x}:{um}:{varios}⟧')
        else:
            args = struct.unpack_from(f'<{ARGS.get(c, 0)}H', b, j); j += 2 * len(args)
            out.append('⟦' + ':'.join([f'{c:02x}'] + [f'{a:04x}' for a in args]) + '⟧')
        i = j
    raise ErroGmsg('sem código de fim')


def para_bytes(s):
    """texto → payload (inverso exato de texto() para payloads que começam alinhados a 4)."""
    out = bytearray()

    def codigo(c):
        out.append(0x7f)
        if len(out) % 2: out.append(0)
        out.extend(struct.pack('<H', c))

    def corrido(t):
        for k, linha in enumerate(t.split('\n')):
            if k: codigo(QUEBRA)
            out.extend(linha.encode('utf-8'))

    pos = 0
    for m in COD.finditer(s):
        corrido(s[pos:m.start()]); pos = m.end()
        c = int(m.group(1), 16); partes = m.group(2).split(':')[1:]
        codigo(c)
        if c == SOM:
            while len(out) % 4: out.append(0)
            out.extend(struct.pack('<I', int(partes[0], 16)))
        elif c == PLURAL:
            out.extend(struct.pack('<H', int(partes[0], 16)))
            for p in partes[1:3]:
                out.extend(p.encode('utf-8') + b'\0')
                if len(out) % 2: out.append(0)
        else:
            if len(partes) != ARGS.get(c, 0): raise ErroGmsg(f'código {c:02x} com {len(partes)} argumentos')
            for a in partes: out.extend(struct.pack('<H', int(a, 16)))
    corrido(s[pos:])
    codigo(FIM)
    return bytes(out)


COR = '3a'


def codigos(s, ignorar=(COR,)):
    """Sequência de códigos para conferir a tradução: as palavras do 0x20 podem mudar, o resto não.

    A cor (0x3a) fica de fora por padrão: os tradutores da Nintendo mudam onde ela cai (231 das 503 mensagens
    em que o espanhol difere do inglês só mudam na cor)."""
    out = []
    for m in COD.finditer(s):
        if m.group(1) in ignorar: continue
        partes = m.group(2).split(':')[1:]
        out.append((m.group(1),) + tuple(partes[:1] if m.group(1) == '20' else partes))
    return out


def visivel(s):
    """O que aparece na tela: o texto corrido mais as palavras dos códigos 0x20."""
    return COD.sub(lambda m: ' '.join(m.group(2).split(':')[2:]) if m.group(1) == '20' else '', s)


def glifos_gzf(d):
    """Pontos de código que a fonte GZFX tem (registros de 12 bytes depois das folhas de 8 bytes)."""
    if d[:4] != b'GZFX':
        raise ErroGmsg('não é GZFX')
    tam, folhas, n = struct.unpack_from('<III', d, 0x0c)
    base = 0x30 + 8 * folhas
    return {struct.unpack_from('<I', d, base + tam * i)[0] for i in range(n)}
