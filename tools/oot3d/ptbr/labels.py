"""Redraws PT-BR menu labels inside the CTXB textures."""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from pathlib import Path

from paths import FONTS
SCALE = 4   # draws 4x larger and downscales: smooth edges like the original


SERIF = '/usr/share/fonts/liberation/LiberationSerif-Bold.ttf'


def font(px, weight='ExtraBold'):
    if weight == 'serif':
        return ImageFont.truetype(SERIF, px)
    return ImageFont.truetype(str(FONTS / f'MPLUSRounded1c-{weight}.ttf'), px)


def label(img, box, text, cap_height=12, color=(255, 255, 255, 255), outline=(0, 0, 0, 255),
           thickness=1.5, clear=True, weight='ExtraBold', align='center', top=None, shadow=0):
    """Writes `text` into `box` (x0, y0, x1, y1), erasing what was there before.

    With `top`, the top of the capitals sits on that line (descenders may extend past the box,
    as in the original); without it, the text is centered vertically."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    if clear:
        img.paste((0, 0, 0, 0), (x0, y0, x1, y1))
    E = SCALE
    f = font(int(cap_height * E / (0.66 if weight == 'serif' else 0.72)), weight)
    l, t, r, b = f.getbbox(text)
    cap_t = f.getbbox('H')[1]                                # top of the capitals, to align the baseline
    tw, th = r - l, b - t
    margin = int(thickness * E) + 2
    layer = Image.new('RGBA', (tw + 2 * margin, th + 2 * margin), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    if shadow:
        d.text((margin - l + shadow * E, margin - t + shadow * E), text, font=f, fill=outline,
               stroke_width=int(thickness * E), stroke_fill=outline)
    d.text((margin - l, margin - t), text, font=f, fill=color, stroke_width=int(thickness * E),
           stroke_fill=outline)
    layer = layer.crop((0, 0, layer.width + shadow * E, layer.height + shadow * E)) if shadow else layer
    target_h = round(layer.height / E)
    target_w = round(layer.width / E)
    if target_w > w:                                          # doesn't fit: compress horizontally only
        target_w = w
    if target_h > h and top is None:
        target_h = h
    layer = layer.resize((target_w, target_h), Image.LANCZOS)
    px = x0 + (w - target_w) // 2 if align == 'center' else x0
    if top is not None:
        py = top - round((margin + cap_t - t) / E)
    else:
        py = y0 + (h - target_h) // 2
    img.alpha_composite(layer, (px, py))
    return img
