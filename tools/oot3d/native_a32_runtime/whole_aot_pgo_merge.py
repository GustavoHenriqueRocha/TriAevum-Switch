"""Merges the console's PGO session streams into .gcda files for the use build.

The instrumented NRO (OOT3D_WHOLE_AOT_PROFILE_MODE=generate, GCC) streams the
whole-AOT shards' counters into sdmc:/switch/oot3dre/pgo/oot3d_pgo_<N>.stream,
one file per session (oot3d_whole_aot_pgo.cpp). Copy that folder to the PC,
then:

  python3 whole_aot_pgo_merge.py --streams <copied pgo folder> --output <profile dir>

Inside the devkitPro image (aarch64-none-elf-gcov-tool of the compiler that
built the NRO). For each session ``gcov-tool merge-stream`` adds the counts to
the .gcda files the stream names: the instrumented build's runtime directory
(OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR, default sdmc:/switch/oot3dre/pgo)
plus the object's flattened path, e.g.
``sdmc:/switch/oot3dre/pgo/CMakeFiles#oot3d_native_whole_aot.dir#home#...#oot3d_whole_aot_shard_000.cpp.gcda``.
They are written under a scratch folder and moved, flat, into --output: the
directory the use build reads (-DOOT3D_WHOLE_AOT_PROFILE_FILE).

Then the object summary is filled in: __gcov_info_to_gcda writes runs = 0 and
sum_max = 0, and GCC's -fprofile-use derives its hot / never-executed
thresholds from them (with zeros every block counts as hot). As libgcov's own
dump would, runs = number of sessions and sum_max = the largest arc count.
records() and fill_summaries() are adapted from sa2-nx's tools/pgo_summary.py
at commit adf3a4ee (https://github.com/ChanseyIsTheBest/sa2-nx,
ChanseyIsTheBest, GPL-3.0).

A lone ``.stream.tmp`` (no ``.stream`` beside it) is skipped: it is a first
dump cut short or, rarely, a complete dump cut between remove and rename, and
the two cannot be told apart.

Format (GCC 12+, little-endian target): magic, version, stamp, checksum, then
records of tag, length in bytes (negative: that many zero counters, no data);
OBJECT_SUMMARY = tag 0xA1000000 (runs, sum_max: u32 each), arc counters = tag
0x01A10000 (u64 each).
"""

from __future__ import annotations

import argparse
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterator, Sequence

DEFAULT_GCOV_TOOL = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcov-tool"
DEFAULT_RUNTIME_DIR = "sdmc:/switch/oot3dre/pgo"
GCDA_MAGIC = 0x67636461  # "gcda"
TAG_SUMMARY = 0xA1000000
TAG_ARCS = 0x01A10000
STREAM_RE = re.compile(r"^oot3d_pgo_(\d+)\.stream$")


def records(data: bytes) -> Iterator[tuple[int, int, int]]:
    """(tag, payload offset, payload length) of each record of a .gcda."""
    if len(data) < 16 or struct.unpack_from("<I", data, 0)[0] != GCDA_MAGIC:
        raise ValueError("not a little-endian .gcda")
    offset = 16
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, offset)
        payload = max(length, 0)
        yield tag, offset + 8, payload
        offset += 8 + payload


def largest_arc(data: bytes) -> int:
    top = 0
    for tag, start, length in records(data):
        if tag == TAG_ARCS:
            for index in range(length // 8):
                top = max(top, struct.unpack_from("<q", data, start + 8 * index)[0])
    return top


def fill_summaries(files: Sequence[Path], sessions: int) -> tuple[int, int]:
    """Writes runs = sessions, sum_max = largest arc count into every file's
    object summary; returns (sum_max, files patched)."""
    top = max((largest_arc(path.read_bytes()) for path in files), default=0)
    sum_max = min(top, 0xFFFFFFFF)
    patched = 0
    for path in files:
        data = bytearray(path.read_bytes())
        for tag, start, length in records(bytes(data)):
            if tag == TAG_SUMMARY and length >= 8:
                struct.pack_into("<II", data, start, sessions, sum_max)
                break
        else:
            # No summary record at all: insert one after the header, where
            # libgcov writes it.
            data[16:16] = struct.pack("<Ii", TAG_SUMMARY, 8) + struct.pack("<II", sessions, sum_max)
        patched += 1
        path.write_bytes(bytes(data))
    return sum_max, patched


def session_streams(folder: Path) -> tuple[list[Path], list[Path]]:
    """(complete streams by session number, skipped lone .tmp files)."""
    streams = sorted(
        (path for path in folder.iterdir() if STREAM_RE.match(path.name)),
        key=lambda path: int(STREAM_RE.match(path.name).group(1)))
    names = {path.name for path in streams}
    skipped = sorted(
        path for path in folder.iterdir()
        if path.name.endswith(".stream.tmp") and path.name[:-len(".tmp")] not in names)
    return streams, skipped


def merge(streams_dir: Path, output: Path, runtime_dir: str, gcov_tool: str) -> int:
    streams, skipped = session_streams(streams_dir)
    for path in skipped:
        print(f"skipped {path.name}: no complete dump of that session", file=sys.stderr)
    if not streams:
        raise SystemExit(f"no oot3d_pgo_<N>.stream in {streams_dir}")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.glob("*.gcda")):
        raise SystemExit(
            f"{output} already has .gcda files: move them aside first "
            "(merge-stream adds to them)")
    if runtime_dir.startswith("/"):
        raise SystemExit(
            f"--runtime-dir {runtime_dir} is absolute: merge-stream would write there, "
            "outside the scratch folder (keep the default sdmc: directory)")
    relative = runtime_dir
    with tempfile.TemporaryDirectory() as scratch:
        staging = Path(scratch)
        (staging / relative).mkdir(parents=True)
        for stream in streams:
            print(f"merging {stream.name} ({stream.stat().st_size / 1e6:.1f} MB)")
            process = subprocess.run(
                [gcov_tool, "merge-stream", str(stream.resolve())],
                cwd=staging, capture_output=True, text=True)
            if process.returncode != 0:
                raise SystemExit(
                    f"gcov-tool merge-stream failed on {stream}:\n{process.stderr}")
        produced = sorted((staging / relative).glob("*.gcda"))
        if not produced:
            found = sorted(str(path.relative_to(staging)) for path in staging.rglob("*.gcda"))
            raise SystemExit(
                f"no .gcda under {runtime_dir} after merging (found: {found[:3]}): pass the "
                "instrumented build's OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR as --runtime-dir")
        for path in produced:
            shutil.move(str(path), output / path.name)
    files = sorted(output.glob("*.gcda"))
    sum_max, patched = fill_summaries(files, len(streams))
    print(f"{len(streams)} session(s) merged into {len(files)} .gcda file(s) in {output}: "
          f"runs = {len(streams)}, sum_max = {sum_max}, {patched} summaries filled")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--streams", type=Path, required=True,
                        help="folder copied from sdmc:/switch/oot3dre/pgo")
    parser.add_argument("--output", type=Path, required=True,
                        help="profile directory for -DOOT3D_WHOLE_AOT_PROFILE_FILE")
    parser.add_argument("--runtime-dir", default=DEFAULT_RUNTIME_DIR,
                        help="the instrumented build's OOT3D_WHOLE_AOT_GCC_PROFILE_RUNTIME_DIR")
    parser.add_argument("--gcov-tool", default=DEFAULT_GCOV_TOOL)
    args = parser.parse_args(argv)
    return merge(args.streams, args.output, args.runtime_dir, args.gcov_tool)


if __name__ == "__main__":
    sys.exit(main())
