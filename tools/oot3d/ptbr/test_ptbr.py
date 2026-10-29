"""Regressões do patch PT-BR (dados sintéticos, sem nada do jogo): python -I test_ptbr.py"""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from conv import converter
from exportar import texto
from importar import para_bytes


class Conversor(unittest.TestCase):
    def test_acentos_e_botao(self):
        # "Ação" + botão A do N64 (0x9f) + fim (0x02)
        self.assertEqual(converter(b'A\x94\x93o \x9f\x02', b''), 'Ação '.encode() + b'\x7f\x24\x06\x7f\x00')

    def test_caixa_nova_comeca_com_quebra(self):
        # 0x04 do N64 vira 7f01 e o 3DS precisa do 7f1c logo depois
        self.assertEqual(converter(b'a\x04b\x02', b''), b'a\x7f\x01\x7f\x1cb\x7f\x00')

    def test_som_usa_argumento_do_3ds(self):
        # 0x12 (som) do N64 → 7f0e com os 6 bytes de argumento da mensagem inglesa do 3DS
        en = b'x\x7f\x0e' + bytes(range(1, 7)) + b'\x7f\x00'
        self.assertEqual(converter(b'\x12\x28\x0e' + b'y\x02', en), b'\x7f\x0e' + bytes(range(1, 7)) + b'y\x7f\x00')


class IdaEVolta(unittest.TestCase):
    def test_texto_para_bytes(self):
        b = 'Olá,'.encode() + b'\x7f\x1c' + 'Link! '.encode() + b'\x7f\x24\x07' + 'Pegue a espada.'.encode() + b'\x7f\x00'
        s = texto(b)
        self.assertIn('⟦7f2407⟧', s)
        self.assertEqual(para_bytes(s), b)


if __name__ == '__main__':
    unittest.main()
