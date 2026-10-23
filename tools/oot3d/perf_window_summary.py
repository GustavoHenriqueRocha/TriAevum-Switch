#!/usr/bin/env python3
"""Médias por janela do logs/perf.txt, só da última sessão do jogo.

O perf.txt é anexado a cada execução: misturar sessões deu números errados
(tour22 contado com o tour21 lento junto: 48 refresh perdidos/janela em vez de 4,1).
A sessão nova começa quando host_frames volta a diminuir.

Uso: perf_window_summary.py perf.txt [--first N] [campo ...]
     perf_window_summary.py --guest-spikes logs/guest_spikes_summary.txt
"""
import re
import sys

DEFAULT_FIELDS = ["fps", "guest", "present", "total_ms", "game_cpu", "missed_refresh",
                  "missed_by_stage_guest", "missed_by_stage_render", "missed_frames",
                  "hold_2", "hold_4p", "interp_frames"]


def last_session(lines):
    """Linhas de janela (host_frames=...) da última sessão."""
    windows, previous = [], None
    for line in lines:
        if not line.startswith("host_frames="):
            continue
        frames = int(re.match(r"host_frames=(\d+)", line).group(1))
        if previous is not None and frames < previous:
            windows = []
        previous = frames
        windows.append(line)
    return windows


def summarize(lines, fields=DEFAULT_FIELDS, first=None):
    windows = last_session(lines)
    if first is not None:
        windows = windows[:first]
    sums, counts = {}, {}
    for line in windows:
        values = dict(re.findall(r"(\w+)=(-?[\d.]+)(?=\s|$)", line))
        for field in fields:
            if field in values:
                sums[field] = sums.get(field, 0.0) + float(values[field])
                counts[field] = counts.get(field, 0) + 1
    return len(windows), {f: sums[f] / counts[f] for f in fields if f in counts}


def guest_spike_saturation(lines):
    """Fração dos quadros que viraram pico no perfil de picos do guest.

    tour21: a instrumentação levou o guest a 39,5 ms/quadro com limite de 14 ms,
    97% dos quadros viraram pico e o "perfil de picos" virou um perfil geral.
    Acima de 0,5 o resultado não isola picos: subir o limite (relativo à média
    medida com o perfil ligado) e repetir."""
    for line in lines:
        match = re.search(r"quadros=(\d+) picos=(\d+)", line)
        if match and int(match.group(1)):
            return int(match.group(2)) / int(match.group(1))
    return None


def main(argv):
    if len(argv) == 3 and argv[1] == "--guest-spikes":
        with open(argv[2], errors="replace") as handle:
            ratio = guest_spike_saturation(handle)
        if ratio is None:
            print("sem cabeçalho quadros=/picos=")
            return 2
        print(f"picos/quadros={ratio:.2f}" + (" SATURADO: não isola picos" if ratio > 0.5 else ""))
        return 1 if ratio > 0.5 else 0
    if len(argv) < 2:
        print(__doc__)
        return 2
    args = argv[2:]
    first = None
    if "--first" in args:
        index = args.index("--first")
        first = int(args[index + 1])
        del args[index:index + 2]
    with open(argv[1], errors="replace") as handle:
        count, averages = summarize(handle, args or DEFAULT_FIELDS, first)
    print(f"janelas={count} " + " ".join(f"{k}={v:.2f}" for k, v in averages.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
