"""Patch PT-BR do Majora's Mask 3D (EUR Rev 1): o português entra no slot do espanhol (message/eu/eus.gmsg).

    python -I mm3d.py exportar   # eue.gmsg → TRABALHO/pendentes.json (textos únicos em inglês, códigos como ⟦..⟧)
    python -I mm3d.py conferir   # os 6 .gmsg: texto → arquivo → texto, byte a byte igual ao original
    python -I mm3d.py montar     # TRABALHO/traducao.json → eus-ptbr.gmsg e romfs-ptbr.bin (corrigida no lugar);
                                 # com texturas/tabela-es-pt.json, também as texturas com texto (mm3d_texturas.py)

TRABALHO = ~/oot3d-work/conv/mm3d-ptbr (TRIAEVUM_PTBR); romfs original = TRABALHO/romfs/romfs.bin (TRIAEVUM_ROMFS),
extraída com tools/ctr/ncch_extract.py. traducao.json: {"0x0002": "texto PT-BR", ...}; a tradução de um id vale
também para os outros ids com o mesmo inglês (a não ser que eles tenham a própria). Glossário: o do OoT3D
(~/oot3d-work/ptbr/glossario.json).
"""
import json, shutil, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gmsg
from caminhos import pastas
from romfs import localizar

TRABALHO, ROMFS_ORIGINAL = pastas('mm3d')
ROMFS_PTBR = TRABALHO / 'romfs-ptbr.bin'
INGLES, ALVO, FONTE = 'message/eu/eue.gmsg', 'message/eu/eus.gmsg', 'message/ltn16.gzf'
IDIOMAS = 'defgis'


def ler_romfs(caminho, alvo):
    o, tam = localizar(caminho, alvo)
    with open(caminho, 'rb') as f:
        f.seek(o); return f.read(tam)


def traduzivel(t):
    return bool(gmsg.visivel(t).strip())


def pendentes(ingles):
    """[{id, en, repetidas?}] dos textos únicos com algo a traduzir, na ordem dos ids."""
    _, ents, pls = gmsg.ler(ingles); primeiro = {}
    for e, p in zip(ents, pls):
        t = gmsg.texto(p)
        if traduzivel(t):
            primeiro.setdefault(t, []).append(f'{e[0]:#06x}')
    return [dict(id=ids[0], en=t, **({'repetidas': ids[1:]} if ids[1:] else {})) for t, ids in primeiro.items()]


def aplicar(ingles, alvo, trad, fonte):
    """→ (novo .gmsg do slot alvo, {id trocado: texto PT-BR}, problemas). Mensagem sem tradução fica byte a byte igual.

    Os códigos da tradução (fora a cor) têm de seguir os do inglês ou os do espanhol do mesmo id: o espanhol
    reorganiza caixas, tira o plural ou usa outro sufixo de ordinal em 503 das 6152 mensagens."""
    _, ents_en, pls_en = gmsg.ler(ingles); cab, ents, pls = gmsg.ler(alvo)
    if [e[0] for e in ents] != [e[0] for e in ents_en]:
        raise gmsg.ErroGmsg('ids do inglês e do alvo diferem')
    en = {e[0]: gmsg.texto(p) for e, p in zip(ents_en, pls_en)}
    es = {e[0]: gmsg.texto(p) for e, p in zip(ents, pls)}
    por_ingles = {}
    for k in trad:
        mid = int(k, 16)
        if mid in en: por_ingles.setdefault(en[mid], k)
    novos, trocados, prob = list(pls), {}, []
    for i, e in enumerate(ents):
        mid = e[0]; k = f'{mid:#06x}'
        pt = trad.get(k) if k in trad else trad.get(por_ingles.get(en[mid], ''))
        if pt is None: continue
        if gmsg.codigos(pt) not in (gmsg.codigos(en[mid]), gmsg.codigos(es[mid])):
            prob.append((k, 'códigos diferentes do inglês e do espanhol')); continue
        fora = {c for c in gmsg.visivel(pt) if c != '\n' and ord(c) not in fonte}
        if fora: prob.append((k, 'sem glifo: ' + ''.join(sorted(fora)))); continue
        novos[i] = gmsg.para_bytes(pt); trocados[mid] = pt
    for k in trad:
        if int(k, 16) not in en: prob.append((k, 'id não existe'))
    return gmsg.montar(cab, ents, novos), trocados, prob


