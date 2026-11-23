# Descompressor do LzS da Grezzo (Majora's Mask 3D: .gar.lzs, .zsi, .ctxb comprimidos).
# Cabeçalho de 0x10: "LzS\x01", u32 desconhecido, u32 tamanho descomprimido, u32 tamanho
# comprimido. Corpo: LZSS clássico (Okumura): janela de 4 KiB iniciada com zeros na posição
# 0xFEE; byte de flags lido do bit menos significativo, 1 = literal, 0 = referência de 2 bytes
# (posição = b0 | (b1 & 0xF0) << 4, comprimento = (b1 & 0x0F) + 3).
# O OoT3D não tem arquivos LzS (romfs sem .lzs): unpack() devolve o conteúdo sem mudança.
import struct

MAGIC = b'LzS\x01'


def decompress(data: bytes) -> bytes:
    if data[:4] != MAGIC:
        raise ValueError('não é LzS')
    _, size, csize = struct.unpack_from('<3I', data, 4)
    src = memoryview(data)[0x10:0x10 + csize]
    window = bytearray(4096)
    pos = 0xFEE
    out = bytearray()
    i = 0
    n = len(src)
    while i < n and len(out) < size:
        flags = src[i]
        i += 1
        for bit in range(8):
            if i >= n or len(out) >= size:
                break
            if flags >> bit & 1:
                b = src[i]
                i += 1
                out.append(b)
                window[pos] = b
                pos = (pos + 1) & 0xFFF
            else:
                if i + 1 >= n:
                    raise ValueError('LzS truncado')
                b0, b1 = src[i], src[i + 1]
                i += 2
                ref = b0 | (b1 & 0xF0) << 4
                for k in range((b1 & 0x0F) + 3):
                    b = window[(ref + k) & 0xFFF]
                    out.append(b)
                    window[pos] = b
                    pos = (pos + 1) & 0xFFF
    if len(out) != size:
        raise ValueError(f'LzS: {len(out)} bytes, cabeçalho diz {size}')
    return bytes(out)


def unpack(data: bytes) -> bytes:
    """Conteúdo descomprimido se for LzS; senão o próprio dado."""
    return decompress(data) if data[:4] == MAGIC else data
