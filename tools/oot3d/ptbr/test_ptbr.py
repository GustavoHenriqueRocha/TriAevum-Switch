"""PT-BR patch regressions (synthetic data, nothing from the game): python -I test_ptbr.py"""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from conv import convert
from export import text
from importer import to_bytes
from msgs import grammar_error


class Converter(unittest.TestCase):
    def test_accents_and_button(self):
        # "Ação" + N64 A button (0x9f) + end (0x02)
        self.assertEqual(convert(b'A\x94\x93o \x9f\x02', b''), 'Ação '.encode() + b'\x7f\x24\x06\x7f\x00')

    def test_new_box_starts_with_line_break(self):
        # N64 0x04 becomes 7f01 and the 3DS needs 7f1c right after it
        self.assertEqual(convert(b'a\x04b\x02', b''), b'a\x7f\x01\x7f\x1cb\x7f\x00')

    def test_sound_uses_3ds_argument(self):
        # N64 0x12 (sound) → 7f0e with the 5 argument bytes from the 3DS English message (the game's table)
        en = b'x\x7f\x0e' + bytes(range(1, 6)) + b'z\x7f\x00'
        self.assertEqual(convert(b'\x12\x28\x0e' + b'y\x02', en), b'\x7f\x0e' + bytes(range(1, 6)) + b'y\x7f\x00')

    def test_sound_followed_by_a_code_stays_parsable(self):
        # With 6 argument bytes the 7f of the next English code ended up in the PT-BR sound code, and the game read
        # the following letter as a code ("7f 4f"), past its table.
        en = b'\x7f\x0e' + bytes(range(1, 6)) + b'\x7f\x1d\x41Hey\x7f\x1d\x00\x7f\x00'
        out = convert(b'\x12\x28\x0eOi\x02', en)
        self.assertEqual(out, b'\x7f\x0e' + bytes(range(1, 6)) + b'Oi\x7f\x00')
        self.assertIsNone(grammar_error(out))

    def test_persistent_takes_3ds_argument(self):
        # N64 0x0a has no argument; the 3DS 7f06 reads one byte, taken from the English message
        self.assertEqual(convert(b'\x0ac\x02', b'a\x7f\x06\x00b\x7f\x00'), b'\x7f\x06\x00c\x7f\x00')


class Grammar(unittest.TestCase):
    def test_parsable_messages(self):
        self.assertIsNone(grammar_error(b'ok\x7f\x00'))
        self.assertIsNone(grammar_error(b'\x7f\x0d\x01\x02\x03\x04\x05\x7f\x1b' + bytes(6) + b'\x7f\x00'))

    def test_what_the_game_cannot_parse(self):
        self.assertIn('unknown code 7f41', grammar_error(b'\x7f\x0e\x01\x02\x03\x04\x05\x7f\x41b\x7f\x00'))
        self.assertIn('unknown code 7f7f', grammar_error(b'\x7f\x7f\x1c\x7f\x00'))
        self.assertIn('without its 5 argument bytes', grammar_error(b'\x7f\x0e\x01\x02'))
        self.assertEqual(grammar_error(b'ok'), 'no 7f00 at the end')
        self.assertIn('bytes after 7f00', grammar_error(b'\x7f\x00x'))


class RoundTrip(unittest.TestCase):
    def test_text_to_bytes(self):
        b = 'Olá,'.encode() + b'\x7f\x1c' + 'Link! '.encode() + b'\x7f\x24\x07' + 'Pegue a espada.'.encode() + b'\x7f\x00'
        s = text(b)
        self.assertIn('⟦7f2407⟧', s)
        self.assertEqual(to_bytes(s), b)

    def test_sound_then_code_round_trip(self):
        # The export shows the code after a sound as a code, not as stray characters
        b = b'\x7f\x0e' + bytes(range(1, 6)) + b'\x7f\x1d\x41Hey\x7f\x1d\x00\x7f\x00'
        s = text(b)
        self.assertEqual(s, '⟦7f0e0102030405⟧⟦7f1d41⟧Hey⟦7f1d00⟧')
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
