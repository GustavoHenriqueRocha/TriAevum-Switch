"""Lists the hot guest functions from a GCC PGO collection (.gcda).

Output: one hex entry per line (with name and count as a comment), in the
format that generate_whole_aot_cpp.py --hot-functions reads. Hot = function
``Execute_<name>_<ENTRY>`` executed at least ``--min-count`` times in the
collection (default 1: every function that ran).

Two modes (``--mode auto`` picks ``gcov`` if there are .gcno for the .gcda):

gcov  Uses aarch64-none-elf-gcov -j -t with the .gcno of the instrumented build
      (they only exist if it was compiled with -ftest-coverage, which
      -fprofile-generate alone does NOT do). The count is the exact number of
      entries into the function (``execution_count``).

gcda  Reads the .gcda directly, without .gcno. Each function in the .gcda is
      identified by the ``profile_id`` GCC computes (coverage_compute_profile_id):
      for an external symbol it is crc32(mangled name) & 0x7fffffff. The Execute_
      functions are external (namespace Oot3dNativeGame::GeneratedWholeAot), so the
      script recomputes that id from the names in the generated C++ and matches it
      against the .gcda. The count used is the function's largest arc counter; a
      function without branches has no arc counter, and then the time_profiler
      counts (order of first execution, > 0 if it ran). For the "ran or not"
      criterion both modes agree; the gcda-mode count is only for ordering.

The .gcda files have names flattened with '#' (-fprofile-prefix-path), for example
CMakeFiles#oot3d_native_whole_aot.dir#home#gustavo#oot3d-work#gen#cpp-lean#oot3d_whole_aot_shard_000.cpp.gcda;
the shard is deduced from the ``oot3d_whole_aot_shard_NNN`` suffix.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Iterable, Sequence

DEFAULT_GCOV = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcov"
# GCC names the .gcda after the object minus its last suffix: CMake's
# oot3d_whole_aot_shard_000.cpp.obj gives oot3d_whole_aot_shard_000.cpp.gcda
# (checked with devkitA64 GCC 16.1); a bare shard_000.gcda is accepted too.
SHARD_RE = re.compile(r"(oot3d_whole_aot_shard_\d+)(?:\.cpp)?\.gcda$")
# Definitions in the generated C++ (the giant-function attribute, if any, comes
# earlier on the same line); declarations end in ");" and are left out.
DEFINITION_RE = re.compile(
    r"^(?:\[\[[^\]]*\]\] )*Oot3dWholeAotFlow (Execute_\w+)\(\n((?:.*\n){0,3}?.*\)) \{$",
    re.MULTILINE,
)
ENTRY_RE = re.compile(r"_([0-9A-F]{8})(?:_Observed|_Unobserved)?$")
NAMESPACE = "_ZN15Oot3dNativeGame17GeneratedWholeAot"
PARAM_MANGLING = {
    "Oot3dWholeAotFrame&": "RNS_18Oot3dWholeAotFrameE",
    "Oot3dWholeAotContext&": "RNS_20Oot3dWholeAotContextE",
    "Oot3dAotArchitecturalState&": "RNS_26Oot3dAotArchitecturalStateE",
    "uint32_t": "j",
}

GCOV_MAGIC = 0x67636461  # "gcda"
TAG_FUNCTION = 0x01000000
TAG_ARCS = 0x01A10000
TAG_TIME_PROFILER = 0x01AF0000


# --- GCC profile_id (coverage.cc / tree.cc) --------------------------------

def _crc32_byte_slow(chksum: int, byte: int) -> int:
    """GCC's crc32_unsigned_n(chksum, byte, 1): CRC-32 0x04C11DB7, MSB first."""
    value = (byte & 0xFF) << 24
    for _ in range(8):
        feedback = 0x04C11DB7 if (value ^ chksum) & 0x80000000 else 0
        chksum = ((chksum << 1) & 0xFFFFFFFF) ^ feedback
        value = (value << 1) & 0xFFFFFFFF
    return chksum


_CRC_TABLE = [_crc32_byte_slow(index << 24, 0) for index in range(256)]