def conferir():
    for L in IDIOMAS:
        d = ler_romfs(ROMFS_ORIGINAL, f'message/eu/eu{L}.gmsg'); cab, ents, pls = gmsg.ler(d)
        volta = gmsg.montar(cab, ents, [gmsg.para_bytes(gmsg.texto(p)) if p else b'' for p in pls])
        print(f'eu{L}.gmsg: {len(ents)} mensagens,', 'byte a byte igual' if volta == d else 'DIFERENTE')
        if volta != d: raise SystemExit(1)


def montar():
    ingles, alvo = ler_romfs(ROMFS_ORIGINAL, INGLES), ler_romfs(ROMFS_ORIGINAL, ALVO)
    fonte = gmsg.glifos_gzf(ler_romfs(ROMFS_ORIGINAL, FONTE))
    trad = json.loads((TRABALHO / 'traducao.json').read_text(encoding='utf-8'))
    novo, trocados, prob = aplicar(ingles, alvo, trad, fonte)
    if prob: raise SystemExit(f'traducao.json com problemas: {prob[:5]}')
    if len(novo) > len(alvo):
        raise SystemExit(f'eus.gmsg PT-BR ficou maior que o original ({len(novo)} > {len(alvo)})')
    if not ROMFS_PTBR.exists(): shutil.copyfile(ROMFS_ORIGINAL, ROMFS_PTBR)
    o, tam = localizar(ROMFS_PTBR, ALVO)
    with open(ROMFS_PTBR, 'r+b') as f:
        f.seek(o); f.write(novo + b'\0' * (tam - len(novo)))
    (TRABALHO / 'eus-ptbr.gmsg').write_bytes(novo)
    # confere o que foi gravado: traduzidas voltam ao texto PT-BR, as outras ficam iguais ao espanhol original
    gravado = ler_romfs(ROMFS_PTBR, ALVO)[:len(novo)]
    _, ents, pls = gmsg.ler(gravado); _, _, orig = gmsg.ler(alvo)
    iguais = sum(p == q for e, p, q in zip(ents, pls, orig) if e[0] not in trocados)
    voltam = sum(gmsg.texto(p) == trocados[e[0]] for e, p in zip(ents, pls) if e[0] in trocados)
    if iguais != len(ents) - len(trocados) or voltam != len(trocados):
        raise SystemExit(f'ida e volta falhou: {iguais} intactas, {voltam} traduzidas voltam ao texto')
    print(f'eus.gmsg PT-BR: {len(novo)} de {tam} bytes; {len(trocados)} mensagens trocadas, '
          f'{iguais} intactas de {len(ents) - len(trocados)}; {voltam} voltam ao texto PT-BR igual')
    if (TRABALHO / 'texturas' / 'tabela-es-pt.json').exists():       # texturas com texto (mm3d_texturas.py)
        import mm3d_texturas
        print(mm3d_texturas.gravar(ROMFS_PTBR), 'arquivos de textura do espanhol trocados pelo PT-BR')


def exportar():
    lista = pendentes(ler_romfs(ROMFS_ORIGINAL, INGLES))
    (TRABALHO / 'pendentes.json').write_text(json.dumps(lista, ensure_ascii=False, indent=1), encoding='utf-8')
    print(len(lista), 'textos únicos;', sum(1 + len(p.get('repetidas', ())) for p in lista), 'mensagens;',
          sum(len(gmsg.COD.sub('', p['en'])) for p in lista), 'caracteres sem códigos')


if __name__ == '__main__':
    {'exportar': exportar, 'conferir': conferir, 'montar': montar}[sys.argv[1] if len(sys.argv) > 1 else 'conferir']()
