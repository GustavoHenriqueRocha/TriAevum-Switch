"""Títulos de área (o nome que aparece ao entrar num lugar): texturas LA8 dentro de cada scene/*.zar,
uma por idioma (`name/<cena>_eusp.ctxb`, `_ussp.ctxb`). Redesenha as do espanhol em PT-BR."""
import struct
from PIL import Image, ImageDraw, ImageFont
from caminhos import FONTES, ROMFS_ORIGINAL
from ctxb import codificar_ctxb, cabecalho
from romfs import localizar, listar

FONTE = 'Belleza-Regular.ttf'          # OFL, parecida com a dos títulos originais
NOMES = {
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


def titulo(texto, w=256, h=32, cap=17, E=4):
    """Branco acinzentado, contorno preto e sombra, como os originais; encolhe até caber em 250 px.
    Nas texturas de 64 px de altura (nomes longos no espanhol) vai em duas linhas."""
    if h >= 64 and ' ' in texto:
        p = texto.split(' '); meio = min(range(1, len(p)), key=lambda i: abs(len(' '.join(p[:i])) - len(' '.join(p[i:]))))
        img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
        for linha, y in ((' '.join(p[:meio]), h // 2 - 27), (' '.join(p[meio:]), h // 2 - 5)):
            img.alpha_composite(titulo(linha, w, 32, cap, E), (0, y))
        return img
    for c in range(cap, 8, -1):
        f = ImageFont.truetype(str(FONTES / FONTE), 100)
        hb = f.getbbox('H'); f = ImageFont.truetype(str(FONTES / FONTE), int(100 * c * E / (hb[3] - hb[1])))
        l, t, r, b = f.getbbox(texto)
        if (r - l) / E + 6 <= w - 6: break
    big = Image.new('RGBA', (w * E, h * E), (0, 0, 0, 0))
    hb = f.getbbox('H'); y = (h * E - (hb[3] - hb[1])) // 2 - hb[1] - E
    x = (w * E - (r - l)) // 2 - l
    sombra = Image.new('RGBA', big.size, (0, 0, 0, 0))
    ImageDraw.Draw(sombra).text((x + 1.5 * E, y + 1.5 * E), texto, font=f, fill=(0, 0, 0, 170),
                                stroke_width=int(2.2 * E), stroke_fill=(0, 0, 0, 170))
    big.alpha_composite(sombra); d = ImageDraw.Draw(big); cor = (220, 220, 220, 255)
    d.text((x, y), texto, font=f, fill=cor, stroke_width=int(2.0 * E), stroke_fill=(0, 0, 0, 255))
    d.text((x, y), texto, font=f, fill=cor, stroke_width=int(0.9 * E), stroke_fill=cor)
    return big.resize((w, h), Image.LANCZOS)


def entradas_zar(z):
    """→ [(nome, offset, tamanho)] de um arquivo ZAR."""
    assert z[:4] == b'ZAR\x01'
    n = struct.unpack_from('<H', z, 10)[0]; meta, dados = struct.unpack_from('<II', z, 16)
    out = []
    for i in range(n):
        tam, no = struct.unpack_from('<II', z, meta + 8 * i)
        out.append((z[no:z.index(b'\0', no)].decode(), struct.unpack_from('<I', z, dados + 4 * i)[0], tam))
    return out


def gravar(romfs, destino_png=None):
    """Troca os títulos espanhóis de todas as cenas em `romfs` (corrige no lugar). → quantas texturas."""
    feitas = {}; total = 0
    with open(ROMFS_ORIGINAL, 'rb') as orig, open(romfs, 'r+b') as f:
        for cam in listar(ROMFS_ORIGINAL):
            if not (cam.startswith('scene/') and cam.endswith('.zar')): continue
            o, t = localizar(ROMFS_ORIGINAL, cam); orig.seek(o); z = orig.read(t)
            for nome, oz, tz in entradas_zar(z):
                base = nome.split('\\')[-1][:-5]
                cena, _, idioma = base.rpartition('_')
                if idioma not in ('eusp', 'ussp') or cena not in NOMES: continue
                velho = z[oz:oz + tz]; w, h, _ = cabecalho(velho)
                if (cena, h) not in feitas:
                    feitas[cena, h] = titulo(NOMES[cena], w, h)
                    if destino_png: feitas[cena, h].save(destino_png / f'titulo_{cena}.png')
                f.seek(o + oz); f.write(codificar_ctxb(velho, feitas[cena, h])); total += 1
    return total
