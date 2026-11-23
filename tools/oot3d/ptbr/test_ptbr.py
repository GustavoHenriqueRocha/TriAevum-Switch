"""Regressões do patch PT-BR (dados sintéticos, sem nada do jogo): python -I test_ptbr.py"""
import struct, sys, unittest
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


def _gar_feito_a_mao():
    """GAR v2 com dois tipos ('unknown' vazio e 'ctxb') e dois membros de 5 e 8 bytes, montado campo a campo."""
    import struct
    b = bytearray(0xA0)
    struct.pack_into('<4sIHHIII8s', b, 0, b'GAR\x02', 0xA0, 2, 2, 0x20, 0x58, 0x88, b'jenkins')
    struct.pack_into('<IIII', b, 0x20, 0, 0xFFFFFFFF, 0x40, 0xFFFFFFFF)      # 'unknown': nenhum membro
    struct.pack_into('<IIII', b, 0x30, 2, 0x48, 0x50, 0xFFFFFFFF)            # 'ctxb': índices 0 e 1
    b[0x40:0x48] = b'unknown\0'; struct.pack_into('<II', b, 0x48, 0, 1); b[0x50:0x55] = b'ctxb\0'
    struct.pack_into('<III', b, 0x58, 5, 0x77, 0x70)                          # tamanho, raiz, nome inteiro
    struct.pack_into('<III', b, 0x64, 8, 0x84, 0x7C)
    b[0x70:0x77] = b'a.ctxb\0'; b[0x77:0x79] = b'a\0'; b[0x7C:0x84] = b'bb.ctxb\0'; b[0x84:0x87] = b'bb\0'
    struct.pack_into('<II', b, 0x88, 0x90, 0x98)
    b[0x90:0x95] = b'AAAAA'; b[0x98:0xA0] = b'BBBBBBBB'
    return bytes(b)


def _ctxb_feito_a_mao(w, h, fmt, tipo, payload):
    """CTXB com uma textura: cabeçalho de 0x48 bytes como o das texturas do MM3D."""
    import struct
    c = bytearray(0x48)
    struct.pack_into('<4sIIIII', c, 0, b'ctxb', 0x48 + len(payload), 1, 0, 0x18, 0x48)
    struct.pack_into('<4sIII', c, 0x18, b'tex ', 0x30, 1, len(payload))
    struct.pack_into('<HHHHHHI', c, 0x28, 1, 1, w, h, fmt, tipo, 0)
    return bytes(c) + payload


# 4 blocos ETC1 (modo individual, tabela 0): cores-base 4 bits diferentes por bloco, índices variados
_ETC1_8x8 = b''.join(struct.pack('<Q', c << 60 | c << 56 | c << 52 | c << 48 | c << 44 | c << 40 | idx)
                     for c, idx in ((0x2, 0x0000FFFF), (0x5, 0x12345678), (0x9, 0xFFFF0000), (0xC, 0x0F0F0F0F)))


class Mm3dGar(unittest.TestCase):
    def test_le_e_remonta_igual(self):
        import gar
        b = _gar_feito_a_mao(); m = gar.ler(b)
        self.assertEqual(m['membros'], [('a.ctxb', b'AAAAA'), ('bb.ctxb', b'BBBBBBBB')])
        self.assertEqual(gar.montar(m), b)

    def test_troca_mesmo_tamanho_muda_so_o_membro(self):
        import gar
        b = _gar_feito_a_mao(); m = gar.ler(b)
        novo = gar.montar(dict(m, membros=[m['membros'][0], ('bb.ctxb', b'CCCCCCCC')]))
        self.assertEqual(len(novo), len(b))
        self.assertEqual([i for i in range(len(b)) if b[i] != novo[i]], list(range(0x98, 0xA0)))

    def test_recusa_fora_do_padrao(self):
        import gar
        b = bytearray(_gar_feito_a_mao()); b[0x95] = 1                 # lixo no enchimento: não remonta igual
        with self.assertRaises(ValueError):
            gar.ler(bytes(b))


