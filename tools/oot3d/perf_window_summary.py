#!/usr/bin/env python3
"""Per-window averages of logs/perf.txt, only from the game's last session.

perf.txt is appended on every run: mixing sessions gave wrong numbers
(tour22 counted together with the slow tour21: 48 missed refreshes/window instead of 4.1).
The new session starts when host_frames goes down again.

Usage: perf_window_summary.py perf.txt [--first N] [field ...]
     perf_window_summary.py --guest-spikes logs/guest_spikes_summary.txt
"""
import re
import sys

DEFAULT_FIELDS = ["fps", "guest", "present", "total_ms", "game_cpu", "missed_refresh",
                  "missed_by_stage_guest", "missed_by_stage_render", "missed_frames",
                  "hold_2", "hold_4p", "interp_frames"]


def last_session(lines):
    """Window lines (host_frames=...) of the last session."""
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
    """Fraction of frames that became spikes in the guest spike profile.

    tour21: the instrumentation took the guest to 39.5 ms/frame with a 14 ms limit,
    97% of frames became spikes and the "spike profile" became a general profile.
    Above 0.5 the result doesn't isolate spikes: raise the limit (relative to the average
    measured with the profile on) and repeat."""
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
            print("no quadros=/picos= header")
            return 2
        print(f"picos/quadros={ratio:.2f}" + (" SATURATED: does not isolate spikes" if ratio > 0.5 else ""))
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
