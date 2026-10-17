#!/usr/bin/env python3
"""Resumo do logs/perf.txt do console (Switch), por sessão.

Cada linha "host_frames=..." é uma janela de 150 quadros de host; uma linha
"glperf ..." logo depois (com config/gpu_timing no SD) traz, para a mesma
janela, tempos de GPU/CPU por etapa da render thread e contadores de chamadas
GL. Uma sessão nova começa quando host_frames volta a diminuir.

Uso:
  python3 tools/oot3d/perf_report.py logs/perf.txt            # todas as sessões
  python3 tools/oot3d/perf_report.py perf.txt --session -1    # só a última
  python3 tools/oot3d/perf_report.py perf.txt --skip 4 --windows
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field

TOKEN = re.compile(r'([A-Za-z_][\w./]*)=("[^"]*"|\S+)|(\S+:)(?=\s|$)')

# Fases da linha principal (ms por quadro de host), na ordem do perf.txt.
PHASES = [
    "guest", "frame_start", "host_frame_start", "input", "renderer_frame_start",
    "dsp_mix", "audio_out", "pica_submit", "pica_plan", "pica_backend",
    "pica_diag", "visual_present", "visual_replay", "present", "pacing_wait",
]
GPU_STAGES = [
    "begin", "clear", "pica_top", "pica_bottom", "pica_off", "pica_canon",
    "transfer", "post_aa", "post_easu", "scanout", "ui", "present",
]
CPU_STAGES = GPU_STAGES + ["other", "pacing", "idle"]
STAGE_HELP = {
    "begin": "início do quadro",
    "clear": "limpezas/fills",
    "pica_top": "cena PICA, tela de cima",
    "pica_bottom": "PICA, tela de baixo",
    "pica_off": "PICA fora da tela (sombras, RTT)",
    "pica_canon": "execução canônica (não exibida)",
    "transfer": "display transfers",
    "post_aa": "FXAA/SMAA",
    "post_easu": "FSR EASU",
    "scanout": "passe final na janela (RCAS/cópia)",
    "ui": "UI TopScreen/HUD",
    "present": "EndFrame + swap",
    "other": "outros comandos",
    "pacing": "espera do pacer",
    "idle": "render thread ociosa",
}
COUNTERS = [
    "draws", "clears", "fbo_binds", "fbo_changes", "tex_binds", "prog_binds",
    "prog_changes", "vao_binds", "ubo_binds", "uniforms", "buf_upload_kb",
    "buf_reallocs", "map_kb", "tex_upload_kb", "blits", "barriers", "syncs",
    "finish", "flush", "get_error", "gets", "readbacks", "gen_delete", "enables",
]
KINDS = ["step", "interp", "hold"]
KIND_HELP = {
    "step": "quadro de passo (quadro novo do jogo)",
    "interp": "quadro interpolado (amostra repetida)",
    "hold": "quadro sem amostra nova (reapresentação)",
}


def parse_line(line: str) -> dict[str, object]:
    # As seções ("ms/frame:", "audio:", "mesa:") não repetem nomes de chave:
    # todas as chaves ficam num só dicionário.
    values: dict[str, object] = {}
    for match in TOKEN.finditer(line):
        key, raw = match.group(1), match.group(2)
        if key is None:
            continue
        if raw.startswith('"'):
            values[key] = raw.strip('"')
            continue
        try:
            values[key] = float(raw)
        except ValueError:
            values[key] = raw
    return values


@dataclass
class Window:
    main: dict[str, object]
    glperf: dict[str, object] | None = None


@dataclass
class Session:
    index: int
    first_line: int
    windows: list[Window] = field(default_factory=list)


def read_sessions(path: str) -> list[Session]:
    sessions: list[Session] = []
    previous = None
    with open(path, encoding="utf-8", errors="replace") as stream:
        for number, line in enumerate(stream, 1):
            line = line.strip()
            if line.startswith("host_frames="):
                values = parse_line(line)
                frames = values.get("host_frames")
                if not isinstance(frames, float):
                    continue
                if previous is None or frames <= previous:
                    sessions.append(Session(len(sessions) + 1, number))
                previous = frames
                sessions[-1].windows.append(Window(values))
            elif line.startswith("glperf "):
                if sessions and sessions[-1].windows:
                    sessions[-1].windows[-1].glperf = parse_line(line[len("glperf "):])
    return sessions


def num(values: dict[str, object] | None, key: str, default: float = 0.0) -> float:
    if values is None:
        return default
    value = values.get(key)
    return value if isinstance(value, float) else default


def percentile(data: list[float], fraction: float) -> float:
    if not data:
        return math.nan
    ordered = sorted(data)
    position = (len(ordered) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def fmt(value: float, digits: int = 2) -> str:
    if value is None or (isinstance(value, float) and math.isnan(value)):
        return "-"
    return f"{value:.{digits}f}"


def bar(value: float, maximum: float, width: int = 24) -> str:
    if maximum <= 0:
        return ""
    filled = int(round(width * min(value / maximum, 1.0)))
    return "#" * filled


def summarize(session: Session, skip: int, show_windows: bool, top: int) -> None:
    all_windows = session.windows
    windows = all_windows[skip:] if len(all_windows) > skip else all_windows
    last_frames = num(all_windows[-1].main, "host_frames")
    # Quadros de cada janela = diferença de host_frames (150 normalmente).
    frames_in = []
    for position, window in enumerate(all_windows):
        before = num(all_windows[position - 1].main, "host_frames") if position else 0.0
        frames_in.append(num(window.main, "host_frames") - before)
    frames_in = frames_in[len(all_windows) - len(windows):]
    seconds = sum(count / max(num(w.main, "fps"), 1e-9) for count, w in zip(frames_in, windows))
    print("=" * 78)
    print(f"Sessão {session.index} (linha {session.first_line}): {len(all_windows)} janelas, "
          f"até host_frames={int(last_frames)}; analisadas {len(windows)} "
          f"(pulando {len(all_windows) - len(windows)} do início), ~{seconds / 60:.1f} min")
    fps = [num(w.main, "fps") for w in windows]
    guest = [num(w.main, "guest_fps") for w in windows]
    total = [num(w.main, "total_ms") for w in windows]
    overall = sum(frames_in) / seconds if seconds > 0 else math.nan
    print(f"  fps: global {fmt(overall, 1)}  média das janelas {fmt(sum(fps) / len(fps), 1)}  p10 {fmt(percentile(fps, 0.1), 1)}  "
          f"mín {fmt(min(fps), 1)}  máx {fmt(max(fps), 1)}   guest_fps média "
          f"{fmt(sum(guest) / len(guest), 1)}   total_ms média {fmt(sum(total) / len(total))}")
    slow = sum(1 for value in fps if value < 0.9 * max(fps))
    if slow:
        print(f"  janelas >10% abaixo do máximo: {slow}/{len(fps)}")

    # Fases do quadro de host (ms por quadro, média das janelas).
    phases = [(name, sum(num(w.main, name) for w in windows) / len(windows))
              for name in PHASES if any(name in w.main for w in windows)]
    if phases:
        peak = max(value for _, value in phases)
        print("  fases (ms/quadro de host, thread do jogo + render):")
        for name, value in sorted(phases, key=lambda item: -item[1]):
            if value < 0.05:
                continue
            print(f"    {name:<22}{fmt(value):>8}  {bar(value, peak)}")

    last = windows[-1].main
    flags = []
    for key in ("pica_plan_deferred", "plan_worker", "plan_worker_ms", "plan_late_ms"):
        if key in last:
            flags.append(f"{key}={fmt(num(last, key), 2 if key.endswith('_ms') else 0)}")
    if flags:
        print("  plano PICA (última janela): " + " ".join(flags))

    memory_keys = ["proc_used_mb", "heap_arena_mb", "heap_used_mb", "gl_tex_mb", "gl_buffer_mb",
                   "gl_textures_live", "gl_programs", "gl_oom", "gl_draws_skipped"]
    memory = []
    for key in memory_keys:
        series = [num(w.main, key, math.nan) for w in all_windows if key in w.main]
        if series:
            memory.append(f"{key} {fmt(series[0], 1)}→{fmt(series[-1], 1)} (máx {fmt(max(series), 1)})")
    if memory:
        print("  memória (início→fim): " + "; ".join(memory))

    gl_windows = [w for w in windows if w.glperf is not None]
    if not gl_windows:
        print("  glperf: ausente (crie config/gpu_timing no SD para medir GPU/CPU por etapa)")
    else:
        refresh = 60.0 if percentile(fps, 0.9) > 45.0 else 30.0
        summarize_glperf(gl_windows, top, refresh)

    if show_windows:
        print("  janelas: host_frames fps total_ms" +
              (" step_gpu_ms interp_gpu_ms step_draws" if gl_windows else ""))
        for window in windows:
            line = (f"    {int(num(window.main, 'host_frames')):>7} {fmt(num(window.main, 'fps'), 1):>6} "
                    f"{fmt(num(window.main, 'total_ms')):>7}")
            if window.glperf is not None:
                line += (f" {fmt(num(window.glperf, 'step_gpu_ms', math.nan)):>7}"
                         f" {fmt(num(window.glperf, 'interp_gpu_ms', math.nan)):>7}"
                         f" {fmt(num(window.glperf, 'step_n_draws', math.nan), 0):>6}")
            print(line)


def summarize_glperf(windows: list[Window], top: int, refresh: float) -> None:
    first = windows[0].glperf or {}
    print(f"  glperf: {len(windows)} janelas; GPU 1 a cada {int(num(first, 'sample_every'))} quadros, "
          f"timer={int(num(first, 'gpu_timer'))} ({int(num(first, 'timer_bits'))} bits), "
          f"ganchos GL={int(num(first, 'gl_hooks'))}; descartadas "
          f"{int(sum(num(w.glperf, 'gpu_dropped') for w in windows))}, estouro de queries "
          f"{int(sum(num(w.glperf, 'gpu_overflows') for w in windows))}, quadros desbalanceados "
          f"{int(sum(num(w.glperf, 'unbalanced') for w in windows))}")
    total_frames = sum(num(w.glperf, f"{kind}_frames") for w in windows for kind in KINDS)
    for kind in KINDS:
        frames = sum(num(w.glperf, f"{kind}_frames") for w in windows)
        if frames == 0:
            continue
        samples = sum(num(w.glperf, f"{kind}_gpu_samples") for w in windows)

        def by_frames(key: str) -> float:
            return sum(num(w.glperf, key) * num(w.glperf, f"{kind}_frames") for w in windows) / frames

        def by_samples(key: str) -> float:
            if samples == 0:
                return math.nan
            return sum(num(w.glperf, key) * num(w.glperf, f"{kind}_gpu_samples")
                       for w in windows) / samples

        gpu_total = by_samples(f"{kind}_gpu_ms")
        gpu_max = max((num(w.glperf, f"{kind}_gpu_max_ms", -1.0) for w in windows), default=-1.0)
        gpu_max = gpu_max if gpu_max >= 0 else math.nan
        cpu_frame = by_frames(f"{kind}_cpu_ms")
        print(f"  [{kind}] {KIND_HELP[kind]}: {int(frames)} quadros "
              f"({100.0 * frames / total_frames:.0f}%), {int(samples)} amostras de GPU")
        print(f"    GPU por quadro: {fmt(gpu_total)} ms (pior amostra {fmt(gpu_max)} ms; "
              f"orçamento {fmt(1000.0 / refresh, 1)} ms a {int(refresh)} Hz);"
              f" render thread StartFrame→swap: {fmt(cpu_frame)} ms")
        gpu = [(stage, by_samples(f"{kind}_gpu_{stage}")) for stage in GPU_STAGES]
        gpu = [(stage, value) for stage, value in gpu if not math.isnan(value) and value > 0]
        cpu = [(stage, by_frames(f"{kind}_cpu_{stage}")) for stage in CPU_STAGES]
        cpu = [(stage, value) for stage, value in cpu if value > 0]
        cpu_map = dict(cpu)
        if gpu:
            peak = max(value for _, value in gpu)
            print(f"    top etapas de GPU (ms/quadro amostrado) | CPU da render thread (ms/quadro):")
            for stage, value in sorted(gpu, key=lambda item: -item[1])[:top]:
                share = 100.0 * value / gpu_total if gpu_total and not math.isnan(gpu_total) else 0
                print(f"      {stage:<12}{fmt(value):>7} ({share:4.0f}%) {bar(value, peak, 20):<20}"
                      f" cpu {fmt(cpu_map.get(stage, 0.0)):>6}  {STAGE_HELP[stage]}")
        if cpu:
            rest = [(stage, value) for stage, value in cpu
                    if stage not in dict(gpu) or stage in ("other", "pacing", "idle")]
            rest = [(stage, value) for stage, value in rest if stage in ("other", "pacing", "idle")]
            if rest:
                print("    CPU fora das etapas de GPU: " +
                      "  ".join(f"{stage} {fmt(value)} ms" for stage, value in rest))
        counters = []
        for name in COUNTERS:
            key = f"{kind}_n_{name}"
            if not any(key in (w.glperf or {}) for w in windows):
                continue
            value = by_frames(key)
            maximum = max((num(w.glperf, f"{kind}_max_{name}", math.nan) for w in windows),
                          default=math.nan)
            text = f"{name}={fmt(value, 1)}"
            if not math.isnan(maximum):
                text += f"(máx {fmt(maximum, 0)})"
            counters.append(text)
        if counters:
            print("    contadores por quadro:")
            line = "     "
            for text in counters:
                if len(line) + len(text) + 1 > 78:
                    print(line)
                    line = "     "
                line += " " + text
            print(line)
        warnings = []
        if by_frames(f"{kind}_n_get_error") > 0:
            warnings.append("glGetError no caminho quente")
        if by_frames(f"{kind}_n_finish") > 0:
            warnings.append("glFinish por quadro")
        if by_frames(f"{kind}_n_readbacks") > 0:
            warnings.append("glReadPixels por quadro")
        if by_frames(f"{kind}_n_buf_reallocs") > 1:
            warnings.append("glBufferData (realocação) por quadro")
        if warnings:
            print("    atenção: " + "; ".join(warnings))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("perf", help="caminho do perf.txt")
    parser.add_argument("--session", type=int, action="append",
                        help="número da sessão (1 = primeira, -1 = última); repetível")
    parser.add_argument("--skip", type=int, default=2,
                        help="janelas do início de cada sessão a ignorar (carregamento); padrão 2")
    parser.add_argument("--min-windows", type=int, default=4,
                        help="omite sessões com menos janelas que isto (padrão 4)")
    parser.add_argument("--windows", action="store_true", help="lista cada janela")
    parser.add_argument("--top", type=int, default=8, help="quantas etapas de GPU listar")
    args = parser.parse_args()

    sessions = read_sessions(args.perf)
    if not sessions:
        print("nenhuma linha host_frames= encontrada", file=sys.stderr)
        return 1
    chosen = sessions
    if args.session:
        chosen = []
        for number in args.session:
            index = number - 1 if number > 0 else len(sessions) + number
            if not 0 <= index < len(sessions):
                print(f"sessão {number} não existe (há {len(sessions)})", file=sys.stderr)
                return 1
            chosen.append(sessions[index])
    shown = [s for s in chosen if len(s.windows) >= args.min_windows]
    print(f"{args.perf}: {len(sessions)} sessões, {sum(len(s.windows) for s in sessions)} janelas"
          + (f"; {len(chosen) - len(shown)} sessões curtas omitidas (--min-windows)"
             if len(shown) != len(chosen) else ""))
    chosen = shown
    for session in chosen:
        summarize(session, args.skip, args.windows, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
