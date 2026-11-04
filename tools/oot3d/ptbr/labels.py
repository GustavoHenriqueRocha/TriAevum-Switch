"""Redraws PT-BR menu labels inside the CTXB textures."""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from pathlib import Path

from paths import FONTES
ESCALA = 4   # draws 4x larger and downscales: smooth edges like the original


SERIFA = '/usr/share/fonts/liberation/LiberationSerif-Bold.ttf'


def fonte(px, peso='ExtraBold'):
    if peso == 'serifa':
        return ImageFont.truetype(SERIFA, px)
    return ImageFont.truetype(str(FONTES / f'MPLUSRounded1c-{peso}.ttf'), px)


def rotulo(img, caixa, texto, altura_maiuscula=12, cor=(255, 255, 255, 255), contorno=(0, 0, 0, 255),
           espessura=1.5, limpar=True, peso='ExtraBold', alinhar='centro', topo=None, sombra=0):
    """Writes `texto` into `caixa` (x0, y0, x1, y1), erasing what was there before.

    With `topo`, the top of the capitals sits on that line (descenders may extend past the box,
    as in the original); without it, the text is centered vertically."""
    x0, y0, x1, y1 = caixa
    w, h = x1 - x0, y1 - y0
    if limpar:
        img.paste((0, 0, 0, 0), (x0, y0, x1, y1))
    E = ESCALA
    f = fonte(int(altura_maiuscula * E / (0.66 if peso == 'serifa' else 0.72)), peso)
    l, t, r, b = f.getbbox(texto)
    cap_t = f.getbbox('H')[1]                                # top of the capitals, to align the baseline
    tw, th = r - l, b - t
    margem = int(espessura * E) + 2
    camada = Image.new('RGBA', (tw + 2 * margem, th + 2 * margem), (0, 0, 0, 0))
    d = ImageDraw.Draw(camada)
    if sombra:
        d.text((margem - l + sombra * E, margem - t + sombra * E), texto, font=f, fill=contorno,
               stroke_width=int(espessura * E), stroke_fill=contorno)
    d.text((margem - l, margem - t), texto, font=f, fill=cor, stroke_width=int(espessura * E),
           stroke_fill=contorno)
    camada = camada.crop((0, 0, camada.width + sombra * E, camada.height + sombra * E)) if sombra else camada
    alvo_h = round(camada.height / E)
    alvo_w = round(camada.width / E)
    if alvo_w > w:                                          # doesn't fit: compress horizontally only
        alvo_w = w
    if alvo_h > h and topo is None:
        alvo_h = h
    camada = camada.resize((alvo_w, alvo_h), Image.LANCZOS)
    px = x0 + (w - alvo_w) // 2 if alinhar == 'centro' else x0
    if topo is not None:
        py = topo - round((margem + cap_t - t) / E)
    else:
        py = y0 + (h - alvo_h) // 2
    img.alpha_composite(camada, (px, py))
    return img
