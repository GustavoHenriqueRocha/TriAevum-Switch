# SPDX-License-Identifier: GPL-2.0-or-later
"""Arquivos GAR v2 da Grezzo (layout/<idioma>/Joker.*.gar do MM3D): ler e remontar byte a byte.

Mesmo layout canônico de parse_gar/emit_gar do upstream (coccofresco/TriAevum, tools/mm3d/restoration_layout.py,
commit ec3a6ea8): cabeçalho de 0x20, tabela de tipos (quantos, lista de índices, nome, -1), por tipo a lista de
índices e o nome (alinhado a 4), registros dos membros (tamanho, raiz, nome inteiro), pares de nomes (alinhados
a 4), offsets e os dados (alinhados a 4). Os 10 GAR do espanhol do MM3D EUR Rev 1 voltam iguais por ele.
"""
import struct


def _u16(b, p): return struct.unpack_from('<H', b, p)[0]
def _u32(b, p): return struct.unpack_from('<I', b, p)[0]


def _nome(b, p):
    if not 0 <= p < len(b): raise ValueError('offset de nome fora do GAR')
    return b[p:b.index(b'\0', p)].decode('ascii')


def ler(b):
    """→ {'hash', 'tipos', 'membros': [(nome, bytes)]}. Recusa o GAR que não remonta igual."""
    if b[:4] != b'GAR\x02' or len(b) < 0x20 or _u32(b, 4) != len(b):
        raise ValueError('cabeçalho de GAR v2 inválido')
    ntipos, n = _u16(b, 8), _u16(b, 10)
    tipos_em, regs, offs = _u32(b, 12), _u32(b, 16), _u32(b, 20)
    tipos = [_nome(b, _u32(b, tipos_em + 16 * i + 8)) for i in range(ntipos)]
    membros = []
    for i in range(n):
        tam, ini = _u32(b, regs + 12 * i), _u32(b, offs + 4 * i)
        if ini + tam > len(b): raise ValueError('membro passa do fim do GAR')
        membros.append((_nome(b, _u32(b, regs + 12 * i + 8)), b[ini:ini + tam]))
    if len({m for m, _ in membros}) != n: raise ValueError('membro repetido no GAR')
    modelo = {'hash': b[24:32].rstrip(b'\0').decode('ascii'), 'tipos': tipos, 'membros': membros}
    if montar(modelo) != b: raise ValueError('GAR fora do layout canônico (não remonta igual)')
    return modelo


def montar(m):
    tipos, membros = m['tipos'], m['membros']
    por_tipo = {t: [] for t in tipos}
    for i, (nome, _) in enumerate(membros):
        t = nome.rsplit('.', 1)[1] if '.' in nome else 'unknown'
        if t not in por_tipo: raise ValueError(f'tipo {t} não declarado no GAR')
        por_tipo[t].append(i)
    out = bytearray(0x20 + 16 * len(tipos))
    for k, t in enumerate(tipos):
        lista = len(out) if por_tipo[t] else 0xFFFFFFFF
        for i in por_tipo[t]: out += struct.pack('<I', i)
        nome_em = len(out); out += t.encode('ascii') + b'\0'; out += b'\0' * (-len(out) % 4)
        struct.pack_into('<IIII', out, 0x20 + 16 * k, len(por_tipo[t]), lista, nome_em, 0xFFFFFFFF)
    regs = len(out); out += bytes(12 * len(membros))
    for i, (nome, dados) in enumerate(membros):
        inteiro = len(out); out += nome.encode('ascii') + b'\0'
        raiz = len(out); out += nome.rsplit('.', 1)[0].encode('ascii') + b'\0'; out += b'\0' * (-len(out) % 4)
        struct.pack_into('<III', out, regs + 12 * i, len(dados), raiz, inteiro)
    offs = len(out); out += bytes(4 * len(membros))
    for i, (_, dados) in enumerate(membros):
        if i: out += b'\0' * (-len(out) % 4)
        struct.pack_into('<I', out, offs + 4 * i, len(out)); out += dados
    h = m['hash'].encode('ascii')
    if len(h) >= 8: raise ValueError('hash do GAR longo demais')
    struct.pack_into('<4sIHHIII8s', out, 0, b'GAR\x02', len(out), len(tipos), len(membros), 0x20, regs, offs, h)
    return bytes(out)


def trocar(b, novos):
    """GAR `b` com os membros de `novos` ({nome: bytes}) trocados; mesmo tamanho de cada membro → mesmo arquivo."""
    m = ler(b)
    falta = set(novos) - {n for n, _ in m['membros']}
    if falta: raise KeyError(f'membros fora do GAR: {sorted(falta)}')
    return montar(dict(m, membros=[(n, novos.get(n, d)) for n, d in m['membros']]))
