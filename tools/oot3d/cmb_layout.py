# Leitura comum dos CMB da Grezzo para cmb_tev_census.py e cmb_tev_registers.py.
# OoT3D (versão 6): ponteiros mats/tex em 0x28/0x2C, material de 0x15C bytes.
# MM3D (versão 10): um bloco a mais ("qtrs") antes de mats desloca os ponteiros para 0x2C/0x30
# e o material tem 0x16C bytes (os 0x10 a mais ficam no fim; contador e índices do TEV em
# 0x120/0x124, luz de fragmento em 0x00, texturas em 0x08 e alpha test em 0x130 não mudam).
# Versão desconhecida usa o leiaute do OoT3D (o que as ferramentas faziam antes).
import struct

import grezzo_lzs

EXTENSIONS = ('.zar', '.zsi', '.gar', '.cmb', '.lzs')
LAYOUTS = {6: (0x28, 0x15C), 10: (0x2C, 0x16C)}


def read(path: str) -> bytes:
    """Conteúdo do arquivo, descomprimido se for LzS (MM3D)."""
    with open(path, 'rb') as f:
        return grezzo_lzs.unpack(f.read())


def layout(c: bytes) -> tuple[int, int, int]:
    """(início de mats, início de tex, tamanho do material) de um CMB que começa em c[0]."""
    pointer, stride = LAYOUTS.get(struct.unpack_from('<I', c, 0x08)[0], LAYOUTS[6])
    mats, tex = struct.unpack_from('<2I', c, pointer)
    return mats, tex, stride
