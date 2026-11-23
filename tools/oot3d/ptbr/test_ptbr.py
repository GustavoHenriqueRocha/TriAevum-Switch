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


class TexturaLA8(unittest.TestCase):
    def test_ida_e_volta(self):
        import struct
        from PIL import Image
        from ctxb import codificar_ctxb, decodificar
        cab = bytearray(0x48); struct.pack_into('<HHHH', cab, 0x2C, 16, 8, 26456, 5121)   # LA8 16x8
        im = Image.new('LA', (16, 8))
        im.putdata([((x * 16) % 256, (y * 32) % 256) for y in range(8) for x in range(16)])
        ctxb = codificar_ctxb(bytes(cab) + bytes(16 * 8 * 2), im.convert('RGBA'))
        self.assertEqual(decodificar(ctxb).convert('LA').tobytes(), im.tobytes())


def _gmsg_arquivo(payloads, idioma=0x80, ids=None):
    import struct
    import gmsg
    ids = ids or list(range(len(payloads)))
    ents = [[mid, 0xffff, 0x3fffffff, 0, 0xff, 0, 0, 0] for mid in ids]
    return gmsg.montar(b'GMSG' + struct.pack('<III', 1, len(ents), idioma), ents, payloads)


class Mm3dGmsg(unittest.TestCase):
    """Formato de mensagens do Majora's Mask 3D (message/eu/eu?.gmsg), montado à mão."""
    # 'Hi' 7f [pad] 3a:0001 'AB' 7f [pad] 32 [pad até 4] u32 | 7f [pad] 01 | 7f [pad] 20 0007 'Rupee\0' 'Rupees\0' [pad]
    # 'x' 7f 00 00 (fim, sem pad porque o 7f caiu em posição ímpar)
    CRU = (b'Hi\x7f\x00\x3a\x00\x01\x00' b'AB\x7f\x00\x32\x00\x00\x00\x49\x08\x00\x01'
           b'\x7f\x00\x01\x00' b'\x7f\x00\x20\x00\x07\x00Rupee\x00Rupees\x00\x00' b'x\x7f\x00\x00')
    TEXTO = 'Hi⟦3a:0001⟧AB⟦32:01000849⟧\n⟦20:0007:Rupee:Rupees⟧x'

    def test_texto_e_volta_com_alinhamentos(self):
        import gmsg
        self.assertEqual(len(self.CRU), 48)
        self.assertEqual(gmsg.texto(self.CRU), self.TEXTO)
        self.assertEqual(gmsg.para_bytes(self.TEXTO), self.CRU)

    def test_plural_completa_cada_palavra(self):
        # bytes do eue.gmsg real: "hour" (5 com o zero) ganha enchimento antes de "hours"; o upstream lia o plural vazio
        import gmsg
        cru = b'\x7f\x00\x20\x00\x16\x00hour\x00\x00hours\x00\x7f\x00\x00\x00'
        self.assertEqual(gmsg.texto(cru), '⟦20:0016:hour:hours⟧')
        self.assertEqual(gmsg.para_bytes('⟦20:0016:hour:hours⟧'), cru)

    def test_arquivo_remontado_igual(self):
        import gmsg
        arq = _gmsg_arquivo([self.CRU, b'Oi\x7f\x00\x00\x00', b''])
        cab, ents, pls = gmsg.ler(arq)
        self.assertEqual(gmsg.montar(cab, ents, [gmsg.para_bytes(gmsg.texto(p)) if p else b'' for p in pls]), arq)
        self.assertEqual([e[6] % 4 for e in ents if e[7]], [0, 0])
        self.assertEqual(len(arq) % 4, 0)

    def test_texto_maior_realinha(self):
        import gmsg
        cab, ents, pls = gmsg.ler(_gmsg_arquivo([b'Oi\x7f\x00\x00\x00', self.CRU]))
        novo = gmsg.montar(cab, ents, [gmsg.para_bytes('Olá, tudo bem?'), pls[1]])
        _, ents2, pls2 = gmsg.ler(novo)
        self.assertEqual(gmsg.texto(pls2[0]), 'Olá, tudo bem?')
        self.assertEqual(pls2[1], self.CRU)
        self.assertEqual(ents2[1][6] % 4, 0)

    def test_argumentos_errados_e_lixo(self):
        import gmsg
        with self.assertRaises(gmsg.ErroGmsg):
            gmsg.para_bytes('⟦3a⟧')                       # cor sem argumento
        with self.assertRaises(gmsg.ErroGmsg):
            gmsg.texto(b'a\x7f\x00\x00\x00zz')           # fim antes do tamanho

    def test_aplicar_segue_ingles_ou_espanhol(self):
        import gmsg
        from mm3d import aplicar
        en = _gmsg_arquivo([gmsg.para_bytes(t) for t in
                            ['You got ⟦3a:0001⟧Bombs⟦3a:000b⟧!⟦31:0000⟧', 'You got ⟦3a:0001⟧Bombs⟦3a:000b⟧!⟦31:0000⟧',
                             'Your ⟦09⟧⟦22:8009⟧ one!', 'Untouched']], idioma=0x10)
        es = _gmsg_arquivo([gmsg.para_bytes(t) for t in
                            ['¡Bombas!⟦31:0000⟧', '¡Bombas!⟦31:0000⟧', '¡La ⟦09⟧⟦22:0009⟧!', 'Intacto']])
        fonte = {ord(c) for c in 'VocêobtevBmas!Éa ⟦⟧0123456789'}
        trad = {'0x0000': 'Você obteve ⟦3a:0001⟧Bombas⟦3a:000b⟧!⟦31:0000⟧', '0x0002': 'É a ⟦09⟧⟦22:0009⟧!'}
        novo, trocados, prob = aplicar(en, es, trad, fonte)
        self.assertEqual(prob, [])
        self.assertEqual(sorted(trocados), [0, 1, 2])           # o id 1 tem o mesmo inglês do 0
        _, _, pls = gmsg.ler(novo); _, _, orig = gmsg.ler(es)
        self.assertEqual(gmsg.texto(pls[1]), trad['0x0000'])
        self.assertEqual(pls[3], orig[3])                       # sem tradução: byte a byte igual
        _, _, prob = aplicar(en, es, {'0x0002': 'É a ⟦09⟧⟦22:1234⟧!', '0x0000': 'Ação⟦31:0000⟧'}, fonte)
        self.assertEqual([p[0] for p in prob], ['0x0000', '0x0001', '0x0002'])   # o 1 herda a do 0

    def test_glifos_gzf(self):
        import struct
        import gmsg
        d = bytearray(0x38 + 24)
        d[:4] = b'GZFX'; struct.pack_into('<III', d, 0x0c, 12, 1, 2)
        struct.pack_into('<I', d, 0x38, 0x20); struct.pack_into('<I', d, 0x38 + 12, 0xe3)
        self.assertEqual(gmsg.glifos_gzf(bytes(d)), {0x20, 0xe3})


if __name__ == '__main__':
    unittest.main()
