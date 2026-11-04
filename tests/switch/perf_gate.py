#!/usr/bin/env python3
"""Performance gate: compares a session from the console's logs/perf.txt with the baseline.

Performance regressions only show up on the console, so every new version sent to the
Switch must pass through here with the perf.txt downloaded after playing a few minutes.
Only compares sessions with the SAME mode (30 or 60) and the SAME CPU clock as the baseline
(without overclock the logic is ~40% slower and the comparison is worthless).

Usage:
  python3 tests/switch/perf_gate.py perf.txt [--session -1] [--mode 30|60]
  python3 tests/switch/perf_gate.py perf.txt --session -1 --mode 30 --update   # saves new baseline

Exit 0 = within tolerance, 1 = regression, 2 = session not comparable.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/oot3d"))
import perf_report  # noqa: E402

BASELINE = Path(__file__).with_name("perf_baseline.json")

# How much each metric may worsen before failing (relative to the baseline).
TOLERANCE = {"fps_global": 0.03, "fps_p10": 0.10, "speed": 0.01, "slow_share": 0.05,
             "hitches_per_min": 0.10, "big_hitches_per_min": 0.10}
# Lower is better for these (the others: higher is better).
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
    # Hitch = frame that stayed on screen for 2+ intervals (hold_2..hold_4p);
    # big = 3+ (>= 100 ms at 30 FPS).
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
    parser.add_argument("--update", action="store_true", help="saves this session as the baseline")
    args = parser.parse_args()

    sessions = perf_report.read_sessions(args.perf)
    index = args.session - 1 if args.session > 0 else len(sessions) + args.session
    if not sessions or not 0 <= index < len(sessions):
        print("session not found", file=sys.stderr)
        return 2
    session = sessions[index]
    if len(session.windows) < args.skip + 4:
        print(f"session {session.index} too short ({len(session.windows)} windows)", file=sys.stderr)
        return 2
    current = metrics(session, args.skip)

    baselines = json.loads(BASELINE.read_text()) if BASELINE.exists() else {}
    if args.update:
        baselines[args.mode] = {**current, "source": f"{Path(args.perf).parent.name}/{Path(args.perf).name} session {session.index}"}
        BASELINE.write_text(json.dumps(baselines, indent=2, ensure_ascii=False) + "\n")
        print(f"{args.mode} FPS baseline saved: {current}")
        return 0
    reference = baselines.get(args.mode)
    if reference is None:
        print(f"no baseline for {args.mode} FPS; run with --update", file=sys.stderr)
        return 2
    if reference.get("cpu_mhz") and current["cpu_mhz"] and reference["cpu_mhz"] != current["cpu_mhz"]:
        print(f"different clock (CPU {current['cpu_mhz']:.0f} vs {reference['cpu_mhz']:.0f} MHz): "
              "not comparable", file=sys.stderr)
        return 2

    failed = False
    print(f"session {session.index}, {current['minutes']:.1f} min, mode {args.mode} FPS "
          f"(baseline: {reference.get('source', '?')})")
    for key, tolerance in TOLERANCE.items():
        ref, now = reference[key], current[key]
        if key == "slow_share":
            worse = now - ref > tolerance
        elif key in LOWER_IS_BETTER:
            worse = now > ref * (1.0 + tolerance) + 0.05
        else:
            worse = now < ref * (1.0 - tolerance)
        failed |= worse
        print(f"  {'REGRESSION' if worse else 'ok':<9} {key:<11} {now:8.3f}  (ref {ref:.3f}, tolerance {tolerance:.0%})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
