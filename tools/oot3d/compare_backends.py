#!/usr/bin/env python3
"""Mesa (GL) vs deko3d on the console: compares two logs/perf.txt (last session of each).

Per window of 150 frames perf.txt holds fps, ms per phase, clocks, CPU % per
thread role ("threads:" line), used memory; GL holds "glperf"
(config/gpu_timing: <kind>_gpu_ms) and deko3d holds "dkperf" (gpu_ms per frame,
timestamps of the frame's start and end). Only compare measurements at the same clock,
in the same stretch of the game and with deko3d without config/deko3d_sync_check and without
libdeko3dd (TRIAEVUM_DEKO3D_DEBUG=OFF).

Usage: compare_backends.py mesa/perf.txt deko3d/perf.txt [--skip 4]
     compare_backends.py perf.txt            # one file: the last GL session and the last deko3d one
"""
import argparse
import re
import statistics

KV = re.compile(r'([A-Za-z_][\w]*)=([-\d.]+)')


def all_sessions(path):
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
                # busy_ms (p6m+) excludes the acquire wait; before there was only gpu_ms including it.
                gpu = float(kv.get('busy_ms', kv.get('gpu_ms', 'nan')))
            else:
                # Average of the frame kinds with a GPU sample, weighted by frames.
                tot = n = 0.0
                for kind in ('step', 'interp', 'hold'):
                    if f'{kind}_gpu_ms' in kv:
                        f = float(kv.get(f'{kind}_frames', 0))
                        tot += float(kv[f'{kind}_gpu_ms']) * f
                        n += f
                gpu = tot / n if n else float('nan')
            cur[-1]['gpu'] = gpu
            cur[-1]['dk'] = line.startswith('dkperf')
    return sessions


def last_session(path, deko3d=None):
    """The last session; with deko3d=True/False, the last one with/without a dkperf line."""
    for session in reversed(all_sessions(path)):
        if deko3d is None or any(w.get('dk', False) for w in session) == deko3d:
            return session
    return []


def summary(wins):
    def col(get):
        vals = [v for v in (get(w) for w in wins) if v is not None and v == v]
        return (statistics.mean(vals), max(vals)) if vals else (float('nan'), float('nan'))
    out = {
        'fps': col(lambda w: float(w['main'].get('fps', 'nan'))),
        'guest_fps': col(lambda w: float(w['main'].get('guest_fps', 'nan'))),
        'frame ms (total_ms)': col(lambda w: float(w['main'].get('total_ms', 'nan'))),
        'GPU ms/frame': col(lambda w: w['gpu']),
        'present ms': col(lambda w: float(w['main'].get('present', 'nan'))),
        'pica_backend ms': col(lambda w: float(w['main'].get('pica_backend', 'nan'))),
        'used memory MB': col(lambda w: float(w['main'].get('used_mb', 'nan'))),
    }
    roles = sorted({r for w in wins for r in w['cpu']})
    for r in roles:
        out[f'CPU {r} %'] = col(lambda w, r=r: w['cpu'].get(r))
    # Rounded to 50 MHz: the reading oscillates a few MHz between windows.
    r50 = lambda v: int(round(float(v) / 50) * 50) if v not in (None, '-1') else None
    clocks = {(r50(w['main'].get('cpu_mhz')), r50(w['main'].get('gpu_mhz')), r50(w['main'].get('mem_mhz')))
              for w in wins}
    return out, clocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mesa')
    ap.add_argument('deko3d', nargs='?')
    ap.add_argument('--skip', type=int, default=4, help='initial windows ignored (loading)')
    a = ap.parse_args()
    res = []
    picks = [(a.mesa, None), (a.deko3d, None)] if a.deko3d else [(a.mesa, False), (a.mesa, True)]
    for path, kind in picks:
        wins = last_session(path, kind)[a.skip:]
        s, clocks = summary(wins)
        res.append((len(wins), s, clocks))
    print(f"windows: mesa {res[0][0]}, deko3d {res[1][0]}")
    for name, (n, s, clocks) in zip(('mesa', 'deko3d'), res):
        print(f"clocks {name} (cpu, gpu, mem MHz): {sorted(clocks)}")
    if res[0][2] != res[1][2]:
        print("WARNING: different clocks between the measurements; the comparison is not valid")
    keys = list(dict.fromkeys(list(res[0][1]) + list(res[1][1])))
    print(f"{'metric':26} {'mesa avg':>9} {'max':>8}  {'deko3d avg':>10} {'max':>8}  {'Δ%':>7}")
    for k in keys:
        m = res[0][1].get(k, (float('nan'),) * 2)
        d = res[1][1].get(k, (float('nan'),) * 2)
        delta = (d[0] - m[0]) / m[0] * 100 if m[0] == m[0] and d[0] == d[0] and m[0] else float('nan')
        print(f"{k:26} {m[0]:9.2f} {m[1]:8.2f}  {d[0]:10.2f} {d[1]:8.2f}  {delta:7.1f}")


if __name__ == '__main__':
    main()
