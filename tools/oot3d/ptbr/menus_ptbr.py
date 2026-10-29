"""Gera as texturas de menu em PT-BR (por enquanto: HUD)."""
import struct, sys
sys.path.insert(0, '.')
from ctxb import decodificar, ler, CTXB_HEADER_SIZE, encode_rgba4_tiled, cabecalho
from rotulos import rotulo
from caminhos import TRABALHO

HUD = [  # colunas de 64 px, linhas de 18 px a partir de y=92 (mesma ordem do original)
    ['Atacar', 'Olhar', 'Entrar', 'Voltar', 'Abrir', 'Pular', 'Decidir', 'Mergulhar', 'Galopar'],
    ['Lançar', 'Navi', 'Subir', 'Soltar', 'Descer', 'Salvar', 'Falar', 'Próximo', 'Agarrar'],
    ['Sair', 'Guardar', 'Puxar', 'Trocar', 'Equipar'],
]


def hud():
    im = decodificar(ler('06_EU_SPANISH', 'hud_all00'))
    im.paste((0, 0, 0, 0), (0, 92, 192, 256))               # grade de palavras inteira
    for c, coluna in enumerate(HUD):
        for r, palavra in enumerate(coluna):
            y = 92 + 18 * r
            rotulo(im, (64 * c, y, 64 * c + 64, y + 18), palavra, topo=y + 5, limpar=False)
    rotulo(im, (0, 58, 200, 88), 'FIM DE JOGO', altura_maiuscula=17, cor=(238, 238, 238, 255),
           espessura=2, peso='serifa', alinhar='esquerda', topo=63, sombra=1)
    return im


def codificar(nome_original, im):
    orig = ler('06_EU_SPANISH', nome_original); w, h, fmt = cabecalho(orig)
    assert fmt == 4 and im.size == (w, h)
    return orig[:CTXB_HEADER_SIZE] + encode_rgba4_tiled(im.tobytes(), w, h)


if __name__ == '__main__':
    S = str(TRABALHO) + '/'
    im = hud(); im.save(S + 'hud_ptbr.png')
    from PIL import Image
    ver = Image.new('RGBA', im.size, (40, 40, 60, 255)); ver.alpha_composite(im)
    ver.resize((512, 512), Image.NEAREST).convert('RGB').save(S + 'hud_ptbr.ver.png')


def gravar_na_romfs(romfs, nome, ctxb_bytes):
    """Substitui menu/06_EU_SPANISH e 07_US_SPANISH/<nome>.ctxb, mesmo tamanho."""
    from romfs import localizar
    for idioma in ('06_EU_SPANISH', '07_US_SPANISH'):
        o, tam = localizar(romfs, f'menu/{idioma}/{nome}.ctxb')
        assert tam == len(ctxb_bytes), (nome, tam, len(ctxb_bytes))
        with open(romfs, 'r+b') as f:
            f.seek(o); f.write(ctxb_bytes)
