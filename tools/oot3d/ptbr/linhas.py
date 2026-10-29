"""Divide uma área de texto em linhas e, em cada linha, em blocos separados por espaço largo."""
import sys; sys.path.insert(0, '.')


def linhas(im, area, vao=10, limiar=150):
    px = im.load(); x0, y0, x1, y1 = area
    texto = lambda x, y: px[x, y][3] > 150 and max(px[x, y][:3]) > limiar
    ys = [y for y in range(y0, y1) if any(texto(x, y) for x in range(x0, x1))]
    faixas = []
    for y in ys:
        if faixas and y - faixas[-1][1] <= 2: faixas[-1][1] = y
        else: faixas.append([y, y])
    out = []
    for a, b in faixas:
        xs = [x for x in range(x0, x1) if any(texto(x, y) for y in range(a, b + 1))]
        grupos = []
        for x in xs:
            if grupos and x - grupos[-1][1] <= vao: grupos[-1][1] = x
            else: grupos.append([x, x])
        out += [(g0 - 1, a - 1, g1 + 2, b + 2) for g0, g1 in grupos]
    return out