def _crc32_byte(chksum: int, byte: int) -> int:
    return ((chksum << 8) & 0xFFFFFFFF) ^ _CRC_TABLE[((chksum >> 24) ^ byte) & 0xFF]


def gcc_profile_id(assembler_name: str) -> int:
    """coverage_compute_profile_id for a TREE_PUBLIC symbol."""
    chksum = 0
    # crc32_string includes the trailing NUL.
    for byte in assembler_name.encode() + b"\0":
        chksum = _crc32_byte(chksum, byte)
    chksum &= 0x7FFFFFFF
    return chksum + (chksum == 0)


def mangle(symbol: str, parameters: str) -> str | None:
    types = []
    for parameter in parameters.strip().rstrip(")").split(","):
        words = parameter.split()
        if not words:
            continue
        mangled = PARAM_MANGLING.get(words[0])
        if mangled is None:
            return None
        types.append(mangled)
    # After its first use, Oot3dNativeGame is the substitution S_ (NS_).
    return f"{NAMESPACE}{len(symbol)}{symbol}E{''.join(types)}"


def generated_functions(generated_dir: Path) -> dict[str, dict[int, tuple[int, str]]]:
    """Per shard: profile_id -> (guest entry, symbol)."""
    result: dict[str, dict[int, tuple[int, str]]] = {}
    for path in sorted(generated_dir.glob("oot3d_whole_aot_shard_*.cpp")):
        table: dict[int, tuple[int, str]] = {}
        for match in DEFINITION_RE.finditer(path.read_text(encoding="utf-8")):
            symbol = match.group(1)
            entry = ENTRY_RE.search(symbol)
            name = mangle(symbol, match.group(2))
            if entry is None or name is None:
                continue
            table[gcc_profile_id(name)] = (int(entry.group(1), 16), symbol)
        result[path.stem] = table
    return result


# --- direct .gcda reading --------------------------------------------------

def read_gcda(path: Path) -> list[tuple[int, int, int]]:
    """[(profile_id, largest arc counter, time_profiler)] of a .gcda."""
    data = path.read_bytes()
    magic, = struct.unpack_from("<I", data, 0)
    if magic != GCOV_MAGIC:
        raise ValueError(f"{path}: not a little-endian .gcda")
    # magic, version, stamp and (GCC >= 12) checksum.
    offset = 16
    functions: list[list[int]] = []
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, offset)
        offset += 8
        # Negative length = all counters zero, no data written.
        payload = data[offset:offset + max(length, 0)]
        offset += max(length, 0)
        if tag == TAG_FUNCTION:
            if length >= 4:
                functions.append([struct.unpack_from("<I", payload, 0)[0], 0, 0])
        elif functions and tag in (TAG_ARCS, TAG_TIME_PROFILER) and length > 0:
            counts = struct.unpack_from(f"<{length // 8}q", payload)
            if tag == TAG_ARCS:
                functions[-1][1] = max(counts, default=0)
            else:
                functions[-1][2] = counts[0]
    return [tuple(item) for item in functions]  # type: ignore[misc]


def counts_from_gcda(
    gcda_files: Iterable[Path], generated_dir: Path
) -> tuple[dict[int, tuple[str, int]], list[str]]:
    tables = generated_functions(generated_dir)
    if not tables:
        raise SystemExit(f"no generated shard in {generated_dir}")
    counts: dict[int, tuple[str, int]] = {}
    notes: list[str] = []
    for path in gcda_files:
        shard = SHARD_RE.search(path.name)
        if shard is None:
            continue
        table = tables.get(shard.group(1))
        if table is None:
            notes.append(f"{path.name}: shard missing from the generated C++")
            continue
        matched = 0
        for ident, arcs, first_run in read_gcda(path):
            item = table.get(ident)
            if item is None:
                continue  # inline runtime helper, not a guest function
            matched += 1
            entry, symbol = item
            count = arcs if arcs > 0 else (1 if first_run > 0 else 0)
            old = counts.get(entry, (symbol, 0))[1]
            counts[entry] = (symbol, max(old, count))
        if matched < len(table):
            # A guest function of a shard that was compiled always appears in the
            # .gcda; if it did not match, the .gcda is from other C++ (other shard/generation).
            notes.append(
                f"{path.name}: {len(table) - matched} of {len(table)} "
                "shard function(s) without a record in the .gcda")
    return counts, notes


