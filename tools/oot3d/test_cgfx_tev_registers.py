"""Regressão de ctr_narc.py e cgfx_tev_registers.py com bytes sintéticos (sem dados do jogo).

    python3 -m unittest tools/oot3d/test_cgfx_tev_registers.py
"""
from __future__ import annotations

import os
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cgfx_tev_registers as cgfx  # noqa: E402
import ctr_narc  # noqa: E402


def put_ptr(buf: bytearray, at: int, target: int) -> None:
    struct.pack_into('<i', buf, at, target - at)


def put_dict(buf: bytearray, at: int, name_at: int, name: bytes, data_at: int) -> None:
    struct.pack_into('<4sII', buf, at, b'DICT', 0x2C, 1)
    struct.pack_into('<IHH', buf, at + 0xC, 0xFFFFFFFF, 1, 0)
    struct.pack_into('<IHH', buf, at + 0x1C, 0, 0, 1)
    put_ptr(buf, at + 0x24, name_at)
    put_ptr(buf, at + 0x28, data_at)
    buf[name_at:name_at + len(name) + 1] = name + b'\0'


STAGE0 = (0x0E3001E2, 0x00000003, 0x00010008, 0x00000001)  # source, operand, combine, scale
PASS = (0x000F000F, 0, 0, 0)


def build_cgfx(flags=1, alpha=0x8061, buffer=0x900, texcoord=1, header0=0x804F00C0,
               mappers=(True, False, True), proctex=False) -> bytes:
    buf = bytearray(0xA00)
    struct.pack_into('<4sHHIII', buf, 0, b'CGFX', 0xFEFF, 0x14, 0x05000000, len(buf), 1)
    struct.pack_into('<4sI', buf, 0x14, b'DATA', 0x100)
    struct.pack_into('<I', buf, 0x1C, 1)
    put_ptr(buf, 0x20, 0x100)
    put_dict(buf, 0x100, 0x180, b'Modelo', 0x200)
    struct.pack_into('<I4s', buf, 0x200, 0x40000092, b'CMDL')
    struct.pack_into('<I', buf, 0x200 + 0xBC, 1)
    put_ptr(buf, 0x200 + 0xC0, 0x300)
    put_dict(buf, 0x300, 0x380, b'Material', 0x400)
    m = 0x400
    struct.pack_into('<I4sI', buf, m, 0x08000000, b'MTOB', 0x06000001)
    struct.pack_into('<II', buf, m + 0x18, flags, texcoord)
    for unit, on in enumerate(mappers):
        if on:
            put_ptr(buf, m + 0x274 + unit * 4, 0x700)
    if proctex:
        put_ptr(buf, m + 0x280, 0x700)
    put_ptr(buf, m + 0x288, 0x800)
    fs = 0x800
    for stage, base in enumerate(cgfx.STAGE_BASE):
        source, operand, combine, scale = STAGE0 if stage == 0 else PASS
        header = header0 if stage == 0 else 0x804F0000 | base
        struct.pack_into('<7I', buf, fs + 0x2C + stage * 0x1C, 5, source, header, operand, combine,
                         0xFF000000, scale)
    struct.pack_into('<8I', buf, fs + 0xD4, alpha, 0x000F0104, 0xFFFFFFFF, 0x000F00FD,
                     buffer, 0x000200E0, 0x400, 0x000201C3)
    return bytes(buf)


def build_narc(files: list[tuple[bytes, bytes]]) -> bytes:
    btaf = bytearray(b'BTAF' + bytes(4) + struct.pack('<HH', len(files), 0))
    data = bytearray()
    for _, blob in files:
        btaf += struct.pack('<II', len(data), len(data) + len(blob))
        data += blob
        data += bytes(-len(data) % 4)
    struct.pack_into('<I', btaf, 4, len(btaf))
    names = bytearray(struct.pack('<IHH', 8, 0, 1))
    for name, _ in files:
        names += bytes([len(name)]) + name
    names += b'\0'
    names += bytes(-len(names) % 4)
    btnf = b'BTNF' + struct.pack('<I', 8 + len(names)) + names
    gmif = b'GMIF' + struct.pack('<I', 8 + len(data)) + data
    body = bytes(btaf) + btnf + gmif
    return b'NARC\xfe\xff\x00\x01' + struct.pack('<IHH', 0x10 + len(body), 0x10, 3) + body


