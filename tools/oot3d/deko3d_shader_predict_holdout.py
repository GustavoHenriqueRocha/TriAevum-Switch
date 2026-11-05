# Teste de previsão: esconde os shaders da v7 e verifica se templates antigos x TEV dos CMB os geram.
# Uso: python3 tools/oot3d/deko3d_shader_predict_holdout.py <pasta com gen/ do register_glsl_dump>
# 05/11: 20 templates x 369 TEV = 7380 previstos; acertou 12 de 16 da v7.
import sys, re, pathlib
sys.path.insert(0, 'tools/oot3d'); import deko3d_shader_pack as p
S = pathlib.Path(sys.argv[1]); H = pathlib.Path.home() / 'oot3d-work'
TEV = re.compile(r'^\s*precise (vec3 color|float alpha)_output_\d = .*$', re.M)
v7 = {f.stem[5:]: f.read_text() for f in (H / 'deko3d-pack-v7/glsl').glob('frag_*.glsl')}
older = {}
for f in H.glob('**/frag_*.glsl'):
    if 'build' in str(f) or f.stem[5:] in v7: continue
    older[f.stem[5:]] = f.read_text()
templates = {TEV.sub('@@', t) for t in older.values() if len(TEV.findall(t)) == 12}
sigs = set()
for g in (S / 'gen').glob('*.glsl'):
    lines = [m.group(0) for m in TEV.finditer(g.read_text())]
    if len(lines) == 12: sigs.add(tuple(lines))
for t in older.values():
    lines = [m.group(0) for m in TEV.finditer(t)]
    if len(lines) == 12: sigs.add(tuple(lines))
pred = set()
for tpl in templates:
    parts = tpl.split('@@')
    if len(parts) != 13: continue
    for s in sigs:
        text = parts[0] + ''.join(l + parts[i + 1] for i, l in enumerate(s))
        pred.add('%016x' % p.shader_text_key(1, text.encode()))
hit = [k for k in v7 if k in pred]
print(f'templates={len(templates)} assinaturas={len(sigs)} previstos={len(pred)}')
print(f'v7 (16 novos): previstos {len(hit)} de {len(v7)}')
