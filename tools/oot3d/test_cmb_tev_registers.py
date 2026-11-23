"""Regressão de grezzo_lzs.py, cmb_layout.py, cmb_tev_registers.py e cmb_tev_census.py com
bytes sintéticos (sem dados do jogo).

Bug original (23/11): o censo e os registradores do OoT3D liam só o CMB versão 6 (ponteiros em
0x28, material de 0x15C) e só arquivos .zar/.zsi/.gar/.cmb sem compressão. No MM3D o CMB é
versão 10 (ponteiros em 0x2C, material de 0x16C) e boa parte dos modelos vem em LzS
(.gar.lzs, .zsi): os materiais sumiam calados.

    python3 -m unittest tools/oot3d/test_cmb_tev_registers.py
"""
from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cmb_tev_registers as regs  # noqa: E402
import grezzo_lzs  # noqa: E402

# Um estágio: cor = MODULATE(textura0, primária, constante); alfa = REPLACE(textura0).
COMBINER = struct.pack('<18HI',
                       0x2100, 0x1E01, 1, 1, 0x8578, 0x8578,
                       0x84C0, 0x8577, 0x8576, 0x300, 0x300, 0x300,
                       0x84C0, 0x8577, 0x8576, 0x302, 0x302, 0x302, 0)
# Registradores esperados, calculados à mão pela tabela do PICA (3dbrew GPU/Internal Registers):
# fonte 3|0<<4|14<<8 nas duas metades, MODULATE=1, buffer PREVIOUS nos bits 8 e 12,
# uma textura, alpha test GREATER (6) com referência 0x80.
EXPECTED = 'm0000 80=1 c0=e030e03 c1=0 c2=1 c4=0 e0=1100 104=8061'


def build_cmb(version: int) -> bytes:
    pointer, stride = {6: (0x28, 0x15C), 10: (0x2C, 0x16C)}[version]
    mats = 0x60
    material = bytearray(stride)
    struct.pack_into('<I', material, 0x08, 1)          # texturas usadas
    struct.pack_into('<IH', material, 0x120, 1, 0)     # 1 estágio, combiner 0
    struct.pack_into('<BBH', material, 0x130, 1, 0x80, 0x204)  # alpha test GL_GREATER
    chunk = b'mats' + struct.pack('<II', 0, 1) + bytes(material) + COMBINER
    tex = mats + len(chunk)
    body = bytearray(tex + 0x10)
    struct.pack_into('<4sII', body, 0, b'cmb ', len(body), version)
    struct.pack_into('<2I', body, pointer, mats, tex)
    body[mats:tex] = chunk
    body[tex:tex + 4] = b'tex '
    return bytes(body)


def lzs_literals(data: bytes) -> bytes:
    """LzS só com literais (flags 0xFF): basta para testar o caminho de leitura."""
    out = bytearray(b'LzS\x01' + struct.pack('<3I', 0, len(data), 0))
    for i in range(0, len(data), 8):
        piece = data[i:i + 8]
        out.append(0xFF)
        out += piece
    struct.pack_into('<I', out, 12, len(out) - 0x10)
    return bytes(out)


def run_registers(files: dict[str, bytes]) -> list[str]:
    with tempfile.TemporaryDirectory() as root:
        for name, data in files.items():
            with open(os.path.join(root, name), 'wb') as f:
                f.write(data)
        output = os.path.join(root, 'saida', 'regs.txt')
        os.makedirs(os.path.dirname(output))
        sys.argv = ['cmb_tev_registers.py', root, output]
        regs.main()
        with open(output) as f:
            return f.read().splitlines()


class LzsTest(unittest.TestCase):
    def test_reference_copies_from_ring_window(self):
        # ABC literais em 0xFEE..0xFF0, depois referência a 0xFEE com 6 bytes (sobreposta).
        stream = b'LzS\x01' + struct.pack('<3I', 0, 9, 6) + bytes([0x07, 0x41, 0x42, 0x43, 0xEE, 0xF3])
        self.assertEqual(grezzo_lzs.decompress(stream), b'ABCABCABC')

    def test_window_starts_zeroed(self):
        # Referência a 0x000 antes de qualquer escrita lá: 3 zeros.
        stream = b'LzS\x01' + struct.pack('<3I', 0, 3, 3) + bytes([0x00, 0x00, 0x00])
        self.assertEqual(grezzo_lzs.decompress(stream), b'\0\0\0')

    def test_size_mismatch_is_an_error(self):
        stream = b'LzS\x01' + struct.pack('<3I', 0, 5, 2) + bytes([0x01, 0x41])
        with self.assertRaises(ValueError):
            grezzo_lzs.decompress(stream)

    def test_unpack_leaves_other_data_alone(self):
        self.assertEqual(grezzo_lzs.unpack(b'ZAR\x01abc'), b'ZAR\x01abc')


class CmbRegistersTest(unittest.TestCase):
    def test_oot3d_version_6(self):
        self.assertEqual(run_registers({'a.zsi': b'ZSI\x01' + build_cmb(6)}), [EXPECTED])

    def test_mm3d_version_10(self):
        self.assertEqual(run_registers({'a.zsi': b'ZSI\t' + build_cmb(10)}), [EXPECTED])

    def test_mm3d_lzs_archive(self):
        self.assertEqual(run_registers({'b.gar.lzs': lzs_literals(b'GAR\x02' + build_cmb(10))}), [EXPECTED])

    def test_census_counts_both_versions(self):
        with tempfile.TemporaryDirectory() as root:
            for name, data in (('a.zar', build_cmb(6)), ('b.zsi', lzs_literals(build_cmb(10)))):
                with open(os.path.join(root, name), 'wb') as f:
                    f.write(data)
            out = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'cmb_tev_census.py'), root],
                                 capture_output=True, text=True, check=True).stdout
        self.assertIn('modelos=2 materiais=2 falhas=0', out)
        self.assertIn('TEV únicos (só combiners)=1', out)


if __name__ == '__main__':
    unittest.main()