class Yaz0NarcTests(unittest.TestCase):
    def test_yaz0_literal_and_overlapping_backref(self) -> None:
        # 'a','b' literais; cópia de 6 a distância 2 (sobreposta); cópia longa de 0x13 a distância 1; 'c'
        packed = b'Yaz0' + struct.pack('>I', 2 + 6 + 0x13 + 1) + bytes(8)
        packed += bytes([0b11001000]) + b'ab' + bytes([0x40, 0x01]) + bytes([0x00, 0x00, 0x01]) + b'c'
        self.assertEqual(ctr_narc.yaz0(packed), b'ab' * 4 + b'b' * 0x13 + b'c')

    def test_yaz0_rejects_reference_before_start(self) -> None:
        packed = b'Yaz0' + struct.pack('>I', 4) + bytes(8) + bytes([0]) + bytes([0x20, 0x05])
        with self.assertRaises(ValueError):
            ctr_narc.yaz0(packed)

    def test_szs_is_narc_not_sarc(self) -> None:
        # SM3DL: o .szs traz NARC (BTAF/BTNF/GMIF), não SARC.
        data = build_narc([(b'Modelo.bcmdl', b'CGFX1234'), (b'InitActor.byml', b'YB\x01\x00xyz')])
        packed = b'Yaz0' + struct.pack('>I', len(data)) + bytes(8)
        for k in range(0, len(data), 8):
            chunk = data[k:k + 8]
            packed += bytes([0xFF]) + chunk
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, 'x.szs')
            with open(path, 'wb') as f:
                f.write(packed)
            files = ctr_narc.open_szs(path)
        self.assertEqual(files, [('Modelo.bcmdl', b'CGFX1234'), ('InitActor.byml', b'YB\x01\x00xyz')])

    def test_narc_rejects_file_outside_gmif(self) -> None:
        data = bytearray(build_narc([(b'a.bin', b'1234')]))
        struct.pack_into('<I', data, 0x10 + 16, 0x1000)
        with self.assertRaises(ValueError):
            ctr_narc.narc(bytes(data))


class CgfxTevTests(unittest.TestCase):
    def collect(self, data: bytes):
        out = []
        models = cgfx.scan_cgfx(data, lambda *row: out.append(row))
        self.assertEqual(models, 1)
        self.assertEqual(len(out), 1)
        return out[0]

    def test_registers_are_copied_from_pica_commands(self) -> None:
        model, material, regs, lit, bad = self.collect(build_cgfx())
        self.assertEqual((model, material), ('Modelo', 'Material'))
        self.assertEqual((regs[0xC0], regs[0xC1], regs[0xC2], regs[0xC4]), STAGE0)
        self.assertEqual(regs[0xF8], 0x000F000F)
        self.assertNotIn(0xC3, regs)  # cor constante fica fora, como no CMB
        self.assertEqual(regs[0xE0], 0x900)
        self.assertEqual(regs[0x104], 0x8061)
        # unidades 0 e 2 com mapper, TexCoordConfig 0110 -> tex2 usa a coordenada 1
        self.assertEqual(regs[0x80], 0b101 | 1 << 13)
        self.assertTrue(lit)
        self.assertEqual(bad, [])

    def test_alpha_test_off_is_not_emitted_and_light_flag_off(self) -> None:
        _, _, regs, lit, _ = self.collect(build_cgfx(flags=0x10, alpha=0x10))
        self.assertNotIn(0x104, regs)
        self.assertFalse(lit)

    def test_procedural_texture_sets_bit_10(self) -> None:
        _, _, regs, _, bad = self.collect(build_cgfx(texcoord=3, proctex=True))
        self.assertEqual(regs[0x80] & 0x0700, 1 << 10 | 2 << 8)
        self.assertTrue(any('procedural' in item for item in bad))

    def test_wrong_offset_is_detected_by_command_header(self) -> None:
        with self.assertRaises(cgfx.CgfxError):
            self.collect(build_cgfx(header0=0x804F00C8))

    def test_out_of_range_fields_are_flagged(self) -> None:
        self.assertEqual(cgfx.validate(*STAGE0), [])
        self.assertIn('source', cgfx.validate(0x0000000A, 0, 0, 0))
        self.assertIn('combine', cgfx.validate(0x000F000F, 0, 0x0000000C, 0))
        self.assertIn('scale', cgfx.validate(0x000F000F, 0, 0, 3))
        self.assertIn('operand', cgfx.validate(0x000F000F, 0x00008000, 0, 0))

    def test_output_matches_cmb_format(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            szs = os.path.join(tmp, 'romfs', 'ObjectData')
            os.makedirs(szs)
            with open(os.path.join(szs, 'Obj.szs'), 'wb') as f:
                f.write(build_narc([(b'Obj.bcmdl', build_cgfx())]))
            out = os.path.join(tmp, 'saida', 'regs.txt')
            argv = sys.argv
            sys.argv = ['cgfx_tev_registers.py', os.path.join(tmp, 'romfs'), out]
            try:
                with open(os.devnull, 'w') as null:
                    stdout, sys.stdout = sys.stdout, null
                    try:
                        self.assertEqual(cgfx.main(), 0)
                    finally:
                        sys.stdout = stdout
            finally:
                sys.argv = argv
            with open(out) as f:
                line = f.read().split('\n')[0]
            with open(out + '.luz') as f:
                self.assertEqual(f.read(), 'm0000\n')
        self.assertTrue(line.startswith('m0000 80=2005 c0=e3001e2 c1=3 c2=10008 c4=1 c8=f000f '))
        self.assertTrue(line.endswith(' e0=900 f0=f000f f1=0 f2=0 f4=0 f8=f000f f9=0 fa=0 fc=0 104=8061'))


if __name__ == '__main__':
    unittest.main()
