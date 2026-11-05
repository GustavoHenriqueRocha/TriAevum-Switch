"""Area titles (the name shown when entering a place): LA8 textures inside each scene/*.zar,
one per language (`name/<scene>_eusp.ctxb`, `_ussp.ctxb`). Redraws the Spanish ones in PT-BR."""
import struct
from PIL import Image, ImageDraw, ImageFont
from paths import FONTS, ROMFS_ORIGINAL
from ctxb import encode_ctxb, header
from romfs import locate, list_files

FONT = 'Belleza-Regular.ttf'          # OFL, similar to the original titles' font
NAMES = {
    'dg_dark': 'Templo da Sombra', 'dg_deku': 'Dentro da Árvore Deku', 'dg_dod': 'Caverna dos Dodongos',
    'dg_fire': 'Templo do Fogo', 'dg_forest': 'Templo da Floresta', 'dg_ganon': 'Dentro do Castelo de Ganon',
    'dg_gerudo': 'Campo de Treino Gerudo', 'dg_ice': 'Caverna de Gelo', 'dg_idosoko': 'Fundo do Poço',
    'dg_jab2': 'Dentro do Jabu-Jabu', 'dg_soul': 'Templo do Espírito', 'dg_water': 'Templo da Água',
    'ganon_castle': 'Castelo de Ganon', 'gerudoway': 'Esconderijo dos Ladrões', 'hairal_alley': 'Beco',
    'hairal_market': 'Mercado', 'hairal_niwa': 'Pátio do Castelo', 'hakaana_ouke': 'Tumba da Família Real',
    'rm_bowling': 'Boliche Bombchu', 'rm_daiyousei': 'Fonte da Grande Fada', 'rm_danpei': 'Cabana do Coveiro',
    'rm_fishing': 'Lagoa de Pesca', 'rm_kenjanoma': 'Câmara dos Sábios', 'rm_labo': 'Laboratório do Lago',
    'rm_shateki': 'Galeria de Tiro', 'rm_stable': 'Estábulo', 'rm_stalltula': 'Casa das Skulltulas',
    'rm_takara': 'Loja do Baú do Tesouro', 'rm_tokinoma': 'Templo do Tempo', 'rm_yousei': 'Fonte das Fadas',
    'shop_any': 'Bazar', 'shop_bomchu': 'Loja de Bombchus', 'shop_drag': 'Loja de Poções',
    'shop_face': 'Loja da Máscara Feliz', 'shop_golon': 'Loja Goron', 'shop_kokiri': 'Loja Kokiri',
    'shop_zora': 'Loja Zora', 'spot00': 'Campo de Hyrule', 'spot01': 'Vila Kakariko', 'spot02': 'Cemitério',
    'spot03': 'Rio Zora', 'spot04': 'Floresta Kokiri', 'spot05': 'Prado Sagrado da Floresta',
    'spot06': 'Lago Hylia', 'spot07': 'Domínio Zora', 'spot08': 'Fonte Zora', 'spot09': 'Vale Gerudo',
    'spot10': 'Bosque Perdido', 'spot11': 'Colosso do Deserto', 'spot12': 'Fortaleza Gerudo',
    'spot13': 'Deserto Assombrado', 'spot15': 'Castelo de Hyrule', 'spot16': 'Trilha da Montanha da Morte',
    'spot17': 'Cratera da Montanha da Morte', 'spot18': 'Cidade Goron', 'spot20': 'Rancho Lon Lon',
}


def render_title(text, w=256, h=32, cap=17, E=4):
    """Grayish white, black outline and shadow, like the originals; shrinks until it fits in 250 px.
    In 64 px tall textures (long names in Spanish) it goes on two lines."""
    if h >= 64 and ' ' in text:
        p = text.split(' '); mid = min(range(1, len(p)), key=lambda i: abs(len(' '.join(p[:i])) - len(' '.join(p[i:]))))
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        for line, y in ((' '.join(p[:mid]), h // 2 - 27), (' '.join(p[mid:]), h // 2 - 5)):
            img.alpha_composite(render_title(line, w, 32, cap, E), (0, y))
        return img
    for c in range(cap, 8, -1):
        f = ImageFont.truetype(str(FONTS / FONT), 100)
        hb = f.getbbox('H'); f = ImageFont.truetype(str(FONTS / FONT), int(100 * c * E / (hb[3] - hb[1])))
        l, t, r, b = f.getbbox(text)
        if (r - l) / E + 6 <= w - 6: break
    big = Image.new('RGBA', (w * E, h * E), (0, 0, 0, 0))
    hb = f.getbbox('H'); y = (h * E - (hb[3] - hb[1])) // 2 - hb[1] - E
    x = (w * E - (r - l)) // 2 - l
    shadow = Image.new('RGBA', big.size, (0, 0, 0, 0))
    ImageDraw.Draw(shadow).text((x + 1.5 * E, y + 1.5 * E), text, font=f, fill=(0, 0, 0, 170),
                                stroke_width=int(2.2 * E), stroke_fill=(0, 0, 0, 170))
    big.alpha_composite(shadow); d = ImageDraw.Draw(big); color = (220, 220, 220, 255)
    d.text((x, y), text, font=f, fill=color, stroke_width=int(2.0 * E), stroke_fill=(0, 0, 0, 255))
    d.text((x, y), text, font=f, fill=color, stroke_width=int(0.9 * E), stroke_fill=color)
    return big.resize((w, h), Image.LANCZOS)


def zar_entries(z):
    """→ [(name, offset, size)] of a ZAR file."""
    assert z[:4] == b'ZAR\x01'
    n = struct.unpack_from('<H', z, 10)[0]; meta, data = struct.unpack_from('<II', z, 16)
    out = []
    for i in range(n):
        size, name_off = struct.unpack_from('<II', z, meta + 8 * i)
        out.append((z[name_off:z.index(b'\0', name_off)].decode(), struct.unpack_from('<I', z, data + 4 * i)[0], size))
    return out


def write(romfs, png_dir=None):
    """Replaces the Spanish titles of every scene in `romfs` (patched in place). → how many textures."""
    done = {}; total = 0
    with open(ROMFS_ORIGINAL, 'rb') as orig, open(romfs, 'r+b') as f:
        for entry_path in list_files(ROMFS_ORIGINAL):
            if not (entry_path.startswith('scene/') and entry_path.endswith('.zar')): continue
            o, t = locate(ROMFS_ORIGINAL, entry_path); orig.seek(o); z = orig.read(t)
            for name, oz, tz in zar_entries(z):
                base = name.split('\\')[-1][:-5]
                scene, _, language = base.rpartition('_')
                if language not in ('eusp', 'ussp') or scene not in NAMES: continue
                old = z[oz:oz + tz]; w, h, _ = header(old)
                if (scene, h) not in done:
                    done[scene, h] = render_title(NAMES[scene], w, h)
                    if png_dir: done[scene, h].save(png_dir / f'title_{scene}.png')
                f.seek(o + oz); f.write(encode_ctxb(old, done[scene, h])); total += 1
    return total
