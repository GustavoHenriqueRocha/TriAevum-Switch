"""Texturas com texto do Majora's Mask 3D (EUR Rev 1) em PT-BR, no lugar das do espanhol.

    python -I mm3d_texturas.py inventario   # as 113 texturas do espanhol (102 arquivos): formato, tamanho, grupo
    python -I mm3d_texturas.py extrair      # PNG de cada uma em TEXTURAS/es/ (para conferir e transcrever)
    python -I mm3d_texturas.py tabela       # TEXTURAS/tabela-es-pt.json a partir de mapas-es.json + falas PT-BR
    python -I mm3d_texturas.py gerar        # PT-BR das telas de dia e dos mapas → TEXTURAS/pt/*.png + .ctxb
    python -I mm3d_texturas.py gravar       # grava as geradas em romfs-ptbr.bin (o mm3d.py montar também chama)

Onde estão: 85 `layout/map/genbamap/EU_Spanish/*.ctxb` (mapas com o nome do lugar numa placa), 7
`menu/daytelop/eu-es/*.ctxb` (telas "Amanecer del primer día", "- Quedan 24 horas -") e 10
`layout/EU_Spanish/*.gar` com 21 CTXB de menu dentro. Tudo é CTXB; o GAR é lido e remontado por `gar.py`.
Cada textura nova sai com o MESMO cabeçalho, formato e tamanho; no ETC1 só os blocos 4x4 que mudaram são
recomprimidos (`ctxb.codificar_blocos`), o resto fica com os bytes do jogo.

Nada do jogo vai para o repositório: a transcrição do espanhol (`mapas-es.json`), a tabela es→pt e as imagens
ficam em TEXTURAS = ~/oot3d-work/conv/mm3d-ptbr/texturas. Detalhes e decisões:
docs/pesquisas/2026-11-23-mm3d-texturas-ptbr.md.
"""
import json, os, struct, sys
from collections import Counter
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gar
from caminhos import FONTES as FONTES_OOT3D
from ctxb import CTXB_HEADER_SIZE, cabecalho, codificar_blocos, decodificar, ordem_blocos
from mm3d import ROMFS_ORIGINAL, ROMFS_PTBR, TRABALHO, ler_romfs
from romfs import listar, localizar

TEXTURAS = TRABALHO / 'texturas'
NOMES_FMT = {0: 'RGBA8', 2: 'RGBA5551', 3: 'RGB565', 4: 'RGBA4', 5: 'LA8', 7: 'L8', 12: 'ETC1', 13: 'ETC1A4'}
# Noto Sans CJK (OFL; pacote noto-fonts-cjk do Arch): o latim dela é o mais parecido com o das placas e das telas
# de dia entre as fontes livres testadas (docs/pesquisas/2026-11-23-mm3d-texturas-ptbr.md, folha fontes_cand.png)
CJK = os.environ.get('TRIAEVUM_FONTE_CJK', '/usr/share/fonts/noto-cjk/NotoSansCJK-{}.ttc')
BELLEZA = FONTES_OOT3D / 'Belleza-Regular.ttf'      # OFL, a mesma dos títulos de área do OoT3D


def espanhol():
    """→ [(caminho na romfs, membro do GAR ou None, bytes do CTXB)] das texturas do espanhol."""
    out = []
    for c in listar(ROMFS_ORIGINAL):
        if not ('EU_Spanish' in c or '/eu-es/' in c): continue
        d = ler_romfs(ROMFS_ORIGINAL, c)
        if c.endswith('.gar'):
            out += [(c, n, p) for n, p in gar.ler(d)['membros']]
        else:
            out.append((c, None, d))
    return out


def grupo(caminho):
    return 'mapa' if '/genbamap/' in caminho else 'dia' if '/daytelop/' in caminho else 'menu'


def nome_png(caminho, membro):
    base = caminho.rsplit('/', 1)[1]
    if membro: return f'gar_{base[:-4]}__{membro[:-5]}'
    return ('mapa_' if grupo(caminho) == 'mapa' else 'dia_') + base[:-5]


def inventario():
    cont = Counter()
    for c, m, d in espanhol():
        w, h, fmt = cabecalho(d)
        cont[grupo(c), NOMES_FMT.get(fmt, fmt), f'{w}x{h}'] += 1
        print(f'{c}{":" + m if m else ""}\t{len(d)}\t{w}x{h}\t{NOMES_FMT.get(fmt, fmt)}')
    for k, v in sorted(cont.items()): print(*k, v)


