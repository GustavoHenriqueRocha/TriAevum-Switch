#!/usr/bin/env python3
"""Tour do lab no console: FPS por área (janelas do perf.txt casadas aos acks pelo quadro do guest).

Uso: lab_tour_report.py <pasta com acks.jsonl e perf.txt> tools/oot3d/native_game_runtime/oot3d_dev_warp_catalog.inc
O roteiro (lab/script.jsonl) é: {"op":"gameplay"}, depois por área {"op":"warp","better":[i,0]} e
{"op":"wait","frames":300}, i = 0..48 (a 49 é a sala Debug e congela)."""
import json, re, sys
d = sys.argv[1]
names, inside = [], False
for line in open(sys.argv[2]):
    inside = inside or "kDevWarpBetterScenes[]" in line
    m = re.match(r'\s*\{" ?\d+:([^"]*)"', line)
    if inside and m: names.append(m.group(1))
acks = [json.loads(l) for l in open(f"{d}/acks.jsonl")]
# Última sessão: host_frames volta a diminuir quando o jogo reabre.
wins, prev = [], 1 << 62
for l in open(f"{d}/perf.txt"):
    if not l.startswith("host_frames="): continue
    kv = dict(re.findall(r'(\w+)=(\S+)', l))
    h = int(kv["host_frames"])
    if h < prev: wins = []
    prev = h
    wins.append(kv)
g, rows = 0.0, []
for kv in wins:
    fps, gfps = float(kv["fps"]), float(kv["guest_fps"])
    sec = 150 / fps if fps else 0
    rows.append((g, g + gfps * sec, fps, gfps, float(kv["guest"]), float(kv["total_ms"])))
    g += gfps * sec
start = {a["id"]: a["frame"] for a in acks if "frame" in a}
out = []
for i in range(49):
    a, b = start.get(i), start.get(f"m{i}")
    if a is None or b is None: continue
    # Janelas com mais da metade dentro do trecho medido (sem o carregamento).
    w = [r for r in rows if min(r[1], b) - max(r[0], a) > (r[1] - r[0]) / 2]
    if not w: continue
    lo = min(r[2] for r in w); avg = sum(r[2] for r in w) / len(w)
    out.append((lo, avg, max(r[5] for r in w), max(r[4] for r in w), sum(r[3] for r in w)/len(w), i))
print(f"{'área':<34} {'FPS mín':>7} {'FPS méd':>7} {'ms máx':>7} {'guest ms':>8} {'guest fps':>9}")
for lo, avg, tot, gu, gf, i in sorted(out):
    print(f"{i:>2} {names[i] if i < len(names) else '?':<31} {lo:7.1f} {avg:7.1f} {tot:7.2f} {gu:8.2f} {gf:9.1f}")
print(f"janelas na sessão: {len(wins)}, guest estimado no fim {g:.0f} vs ack final {acks[-1]['frame']}")
