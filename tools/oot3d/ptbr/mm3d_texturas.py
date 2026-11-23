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
from lote_menus_mm3d import LOTE
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


def mascara(texto, fonte, xh, base, w, h, centro=None, largura_max=None, contorno=0.0, esticar=1.0, E=8, ref='x',
            esquerda=None):
    """Cobertura (imagem 'L' w x h) de `texto` com altura-x `xh` px e linha de base em `base` (coordenada
    contínua: a base fica na borda de cima da linha `base`), centrado em `centro`. Desenha E vezes maior e reduz
    (bordas suaves como as do original); `contorno` engrossa o traço (px), `esticar` alarga/estreita; mais largo que
    `largura_max` → comprime só na horizontal."""
    from PIL import Image, ImageDraw
    f = _fonte(fonte, 200); bx = f.getbbox(ref, anchor='ls')                # ref 'H': xh é a altura da maiúscula
    f = _fonte(fonte, round(200 * xh * E / -bx[1]))
    s = round(contorno * E)
    l, t, r, b = f.getbbox(texto, anchor='ls', stroke_width=s)
    camada = Image.new('L', (r - l + 2, b - t + 2), 0)
    ImageDraw.Draw(camada).text((1 - l, 1 - t), texto, font=f, fill=255, anchor='ls', stroke_width=s, stroke_fill=255)
    if esticar != 1.0:                              # largura das letras e do espaçamento, medida contra o original
        camada = camada.resize((max(1, round(camada.width * esticar)), camada.height), Image.LANCZOS)
    lim = (largura_max or w) * E
    if camada.width > lim:
        camada = camada.resize((lim, camada.height), Image.LANCZOS)
    grande = Image.new('L', (w * E, h * E), 0)
    cx = (centro if centro is not None else w / 2) * E
    x = round(esquerda * E) if esquerda is not None else round(cx - camada.width / 2)
    grande.paste(camada, (x, round(base * E) - (1 - t)))
    return grande.resize((w, h), Image.LANCZOS)


def pintar(im, cor, m):
    from PIL import Image
    im.paste(Image.new(im.mode, im.size, cor), (0, 0), m)


# ------------------------------------------------------------- telas de dia --
# (fonte, linhas [(altura-x, base)], tinta, contorno, esticar). Altura-x e base medidas no espanhol (ex.:
# daytelop_first, linha 1 com x de 10 a 24 → xh 15, base 25). Contorno e esticar calibrados desenhando o PRÓPRIO texto
# espanhol e comparando massa de tinta e largura com o original (fica a ±5%; tabela na pesquisa). No daytelop_last a
# altura-x caiu de 20 para 18: o acento do "Ú" de "Último" encostava na linha de cima.
DIAS = {
    'daytelop_24': ('belleza', [(10, 22)], 225, 0.375, 0.96),
    'daytelop_48': ('belleza', [(10, 22)], 225, 0.375, 0.96),
    'daytelop_72': ('belleza', [(10, 22)], 225, 0.375, 0.96),
    'daytelop_first': ('Black', [(15, 25), (15, 55)], 224, 0.125, 0.92),
    'daytelop_scound': ('Black', [(17, 26), (17, 56)], 224, 0.5, 0.975),
    'daytelop_last': ('Black', [(18, 28), (18, 63)], 255, 0.625, 1.03),
    'daytelop_new': ('Black', [(16, 28), (16, 58)], 0, 0.75, 1.12),
}