# --- gcov mode (.gcno + .gcda) ----------------------------------------------

def gcno_for(gcda: Path, build_dir: Path) -> Path:
    return build_dir / (gcda.name.replace("#", "/")[:-len(".gcda")] + ".gcno")


def counts_from_gcov(
    gcda_files: Iterable[Path], build_dir: Path, gcov: str
) -> tuple[dict[int, tuple[str, int]], list[str]]:
    counts: dict[int, tuple[str, int]] = {}
    notes: list[str] = []
    for gcda in gcda_files:
        if SHARD_RE.search(gcda.name) is None:
            continue
        gcno = gcno_for(gcda, build_dir)
        if not gcno.is_file():
            notes.append(f"{gcda.name}: missing {gcno}")
            continue
        with tempfile.TemporaryDirectory() as temp:
            stem = Path(temp) / "shard"
            stem.with_suffix(".gcno").symlink_to(gcno.resolve())
            stem.with_suffix(".gcda").symlink_to(gcda.resolve())
            process = subprocess.run(
                [gcov, "-j", "-t", "-o", str(stem.with_suffix(".gcno")),
                 str(stem) + ".cpp"],
                capture_output=True, text=True, cwd=temp)
        if process.returncode != 0:
            notes.append(f"{gcda.name}: gcov failed: {process.stderr.strip()}")
            continue
        report = json.loads(process.stdout)
        for source in report.get("files", []):
            for function in source.get("functions", []):
                name = str(function.get("demangled_name") or function["name"])
                match = re.search(r"(Execute_\w+)\(", name)
                entry = ENTRY_RE.search(match.group(1)) if match else None
                if entry is None:
                    continue
                address = int(entry.group(1), 16)
                count = int(function.get("execution_count", 0))
                old = counts.get(address, (match.group(1), 0))[1]
                counts[address] = (match.group(1), max(old, count))
    return counts, notes


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--gcda-dir", type=Path, required=True,
                        help="directory with the collection's .gcda files (names with '#')")
    parser.add_argument("--generated-dir", type=Path,
                        help="generated C++ that was compiled for the collection (gcda mode)")
    parser.add_argument("--build-dir", type=Path,
                        help="instrumented build, with the .gcno files (gcov mode)")
    parser.add_argument("--mode", choices=("auto", "gcov", "gcda"), default="auto")
    parser.add_argument("--gcov", default=DEFAULT_GCOV)
    parser.add_argument("--min-count", type=int, default=1,
                        help="minimum count to be hot (default 1)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    gcda_files = sorted(args.gcda_dir.glob("*.gcda"))
    if not any(SHARD_RE.search(path.name) for path in gcda_files):
        parser.error(f"no shard .gcda in {args.gcda_dir}")
    mode = args.mode
    if mode == "auto":
        has_notes = args.build_dir is not None and any(
            gcno_for(path, args.build_dir).is_file()
            for path in gcda_files if SHARD_RE.search(path.name))
        mode = "gcov" if has_notes else "gcda"
    if mode == "gcov":
        if args.build_dir is None:
            parser.error("--mode gcov requires --build-dir")
        counts, notes = counts_from_gcov(gcda_files, args.build_dir, args.gcov)
    else:
        if args.generated_dir is None:
            parser.error("no .gcno: gcda mode requires --generated-dir")
        counts, notes = counts_from_gcda(gcda_files, args.generated_dir)
    for note in notes:
        print("warning:", note, file=sys.stderr)
    if not counts:
        raise SystemExit("no guest function matched the collection")
    hot = sorted(
        (entry for entry, (_, count) in counts.items() if count >= args.min_count),
        key=lambda entry: (-counts[entry][1], entry),
    )
    args.output.write_text(
        "".join(f"{entry:08X}  # {counts[entry][0]} {counts[entry][1]}\n"
                for entry in hot),
        encoding="utf-8",
    )
    print(f"mode {mode}: {len(counts)} guest function(s) in the collection, "
          f"{len(hot)} hot (count >= {args.min_count})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