class Mm3dCtxb(unittest.TestCase):
    def test_etc1_sem_mudanca_volta_igual(self):
        from ctxb import codificar_blocos, decodificar
        orig = _ctxb_feito_a_mao(8, 8, 0x675A, 0, _ETC1_8x8)
        self.assertEqual(codificar_blocos(orig, decodificar(orig)), orig)

    def test_etc1_so_o_bloco_mexido_muda(self):
        from ctxb import codificar_blocos, decodificar
        orig = _ctxb_feito_a_mao(8, 8, 0x675A, 0, _ETC1_8x8)
        im = decodificar(orig); im.paste((200, 30, 30, 255), (4, 4, 8, 8))   # bloco (4,4) = 4º na ordem PICA
        novo = codificar_blocos(orig, im)
        self.assertEqual(novo[:0x48 + 24], orig[:0x48 + 24])
        self.assertNotEqual(novo[0x48 + 24:], orig[0x48 + 24:])
        # o teste pega troca de canal, de ordem de bytes ou de bloco (erro > 100); a cor exata não: ETC1 tem base
        # de 5 bits e modificador (nunca zero) igual nos 3 canais, e o etcpak (rápido) erra até 8 neste bloco liso
        px = decodificar(novo).getpixel((6, 6))
        self.assertTrue(all(abs(a - b) <= 10 for a, b in zip(px, (200, 30, 30, 255))), px)

    def test_bloco_conhecido_reaproveita_bytes(self):
        from ctxb import codificar_blocos, decodificar
        orig = _ctxb_feito_a_mao(8, 8, 0x675A, 0, _ETC1_8x8)
        dono = decodificar(_ctxb_feito_a_mao(8, 8, 0x675A, 0, _ETC1_8x8[8:16] * 4))   # o bloco 2 em todo lugar
        im = decodificar(orig); im.paste(dono.crop((0, 0, 4, 4)), (0, 0))
        conhecidos = {dono.crop((0, 0, 4, 4)).tobytes(): _ETC1_8x8[8:16]}
        novo = codificar_blocos(orig, im, conhecidos)
        self.assertEqual(novo[0x48:0x50], _ETC1_8x8[8:16])
        self.assertEqual(novo[0x50:], orig[0x50:])

    def test_etc1a4_alfa_e_volta(self):
        import struct
        from ctxb import codificar_blocos, decodificar
        p = b''.join(struct.pack('<Q', 0xFEDCBA9876543210 >> k | 0xF) + _ETC1_8x8[k // 4 * 8:k // 4 * 8 + 8]
                     for k in (0, 4, 8, 12))
        orig = _ctxb_feito_a_mao(8, 8, 0x675B, 0, p)
        self.assertEqual(codificar_blocos(orig, decodificar(orig)), orig)

    def test_l8_ida_e_volta(self):
        from ctxb import codificar_blocos, decodificar
        orig = _ctxb_feito_a_mao(8, 8, 0x6757, 0x1401, bytes(range(0, 256, 4)))
        im = decodificar(orig)
        self.assertEqual(im.getpixel((0, 0)), (0, 0, 0, 255))
        self.assertEqual(codificar_blocos(orig, im), orig)

    def test_rgba4_ida_e_volta(self):
        from ctxb import codificar_blocos, decodificar
        orig = _ctxb_feito_a_mao(8, 8, 0x6752, 0x8033, bytes((i * 37) % 256 for i in range(128)))
        self.assertEqual(codificar_blocos(orig, decodificar(orig)), orig)


class Mm3dPlaca(unittest.TestCase):
    def test_bloco_modal(self):
        from mm3d_texturas import blocos_modais
        a, b, c = (bytes([k]) * 8 for k in (1, 2, 3))
        moda, freq = blocos_modais([a + b, a + c, a + b], 8)
        self.assertEqual(moda, [a, b])
        self.assertEqual(freq, [3, 2])


class Mm3dMenu(unittest.TestCase):
    """Apagar rótulo de menu: botão em degradê com letra clara, contorno escuro e ícone ao lado (dado feito à mão)."""

    def _botao(self):
        from PIL import Image
        im = Image.new('RGBA', (40, 12), (0, 0, 0, 0)); px = im.load()
        for y in range(12):
            for x in range(40): px[x, y] = (200 - 8 * y, 90, 20, 255)          # degradê vertical
        for y in range(3, 9):                                                  # "texto": branco com borda preta
            for x in range(12, 30): px[x, y] = (0, 0, 0, 255) if x in (12, 29) or y in (3, 8) else (255, 255, 255, 255)
        for y in range(3, 9): px[8, y] = px[9, y] = (0, 0, 0, 255)  # ícone igual em todos os idiomas, na margem lida
        return im

    def test_apagar_botao_mantem_degrade(self):
        from mm3d_texturas import _apagar
        im = self._botao()
        dif = [[12 <= x < 30 and 3 <= y < 9 for x in range(40)] for y in range(12)]
        _apagar(im, (12, 3, 30, 9), dif, folga=0)
        for y in range(3, 9):
            for x in range(12, 30): self.assertEqual(im.getpixel((x, y)), (200 - 8 * y, 90, 20, 255), (x, y))
        self.assertEqual(im.getpixel((8, 5)), (0, 0, 0, 255))                  # o ícone fora da caixa fica

    def test_apagar_fundo_transparente(self):
        from PIL import Image
        from mm3d_texturas import _apagar
        im = Image.new('RGBA', (20, 10), (0, 0, 0, 0)); im.paste((255, 255, 255, 255), (5, 3, 15, 7))
        _apagar(im, (5, 3, 15, 7), [[False] * 20 for _ in range(10)], folga=0)
        self.assertEqual(im.getbbox(), None)

    def test_apagar_transparente_com_rotulo_vizinho(self):
        # "Visões" encostado em "Canção do Tempo Duplo", entre dois frisos: o que decide é o fundo das linhas do texto,
        # fora a letra do vizinho (que muda por idioma)
        from PIL import Image
        from mm3d_texturas import _apagar
        im = Image.new('RGBA', (30, 10), (0, 0, 0, 0))
        im.paste((255, 255, 255, 255), (0, 1, 12, 9)); im.paste((255, 255, 255, 255), (14, 3, 24, 7))
        im.paste((49, 49, 15, 255), (0, 0, 30, 1)); im.paste((49, 49, 15, 255), (0, 9, 30, 10))  # frisos de cima e de baixo
        dif = [[(x < 12 and 1 <= y < 9) or (14 <= x < 24 and 3 <= y < 7) for x in range(30)] for y in range(10)]
        _apagar(im, (14, 3, 24, 7), dif, folga=1)
        self.assertEqual(im.crop((13, 2, 25, 8)).getbbox(), None)

    def test_estilo_letra_clara_contorno_escuro(self):
        from PIL import Image
        from mm3d_texturas import _estilo_rotulo
        orig = self._botao(); limpa = Image.new('RGBA', orig.size, (0, 0, 0, 0)); limpa.paste(orig)
        lp = limpa.load()
        for y in range(3, 9):
            for x in range(12, 30): lp[x, y] = (200 - 8 * y, 90, 20, 255)
        letra, cont, tem, topo, fim, traco = _estilo_rotulo(orig, limpa, (12, 3, 30, 9))
        self.assertTrue(tem)
        self.assertEqual(letra[:3], (255, 255, 255)); self.assertEqual(cont[:3], (0, 0, 0))
        self.assertEqual((topo, fim), (3, 9))
        self.assertEqual(traco, 18)                                            # linhas cheias de 18 px

    def test_traco(self):
        from mm3d_texturas import _traco
        cheio = {(1, 0), (2, 0), (5, 0), (6, 0), (7, 0), (8, 0)}
        self.assertEqual(_traco(lambda x, y: (x, y) in cheio, 0, 10, [0]), 3)  # sequências de 2 e 4


if __name__ == '__main__':
    unittest.main()