def tela_dia(nome, texto, w, h):
    from PIL import Image
    fonte, linhas, tinta, cont, est = DIAS[nome]
    caminho = BELLEZA if fonte == 'belleza' else CJK.format(fonte)
    if nome == 'daytelop_new':                     # letra preta com borda amarelada sobre branco (como o original)
        im = Image.new('RGB', (w, h), (255, 255, 255))
        for txt, (xh, base) in zip(texto.split('|'), linhas):
            pintar(im, (251, 251, 183), mascara(txt, caminho, xh, base, w, h, largura_max=w - 8, contorno=cont + 1.4,
                                                    esticar=est))
        for txt, (xh, base) in zip(texto.split('|'), linhas):
            pintar(im, (0, 0, 0), mascara(txt, caminho, xh, base, w, h, largura_max=w - 8, contorno=cont, esticar=est))
        return im.convert('RGBA')
    im = Image.new('RGB', (w, h), (0, 0, 0))
    for txt, (xh, base) in zip(texto.split('|'), linhas):
        pintar(im, (tinta,) * 3, mascara(txt, caminho, xh, base, w, h, largura_max=w - 4, contorno=cont,
                                              esticar=est))
    return im.convert('RGBA')


# ------------------------------------------------------------------ mapas --
PLACA = (11, 211, 245, 231)      # miolo da placa do nome (entre as linhas escuras da moldura), igual nos 85 mapas
TINTA_MAPA = (38, 20, 4)
PESO_MAPA = 'Black'   # calibrado contra o espanhol: massa de tinta 0,99x e largura 1,00x (Bold: 0,86x)


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
    m = mascara(texto, CJK.format(PESO_MAPA), 9, 227, 256, 256, centro=128, largura_max=PLACA[2] - PLACA[0] - 8)
    pintar(im, TINTA_MAPA + (255,), m)
    return im, codificar_blocos(original, im, conhecidos if cabecalho(original)[2] == 12 else None)


# ------------------------------------------------------------------ menus --
def _tinta(im, caixa):
    """[[bool]] do tamanho da imagem: pixel de texto dentro de `caixa` = longe da cor mais comum dela (o fundo);
    num fundo transparente, qualquer pixel visível."""
    x0, y0, x1, y1 = caixa; px = im.load()
    fundo = Counter(px[x, y] for y in range(y0, y1) for x in range(x0, x1)).most_common(1)[0][0]
    w, h = im.size; out = [[False] * w for _ in range(h)]
    for y in range(y0, y1):
        for x in range(x0, x1):
            p = px[x, y]
            out[y][x] = p[3] > 60 if fundo[3] < 60 else \
                (abs(p[3] - fundo[3]) > 60 or sum(abs(p[i] - fundo[i]) for i in range(3)) > 60)
    return out


