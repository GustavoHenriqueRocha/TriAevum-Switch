"""Finds the text areas of a texture: where English and Spanish differ, grouped into rectangles."""
import sys; sys.path.insert(0, '.')
from ctxb import decode, read


def regions(name, join_radius=4, min_area=6):
    a = decode(read('01_US_ENGLISH', name)); b = decode(read('06_EU_SPANISH', name))
    w, h = a.size; pa, pb = a.load(), b.load()
    diff = [[abs(pa[x, y][3] - pb[x, y][3]) > 30 or sum(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > 90
            for x in range(w)] for y in range(h)]
    seen = [[False] * w for _ in range(h)]; boxes = []
    for y in range(h):
        for x in range(w):
            if diff[y][x] and not seen[y][x]:
                stack = [(x, y)]; seen[y][x] = True; x0 = x1 = x; y0 = y1 = y
                while stack:
                    cx, cy = stack.pop()
                    x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                    for nx in range(cx - join_radius, cx + join_radius + 1):
                        for ny in range(cy - 2, cy + 3):
                            if 0 <= nx < w and 0 <= ny < h and diff[ny][nx] and not seen[ny][nx]:
                                seen[ny][nx] = True; stack.append((nx, ny))
                if (x1 - x0) * (y1 - y0) >= min_area:
                    boxes.append((x0, y0, x1 + 1, y1 + 1))
    return sorted(boxes, key=lambda c: (c[1] // 8, c[0]))


if __name__ == '__main__':
    for c in regions(sys.argv[1]): print(c)
