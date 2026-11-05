# Yaz0 + NARC (Super Mario 3D Land e outros jogos da Nintendo EAD no 3DS):
# lista, extrai e faz o censo dos arquivos dentro dos .szs do romfs.
#
#   python3 -I tools/oot3d/ctr_narc.py list <arquivo.szs>
#   python3 -I tools/oot3d/ctr_narc.py extract <arquivo.szs> <pasta-saida>
#   python3 -I tools/oot3d/ctr_narc.py census <pasta-romfs>
#
# Os .szs do SM3DL não têm SARC: depois do Yaz0 vem um NARC little-endian
# (validado em todos os 1681 .szs do romfs, 05/11):
#   0x00 "NARC" fe ff 00 01 | u32 tamanho | u16 tamanho do header (0x10) | u16 nº de seções (3)
#   BTAF  u32 tamanho da seção | u16 nº de arquivos | u16 0 | n × (u32 início, u32 fim),
#         relativos ao começo dos dados de GMIF
#   BTNF  u32 tamanho | tabela de diretórios: 8 bytes por diretório
#         (u32 offset da subtabela, u16 id do 1º arquivo, u16 pai/nº de diretórios na raiz);
#         subtabela: u8 len (bit 0x80 = subdiretório, seguido de u16 id), nome; 0 termina
#   GMIF  u32 tamanho | dados (os offsets do BTAF começam aqui, em GMIF+8)
# Censo 05/11 (SM3DL): 10275 arquivos; 1078 .bcmdl (CGFX, 193 MB), 6492 .byml,
# 584 .bclan, 577 .kcl, 577 .pa, 404 .bclim, 392 .msbt, 4 .shbin (DVLB), 1 .bcsdr (CGFX).
# Os dados do jogo são não confiáveis: rode sempre com python3 -I.
import collections
import os
import struct
import sys


def yaz0(data: bytes) -> bytes:
    if data[:4] != b'Yaz0':
        raise ValueError('não é Yaz0')
    size = struct.unpack_from('>I', data, 4)[0]
    out = bytearray()
    src = 16
    while len(out) < size:
        header = data[src]
        src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if header & (0x80 >> bit):
                out.append(data[src])
                src += 1
                continue
            value = data[src] << 8 | data[src + 1]
            src += 2
            distance = (value & 0xFFF) + 1
            count = value >> 12
            if count == 0:
                count = data[src] + 0x12
                src += 1
            else:
                count += 2
            start = len(out) - distance
            if start < 0:
                raise ValueError('Yaz0 com referência antes do início')
            if distance >= count:
                out += out[start:start + count]
            else:
                for k in range(count):
                    out.append(out[start + k])
    return bytes(out)


def _section(data: bytes, offset: int, magic: bytes) -> tuple[int, int]:
    if data[offset:offset + 4] != magic:
        raise ValueError(f'esperava {magic!r} em 0x{offset:x}, achei {data[offset:offset + 4]!r}')
    return offset, struct.unpack_from('<I', data, offset + 4)[0]


def _names(data: bytes, btnf: int, count: int) -> list[str]:
    """Nomes completos (com subpastas) na ordem dos ids do BTAF."""
    names = [f'{i:04d}.bin' for i in range(count)]
    table = btnf + 8
    root_dirs = struct.unpack_from('<H', data, table + 6)[0]
    if not 0 < root_dirs <= 0x1000:
        return names

    def walk(index: int, prefix: str, depth: int) -> None:
        if depth > 16 or index >= root_dirs:
            return
        sub, file_id = struct.unpack_from('<IH', data, table + index * 8)
        pos = table + sub
        while pos < len(data):
            length = data[pos]
            pos += 1
            if length == 0:
                break
            name = data[pos:pos + (length & 0x7F)].decode('latin1')
            pos += length & 0x7F
            if length & 0x80:
                child = struct.unpack_from('<H', data, pos)[0] & 0x0FFF
                pos += 2
                walk(child, prefix + name + '/', depth + 1)
            else:
                if file_id < count:
                    names[file_id] = prefix + name
                file_id += 1

    walk(0, '', 0)
    return names


def narc(data: bytes) -> list[tuple[str, bytes]]:
    if data[:8] != b'NARC\xfe\xff\x00\x01':
        raise ValueError('não é NARC little-endian')
    header_size = struct.unpack_from('<H', data, 0xC)[0]
    btaf, btaf_size = _section(data, header_size, b'BTAF')
    count = struct.unpack_from('<H', data, btaf + 8)[0]
    if btaf + 12 + count * 8 > btaf + btaf_size:
        raise ValueError('BTAF menor que o nº de arquivos')
    btnf, btnf_size = _section(data, btaf + btaf_size, b'BTNF')
    gmif, gmif_size = _section(data, btnf + btnf_size, b'GMIF')
    base, end = gmif + 8, gmif + gmif_size
    names = _names(data, btnf, count)
    files = []
    for i in range(count):
        start, stop = struct.unpack_from('<II', data, btaf + 12 + i * 8)
        if not start <= stop <= end - base:
            raise ValueError(f'arquivo {i} fora do GMIF (0x{start:x}..0x{stop:x})')
        files.append((names[i], data[base + start:base + stop]))
    return files


def open_szs(path: str) -> list[tuple[str, bytes]]:
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] == b'Yaz0':
        data = yaz0(data)
    return narc(data)


def kind(name: str, blob: bytes) -> str:
    magic = blob[:4]
    ext = os.path.splitext(name)[1].lower() or '(sem ext)'
    if len(magic) == 4 and all(0x41 <= c <= 0x5A or 0x61 <= c <= 0x7A for c in magic[:2]):
        return f'{ext} {bytes(c for c in magic if 0x20 < c < 0x7F).decode()}'
    return f'{ext} (sem magia no início)'


def census(root: str) -> int:
    kinds: collections.Counter = collections.Counter()
    sizes: collections.Counter = collections.Counter()
    archives = files = 0
    errors = []
    for directory, _, names in os.walk(root):
        for name in sorted(names):
            if not name.endswith('.szs'):
                continue
            path = os.path.join(directory, name)
            try:
                content = open_szs(path)
            except (ValueError, IndexError, struct.error) as error:
                errors.append(f'{os.path.relpath(path, root)}: {error}')
                continue
            archives += 1
            for inner, blob in content:
                files += 1
                kinds[kind(inner, blob)] += 1
                sizes[kind(inner, blob)] += len(blob)
    print(f'{archives} .szs com NARC, {files} arquivos dentro, {len(errors)} falhas')
    for key, value in kinds.most_common():
        print(f'{value:6d} {sizes[key] / 1e6:9.2f} MB  {key}')
    for line in errors[:10]:
        print('  falha:', line)
    return 1 if errors else 0


def main() -> int:
    if len(sys.argv) < 3 or sys.argv[1] not in ('list', 'extract', 'census'):
        print(__doc__ or 'uso: ctr_narc.py list|extract|census ...', file=sys.stderr)
        return 2
    if sys.argv[1] == 'census':
        return census(sys.argv[2])
    content = open_szs(sys.argv[2])
    if sys.argv[1] == 'list':
        for name, blob in content:
            print(f'{len(blob):9d} {blob[:4]!r} {name}')
        return 0
    out = os.path.realpath(sys.argv[3])
    for name, blob in content:
        target = os.path.realpath(os.path.join(out, name))
        if not target.startswith(out + os.sep):
            print('nome suspeito ignorado:', name, file=sys.stderr)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as f:
            f.write(blob)
    return 0


if __name__ == '__main__':
    sys.exit(main())
