"""Swaps labels on buttons: rebuilds the background and writes the PT-BR with the original's colors."""
import sys; sys.path.insert(0, '.')
from PIL import Image
from ctxb import decode, read
from regions import regions
from labels import label


def _background(im, box, margin=2, plate=False):
    """Erases the text by repeating, on each row, the color just to the left (the buttons have a vertical gradient).

    With `plate`, the box is the title plate itself: keeps the ends and redraws only the middle."""
    x0, y0, x1, y1 = box; px = im.load(); w, h = im.size
    if plate:
        refs = {}
        for y in range(y0, y1):
            colors = sorted((px[x, y] for x in range(x0 + 4, x1 - 4)
                            if px[x, y][3] > 200 and max(px[x, y][:3]) - min(px[x, y][:3]) > 25),
                           key=lambda c: sum(c[:3]))
            if len(colors) >= 3:
                refs[y] = colors[len(colors) // 2]
        if not refs:
            return
        for y in range(y0, y1):
            ref = refs.get(y) or refs[min(refs, key=lambda k: abs(k - y))]
            for x in range(x0 + 2, x1 - 2):
                if px[x, y][3] > 200:
                    px[x, y] = ref
        return
    for y in range(max(0, y0 - margin), min(h, y1 + margin)):
        ref = px[max(0, x0 - margin - 1), y]
        for x in range(max(0, x0 - margin), min(w, x1 + margin)):
            px[x, y] = ref


def _style(orig, background, box):
    po, pf = orig.load(), background.load(); x0, y0, x1, y1 = box; text = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            a, b = po[x, y], pf[x, y]
            if a[3] > 200 and sum(abs(a[i] - b[i]) for i in range(3)) > 60:
                text.append(a)
    text.sort(key=lambda c: sum(c[:3]))
    n = max(1, len(text) // 5)
    mean = lambda l: tuple(sum(c[i] for c in l) // len(l) for i in range(3)) + (255,)
    return mean(text[-n:]), mean(text[:n])


def text_box(im, area, threshold=235):
    """Box of the light, opaque pixels inside `area` (for labels identical in English and Spanish)."""
    px = im.load(); x0, y0, x1, y1 = area; pts = [(x, y) for y in range(y0, y1) for x in range(x0, x1)
                                              if px[x, y][3] > 200 and min(px[x, y][:3]) > threshold]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return (min(xs) - 1, min(ys) - 1, max(xs) + 2, max(ys) + 2)


def swap(name, labels, base='06_EU_SPANISH', height=None, weight='ExtraBold', factor=0.8, plate=False, align='center',
           transparent=False, colors=None, initial=None):
    """labels: {region index | ('area', x0, y0, x1, y1) | ('box', x0, y0, x1, y1): 'PT text'}."""
    orig = decode(read(base, name)); im = (initial or orig).copy(); boxes = regions(name)
    for i, txt in labels.items():
        if isinstance(i, tuple):
            box = text_box(orig, i[1:]) if i[0] == 'area' else i[1:]
        else:
            box = boxes[i]
        x0, y0, x1, y1 = box
        if transparent:
            im.paste((0, 0, 0, 0), box)
        else:
            _background(im, box, plate=plate)
        color, outline = colors or _style(orig, im, box)
        cap = height or max(6, round((y1 - y0 - 4) * factor))
        target = (x0 + 5, y0, x1 - 5, y1) if plate else (x0, y0 - 1, x1, y1 + 1) if align == 'left' \
            else (x0 - 4, y0 - 2, x1 + 4, y1 + 2)
        label(im, target, txt, cap_height=cap, color=color, outline=outline,
               thickness=1.5, clear=False, weight=weight, align=align)
    return im, boxes


def preview(im, path, scale=2):
    canvas = Image.new('RGBA', im.size, (40, 40, 60, 255)); canvas.alpha_composite(im)
    canvas.resize((im.width * scale, im.height * scale), Image.NEAREST).convert('RGB').save(path)
