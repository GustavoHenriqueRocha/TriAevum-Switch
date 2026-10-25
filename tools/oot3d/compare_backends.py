#!/usr/bin/env python3
"""Mesa (GL) x deko3d no console: compara dois logs/perf.txt (última sessão de cada).

Por janela de 150 quadros o perf.txt traz fps, ms por fase, clocks, CPU % por
papel de thread (linha "threads:"), memória usada; o GL traz "glperf"
(config/gpu_timing: <tipo>_gpu_ms) e o deko3d traz "dkperf" (gpu_ms por quadro,
timestamps do começo e do fim do quadro). Só compare medições no mesmo clock,
no mesmo trecho do jogo e com o deko3d sem config/deko3d_sync_check e sem a
libdeko3dd (TRIAEVUM_DEKO3D_DEBUG=OFF).

Uso: compare_backends.py mesa/perf.txt deko3d/perf.txt [--skip 4]
"""
import argparse
import re
import statistics

KV = re.compile(r'([A-Za-z_][\w]*)=([-\d.]+)')


def last_session(path):
    sessions, cur, prev = [], None, 1 << 62
    for line in open(path, errors='replace'):
        if line.startswith('host_frames='):
            kv = dict(KV.findall(line))
            h = int(float(kv.get('host_frames', 0)))
            if h < prev or cur is None:
                cur = []
                sessions.append(cur)
            prev = h
            cpu = {m.group(1): float(m.group(2)) for m in re.finditer(r' (\w+)_cpu=([\d.]+)', line.split('threads:', 1)[-1])} \
                if 'threads:' in line else {}
            cur.append({'main': kv, 'cpu': cpu, 'gpu': None, 'op': re.search(r'op_mode=(\w+)', line)})
        elif cur and line.startswith(('glperf ', 'dkperf ')):
            kv = dict(KV.findall(line))
            if line.startswith('dkperf'):
                gpu = float(kv.get('gpu_ms', 'nan'))
            else:
                # Média dos tipos de quadro com amostra de GPU, ponderada pelos quadros.
                tot = n = 0.0
                for kind in ('step', 'interp', 'hold'):
                    if f'{kind}_gpu_ms' in kv:
                        f = float(kv.get(f'{kind}_frames', 0))
                        tot += float(kv[f'{kind}_gpu_ms']) * f
                        n += f
                gpu = tot / n if n else float('nan')
            cur[-1]['gpu'] = gpu
    return sessions[-1] if sessions else []


def summary(wins):
    def col(get):
        vals = [v for v in (get(w) for w in wins) if v is not None and v == v]
        return (statistics.mean(vals), max(vals)) if vals else (float('nan'), float('nan'))
    out = {
        'fps': col(lambda w: float(w['main'].get('fps', 'nan'))),
        'guest_fps': col(lambda w: float(w['main'].get('guest_fps', 'nan'))),
        'quadro ms (total_ms)': col(lambda w: float(w['main'].get('total_ms', 'nan'))),
        'GPU ms/quadro': col(lambda w: w['gpu']),
        'present ms': col(lambda w: float(w['main'].get('present', 'nan'))),
        'pica_backend ms': col(lambda w: float(w['main'].get('pica_backend', 'nan'))),
        'memória usada MB': col(lambda w: float(w['main'].get('used_mb', 'nan'))),
    }
    roles = sorted({r for w in wins for r in w['cpu']})
    for r in roles:
        out[f'CPU {r} %'] = col(lambda w, r=r: w['cpu'].get(r))
    # Arredondado a 50 MHz: a leitura oscila alguns MHz entre janelas.
    r50 = lambda v: int(round(float(v) / 50) * 50) if v not in (None, '-1') else None
    clocks = {(r50(w['main'].get('cpu_mhz')), r50(w['main'].get('gpu_mhz')), r50(w['main'].get('mem_mhz')))
              for w in wins}
    return out, clocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mesa')
    ap.add_argument('deko3d')
    ap.add_argument('--skip', type=int, default=4, help='janelas iniciais ignoradas (carregamento)')
    a = ap.parse_args()
    res = []
    for path in (a.mesa, a.deko3d):
        wins = last_session(path)[a.skip:]
        s, clocks = summary(wins)
        res.append((len(wins), s, clocks))
    print(f"janelas: mesa {res[0][0]}, deko3d {res[1][0]}")
    for name, (n, s, clocks) in zip(('mesa', 'deko3d'), res):
        print(f"clocks {name} (cpu, gpu, mem MHz): {sorted(clocks)}")
    if res[0][2] != res[1][2]:
        print("AVISO: clocks diferentes entre as medições; a comparação não vale")
    keys = list(dict.fromkeys(list(res[0][1]) + list(res[1][1])))
    print(f"{'métrica':26} {'mesa méd':>9} {'máx':>8}  {'deko3d méd':>10} {'máx':>8}  {'Δ%':>7}")
    for k in keys:
        m = res[0][1].get(k, (float('nan'),) * 2)
        d = res[1][1].get(k, (float('nan'),) * 2)
        delta = (d[0] - m[0]) / m[0] * 100 if m[0] == m[0] and d[0] == d[0] and m[0] else float('nan')
        print(f"{k:26} {m[0]:9.2f} {m[1]:8.2f}  {d[0]:10.2f} {d[1]:8.2f}  {delta:7.1f}")


if __name__ == '__main__':
    main()
