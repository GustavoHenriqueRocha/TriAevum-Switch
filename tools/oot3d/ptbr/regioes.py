"""Acha as áreas de texto de uma textura: onde inglês e espanhol diferem, agrupadas em retângulos."""
import sys; sys.path.insert(0, '.')
from ctxb import decodificar, ler


def regioes(nome, juntar=4, minimo=6):
    a = decodificar(ler('01_US_ENGLISH', nome)); b = decodificar(ler('06_EU_SPANISH', nome))
    w, h = a.size; pa, pb = a.load(), b.load()
    dif = [[abs(pa[x, y][3] - pb[x, y][3]) > 30 or sum(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > 90
            for x in range(w)] for y in range(h)]
    visto = [[False] * w for _ in range(h)]; caixas = []
    for y in range(h):
        for x in range(w):
            if dif[y][x] and not visto[y][x]:
                pilha = [(x, y)]; visto[y][x] = True; x0 = x1 = x; y0 = y1 = y
                while pilha:
                    cx, cy = pilha.pop()
                    x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                    for nx in range(cx - juntar, cx + juntar + 1):
                        for ny in range(cy - 2, cy + 3):
                            if 0 <= nx < w and 0 <= ny < h and dif[ny][nx] and not visto[ny][nx]:
                                visto[ny][nx] = True; pilha.append((nx, ny))
                if (x1 - x0) * (y1 - y0) >= minimo:
                    caixas.append((x0, y0, x1 + 1, y1 + 1))
    return sorted(caixas, key=lambda c: (c[1] // 8, c[0]))


if __name__ == '__main__':
    for c in regioes(sys.argv[1]): print(c)
