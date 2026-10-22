#!/usr/bin/env python3
"""Portão de desempenho: compara uma sessão do logs/perf.txt do console com a referência.

Regressões de desempenho só aparecem no console, então cada versão nova enviada ao
Switch deve passar por aqui com o perf.txt baixado depois de jogar alguns minutos.
Só compara sessões com o MESMO modo (30 ou 60) e o MESMO clock de CPU da referência
(sem overclock a lógica fica ~40% mais lenta e a comparação não vale nada).

Uso:
  python3 tests/switch/perf_gate.py perf.txt [--session -1] [--mode 30|60]
  python3 tests/switch/perf_gate.py perf.txt --session -1 --mode 30 --update   # grava nova referência

Saída 0 = dentro da tolerância, 1 = regressão, 2 = sessão não comparável.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/oot3d"))
import perf_report  # noqa: E402

BASELINE = Path(__file__).with_name("perf_baseline.json")

# Quanto cada métrica pode piorar antes de reprovar (relativo à referência).
TOLERANCE = {"fps_global": 0.03, "fps_p10": 0.10, "speed": 0.01, "slow_share": 0.05,
             "hitches_per_min": 0.10, "big_hitches_per_min": 0.10}
# Menor é melhor nestas (as outras: maior é melhor).
LOWER_IS_BETTER = {"slow_share", "hitches_per_min", "big_hitches_per_min"}


def metrics(session: perf_report.Session, skip: int) -> dict[str, float]:
    windows = session.windows[skip:] if len(session.windows) > skip else session.windows
    num = perf_report.num
    frames, seconds, previous = 0.0, 0.0, None
    for window in session.windows:
        current = num(window.main, "host_frames")
        if window in windows and previous is not None:
            frames += current - previous
            seconds += (current - previous) / max(num(window.main, "fps"), 1e-9)
        previous = current
    fps = [num(w.main, "fps") for w in windows]
    guest = [num(w.main, "guest_fps") for w in windows]
    clock = perf_report.clock_key(windows[-1].main)
    # Engasgo = quadro que ficou 2+ intervalos na tela (hold_2..hold_4p);
    # grande = 3+ (>= 100 ms a 30 FPS).
    hitches = sum(num(w.main, k) for w in windows for k in ("hold_2", "hold_3", "hold_4p"))
    big = sum(num(w.main, k) for w in windows for k in ("hold_3", "hold_4p"))
    return {
        "hitches_per_min": hitches / (seconds / 60.0) if seconds else 0.0,
        "big_hitches_per_min": big / (seconds / 60.0) if seconds else 0.0,
        "fps_global": frames / seconds if seconds else 0.0,
        "fps_p10": perf_report.percentile(fps, 0.1),
        "speed": sum(guest) / len(guest) / 60.0,
        "slow_share": sum(1 for v in fps if v < 0.9 * max(fps)) / len(fps),
        "minutes": seconds / 60.0,
        "cpu_mhz": float(clock[0]) if clock and isinstance(clock[0], (int, float)) else 0.0,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("perf")
    parser.add_argument("--session", type=int, default=-1)
    parser.add_argument("--mode", choices=["30", "60"], default="30")
    parser.add_argument("--skip", type=int, default=2)
    parser.add_argument("--update", action="store_true", help="grava esta sessão como referência")
    args = parser.parse_args()

    sessions = perf_report.read_sessions(args.perf)
    index = args.session - 1 if args.session > 0 else len(sessions) + args.session
    if not sessions or not 0 <= index < len(sessions):
        print("sessão não encontrada", file=sys.stderr)
        return 2
    session = sessions[index]
    if len(session.windows) < args.skip + 4:
        print(f"sessão {session.index} curta demais ({len(session.windows)} janelas)", file=sys.stderr)
        return 2
    current = metrics(session, args.skip)

    baselines = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    if args.update:
        baselines[args.mode] = {**current, "source": f"{Path(args.perf).parent.name}/{Path(args.perf).name} sessão {session.index}"}
        BASELINE.write_text(json.dumps(baselines, indent=2, ensure_ascii=False) + "\n")
        print(f"referência {args.mode} FPS gravada: {current}")
        return 0
    reference = baselines.get(args.mode)
    if reference is None:
        print(f"sem referência para {args.mode} FPS; rode com --update", file=sys.stderr)
        return 2
    if reference.get("cpu_mhz") and current["cpu_mhz"] and reference["cpu_mhz"] != current["cpu_mhz"]:
        print(f"clock diferente (CPU {current['cpu_mhz']:.0f} contra {reference['cpu_mhz']:.0f} MHz): "
              "não comparável", file=sys.stderr)
        return 2

    failed = False
    print(f"sessão {session.index}, {current['minutes']:.1f} min, modo {args.mode} FPS "
          f"(referência: {reference.get('source', '?')})")
    for key, tolerance in TOLERANCE.items():
        ref, now = reference[key], current[key]
        if key == "slow_share":
            worse = now - ref > tolerance
        elif key in LOWER_IS_BETTER:
            worse = now > ref * (1.0 + tolerance) + 0.05
        else:
            worse = now < ref * (1.0 - tolerance)
        failed |= worse
        print(f"  {'REGRESSÃO' if worse else 'ok':<9} {key:<11} {now:8.3f}  (ref {ref:.3f}, tolerância {tolerance:.0%})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