def extrair():
    from PIL import Image
    (TEXTURAS / 'es').mkdir(parents=True, exist_ok=True)
    for c, m, d in espanhol():
        im = decodificar(d); n = nome_png(c, m); im.save(TEXTURAS / 'es' / f'{n}.png')
        ver = Image.new('RGBA', im.size, (60, 40, 80, 255)); ver.alpha_composite(im)
        ver.convert('RGB').save(TEXTURAS / 'es' / f'{n}.ver.png')


# ---------------------------------------------------------------- texto --
def _fonte(caminho, px):
    from PIL import ImageFont
    return ImageFont.truetype(str(caminho), px, index=0) if str(caminho).endswith('.ttc') \
        else ImageFont.truetype(str(caminho), px)


def mascara(texto, fonte, xh, base, w, h, centro=None, largura_max=None, contorno=0.0, E=4):
    """Cobertura (imagem 'L' w x h) de `texto` com altura-x `xh` px e linha de base em `base` (coordenada
    contínua: a base fica na borda de cima da linha `base`), centrado em `centro`. Desenha E vezes maior e reduz
    (bordas suaves como as do original); mais largo que `largura_max` → comprime só na horizontal."""
    from PIL import Image, ImageDraw
    f = _fonte(fonte, 200); bx = f.getbbox('x', anchor='ls')
    f = _fonte(fonte, round(200 * xh * E / -bx[1]))
    s = round(contorno * E)
    l, t, r, b = f.getbbox(texto, anchor='ls', stroke_width=s)
    camada = Image.new('L', (r - l + 2, b - t + 2), 0)
    ImageDraw.Draw(camada).text((1 - l, 1 - t), texto, font=f, fill=255, anchor='ls', stroke_width=s, stroke_fill=255)
    lim = (largura_max or w) * E
    if camada.width > lim:
        camada = camada.resize((lim, camada.height), Image.LANCZOS)
    grande = Image.new('L', (w * E, h * E), 0)
    cx = (centro if centro is not None else w / 2) * E
    grande.paste(camada, (round(cx - camada.width / 2), round(base * E) - (1 - t)))
    return grande.resize((w, h), Image.LANCZOS)


def pintar(im, cor, m):
    from PIL import Image
    im.paste(Image.new(im.mode, im.size, cor), (0, 0), m)


# ------------------------------------------------------------- telas de dia --
# (fonte, linhas [(altura-x, base)], tinta, engrossar em px). Medidas do espanhol: a altura-x e a base de cada linha
# (ex.: daytelop_first, linha 1 com x de 10 a 24 → xh 15, base 25). No daytelop_last o espanhol usa xh 20; aqui 18,
# porque o acento do "Ú" de "Último" encostava na linha de cima.
DIAS = {
    'daytelop_24': ('belleza', [(10, 22)], 225, 0.5),
    'daytelop_48': ('belleza', [(10, 22)], 225, 0.5),
    'daytelop_72': ('belleza', [(10, 22)], 225, 0.5),
    'daytelop_first': ('Black', [(15, 25), (15, 55)], 224, 0),
    'daytelop_scound': ('Black', [(17, 26), (17, 56)], 224, 0),
    'daytelop_last': ('Black', [(18, 28), (18, 63)], 255, 0),
    'daytelop_new': ('Black', [(16, 28), (16, 58)], 0, 0),
}


def tela_dia(nome, texto, w, h):
    from PIL import Image
    fonte, linhas, tinta, cont = DIAS[nome]
    caminho = BELLEZA if fonte == 'belleza' else CJK.format(fonte)
    if nome == 'daytelop_new':                     # letra preta com borda amarelada sobre branco (como o original)
        im = Image.new('RGB', (w, h), (255, 255, 255))
        for txt, (xh, base) in zip(texto.split('|'), linhas):
            pintar(im, (251, 251, 183), mascara(txt, caminho, xh, base, w, h, largura_max=w - 8, contorno=1.4))
        for txt, (xh, base) in zip(texto.split('|'), linhas):
            pintar(im, (0, 0, 0), mascara(txt, caminho, xh, base, w, h, largura_max=w - 8))
        return im.convert('RGBA')
    im = Image.new('RGB', (w, h), (0, 0, 0))
    for txt, (xh, base) in zip(texto.split('|'), linhas):
        pintar(im, (tinta,) * 3, mascara(txt, caminho, xh, base, w, h, largura_max=w - 4, contorno=cont))
    return im.convert('RGBA')


