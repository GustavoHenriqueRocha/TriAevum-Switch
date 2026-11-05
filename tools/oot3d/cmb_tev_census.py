# Uso: python3 tools/oot3d/cmb_tev_census.py <pasta-romfs-extraida>
# Spike 05/11: 1998 modelos, 11173 materiais, 534 programas TEV únicos (pack v7 tem 346 fragment).
# Conta programas TEV únicos em todos os CMB do romfs (procura a magia "cmb ").
import struct, sys, os, collections, json
root = sys.argv[1]
progs = collections.Counter(); progs_full = collections.Counter(); stagesets = collections.Counter()
nmodels = nmats = 0; bad = 0
def scan(d, src):
    global nmodels, nmats, bad
    i = d.find(b'cmb ')
    while i >= 0:
        try:
            size = struct.unpack_from('<I', d, i+4)[0]
            c = d[i:i+size]
            mats = struct.unpack_from('<I', c, 0x28)[0]; tex = struct.unpack_from('<I', c, 0x2C)[0]
            if c[mats:mats+4] != b'mats': raise ValueError
            n = struct.unpack_from('<I', c, mats+8)[0]
            end = mats+0xC+n*0x15C
            ncomb = (tex-end)//0x28
            combs = [c[end+k*0x28:end+k*0x28+0x24] for k in range(ncomb)]  # sem o índice de cor constante
            nmodels += 1
            for m in range(n):
                b = mats+0xC+m*0x15C
                cnt = struct.unpack_from('<I', c, b+0x120)[0]
                idx = struct.unpack_from('<6H', c, b+0x124)[:cnt]
                stages = tuple(combs[k] for k in idx)
                frag = c[b]; tex_used = struct.unpack_from('<I', c, b+8)[0]
                atest = c[b+0x130]; afunc = struct.unpack_from('<H', c, b+0x132)[0] if atest else 0
                progs[stages] += 1
                progs_full[(stages, frag, tex_used, atest, afunc)] += 1
                nmats += 1
        except Exception:
            bad += 1
        i = d.find(b'cmb ', i+4)
for dp, _, fs in os.walk(root):
    for fn in fs:
        if fn.endswith(('.zar', '.zsi', '.gar', '.cmb')):
            scan(open(os.path.join(dp, fn), 'rb').read(), fn)
print(f'modelos={nmodels} materiais={nmats} falhas={bad}')
print(f'TEV únicos (só combiners)={len(progs)}')
print(f'TEV + luz de fragmento + nº texturas + alpha test={len(progs_full)}')
print('estágios por programa:', sorted(collections.Counter(len(s) for s in progs).items()))
