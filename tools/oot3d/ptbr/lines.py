"""Splits a text area into lines and, in each line, into blocks separated by wide spacing."""
import sys; sys.path.insert(0, '.')


def split_lines(im, area, gap=10, threshold=150):
    px = im.load(); x0, y0, x1, y1 = area
    is_text = lambda x, y: px[x, y][3] > 150 and max(px[x, y][:3]) > threshold
    ys = [y for y in range(y0, y1) if any(is_text(x, y) for x in range(x0, x1))]
    bands = []
    for y in ys:
        if bands and y - bands[-1][1] <= 2: bands[-1][1] = y
        else: bands.append([y, y])
    out = []
    for a, b in bands:
        xs = [x for x in range(x0, x1) if any(is_text(x, y) for y in range(a, b + 1))]
        groups = []
        for x in xs:
            if groups and x - groups[-1][1] <= gap: groups[-1][1] = x
            else: groups.append([x, x])
        out += [(g0 - 1, a - 1, g1 + 2, b + 2) for g0, g1 in groups]
    return out