def rotulos_menu(es, outras):
    """Rótulos de uma textura de menu: áreas onde o espanhol difere dos outros idiomas (regioes_de), cortadas em
    linhas e palavras pela tinta do próprio espanhol. → [(caixa justa do texto, (x esquerdo, x direito) livres)]."""
    from regioes import regioes_de, cortes_xy
    out = []
    for r in regioes_de(es, outras):
        caixas = cortes_xy(_tinta(es, r), r, vao_x=6, vao_y=1, minimo=8)
        for c in caixas:
            viz = [d for d in caixas if d is not c and d[1] < c[3] and c[1] < d[3]]
            esq = max([(d[2] + c[0]) // 2 for d in viz if d[2] <= c[0]], default=r[0])
            dir_ = min([(c[2] + d[0]) // 2 for d in viz if d[0] >= c[2]], default=r[2])
            out.append((c, (esq, dir_)))
    return out


def outras_linguas(caminho, membro, tam):
    """As mesmas texturas de menu nos outros idiomas europeus (para achar o que é texto)."""
    out = []
    for L in ('English', 'French', 'German', 'Italian', 'Dutch'):
        g = dict(gar.ler(ler_romfs(ROMFS_ORIGINAL, caminho.replace('EU_Spanish', 'EU_' + L)))['membros'])
        if len(g.get(membro, b'')) == tam: out.append(decodificar(g[membro]))
    return out


def _apagar(im, caixa, dif, folga=2):
    """Tira o texto da caixa: fundo transparente fica transparente; senão cada linha recebe a cor média dos pixels
    da linha (caixa + 6 px de cada lado) que são iguais em todos os idiomas (`dif` falso: fundo com certeza), só os
    de luma média (nem a letra clara nem contorno/moldura escuros); em botão (fundo diferente por idioma), os de luma
    média da própria caixa. A cor é a
    do grupo mais comum. Linha sem nenhum desses copia a vizinha. Mantém o degradê vertical dos botões, como o _fundo
    do OoT3D."""
    px = im.load(); w, h = im.size
    x0, y0, x1, y1 = max(0, caixa[0] - folga), max(0, caixa[1] - folga), min(w, caixa[2] + folga), min(h, caixa[3] + folga)
    anel = [px[x, y] for x in range(max(0, x0 - 2), min(w, x1 + 2)) for y in (max(0, y0 - 2), min(h - 1, y1 + 1))] + \
           [px[x, y] for y in range(y0, y1) for x in (max(0, x0 - 2), min(w - 1, x1 + 1))]
    # transparente se o anel em volta é, ou se os lados da caixa nas linhas do texto, fora o que muda por idioma, são
    # (o anel engana com frisos em cima/embaixo e a letra de um rótulo vizinho; os lados, com um ícone ao lado)
    lados = [px[x, y] for y in range(y0, y1) for x in [*range(max(0, x0 - 6), x0), *range(x1, min(w, x1 + 6))]
             if not dif[y][x]]
    vazio = lambda l: len(l) >= 4 and sum(p[3] < 60 for p in l) * 2 > len(l)
    if vazio(anel) or vazio(lados):
        im.paste((0, 0, 0, 0), (x0, y0, x1, y1)); return
    refs = {}; luma = lambda p: (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
    for y in range(y0, y1):
        meio = lambda p: p[3] > 200 and 35 < luma(p) < 215   # nem letra clara nem contorno/moldura escuros
        sel = [px[x, y] for x in range(max(0, x0 - 6), min(w, x1 + 6)) if not dif[y][x] and meio(px[x, y])]
        if len(sel) < 4:   # botão: o fundo também muda entre idiomas
            sel = [p for p in (px[x, y] for x in range(x0, x1)) if meio(p)]
        if not sel: continue
        # só o grupo de cor mais comum (de 24 em 24): ícones vizinhos iguais em todos os idiomas (o Ⓑ) não entram
        g = Counter(tuple(c // 24 for c in p) for p in sel).most_common(1)[0][0]
        sel = [p for p in sel if tuple(c // 24 for c in p) == g]
        refs[y] = tuple(sum(p[i] for p in sel) // len(sel) for i in range(4))
    if not refs: return
    # linha cuja cor foge da mediana (contorno escuro ou friso claro dominando a linha) copia a vizinha boa
    med = sorted(luma(c) for c in refs.values())[len(refs) // 2]
    refs = {y: c for y, c in refs.items() if abs(luma(c) - med) <= 40} or refs
    for y in range(y0, y1):
        ref = refs.get(y) or refs[min(refs, key=lambda k: abs(k - y))]
        for x in range(x0, x1): px[x, y] = ref


def _estilo_rotulo(orig, limpa, caixa, dif=None):
    """Cor da letra, do contorno e faixa da altura-x/maiúscula (linhas com mais tinta) do rótulo espanhol.
    Texto = pixels que diferem entre idiomas e do fundo refeito; divididos em claros (letra) e escuros
    (contorno). → (letra, contorno, tem contorno, topo e fim da faixa, espessura do traço)."""
    po, pl = orig.load(), limpa.load(); x0, y0, x1, y1 = caixa
    texto = {(x, y) for y in range(y0, y1) for x in range(x0, x1) if (dif is None or dif[y][x]) and
             (abs(po[x, y][3] - pl[x, y][3]) > 60 or (po[x, y][3] > 60 and sum(abs(po[x, y][i] - pl[x, y][i]) for i in range(3)) > 60))}
    if not texto: return None
    luma = lambda p: (po[p][0] * 299 + po[p][1] * 587 + po[p][2] * 114) // 1000
    mediana = lambda l: tuple(sorted(po[p][i] for p in l)[len(l) // 2] for i in range(3)) + (255,)
    # dois grupos por luma (corte no meio)
    ls = [luma(p) for p in texto]; corte = (min(ls) + max(ls)) / 2
    claros = [p for p in texto if luma(p) > corte]; escuros = [p for p in texto if luma(p) <= corte]
    tem = min(len(claros), len(escuros)) >= 0.15 * len(texto) and max(ls) - min(ls) > 100
    if tem:   # nos menus do MM3D a letra é sempre a parte clara (exceção explícita: 'cores' no lote)
        letra, contorno = mediana(claros), mediana(escuros)
    else:
        letra, contorno = mediana(list(texto)), None
    linhas = Counter(y for _, y in texto); mx = max(linhas.values())
    faixa = [y for y in range(y0, y1) if linhas.get(y, 0) >= 0.45 * mx]
    # traço total (letra + contorno): o PT é comparado pela letra mais 2 contornos
    return letra, contorno, tem, faixa[0], faixa[-1] + 1, _traco(lambda x, y: (x, y) in texto, x0, x1, faixa)


def _traco(cheio, x0, x1, linhas):
    """Espessura média do traço: comprimento médio das sequências horizontais de pixels cheios nessas linhas."""
    seq = []
    for y in linhas:
        n = 0
        for x in range(x0, x1 + 1):
            if x < x1 and cheio(x, y): n += 1
            elif n: seq.append(n); n = 0
    return sum(seq) / len(seq) if seq else 0


def menu_pt(membro, original, rotulos, outras):
    """CTXB de menu com os `rotulos` [(x0, y0, x1, y1, 'PT', opções)] trocados; `outras` = a mesma textura em outros
    idiomas (o que é igual em todos é fundo). → (imagem, bytes)."""
    from PIL import Image
    from regioes import mascara_dif
    orig = decodificar(original); im = orig.copy(); dif = mascara_dif(orig, outras)
    for x0, y0, x1, y1, _, op in rotulos:
        if 'fundo' in op: im.paste(op['fundo'], (x0, y0, x1, y1))   # placa lisa (etiqueta branca do "¡Nuevo!")
        else: _apagar(im, (x0, y0, x1, y1), dif, op.get('folga', 2))
    ests = [_estilo_rotulo(orig, im, r[:4], dif) for r in rotulos]
    # altura-x e traço: mediana do grupo ('grade'), porque o que se mede numa palavra só varia com as letras dela
    grupos = {}
    for r, e in zip(rotulos, ests):
        if e and r[5].get('grade'): grupos.setdefault(r[5]['grade'], []).append((e[4] - e[3], e[5]))
    med = {k: (sorted(a for a, _ in v)[len(v) // 2], sorted(b for _, b in v)[len(v) // 2]) for k, v in grupos.items()}
    w, h = im.size
    for (x0, y0, x1, y1, pt, op), est in zip(rotulos, ests):
        if est is None: continue
        letra, cont, tem_c, top, fim, traco = est
        letra, cont = op.get('cores', (letra, cont))
        faixa, traco = med[op['grade']] if op.get('grade') else (fim - top, traco)
        faixa += op.get('crescer', 0)
        o = (1.25 if y1 - y0 <= 20 else 1.5 if y1 - y0 <= 30 else 2.0) if tem_c else 0.0
        xh, base = max(4, faixa - 2 * o), fim + op.get('descer', 0) - o       # a base é a de cada um (estável)
        xh, base = op.get('xh', xh), op.get('base', base)
        camadas = op.get('camadas', [(cont, o)] if tem_c else [])   # contornos de fora para dentro
        l0, l1 = op.get('livre', (x0 - 4, x1 + 4))
        kw = dict(largura_max=l1 - l0 - 1, ref='H' if op.get('maiusc') else 'x')
        kw.update(esquerda=x0 + o) if op.get('alinhar') == 'esq' else kw.update(centro=(x0 + x1) / 2)
        # peso da fonte: o de traço mais parecido com o do espanhol (as placas do menu vão do fino ao negrito)
        cand = []
        for peso in ([op['peso']] if 'peso' in op else ('Medium', 'Bold', 'Black')):
            m = mascara(pt, CJK.format(peso), xh, base, w, h, **kw); mp = m.load()
            b = round(base); t_pt = _traco(lambda x, y: mp[x, y] > 127, 0, w, range(max(0, b - round(xh)), min(h, b)))
            cand.append((abs(t_pt + 2 * o - traco), peso, m))
        _, peso, m_letra = min(cand, key=lambda c: c[0])
        f = CJK.format(peso)
        # contorno de 1 px fica meio transparente depois da redução; o original é duro: reforça o alfa
        masc = [(cor, mascara(pt, f, xh, base, w, h, contorno=e, **kw).point(lambda v: min(255, v * 2))) for cor, e in camadas]
        masc.append((letra, m_letra))
        bb = masc[0][1].getbbox()
        dx = (l0 - bb[0]) if bb and bb[0] < l0 else (l1 - bb[2]) if bb and bb[2] > l1 else 0
        for cor, m in masc:
            if dx: n = Image.new('L', m.size, 0); n.paste(m, (dx, 0)); m = n
            camada = Image.new('RGBA', im.size, cor); camada.putalpha(m)
            im.alpha_composite(camada)
    return im, codificar_blocos(original, im)


def lote_mundi():
    """Rótulos do mapa-múndi: transcrição mundi-es.json → PT pela tabela dos mapas, pelas falas ou pelos soltos."""
    from lote_menus_mm3d import MUNDI, MUNDI_AJUSTE, MUNDI_GRUPO, MUNDI_SOLTOS
    es_lista = json.loads((TEXTURAS / 'mundi-es.json').read_text(encoding='utf-8'))
    tab = json.loads((TEXTURAS / 'tabela-es-pt.json').read_text(encoding='utf-8'))['nomes']
    es, pt = _falas(); out = []
    for k, ((caixa, livre), nome) in enumerate(zip(MUNDI, es_lista)):
        ids = [k for k, v in sorted(es.items()) if v == nome]
        p = tab.get(nome) or MUNDI_SOLTOS.get(nome) or (pt.get(ids[0]) if ids else None)
        if not p: raise SystemExit(f'mapa-múndi: sem PT para {nome!r}')
        out.append(caixa + (p, {'livre': livre, 'grade': MUNDI_GRUPO[k], **MUNDI_AJUSTE.get(k, {})}))
    return out


# ------------------------------------------------------------ tabela e geração --
def _falas():
    """→ ({id: texto visível espanhol}, {id: texto visível PT-BR}) das mensagens."""
    import gmsg
    vis = lambda d: {e[0]: gmsg.visivel(gmsg.texto(p)).strip() for e, p in zip(*gmsg.ler(d)[1:]) if p}
    return vis(ler_romfs(ROMFS_ORIGINAL, 'message/eu/eus.gmsg')), vis((TRABALHO / 'eus-ptbr.gmsg').read_bytes())


def tabela():
    """mapas-es.json ({"001": "Norte de Ciudad Reloj"}) + eus.gmsg/eus-ptbr.gmsg → tabela-es-pt.json.

    O PT de cada nome é o da fala com o mesmo texto no espanhol (nomes de área, ids 0x100–0x1a0), para a placa
    dizer o mesmo que as falas. Nome sem fala igual fica em branco para preencher à mão."""
    es, pt = _falas()
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
    """→ {(caminho, membro): ctxb PT-BR}; grava PNG e CTXB em TEXTURAS/pt/. `so`: 'dia', 'mapa' ou 'menu'."""
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
        elif g == 'menu' and (m in LOTE or m == 'world_map_base00.ctxb'):
            im, novo = menu_pt(m, d, lote_mundi() if m == 'world_map_base00.ctxb' else LOTE[m],
                               outras_linguas(c, m, len(d)))
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
