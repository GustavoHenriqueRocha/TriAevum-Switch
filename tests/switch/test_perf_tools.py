#!/usr/bin/env python3
"""Regression tests for the perf.txt analysis tools (no build)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools" / "oot3d"))
import perf_window_summary as summary  # noqa: E402
import spike_report  # noqa: E402


def check(name, condition):
    print(("ok   " if condition else "FAIL ") + name)
    return condition


def main():
    # tour22 (23/10): the perf.txt had the slow tour21 before it; the whole-file
    # average gave 48 missed refreshes per window, the right session gave 4.
    lines = [
        "host_frames=150 fps=30 missed_refresh=90 guest=39.5\n",
        "host_frames=300 fps=30 missed_refresh=90 guest=39.5\n",
        "some other line\n",
        "host_frames=150 fps=59 missed_refresh=4 guest=8.5\n",
        "host_frames=300 fps=60 missed_refresh=2 guest=8.0\n",
        "host_frames=450 fps=58 missed_refresh=6 guest=9.0\n",
    ]
    ok = True
    count, avg = summary.summarize(lines, ["fps", "missed_refresh", "guest"])
    ok &= check("only the last session goes into the average (accumulated perf.txt)",
                count == 3 and abs(avg["missed_refresh"] - 4.0) < 1e-9)
    count, avg = summary.summarize(lines, ["missed_refresh"], first=2)
    ok &= check("--first counts windows of the last session",
                count == 2 and abs(avg["missed_refresh"] - 3.0) < 1e-9)
    ok &= check("missing field doesn't break", "x" not in summary.summarize(lines, ["x"])[1])
    # tour21: 14 ms limit with instrumentation on (39.5 ms/frame).
    header = ["# threshold_ms=14.00 load_cutoff_ms=50.00 frames=13051 spikes=12659 load_spikes=728\n"]
    ratio = summary.guest_spike_saturation(header)
    ok &= check("saturated spike profile is detected (tour21)", ratio is not None and ratio > 0.5)
    ok &= check("useful profile is not flagged",
                summary.guest_spike_saturation(["frames=1000 spikes=40\n"]) < 0.5)
    # 24/10: spikes.txt accumulates sessions (tour20: 591 lines, 428 from tour19).
    spikes = [
        "session_start pacer_vsync=0\n",
        "spike host=500 guest=500 expected=1 stage=guest guest_ms=30 game_ms=40 ctx=-\n",
        "session_start pacer_vsync=0\n",
        "spike host=10 guest=10 expected=1 stage=guest guest_ms=20 game_ms=25 ctx=-\n",
        "spike host=700 guest=700 expected=1 stage=guest guest_ms=17 game_ms=18 ctx=-\n",
        "spike host=900 guest=900 expected=1 stage=render guest_ms=5 game_ms=6 ctx=load,tex\n",
        "spike host=1200 guest=1200 expected=1 stage=guest guest_ms=22 game_ms=26 ctx=-\n",
        "spike host=1201 guest=1201 expected=1 stage=guest guest_ms=22 game_ms=27 ctx=-\n",
    ]
    rows = spike_report.last_session(spikes)
    ok &= check("spikes: only the last session of spikes.txt", len(rows) == 5)
    out = spike_report.outside_load(rows)
    ok &= check("spikes: ctx=load is left out", len(out) == 4)
    iso, seq = spike_report.split_isolated(out)
    ok &= check("spikes: boot excluded, isolated vs sequence",
                [int(r["host"]) for r in iso] == [700] and len(seq) == 2)
    # sound/DSP on another core (~1.7 ms) only saves small overruns (3/95 in tour20+22).
    ok &= check("spikes: reduction needed per guest spike",
                spike_report.fixed_by(out, 1.7) == 1 and spike_report.fixed_by(out, 10) == 3)
    acks = [{"id": 6, "frame": 600}, {"id": "m6", "frame": 800}]
    spans = spike_report.area_spans(acks)
    ok &= check("spikes: area by guest frame (acks)",
                spike_report.area_of(out[1], spans) == 6 and spike_report.area_of(out[2], spans) is None)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
