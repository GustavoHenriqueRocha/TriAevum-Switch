#!/usr/bin/env python3
"""Frame spikes (logs/spikes.txt) outside loading, by area and cause.

Usage: spike_report.py <folder with spikes.txt [and acks.jsonl]> [oot3d_dev_warp_catalog.inc]

- Only the last session (spikes.txt accumulates sessions: tour20 had 591 lines,
  428 were from tour19).
- ctx with "load" is left out (loading is acceptable).
- Isolated = no spike on the neighboring host frame; an isolated long step
  followed by a short frame fits in 2 refreshes (what config/early_frame_slot
  absorbs). In sequence = scene too heavy (needs real optimization).
- "reduce" = how much the game frame would have to shrink to fit the
  budget (expected x 16.67 ms): sound/DSP on another core (~1.7 ms)
  would fix 3 of 95 guest spikes outside load in tour20+tour22
  (discarded).
"""
import json
import re
import statistics
import sys
from collections import Counter, defaultdict

REFRESH_MS = 1000.0 / 60.0
BOOT_FRAMES = 300


def last_session(lines):
    rows = []
    for line in lines:
        if line.startswith("session_start"):
            rows = []
        elif line.startswith("spike "):
            rows.append(dict(re.findall(r"(\w+)=(\S+)", line)))
    return rows


def outside_load(rows):
    return [r for r in rows if "load" not in r.get("ctx", "-")]


def split_isolated(rows):
    """(isolated, in sequence) among the post-boot spikes outside load."""
    hosts = {int(r["host"]) for r in rows}
    iso, seq = [], []
    for r in rows:
        h = int(r["host"])
        if h <= BOOT_FRAMES:
            continue
        (seq if (h - 1 in hosts or h + 1 in hosts) else iso).append(r)
    return iso, seq


def overrun_ms(r):
    return float(r["game_ms"]) - REFRESH_MS * int(r.get("expected", "1"))


def fixed_by(rows, saving_ms):
    """Guest spikes that would fit the budget with saving_ms less."""
    return sum(1 for r in rows if r["stage"] == "guest" and overrun_ms(r) <= saving_ms)


def area_spans(acks):
    start = {a["id"]: a["frame"] for a in acks if "frame" in a}
    spans = []
    for key, frame in start.items():
        if isinstance(key, int) and f"m{key}" in start:
            spans.append((frame, start[f"m{key}"], key))
    return spans


def area_of(r, spans):
    g = int(r["guest"])
    for a, b, i in spans:
        if a <= g <= b:
            return i
    return None


def main(argv):
    d = argv[1]
    rows = last_session(open(f"{d}/spikes.txt"))
    out = outside_load(rows)
    print(f"session: {len(rows)} spikes, {len(rows) - len(out)} in load, {len(out)} outside")
    boot = [r for r in out if int(r["host"]) <= BOOT_FRAMES]
    iso, seq = split_isolated(out)
    print(f"boot (<= {BOOT_FRAMES} frames): {len(boot)}  isolated: {len(iso)}  in sequence: {len(seq)}")
    print("by stage:", dict(Counter(r["stage"] for r in out)))
    guest = [r for r in out if r["stage"] == "guest"]
    if guest:
        print("guest_ms median %.1f, game_ms median %.1f" % (
            statistics.median(float(r["guest_ms"]) for r in guest),
            statistics.median(float(r["game_ms"]) for r in guest)))
        print("guest spikes fixed if the frame shrank by:",
              ", ".join(f"{s} ms → {fixed_by(guest, s)}/{len(guest)}" for s in (1.7, 3, 5, 8, 10, 15)))
    names = []
    if len(argv) > 2:
        inside = False
        for line in open(argv[2]):
            inside = inside or "kDevWarpBetterScenes[]" in line
            m = re.match(r'\s*\{" ?\d+:([^"]*)"', line)
            if inside and m:
                names.append(m.group(1))
    try:
        acks = [json.loads(l) for l in open(f"{d}/acks.jsonl")]
    except OSError:
        acks = []
    if acks:
        spans = area_spans(acks)
        per = defaultdict(list)
        for r in out:
            per[area_of(r, spans)].append(r)
        print(f"{'area':<30}{'n':>4}{'guest':>6}{'pacer':>6}{'rend':>5}{'gpu':>5}")
        for area, group in sorted(per.items(), key=lambda kv: -len(kv[1])):
            c = Counter(r["stage"] for r in group)
            name = names[area] if area is not None and area < len(names) else "outside the tour"
            print(f"{str(area) + ' ' + name:<30}{len(group):>4}{c['guest']:>6}{c['pacer']:>6}{c['render']:>5}{c['gpu']:>5}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
