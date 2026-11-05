"""PT-BR patch regressions (synthetic data, nothing from the game): python -I test_ptbr.py"""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from conv import convert
from export import text
from importer import to_bytes


class Converter(unittest.TestCase):
    def test_accents_and_button(self):
        # "Ação" + N64 A button (0x9f) + end (0x02)
        self.assertEqual(convert(b'A\x94\x93o \x9f\x02', b''), 'Ação '.encode() + b'\x7f\x24\x06\x7f\x00')

    def test_new_box_starts_with_line_break(self):
        # N64 0x04 becomes 7f01 and the 3DS needs 7f1c right after it
        self.assertEqual(convert(b'a\x04b\x02', b''), b'a\x7f\x01\x7f\x1cb\x7f\x00')

    def test_sound_uses_3ds_argument(self):
        # N64 0x12 (sound) → 7f0e with the 6 argument bytes from the 3DS English message
        en = b'x\x7f\x0e' + bytes(range(1, 7)) + b'\x7f\x00'
        self.assertEqual(convert(b'\x12\x28\x0e' + b'y\x02', en), b'\x7f\x0e' + bytes(range(1, 7)) + b'y\x7f\x00')


class RoundTrip(unittest.TestCase):
    def test_text_to_bytes(self):
        b = 'Olá,'.encode() + b'\x7f\x1c' + 'Link! '.encode() + b'\x7f\x24\x07' + 'Pegue a espada.'.encode() + b'\x7f\x00'
        s = text(b)
        self.assertIn('⟦7f2407⟧', s)
        self.assertEqual(to_bytes(s), b)


class TextureLA8(unittest.TestCase):
    def test_round_trip(self):
        import struct
        from PIL import Image
        from ctxb import encode_ctxb, decode
        header = bytearray(0x48); struct.pack_into('<HHHH', header, 0x2C, 16, 8, 26456, 5121)   # LA8 16x8
        im = Image.new('LA', (16, 8))
        im.putdata([((x * 16) % 256, (y * 32) % 256) for y in range(8) for x in range(16)])
        ctxb = encode_ctxb(bytes(header) + bytes(16 * 8 * 2), im.convert('RGBA'))
        self.assertEqual(decode(ctxb).convert('LA').tobytes(), im.tobytes())


if __name__ == '__main__':
    unittest.main()