# ------------------------------------------------------------------ mapas --
PLACA = (11, 211, 245, 231)      # miolo da placa do nome (entre as linhas escuras da moldura), igual nos 85 mapas
TINTA_MAPA = (38, 20, 4)


def blocos_modais(payloads, passo):
    """Bloco mais comum em cada posição de vários payloads do mesmo tamanho → (blocos, frequências)."""
    n = len(payloads[0]) // passo; moda, freq = [], []
    for k in range(n):
        b, f = Counter(p[k * passo:(k + 1) * passo] for p in payloads).most_common(1)[0]
        moda.append(b); freq.append(f)
    return moda, freq


def _mapas_etc1():
    """Payloads ETC1 256x256 dos mapas de todos os idiomas (o 055 é ETC1A4 e o 000 é um aviso em japonês)."""
    out = []
    for c in listar(ROMFS_ORIGINAL):
        if '/genbamap/' in c and not c.endswith(('/000.ctxb', '/055.ctxb')):
            d = ler_romfs(ROMFS_ORIGINAL, c)
            if d and cabecalho(d)[2] == 12: out.append(d)
    return out


def placa_limpa():
    """→ (imagem RGBA 256x256 com a placa sem texto em PLACA, {pixels do bloco: bytes}) e cache em TEXTURAS.

    Onde o bloco mais comum aparece em mais da metade dos mapas (≈490, 6 idiomas) ele é a placa limpa e vem byte a
    byte do jogo; no meio, onde quase todo nome tem letra, cada pixel é a mediana dos mapas sem tinta perto dele."""
    from PIL import Image
    import statistics
    cache = TEXTURAS / 'placa_limpa.png'
    mapas = _mapas_etc1()
    moda, freq = blocos_modais([d[CTXB_HEADER_SIZE:] for d in mapas], 8)
    base = decodificar(mapas[0][:CTXB_HEADER_SIZE] + b''.join(moda))
    conhecidos, pos = {}, ordem_blocos(256, 256)
    for k, (x, y) in enumerate(pos):
        if freq[k] * 2 > len(mapas):
            conhecidos[base.crop((x, y, x + 4, y + 4)).tobytes()] = moda[k]
    if cache.exists():
        return Image.open(cache).convert('RGBA'), conhecidos
    x0, y0, x1, y1 = PLACA
    ims = [decodificar(d).convert('RGB').crop((x0 - 1, y0 - 1, x1 + 1, y1 + 1)).load() for d in mapas]
    limpa = base.copy(); px = limpa.load()
    for k, (bx, by) in enumerate(pos):
        if freq[k] * 2 > len(mapas): continue
        for y in range(max(by, y0), min(by + 4, y1)):
            for x in range(max(bx, x0), min(bx + 4, x1)):
                v = [p[x - x0 + 1, y - y0 + 1] for p in ims
                     if all(sum(p[x - x0 + 1 + dx, y - y0 + 1 + dy]) >= 450 for dx in (-1, 0, 1) for dy in (-1, 0, 1))]
                v = v if len(v) >= 10 else [p[x - x0 + 1, y - y0 + 1] for p in ims]
                px[x, y] = tuple(statistics.median_low(c[i] for c in v) for i in range(3)) + (255,)
    TEXTURAS.mkdir(parents=True, exist_ok=True); limpa.save(cache)
    return limpa, conhecidos


def mapa_pt(original, texto, limpa, conhecidos=None):
    """CTXB do mapa com a placa limpa e `texto` (altura-x 9, base 227, centro 128, como o espanhol)."""
    im = decodificar(original)
    im.paste(limpa.crop(PLACA), PLACA[:2])
    m = mascara(texto, CJK.format('Bold'), 9, 227, 256, 256, centro=128, largura_max=PLACA[2] - PLACA[0] - 8)
    pintar(im, TINTA_MAPA + (255,), m)
    return im, codificar_blocos(original, im, conhecidos if cabecalho(original)[2] == 12 else None)


