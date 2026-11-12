#!/usr/bin/env python3
"""Lab tour on the console: FPS per area (perf.txt windows matched to the acks by guest frame).

Usage: lab_tour_report.py <folder with acks.jsonl and perf.txt> tools/oot3d/native_game_runtime/oot3d_dev_warp_catalog.inc
The script (lab/script.jsonl) is: {"op":"gameplay"}, then per area {"op":"warp","better":[i,0]} and
{"op":"wait","frames":300}, i = 0..48 (49 is the Debug room and freezes)."""
import json, re, sys
d = sys.argv[1]
names, inside = [], False
for line in open(sys.argv[2]):
    inside = inside or "kDevWarpBetterScenes[]" in line
    m = re.match(r'\s*\{" ?\d+:([^"]*)"', line)
    if inside and m: names.append(m.group(1))
acks = [json.loads(l) for l in open(f"{d}/acks.jsonl")]
# Last session: host_frames goes down again when the game reopens.
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
    miss = sum(int(kv.get(k, 0)) for k in ("hold_2", "hold_3", "hold_4p"))
    rows.append((g, g + gfps * sec, fps, gfps, float(kv["guest"]), float(kv["total_ms"]), miss))
    g += gfps * sec
start = {a["id"]: a["frame"] for a in acks if "frame" in a}
out = []
for i in range(49):
    a, b = start.get(i), start.get(f"m{i}")
    if a is None or b is None: continue
    # Windows with more than half inside the measured stretch (without the loading).
    w = [r for r in rows if min(r[1], b) - max(r[0], a) > (r[1] - r[0]) / 2]
    if not w: continue
    lo = min(r[2] for r in w); avg = sum(r[2] for r in w) / len(w)
    # missed: images that stayed 2+ refreshes on screen (smoothness), per window.
    out.append((lo, avg, max(r[5] for r in w), max(r[4] for r in w), sum(r[3] for r in w)/len(w), i,
                sum(r[6] for r in w) / len(w)))
print(f"{'area':<34} {'FPS min':>7} {'FPS avg':>7} {'ms max':>7} {'guest ms':>8} {'guest fps':>9} {'missed':>8}")
for lo, avg, tot, gu, gf, i, miss in sorted(out):
    print(f"{i:>2} {names[i] if i < len(names) else '?':<31} {lo:7.1f} {avg:7.1f} {tot:7.2f} {gu:8.2f} {gf:9.1f} {miss:8.1f}")
print(f"windows in the session: {len(wins)}, estimated guest at the end {g:.0f} vs final ack {acks[-1]['frame']}")
