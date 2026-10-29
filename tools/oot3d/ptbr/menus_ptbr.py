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


SANS_NEGRITO = '/usr/share/fonts/liberation/LiberationSans-Bold.ttf'
# selos girados de menu_hint_movie_parts00: caixa no original, texto, cor
SELOS = [((393, 0, 447, 39), 'Completo', (32, 166, 32, 255), 9), ((458, 4, 501, 37), 'Novo', (255, 0, 0, 255), 12)]


def selos(im, angulo=33, E=4):
    """Redesenha os selos "Completado"/"Nuevo": texto colorido, contorno branco, girado como o original."""
    from PIL import Image, ImageDraw, ImageFont
    im = im.copy()
    for (x0, y0, x1, y1), texto, cor, altura in SELOS:
        im.paste((0, 0, 0, 0), (x0, y0, x1, y1))
        f = ImageFont.truetype(SANS_NEGRITO, int(altura * E / 0.72))
        l, t, r, b = f.getbbox(texto, stroke_width=int(1.6 * E))
        camada = Image.new('RGBA', (r - l + 4 * E, b - t + 4 * E), (0, 0, 0, 0))
        d = ImageDraw.Draw(camada)
        d.text((2 * E - l, 2 * E - t), texto, font=f, fill=(255, 255, 255, 255), stroke_width=int(1.6 * E),
               stroke_fill=(255, 255, 255, 255))
        d.text((2 * E - l, 2 * E - t), texto, font=f, fill=cor)
        camada = camada.rotate(angulo, resample=Image.BICUBIC, expand=True)
        camada = camada.resize((max(1, camada.width // E), max(1, camada.height // E)), Image.LANCZOS)
        camada = camada.crop(camada.getbbox())
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        im.alpha_composite(camada, (cx - camada.width // 2, max(0, cy - camada.height // 2)))
    return im
