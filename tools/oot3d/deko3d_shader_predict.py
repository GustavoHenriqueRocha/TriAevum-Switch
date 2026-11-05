# Gera o GLSL do pack previsto: partes de cena (texto fora do bloco TEV+alpha test dos shaders
# conhecidos) x partes de material (bloco TEV dos CMB via oot3d_deko3d_register_glsl_dump),
# podado pela luz. Uso: python3 tools/oot3d/deko3d_shader_predict.py <pasta com gen/, regs.txt.luz, srcglsl/>
# 05/11: 8 cenas x 540 materiais -> 2181 fragment (v8glsl/); pack v8 = 2187 shaders, 7,5 MiB.
import sys, re, pathlib, collections
sys.path.insert(0, 'tools/oot3d'); import deko3d_shader_pack as p
S = pathlib.Path(sys.argv[1]); H = pathlib.Path.home() / 'oot3d-work'
# Parte do material: tudo entre o início do estágio 0 e o fim do estágio 5, mais o alpha test.
TEVBLOCK = re.compile(r'(\n[^\n]*precise vec3 color_output_0 = .*?\n[^\n]*precise float alpha_output_5 = [^\n]*\n(?:[^\n]*(?:combiner_output =|combiner_buffer|next_combiner_buffer)[^\n]*\n)*(?:[^\n]*int alpha_byte = [^\n]*\n[^\n]*discard;[^\n]*\n)?)', re.S)
ALPHA = re.compile(r'(\n[^\n]*int alpha_byte = [^\n]*\n[^\n]*discard;[^\n]*)')
def split(t):
    m = TEVBLOCK.search(t)
    if not m: return None
    tev = m.group(1); rest = t[:m.start()] + '\n@@TEV@@' + t[m.end():]
    alpha = ''
    return (tev, alpha), rest
def load(exclude=()):
    texts = {}
    for f in list(H.glob('**/frag_*.glsl')) + list((S / 'srcglsl').glob('frag_*.glsl')):
        if 'build' in str(f) or '/spike/gen' in str(f) or f.stem[5:] in exclude: continue
        texts[f.stem[5:]] = f.read_text()
    return texts
v7 = {f.stem[5:] for f in (H / 'deko3d-pack-v7/glsl').glob('frag_*.glsl')}
known = load(v7)
scenes = collections.Counter(); mats = set()
for t in known.values():
    s = split(t)
    if s: mats.add(s[0]); scenes[s[1]] += 1
for g in (S / 'gen').glob('*.glsl'):
    s = split(g.read_text())
    if s: mats.add(s[0])
print(f'partes de cena distintas: {len(scenes)}; partes de material: {len(mats)}')
print('cenas com @@ALPHA@@:', sum('@@ALPHA@@' in c for c in scenes))
pred = set()
for scene in scenes:
    for tev, alpha in mats:
        pred.add('%016x' % p.shader_text_key(1, scene.replace('\n@@TEV@@', tev).encode()))
alltexts = load()
print(f'previstos={len(pred)}; v7 previstos: {len(v7 & pred)} de {len(v7)}; conhecidos reproduzidos: {len(set(known) & pred)} de {len(known)}')
for scene, n in scenes.most_common():
    feats = [k for k, pat in (('luz', 'pica_diffuse_sum'), ('fog', 'fog_index'), ('wbuf', '/= gl_FragCoord.w'), ('shadow', 'pica_shadow'), ('proctex', 'proctex')) if pat in scene]
    used = sorted(set(re.findall(r'pica_sample_texture(\d)', scene)))
    print(f'{n:4d} usos, {len(scene.splitlines())} linhas, {feats}')
lit = sum(1 for tev, _ in mats if 'primary_fragment_color' in tev or 'secondary_fragment_color' in tev)
print('materiais que usam a cor da luz no TEV:', lit, 'de', len(mats))
def uses_light(tev): return 'primary_fragment_color' in tev or 'secondary_fragment_color' in tev
pruned = {}
for scene in scenes:
    lit_scene = 'pica_diffuse_sum' in scene
    for tev, _ in mats:
        if uses_light(tev) != lit_scene: continue
        text = scene.replace('\n@@TEV@@', tev)
        pruned['%016x' % p.shader_text_key(1, text.encode())] = text
kn = set(known)
print(f'podado: {len(pruned)}; v7 previstos: {len(v7 & set(pruned))} de 16; conhecidos: {len(kn & set(pruned))} de {len(kn)}')
out = S / 'v8glsl'; out.mkdir(exist_ok=True)
for k, t in pruned.items(): (out / f'frag_{k}.glsl').write_text(t)
flag = {l.strip() for l in open(S / 'regs.txt.luz')}
flagged = set()
for g in (S / 'gen').glob('*.glsl'):
    if g.stem in flag:
        s = split(g.read_text())
        if s: flagged.add(s[0][0])
for scene in scenes:
    if 'pica_diffuse_sum' not in scene: continue
    for tev in flagged:
        text = scene.replace('\n@@TEV@@', tev); pruned['%016x' % p.shader_text_key(1, text.encode())] = text
for k, t in load().items():
    if split(t): pruned[k] = t
print(f'final: {len(pruned)}; v7: {len(v7 & set(pruned))} de 16 (só com dados até a v6: ver acima); conhecidos: {len(kn & set(pruned))} de {len(kn)}')
import shutil; shutil.rmtree(out); out.mkdir()
for k, t in pruned.items(): (out / f'frag_{k}.glsl').write_text(t)
