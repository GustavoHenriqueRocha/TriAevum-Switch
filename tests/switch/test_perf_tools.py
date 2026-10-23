#!/usr/bin/env python3
"""Regressão das ferramentas de análise do perf.txt (sem build)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "oot3d"))
import perf_window_summary as summary  # noqa: E402


def check(name, condition):
    print(("ok   " if condition else "FAIL ") + name)
    return condition


def main():
    # tour22 (23/10): o perf.txt tinha o tour21 lento antes; a média do arquivo
    # inteiro deu 48 refresh perdidos por janela, a sessão certa dava 4.
    lines = [
        "host_frames=150 fps=30 missed_refresh=90 guest=39.5\n",
        "host_frames=300 fps=30 missed_refresh=90 guest=39.5\n",
        "outra linha qualquer\n",
        "host_frames=150 fps=59 missed_refresh=4 guest=8.5\n",
        "host_frames=300 fps=60 missed_refresh=2 guest=8.0\n",
        "host_frames=450 fps=58 missed_refresh=6 guest=9.0\n",
    ]
    ok = True
    count, avg = summary.summarize(lines, ["fps", "missed_refresh", "guest"])
    ok &= check("só a última sessão entra na média (perf.txt acumulado)",
                count == 3 and abs(avg["missed_refresh"] - 4.0) < 1e-9)
    count, avg = summary.summarize(lines, ["missed_refresh"], first=2)
    ok &= check("--first conta janelas da última sessão",
                count == 2 and abs(avg["missed_refresh"] - 3.0) < 1e-9)
    ok &= check("campo ausente não quebra", "x" not in summary.summarize(lines, ["x"])[1])
    # tour21: limite de 14 ms com a instrumentação ligada (39,5 ms/quadro).
    header = ["# limite_ms=14.00 corte_carregamento_ms=50.00 quadros=13051 picos=12659 picos_carregamento=728\n"]
    ratio = summary.guest_spike_saturation(header)
    ok &= check("perfil de picos saturado é detectado (tour21)", ratio is not None and ratio > 0.5)
    ok &= check("perfil útil não é marcado",
                summary.guest_spike_saturation(["quadros=1000 picos=40\n"]) < 0.5)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
