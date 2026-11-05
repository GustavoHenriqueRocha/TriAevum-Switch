"""Generates the PT-BR menu textures (for now: HUD)."""
import struct, sys
sys.path.insert(0, '.')
from ctxb import decode, read, CTXB_HEADER_SIZE, encode_rgba4_tiled, header
from labels import label
from paths import WORK_DIR

HUD = [  # 64 px columns, 18 px rows starting at y=92 (same order as the original)
    ['Atacar', 'Olhar', 'Entrar', 'Voltar', 'Abrir', 'Pular', 'Decidir', 'Mergulhar', 'Galopar'],
    ['Lançar', 'Navi', 'Subir', 'Soltar', 'Descer', 'Salvar', 'Falar', 'Próximo', 'Agarrar'],
    ['Sair', 'Guardar', 'Puxar', 'Trocar', 'Equipar'],
]


def hud():
    im = decode(read('06_EU_SPANISH', 'hud_all00'))
    im.paste((0, 0, 0, 0), (0, 92, 192, 256))               # whole word grid
    for c, column in enumerate(HUD):
        for r, word in enumerate(column):
            y = 92 + 18 * r
            label(im, (64 * c, y, 64 * c + 64, y + 18), word, top=y + 5, clear=False)
    label(im, (0, 58, 200, 88), 'FIM DE JOGO', cap_height=17, color=(238, 238, 238, 255),
           thickness=2, weight='serif', align='left', top=63, shadow=1)
    return im


def encode(original_name, im):
    orig = read('06_EU_SPANISH', original_name); w, h, fmt = header(orig)
    assert fmt == 4 and im.size == (w, h)
    return orig[:CTXB_HEADER_SIZE] + encode_rgba4_tiled(im.tobytes(), w, h)


if __name__ == '__main__':
    S = str(WORK_DIR) + '/'
    im = hud(); im.save(S + 'hud_ptbr.png')
    from PIL import Image
    canvas = Image.new('RGBA', im.size, (40, 40, 60, 255)); canvas.alpha_composite(im)
    canvas.resize((512, 512), Image.NEAREST).convert('RGB').save(S + 'hud_ptbr.ver.png')


def write_to_romfs(romfs, name, ctxb_bytes):
    """Replaces menu/06_EU_SPANISH and 07_US_SPANISH/<name>.ctxb, same size."""
    from romfs import locate
    for language in ('06_EU_SPANISH', '07_US_SPANISH'):
        o, size = locate(romfs, f'menu/{language}/{name}.ctxb')
        assert size == len(ctxb_bytes), (name, size, len(ctxb_bytes))
        with open(romfs, 'r+b') as f:
            f.seek(o); f.write(ctxb_bytes)


SANS_BOLD = '/usr/share/fonts/liberation/LiberationSans-Bold.ttf'
# rotated badges of menu_hint_movie_parts00: box in the original, text, color
BADGES = [((393, 0, 447, 39), 'Completo', (32, 166, 32, 255), 9), ((458, 4, 501, 37), 'Novo', (255, 0, 0, 255), 12)]


def badges(im, angle=33, E=4):
    """Redraws the "Completado"/"Nuevo" badges: colored text, white outline, rotated like the original."""
    from PIL import Image, ImageDraw, ImageFont
    im = im.copy()
    for (x0, y0, x1, y1), text, color, height in BADGES:
        im.paste((0, 0, 0, 0), (x0, y0, x1, y1))
        f = ImageFont.truetype(SANS_BOLD, int(height * E / 0.72))
        l, t, r, b = f.getbbox(text, stroke_width=int(1.6 * E))
        layer = Image.new('RGBA', (r - l + 4 * E, b - t + 4 * E), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        d.text((2 * E - l, 2 * E - t), text, font=f, fill=(255, 255, 255, 255), stroke_width=int(1.6 * E),
               stroke_fill=(255, 255, 255, 255))
        d.text((2 * E - l, 2 * E - t), text, font=f, fill=color)
        layer = layer.rotate(angle, resample=Image.BICUBIC, expand=True)
        layer = layer.resize((max(1, layer.width // E), max(1, layer.height // E)), Image.LANCZOS)
        layer = layer.crop(layer.getbbox())
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        im.alpha_composite(layer, (cx - layer.width // 2, max(0, cy - layer.height // 2)))
    return im
