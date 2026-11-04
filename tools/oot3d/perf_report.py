#!/usr/bin/env python3
"""Summary of the console's (Switch) logs/perf.txt, per session.

Each "host_frames=..." line is a window of 150 host frames; a
"glperf ..." line right after it (with config/gpu_timing on the SD) holds, for the same
window, GPU/CPU times per render thread stage and GL call
counters. A new session starts when host_frames goes down again.

Since clock logging, each window also holds "clocks: cpu_mhz=
gpu_mhz= mem_mhz= op_mode=handheld|docked ..." (read from sys-clk; -1 when
it could not be read). The summary shows each session's clock, warns when it
changes midway and splits the statistics by clock: only compare measurements
taken at the same clock.

Usage:
  python3 tools/oot3d/perf_report.py logs/perf.txt            # all sessions
  python3 tools/oot3d/perf_report.py perf.txt --session -1    # only the last one
  python3 tools/oot3d/perf_report.py perf.txt --skip 4 --windows
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field

TOKEN = re.compile(r'([A-Za-z_][\w./]*)=("[^"]*"|\S+)|(\S+:)(?=\s|$)')

# Phases of the main line (ms per host frame), in perf.txt order.
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
    "begin": "frame start",
    "clear": "clears/fills",
    "pica_top": "PICA scene, top screen",
    "pica_bottom": "PICA, bottom screen",
    "pica_off": "PICA offscreen (shadows, RTT)",
    "pica_canon": "canonical execution (not displayed)",
    "transfer": "display transfers",
    "post_aa": "FXAA/SMAA",
    "post_easu": "FSR EASU",
    "scanout": "final pass to the window (RCAS/copy)",
    "ui": "UI TopScreen/HUD",
    "present": "EndFrame + swap",
    "other": "other commands",
    "pacing": "pacer wait",
    "idle": "render thread idle",
}
COUNTERS = [
    "draws", "clears", "fbo_binds", "fbo_changes", "tex_binds", "prog_binds",
    "prog_changes", "vao_binds", "ubo_binds", "uniforms", "buf_upload_kb",
    "buf_reallocs", "map_kb", "tex_upload_kb", "blits", "barriers", "syncs",
    "finish", "flush", "get_error", "gets", "readbacks", "gen_delete", "enables",
]
KINDS = ["step", "interp", "hold"]
KIND_HELP = {
    "step": "step frame (new game frame)",
    "interp": "interpolated frame (repeated sample)",
    "hold": "frame without a new sample (re-presentation)",
}


def parse_line(line: str) -> dict[str, object]:
    # The sections ("ms/frame:", "audio:", "mesa:") don't repeat key names:
    # all keys go into a single dictionary.
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


MODE_NAMES = {"handheld": "handheld", "docked": "dock"}


# Switch clock steps (the sys-clk profiles). The measured clock (realFreqs)
# oscillates a few MHz around the step: 1780..1789 is the same 1785.
CLOCK_STEPS = {
    "cpu_mhz": (612, 714, 816, 918, 1020, 1122, 1224, 1326, 1428, 1581, 1683,
                1785, 1887, 1963, 2091, 2193, 2295, 2397),
    "gpu_mhz": (77, 154, 230, 307, 384, 461, 538, 614, 691, 768, 845, 922, 998,
                1075, 1152, 1229, 1267),
    "mem_mhz": (665, 800, 1065, 1331, 1600, 1862, 2131),
}


def snap_clock(key: str, mhz: int) -> int:
    """Snaps the measured MHz to the nearest step (if within 3%)."""
    steps = CLOCK_STEPS.get(key)
    if mhz <= 0 or not steps:
        return mhz
    nearest = min(steps, key=lambda step: abs(step - mhz))
    return nearest if abs(nearest - mhz) <= nearest * 0.03 else mhz


def clock_key(values: dict[str, object]) -> tuple | None:
    """(cpu, gpu, mem, mode) of the window; None for perf.txt without clocks."""
    if "cpu_mhz" not in values and "op_mode" not in values:
        return None

    def mhz(key: str) -> int:
        value = values.get(key)
        return snap_clock(key, int(value)) if isinstance(value, float) and value > 0 else -1

    mode = values.get("op_mode")
    return (mhz("cpu_mhz"), mhz("gpu_mhz"), mhz("mem_mhz"),
            mode if isinstance(mode, str) and mode != "-1" else "?")


def clock_text(key: tuple | None) -> str:
    if key is None:
        return "not logged"
    cpu, gpu, mem, mode = key
    parts = [f"{name} {value if value > 0 else '?'}" for name, value in
             (("CPU", cpu), ("GPU", gpu), ("mem", mem))]
    return " / ".join(parts) + f" MHz, {MODE_NAMES.get(mode, mode)}"


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
    # Frames of each window = host_frames difference (150 normally).
    frames_in = []
    for position, window in enumerate(all_windows):
        before = num(all_windows[position - 1].main, "host_frames") if position else 0.0
        frames_in.append(num(window.main, "host_frames") - before)
    frames_in = frames_in[len(all_windows) - len(windows):]
    seconds = sum(count / max(num(w.main, "fps"), 1e-9) for count, w in zip(frames_in, windows))
    print("=" * 78)
    print(f"Session {session.index} (line {session.first_line}): {len(all_windows)} windows, "
          f"up to host_frames={int(last_frames)}; analyzed {len(windows)} "
          f"(skipping {len(all_windows) - len(windows)} from the start), ~{seconds / 60:.1f} min")
    fps = [num(w.main, "fps") for w in windows]
    guest = [num(w.main, "guest_fps") for w in windows]
    total = [num(w.main, "total_ms") for w in windows]
    overall = sum(frames_in) / seconds if seconds > 0 else math.nan
    print(f"  fps: overall {fmt(overall, 1)}  window average {fmt(sum(fps) / len(fps), 1)}  p10 {fmt(percentile(fps, 0.1), 1)}  "
          f"min {fmt(min(fps), 1)}  max {fmt(max(fps), 1)}   guest_fps average "
          f"{fmt(sum(guest) / len(guest), 1)}   total_ms average {fmt(sum(total) / len(total))}")
    slow = sum(1 for value in fps if value < 0.9 * max(fps))
    if slow:
        print(f"  windows >10% below the maximum: {slow}/{len(fps)}")
    summarize_clocks(all_windows, windows, frames_in)

    # Host frame phases (ms per frame, window average).
    phases = [(name, sum(num(w.main, name) for w in windows) / len(windows))
              for name in PHASES if any(name in w.main for w in windows)]
    if phases:
        peak = max(value for _, value in phases)
        print("  phases (ms/host frame, game thread + render):")
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
        print("  PICA plan (last window): " + " ".join(flags))

    memory_keys = ["proc_used_mb", "heap_arena_mb", "heap_used_mb", "gl_tex_mb", "gl_buffer_mb",
                   "gl_textures_live", "gl_programs", "gl_oom", "gl_draws_skipped"]
    memory = []
    for key in memory_keys:
        series = [num(w.main, key, math.nan) for w in all_windows if key in w.main]
        if series:
            memory.append(f"{key} {fmt(series[0], 1)}→{fmt(series[-1], 1)} (max {fmt(max(series), 1)})")
    if memory:
        print("  memory (start→end): " + "; ".join(memory))

    summarize_cadence(windows, seconds)
    summarize_offload(windows)

    gl_windows = [w for w in windows if w.glperf is not None]
    if not gl_windows:
        print("  glperf: absent (create config/gpu_timing on the SD to measure GPU/CPU per stage)")
    else:
        refresh = 60.0 if percentile(fps, 0.9) > 45.0 else 30.0
        summarize_glperf(gl_windows, top, refresh)
    if percentile(fps, 0.9) > 45.0:
        summarize_verdict(windows, gl_windows, overall)

    if show_windows:
        has_clocks = any(clock_key(w.main) is not None for w in windows)
        has_cadence = any("present_1" in w.main for w in windows)
        print("  windows: host_frames fps total_ms" +
              (" step/interp/drop hold1/2/3+ switches lock" if has_cadence else "") +
              (" step_gpu_ms interp_gpu_ms step_draws" if gl_windows else "") +
              (" cpu/gpu/mem mode" if has_clocks else ""))
        for window in windows:
            line = (f"    {int(num(window.main, 'host_frames')):>7} {fmt(num(window.main, 'fps'), 1):>6} "
                    f"{fmt(num(window.main, 'total_ms')):>7}")
            if has_cadence:
                m = window.main
                line += (f" {int(num(m, 'step_frames')):>3}/{int(num(m, 'interp_frames')):>3}"
                         f"/{int(num(m, 'interp_dropped')):>3}"
                         f" {int(num(m, 'hold_1')):>3}/{int(num(m, 'hold_2')):>3}"
                         f"/{int(num(m, 'hold_3') + num(m, 'hold_4p')):>3}"
                         f" {int(num(m, 'cadence_switches')):>4} {int(num(m, 'lock', 60)):>3}")
            if window.glperf is not None:
                line += (f" {fmt(num(window.glperf, 'step_gpu_ms', math.nan)):>7}"
                         f" {fmt(num(window.glperf, 'interp_gpu_ms', math.nan)):>7}"
                         f" {fmt(num(window.glperf, 'step_n_draws', math.nan), 0):>6}")
            key = clock_key(window.main)
            if key is not None:
                line += f"  {key[0]}/{key[1]}/{key[2]} {MODE_NAMES.get(key[3], key[3])}"
            print(line)


def summarize_clocks(all_windows: list[Window], windows: list[Window],
                     frames_in: list[float]) -> None:
    keys = [clock_key(w.main) for w in all_windows]
    if all(key is None for key in keys):
        print("  clocks: not logged (perf.txt predates clock logging)")
        return
    last = next(w.main for w in reversed(all_windows) if clock_key(w.main) is not None)
    source = last.get("clk_src", "?")
    profile = last.get("sysclk_profile", "?")
    profile = profile if isinstance(profile, str) else "?"
    ipc = num(last, "clk_ipc_us", -1.0)
    changes = [(position, keys[position - 1], keys[position])
               for position in range(1, len(keys))
               if keys[position] != keys[position - 1]]
    distinct = list(dict.fromkeys(key for key in keys))
    if not changes:
        print(f"  clocks: {clock_text(keys[0])} (source {source}, sys-clk profile {profile}"
              + (f", IPC {fmt(ipc, 0)} µs" if ipc >= 0 else "") + ")")
        if keys[0] is not None and -1 in keys[0][:3]:
            print("    unknown MHz (sys-clk missing?): only compare with sessions "
                  "measured at the same clock")
        return
    print(f"  WARNING: the clock changed {len(changes)}x midway through the session "
          f"({len(distinct)} distinct clocks); separate statistics below")
    for position, before, after in changes[:10]:
        frames = int(num(all_windows[position].main, "host_frames"))
        print(f"    window {position + 1} (host_frames={frames}): "
              f"{clock_text(before)} → {clock_text(after)}")
    if len(changes) > 10:
        print(f"    ... {len(changes) - 10} more changes")
    # Analyzed windows (without the ones skipped at the start), grouped by clock.
    groups: dict[tuple | None, list[tuple[float, Window]]] = {}
    for count, window in zip(frames_in, windows):
        groups.setdefault(clock_key(window.main), []).append((count, window))
    print("  per clock (analyzed windows):")
    for key, members in groups.items():
        fps = [num(w.main, "fps") for _, w in members]
        frames = sum(count for count, _ in members)
        seconds = sum(count / max(num(w.main, "fps"), 1e-9) for count, w in members)
        total = [num(w.main, "total_ms") for _, w in members]
        overall = frames / seconds if seconds > 0 else math.nan
        print(f"    {clock_text(key)}: {len(members)} windows (~{seconds / 60:.1f} min), "
              f"fps overall {fmt(overall, 1)} average {fmt(sum(fps) / len(fps), 1)} "
              f"p10 {fmt(percentile(fps, 0.1), 1)} min {fmt(min(fps), 1)}, "
              f"total_ms average {fmt(sum(total) / len(total))}")


CADENCE_BUCKETS = ["1", "2", "3", "4p"]


def summarize_offload(windows: list[Window]) -> None:
    # "offload:" block (config/offload_*): work taken off the game thread.
    marked = [w.main for w in windows if "offload_capture" in w.main]
    if not marked:
        return
    def mean(key: str) -> float:
        series = [num(m, key) for m in marked if key in m]
        return sum(series) / len(series) if series else math.nan
    modes = " ".join(f"{name}={int(num(marked[-1], 'offload_' + name))}"
                     for name in ("capture", "plan", "perf", "actors"))
    print(f"  offload ({len(marked)} windows; last: {modes}):")
    print(f"    PICA capture inside the guest {fmt(mean('pica_capture_ms'))} ms/frame"
          f" ({fmt(mean('pica_capture_draws'), 0)} draws, copy {fmt(mean('capture_copy_kb'), 0)} KiB,"
          f" hash {fmt(mean('capture_hash_kb'), 0)} KiB, reused index ranges"
          f" {fmt(mean('index_range_reuses'), 0)})")
    print(f"    pica_submit without the guest {fmt(mean('pica_submit_host_ms'))} ms/frame;"
          f" perf.txt line on the game thread {fmt(mean('perf_line_ms'), 3)} ms/window"
          + (f"; writer {fmt(mean('perf_writer_ms'), 2)} ms/window,"
             f" dropped {int(num(marked[-1], 'perf_dropped'))}" if "perf_writer_ms" in marked[-1] else ""))
    # pica_plan is inside pica_submit; frame_start contains input and
    # host_frame_start.
    host = sum(mean(key) for key in ("pica_submit_host_ms", "visual_present",
                                     "dsp_mix", "frame_start")
               if not math.isnan(mean(key)))
    print(f"    game thread outside the guest (pica_submit without guest + visual_present"
          f" + dsp_mix + frame_start): {fmt(host)} ms/frame")


def summarize_cadence(windows: list[Window], seconds: float) -> None:
    # "cadence:" block (config/fps60_mode): presentation regularity.
    cadence = [w for w in windows if "present_1" in w.main]
    if not cadence:
        return
    def total(key: str) -> float:
        return sum(num(w.main, key) for w in cadence)
    presents = [total(f"present_{b}") for b in CADENCE_BUCKETS]
    holds = [total(f"hold_{b}") for b in CADENCE_BUCKETS]
    pictures = sum(holds)
    modes = sorted({str(w.main.get("fps60_mode")) for w in cadence})
    print(f"  cadence ({'/'.join(modes)}, {len(cadence)} windows):")
    print("    intervals between presentations (refreshes 1/2/3/4+): " +
          "/".join(str(int(v)) for v in presents))
    if pictures:
        print("    each new frame stayed on screen (1/2/3/4+ refreshes): " +
              "/".join(f"{100 * v / pictures:.1f}%" for v in holds))
    print(f"    cadence switches {fmt(total('cadence_switches') / max(seconds, 1e-9))}/s, "
          f"late steps {int(total('late_steps'))}, repeats {int(total('repeats'))}, "
          f"game frames never shown {int(total('superseded_frames'))}")
    print(f"    locked at 30: {fmt(100 * total('lock30_ms') / 1000 / max(seconds, 1e-9), 1)}% of the time, "
          f"{int(total('lock_changes'))} lock switches; game pair (game thread) "
          f"average {fmt(sum(num(w.main, 'game_pair_ms') for w in cadence) / len(cadence), 1)} ms, "
          f"p90 max {fmt(max(num(w.main, 'game_pair_p90_ms') for w in cadence), 1)} ms")


def summarize_glperf(windows: list[Window], top: int, refresh: float) -> None:
    first = windows[0].glperf or {}
    print(f"  glperf: {len(windows)} windows; GPU 1 every {int(num(first, 'sample_every'))} frames, "
          f"timer={int(num(first, 'gpu_timer'))} ({int(num(first, 'timer_bits'))} bits), "
          f"GL hooks={int(num(first, 'gl_hooks'))}; dropped "
          f"{int(sum(num(w.glperf, 'gpu_dropped') for w in windows))}, query overflows "
          f"{int(sum(num(w.glperf, 'gpu_overflows') for w in windows))}, unbalanced frames "
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
        print(f"  [{kind}] {KIND_HELP[kind]}: {int(frames)} frames "
              f"({100.0 * frames / total_frames:.0f}%), {int(samples)} GPU samples")
        print(f"    GPU per frame: {fmt(gpu_total)} ms (worst sample {fmt(gpu_max)} ms; "
              f"budget {fmt(1000.0 / refresh, 1)} ms at {int(refresh)} Hz);"
              f" render thread StartFrame→swap: {fmt(cpu_frame)} ms")
        gpu = [(stage, by_samples(f"{kind}_gpu_{stage}")) for stage in GPU_STAGES]
        gpu = [(stage, value) for stage, value in gpu if not math.isnan(value) and value > 0]
        cpu = [(stage, by_frames(f"{kind}_cpu_{stage}")) for stage in CPU_STAGES]
        cpu = [(stage, value) for stage, value in cpu if value > 0]
        cpu_map = dict(cpu)
        if gpu:
            peak = max(value for _, value in gpu)
            print(f"    top GPU stages (ms/sampled frame) | render thread CPU (ms/frame):")
            for stage, value in sorted(gpu, key=lambda item: -item[1])[:top]:
                share = 100.0 * value / gpu_total if gpu_total and not math.isnan(gpu_total) else 0
                print(f"      {stage:<12}{fmt(value):>7} ({share:4.0f}%) {bar(value, peak, 20):<20}"
                      f" cpu {fmt(cpu_map.get(stage, 0.0)):>6}  {STAGE_HELP[stage]}")
        if cpu:
            rest = [(stage, value) for stage, value in cpu
                    if stage not in dict(gpu) or stage in ("other", "pacing", "idle")]
            rest = [(stage, value) for stage, value in rest if stage in ("other", "pacing", "idle")]
            if rest:
                print("    CPU outside the GPU stages: " +
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
                text += f"(max {fmt(maximum, 0)})"
            counters.append(text)
        if counters:
            print("    counters per frame:")
            line = "     "
            for text in counters:
                if len(line) + len(text) + 1 > 78:
                    print(line)
                    line = "     "
                line += " " + text
            print(line)
        warnings = []
        if by_frames(f"{kind}_n_get_error") > 0:
            warnings.append("glGetError on the hot path")
        if by_frames(f"{kind}_n_finish") > 0:
            warnings.append("glFinish per frame")
        if by_frames(f"{kind}_n_readbacks") > 0:
            warnings.append("glReadPixels per frame")
        if by_frames(f"{kind}_n_buf_reallocs") > 1:
            warnings.append("glBufferData (reallocation) per frame")
        if warnings:
            print("    warning: " + "; ".join(warnings))


HITCHES = ["hitch_shader", "hitch_texturas", "hitch_logica", "hitch_render"]


def summarize_verdict(windows: list[Window], gl_windows: list[Window], overall: float) -> None:
    """What holds back 60 FPS: each resource against the frame budget.

    At 60 Hz each frame on screen has 16.7 ms of GPU and render thread; the
    logic runs at 30 steps per second, so the game's step pair
    (game_pair_ms, two guest steps) has 33.3 ms. The hitch_* counters are
    accumulated over the session: the difference between the first and last window.
    """
    budget = 1000.0 / 60.0
    rows = []

    def frames_of(kind: str) -> float:
        return sum(num(w.glperf, f"{kind}_frames") for w in gl_windows)

    def weighted(key: str, weight: str) -> float:
        total = sum(num(w.glperf, weight) for w in gl_windows)
        if total == 0:
            return math.nan
        return sum(num(w.glperf, key) * num(w.glperf, weight) for w in gl_windows) / total

    if gl_windows:
        # The most common frame at 60 FPS is the interpolated one; the step one counts too.
        for kind in ("interp", "step"):
            if frames_of(kind) == 0:
                continue
            gpu = weighted(f"{kind}_gpu_ms", f"{kind}_gpu_samples")
            cpu = weighted(f"{kind}_cpu_ms", f"{kind}_frames")
            if not math.isnan(gpu):
                rows.append((f"GPU ({kind})", gpu, budget))
            if not math.isnan(cpu):
                # StartFrame→swap minus the wait (idle and pacer): the work.
                waits = (weighted(f"{kind}_cpu_idle", f"{kind}_frames") +
                         weighted(f"{kind}_cpu_pacing", f"{kind}_frames"))
                rows.append((f"render thread busy ({kind})", cpu - waits, budget))
    pairs = [num(w.main, "game_pair_ms", math.nan) for w in windows if "game_pair_ms" in w.main]
    p90 = [num(w.main, "game_pair_p90_ms", math.nan) for w in windows if "game_pair_p90_ms" in w.main]
    if pairs:
        rows.append(("logic (step pair, average)", sum(pairs) / len(pairs), 2.0 * budget))
    if p90:
        rows.append(("logic (step pair, p90)", sum(p90) / len(p90), 2.0 * budget))

    print(f"  60 FPS verdict (overall fps {fmt(overall, 1)}; budget {fmt(budget, 1)} ms per frame):")
    if not gl_windows:
        print("    no glperf: GPU and render thread not measured (create config/gpu_timing on the SD)")
    over = []
    for name, value, limit in rows:
        share = 100.0 * value / limit
        mark = "  <-- OVER" if share > 100.0 else ("  (at the limit)" if share > 90.0 else "")
        print(f"    {name:<34}{fmt(value):>7} ms of {fmt(limit, 1):>5} ({share:4.0f}%){mark}")
        if share > 100.0:
            over.append(name)
    hitches = []
    for key in HITCHES:
        series = [num(w.main, key) for w in windows if key in w.main]
        if series and series[-1] > series[0]:
            hitches.append(f"{key[6:]} {int(series[-1] - series[0])}")
    if hitches:
        print("    hitches in the session: " + "  ".join(hitches))
    if over:
        print("    bottleneck: " + ", ".join(over))
    elif rows and overall < 57.0:
        print("    nothing exceeds on average: the drop comes from spikes (hitches or slow windows)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("perf", help="path to perf.txt")
    parser.add_argument("--session", type=int, action="append",
                        help="session number (1 = first, -1 = last); repeatable")
    parser.add_argument("--skip", type=int, default=2,
                        help="windows at the start of each session to ignore (loading); default 2")
    parser.add_argument("--min-windows", type=int, default=4,
                        help="omits sessions with fewer windows than this (default 4)")
    parser.add_argument("--windows", action="store_true", help="lists each window")
    parser.add_argument("--top", type=int, default=8, help="how many GPU stages to list")
    args = parser.parse_args()

    sessions = read_sessions(args.perf)
    if not sessions:
        print("no host_frames= line found", file=sys.stderr)
        return 1
    chosen = sessions
    if args.session:
        chosen = []
        for number in args.session:
            index = number - 1 if number > 0 else len(sessions) + number
            if not 0 <= index < len(sessions):
                print(f"session {number} does not exist (there are {len(sessions)})", file=sys.stderr)
                return 1
            chosen.append(sessions[index])
    shown = [s for s in chosen if len(s.windows) >= args.min_windows]
    print(f"{args.perf}: {len(sessions)} sessions, {sum(len(s.windows) for s in sessions)} windows"
          + (f"; {len(chosen) - len(shown)} short sessions omitted (--min-windows)"
             if len(shown) != len(chosen) else ""))
    chosen = shown
    for session in chosen:
        summarize(session, args.skip, args.windows, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
