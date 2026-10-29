"""Troca rótulos em botões: reconstrói o fundo e escreve o PT-BR com as cores do original."""
import sys; sys.path.insert(0, '.')
from PIL import Image
from ctxb import decodificar, ler
from regioes import regioes
from rotulos import rotulo


def _fundo(im, caixa, folga=2, placa=False):
    """Apaga o texto repetindo, em cada linha, a cor logo à esquerda (os botões têm degradê vertical).

    Com `placa`, a caixa é a própria placa do título: mantém as pontas e refaz só o miolo."""
    x0, y0, x1, y1 = caixa; px = im.load(); w, h = im.size
    if placa:
        refs = {}
        for y in range(y0, y1):
            cores = sorted((px[x, y] for x in range(x0 + 4, x1 - 4)
                            if px[x, y][3] > 200 and max(px[x, y][:3]) - min(px[x, y][:3]) > 25),
                           key=lambda c: sum(c[:3]))
            if len(cores) >= 3:
                refs[y] = cores[len(cores) // 2]
        if not refs:
            return
        for y in range(y0, y1):
            ref = refs.get(y) or refs[min(refs, key=lambda k: abs(k - y))]
            for x in range(x0 + 2, x1 - 2):
                if px[x, y][3] > 200:
                    px[x, y] = ref
        return
    for y in range(max(0, y0 - folga), min(h, y1 + folga)):
        ref = px[max(0, x0 - folga - 1), y]
        for x in range(max(0, x0 - folga), min(w, x1 + folga)):
            px[x, y] = ref


def _estilo(orig, fundo, caixa):
    po, pf = orig.load(), fundo.load(); x0, y0, x1, y1 = caixa; texto = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            a, b = po[x, y], pf[x, y]
            if a[3] > 200 and sum(abs(a[i] - b[i]) for i in range(3)) > 60:
                texto.append(a)
    texto.sort(key=lambda c: sum(c[:3]))
    n = max(1, len(texto) // 5)
    media = lambda l: tuple(sum(c[i] for c in l) // len(l) for i in range(3)) + (255,)
    return media(texto[-n:]), media(texto[:n])


def caixa_do_texto(im, area, limiar=235):
    """Caixa dos pixels claros e opacos dentro de `area` (para rótulos iguais em inglês e espanhol)."""
    px = im.load(); x0, y0, x1, y1 = area; pts = [(x, y) for y in range(y0, y1) for x in range(x0, x1)
                                              if px[x, y][3] > 200 and min(px[x, y][:3]) > limiar]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return (min(xs) - 1, min(ys) - 1, max(xs) + 2, max(ys) + 2)


def trocar(nome, rotulos, base='06_EU_SPANISH', altura=None, peso='ExtraBold', fator=0.8, placa=False, alinhar='centro',
           transparente=False, cores=None, inicial=None):
    """rotulos: {índice da região | ('area', x0, y0, x1, y1) | ('caixa', x0, y0, x1, y1): 'texto PT'}."""
    orig = decodificar(ler(base, nome)); im = (inicial or orig).copy(); caixas = regioes(nome)
    for i, txt in rotulos.items():
        if isinstance(i, tuple):
            caixa = caixa_do_texto(orig, i[1:]) if i[0] == 'area' else i[1:]
        else:
            caixa = caixas[i]
        x0, y0, x1, y1 = caixa
        if transparente:
            im.paste((0, 0, 0, 0), caixa)
        else:
            _fundo(im, caixa, placa=placa)
        cor, contorno = cores or _estilo(orig, im, caixa)
        cap = altura or max(6, round((y1 - y0 - 4) * fator))
        alvo = (x0 + 5, y0, x1 - 5, y1) if placa else (x0, y0 - 1, x1, y1 + 1) if alinhar == 'esquerda' \
            else (x0 - 4, y0 - 2, x1 + 4, y1 + 2)
        rotulo(im, alvo, txt, altura_maiuscula=cap, cor=cor, contorno=contorno,
               espessura=1.5, limpar=False, peso=peso, alinhar=alinhar)
    return im, caixas


def previa(im, caminho, escala=2):
    ver = Image.new('RGBA', im.size, (40, 40, 60, 255)); ver.alpha_composite(im)
    ver.resize((im.width * escala, im.height * escala), Image.NEAREST).convert('RGB').save(caminho)
