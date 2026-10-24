#!/usr/bin/env python3
"""Picos de quadro (logs/spikes.txt) fora de carregamento, por área e causa.

Uso: spike_report.py <pasta com spikes.txt [e acks.jsonl]> [oot3d_dev_warp_catalog.inc]

- Só a última sessão (spikes.txt acumula sessões: o tour20 tinha 591 linhas,
  428 eram do tour19).
- ctx com "load" fica de fora (carregamento é aceitável).
- Isolado = nenhum pico no quadro de host vizinho; um passo longo isolado
  seguido de um quadro curto cabe em 2 refreshes (o que config/early_frame_slot
  absorve). Em sequência = cena pesada demais (precisa de otimização real).
- "reduzir" = quanto o quadro do jogo teria de encolher para caber no
  orçamento (expected x 16,67 ms): o som/DSP em outro núcleo (~1,7 ms)
  resolveria 3 de 95 picos de guest fora de load do tour20+tour22
  (descartado).
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
    """(isolados, em sequência) entre os picos fora de load pós-boot."""
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
    """Picos de guest que caberiam no orçamento com saving_ms a menos."""
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
    print(f"sessão: {len(rows)} picos, {len(rows) - len(out)} em load, {len(out)} fora")
    boot = [r for r in out if int(r["host"]) <= BOOT_FRAMES]
    iso, seq = split_isolated(out)
    print(f"boot (<= {BOOT_FRAMES} quadros): {len(boot)}  isolados: {len(iso)}  em sequência: {len(seq)}")
    print("por etapa:", dict(Counter(r["stage"] for r in out)))
    guest = [r for r in out if r["stage"] == "guest"]
    if guest:
        print("guest_ms mediana %.1f, game_ms mediana %.1f" % (
            statistics.median(float(r["guest_ms"]) for r in guest),
            statistics.median(float(r["game_ms"]) for r in guest)))
        print("picos de guest resolvidos se o quadro encolhesse:",
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
        print(f"{'área':<30}{'n':>4}{'guest':>6}{'pacer':>6}{'rend':>5}{'gpu':>5}")
        for area, group in sorted(per.items(), key=lambda kv: -len(kv[1])):
            c = Counter(r["stage"] for r in group)
            name = names[area] if area is not None and area < len(names) else "fora do tour"
            print(f"{str(area) + ' ' + name:<30}{len(group):>4}{c['guest']:>6}{c['pacer']:>6}{c['render']:>5}{c['gpu']:>5}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