# ------------------------------------------------------------ tabela e geração --
def tabela():
    """mapas-es.json ({"001": "Norte de Ciudad Reloj"}) + eus.gmsg/eus-ptbr.gmsg → tabela-es-pt.json.

    O PT de cada nome é o da fala com o mesmo texto no espanhol (nomes de área, ids 0x100–0x1a0), para a placa
    dizer o mesmo que as falas. Nome sem fala igual fica em branco para preencher à mão."""
    import gmsg
    vis = lambda d: {e[0]: gmsg.visivel(gmsg.texto(p)).strip() for e, p in zip(*gmsg.ler(d)[1:]) if p}
    es = vis(ler_romfs(ROMFS_ORIGINAL, 'message/eu/eus.gmsg'))
    pt = vis((TRABALHO / 'eus-ptbr.gmsg').read_bytes())
    arq = TEXTURAS / 'tabela-es-pt.json'
    velha = json.loads(arq.read_text(encoding='utf-8')) if arq.exists() else {}
    mapas = json.loads((TEXTURAS / 'mapas-es.json').read_text(encoding='utf-8'))
    nomes = {}
    for lab in sorted(set(mapas.values())):
        ids = [k for k, v in sorted(es.items()) if v == lab]
        nomes[lab] = velha.get('nomes', {}).get(lab) or (pt.get(ids[0], '') if ids else '')
    novo = {'nomes': nomes, 'mapas': mapas, 'dias': velha.get('dias', {})}
    arq.write_text(json.dumps(novo, ensure_ascii=False, indent=1), encoding='utf-8')
    for k, v in nomes.items(): print(f'{k!r} → {v!r}')


def gerar(so=None):
    """→ {(caminho, membro): ctxb PT-BR}; grava PNG e CTXB em TEXTURAS/pt/. `so`: 'dia' ou 'mapa'."""
    tab = json.loads((TEXTURAS / 'tabela-es-pt.json').read_text(encoding='utf-8'))
    (TEXTURAS / 'pt').mkdir(parents=True, exist_ok=True)
    out = {}; limpa = conhecidos = None
    for c, m, d in espanhol():
        g = grupo(c); base = c.rsplit('/', 1)[1][:-5]
        if so and g != so: continue
        if g == 'dia' and base in tab['dias']:
            w, h, _ = cabecalho(d); im = tela_dia(base, tab['dias'][base], w, h); novo = codificar_blocos(d, im)
        elif g == 'mapa' and base in tab['mapas']:
            if limpa is None: limpa, conhecidos = placa_limpa()
            pt = tab['nomes'].get(tab['mapas'][base])
            if not pt: raise SystemExit(f'sem PT para {tab["mapas"][base]!r} (tabela-es-pt.json)')
            im, novo = mapa_pt(d, pt, limpa, conhecidos)
        else:
            continue
        assert len(novo) == len(d) and novo[:CTXB_HEADER_SIZE] == d[:CTXB_HEADER_SIZE]
        n = nome_png(c, m); im.save(TEXTURAS / 'pt' / f'{n}.png'); (TEXTURAS / 'pt' / f'{n}.ctxb').write_bytes(novo)
        out[c, m] = novo
    return out


def gravar(romfs=ROMFS_PTBR, novos=None):
    """Troca na romfs (no lugar, mesmo tamanho) os arquivos do espanhol pelas versões PT-BR. → quantos arquivos."""
    novos = gerar() if novos is None else novos
    por_arquivo = {}
    for (c, m), d in novos.items(): por_arquivo.setdefault(c, {})[m] = d
    for c, membros in por_arquivo.items():
        orig = ler_romfs(ROMFS_ORIGINAL, c)
        dado = gar.trocar(orig, membros) if c.endswith('.gar') else membros[None]
        o, tam = localizar(romfs, c)
        if tam != len(dado): raise SystemExit(f'{c}: {len(dado)} bytes, o original tem {tam}')
        with open(romfs, 'r+b') as f:
            f.seek(o); f.write(dado)
    return len(por_arquivo)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'inventario'
    if cmd == 'gerar': print(len(gerar(sys.argv[2] if len(sys.argv) > 2 else None)), 'texturas PT-BR')
    elif cmd == 'gravar': print(gravar(), 'arquivos trocados em', ROMFS_PTBR)
    else: {'inventario': inventario, 'extrair': extrair, 'tabela': tabela}[cmd]()
