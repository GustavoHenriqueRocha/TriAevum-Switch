"""Acha as áreas de texto de uma textura: onde inglês e espanhol diferem, agrupadas em retângulos."""
import sys; sys.path.insert(0, '.')


def regioes(nome, juntar=4, minimo=6):
    from ctxb import decodificar, ler
    return regioes_de(decodificar(ler('06_EU_SPANISH', nome)), [decodificar(ler('01_US_ENGLISH', nome))], juntar, minimo)


def regioes_de(base, outras, juntar=4, minimo=6, juntar_v=2):
    """Retângulos onde `base` difere de qualquer uma das `outras` (mesma textura em outros idiomas)."""
    w, h = base.size; pb = base.load(); pos = [o.load() for o in outras]
    dif = [[any(abs(pa[x, y][3] - pb[x, y][3]) > 30 or sum(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > 90
                for pa in pos) for x in range(w)] for y in range(h)]
    visto = [[False] * w for _ in range(h)]; caixas = []
    for y in range(h):
        for x in range(w):
            if dif[y][x] and not visto[y][x]:
                pilha = [(x, y)]; visto[y][x] = True; x0 = x1 = x; y0 = y1 = y
                while pilha:
                    cx, cy = pilha.pop()
                    x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
                    for nx in range(cx - juntar, cx + juntar + 1):
                        for ny in range(cy - juntar_v, cy + juntar_v + 1):
                            if 0 <= nx < w and 0 <= ny < h and dif[ny][nx] and not visto[ny][nx]:
                                visto[ny][nx] = True; pilha.append((nx, ny))
                if (x1 - x0) * (y1 - y0) >= minimo:
                    caixas.append((x0, y0, x1 + 1, y1 + 1))
    return sorted(caixas, key=lambda c: (c[1] // 8, c[0]))


if __name__ == '__main__':
    for c in regioes(sys.argv[1]): print(c)


def mascara_dif(base, outras):
    """[[bool]]: pixel de `base` que difere de alguma das `outras` (mesmo critério de regioes_de)."""
    w, h = base.size; pb = base.load(); pos = [o.load() for o in outras]
    return [[any(abs(pa[x, y][3] - pb[x, y][3]) > 30 or sum(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3)) > 90
                 for pa in pos) for x in range(w)] for y in range(h)]


def cortes_xy(dif, caixa, vao_x=5, vao_y=1, minimo=6):
    """Divide `caixa` em rótulos por cortes alternados: linhas vazias (≥ vao_y) e colunas vazias (≥ vao_x) da
    máscara. → caixas justas, em ordem de leitura."""
    def cheio(x0, y0, x1, y1, horiz):
        return [any(dif[y][x] for x in range(x0, x1)) for y in range(y0, y1)] if horiz else \
               [any(dif[y][x] for y in range(y0, y1)) for x in range(x0, x1)]

    def partes(flags, vao):
        out, ini, vazio = [], None, 0
        for i, f in enumerate(flags + [False] * vao):
            if f:
                if ini is None: ini = i
                vazio = 0; fim = i
            elif ini is not None:
                vazio += 1
                if vazio >= vao: out.append((ini, fim + 1)); ini = None
        return out

    def cortar(x0, y0, x1, y1, horiz, tentou=False):
        p = partes(cheio(x0, y0, x1, y1, horiz), vao_y if horiz else vao_x)
        if not p: return []
        if len(p) == 1:
            a, b = p[0]
            nova = (x0, y0 + a, x1, y0 + b) if horiz else (x0 + a, y0, x0 + b, y1)
            if tentou: return [nova] if (nova[2] - nova[0]) * (nova[3] - nova[1]) >= minimo else []
            return cortar(*nova, not horiz, True)
        out = []
        for a, b in p:
            out += cortar(*((x0, y0 + a, x1, y0 + b) if horiz else (x0 + a, y0, x0 + b, y1)), not horiz)
        return out
    return cortar(*caixa, True)
