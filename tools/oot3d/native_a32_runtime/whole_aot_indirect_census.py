"""Adds up the console's indirect call censuses for guarded direct calls (N2).

The instrumented NRO (OOT3D_WHOLE_AOT_PROFILE_MODE=generate) counts every
whole-AOT indirect call (BLX reg and computed ADD pc calls) per return address
and target for the whole session and writes the pairs beside each PGO stream
as sdmc:/switch/oot3dre/pgo/oot3d_pgo_<N>.indirect.txt
(oot3d_native_indirect_profile.h, oot3d_whole_aot_pgo.cpp). Copy that folder
to the PC, then:

  python3 whole_aot_indirect_census.py <copied pgo folder or files>... \
      [--program aot_program.json] [--output census.json] [--top 20]

Directories contribute their ``*.indirect.txt`` (a ``.tmp`` is a dump cut
short and is skipped, as are the .stream files); counts of the same pair are
summed over files. With --program (the generator's aot_program.json) each
return address is mapped to its call site and the function containing it: a
BLX returns to site + 4, a computed call to the block's ``resume`` edge.
Targets are named by the function whose entry they are.

Prints a summary (calls, sites, the share of calls from single-target sites,
the share covered by the 1/2/3 hottest targets of each site, the hottest
sites: the N2 candidates) and writes every site with its targets and shares
to --output as JSON.

File format (v1): three header lines, then one row per pair, most calls first:

  # oot3d indirect census v1
  # calls=<N> pairs=<rows> dropped=<calls that found no slot> threads=<T>
  # return target count
  0034A1C4 0021A000 12345

return and target in hex (return = guest LR at the call), count in decimal.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable, Sequence

MAGIC = "# oot3d indirect census v1"
CENSUS_RE = re.compile(r"^oot3d_pgo_(\d+)\.indirect\.txt$")
HEADER_RE = re.compile(r"^# calls=(\d+) pairs=(\d+) dropped=(\d+) threads=(\d+)$")
FORMAT = "oot3d_whole_aot_indirect_census_v1"


@dataclass
class Census:
    files: list[str] = field(default_factory=list)
    header_calls: int = 0
    dropped: int = 0
    pairs: dict[tuple[int, int], int] = field(default_factory=dict)


def census_files(inputs: Iterable[Path]) -> list[Path]:
    """The census files of the inputs: files as given, directories' sessions
    by number."""
    files: list[Path] = []
    for path in inputs:
        if path.is_dir():
            found = [entry for entry in path.iterdir() if CENSUS_RE.match(entry.name)]
            files.extend(sorted(found, key=lambda entry: int(CENSUS_RE.match(entry.name).group(1))))
        elif path.is_file():
            files.append(path)
        else:
            raise SystemExit(f"{path}: no such file or directory")
    return files


def read_census(path: Path, census: Census) -> None:
    """Adds one .indirect.txt to `census`."""
    lines = path.read_text(encoding="ascii").splitlines()
    if not lines or lines[0] != MAGIC:
        raise ValueError(f"{path}: not an indirect call census (first line {lines[:1]})")
    header = HEADER_RE.match(lines[1]) if len(lines) > 1 else None
    if header is None:
        raise ValueError(f"{path}: no '# calls= pairs= dropped= threads=' line")
    rows = 0
    for number, line in enumerate(lines[2:], start=3):
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 3:
            raise ValueError(f"{path}:{number}: expected 'return target count': {line!r}")
        key = (int(fields[0], 16), int(fields[1], 16))
        census.pairs[key] = census.pairs.get(key, 0) + int(fields[2])
        rows += 1
    if rows != int(header.group(2)):
        raise ValueError(f"{path}: {rows} rows, header says pairs={header.group(2)}")
    census.files.append(str(path))
    census.header_calls += int(header.group(1))
    census.dropped += int(header.group(3))


@dataclass
class Program:
    """What --program contributes: return address -> (site, kind) and the
    function names."""
    returns: dict[int, tuple[int, str]]
    site_functions: dict[int, str]
    entries: dict[int, str]
    ranges: list[tuple[int, int, str]]  # (entry, end, name)

    def function_at(self, pc: int) -> str | None:
        """The innermost (latest entry) function whose range holds pc (only
        for the few return addresses that match no call site)."""
        holding = [(entry, name) for entry, end, name in self.ranges if entry <= pc < end]
        return max(holding)[1] if holding else None


def load_program(path: Path) -> Program:
    program = json.loads(path.read_text(encoding="utf-8"))
    sites: dict[int, str] = {}
    site_functions: dict[int, str] = {}
    site_entries: dict[int, int] = {}
    entries: dict[int, str] = {}
    ranges: list[tuple[int, int, str]] = []
    # Some functions have no "end": their last block's end then.
    block_ends = {block["id"]: block["end_pc"] for block in program["blocks"] if "id" in block}
    for function in program["functions"]:
        name = function.get("name") or f"sub_{function['entry']:08X}"
        entries.setdefault(function["entry"], name)
        end = function.get("end") or max(
            (block_ends[block] for block in function.get("blocks", []) if block in block_ends),
            default=function["entry"] + 4)
        ranges.append((function["entry"], end, name))
        for site in function.get("indirect_sites", []):
            pc = site["site"]
            sites[pc] = site["kind"]
            # A site listed by nested functions belongs to the innermost one.
            if pc not in site_entries or function["entry"] > site_entries[pc]:
                site_entries[pc] = function["entry"]
                site_functions[pc] = name
    returns: dict[int, tuple[int, str]] = {}
    for block in program["blocks"]:
        for edge in block["successors"]:
            if edge["kind"] == "resume" and sites.get(edge["site"]) == "indirect_call":
                returns[edge["target"]] = (edge["site"], "indirect_call")
    # A BLX returns to site + 4 (the resume edge's target when there is one).
    resumed = {site for site, _ in returns.values()}
    for pc, kind in sites.items():
        if kind == "indirect_call" and pc not in resumed:
            returns.setdefault(pc + 4, (pc, kind))
    return Program(returns, site_functions, entries, ranges)


def summarize(census: Census, program: Program | None) -> dict:
    by_return: dict[int, dict[int, int]] = {}
    for (returned, target), count in census.pairs.items():
        by_return.setdefault(returned, {})[target] = count
    calls = sum(census.pairs.values())
    sites = []
    single_calls = 0
    top_calls = {1: 0, 2: 0, 3: 0}
    unmapped_sites = 0
    unmapped_calls = 0
    for returned, targets in by_return.items():
        total = sum(targets.values())
        ordered = sorted(targets.items(), key=lambda item: (-item[1], item[0]))
        if len(ordered) == 1:
            single_calls += total
        for k in top_calls:
            top_calls[k] += sum(count for _, count in ordered[:k])
        site: dict = {"return": f"0x{returned:08X}", "calls": total,
                      "share": total / calls if calls else 0.0, "target_count": len(ordered)}
        if program is not None:
            mapped = program.returns.get(returned)
            if mapped is None:
                unmapped_sites += 1
                unmapped_calls += total
                site.update({"site": None, "kind": None,
                             "function": program.function_at((returned - 4) & 0xFFFFFFFF)})
            else:
                site.update({"site": f"0x{mapped[0]:08X}", "kind": mapped[1],
                             "function": program.site_functions.get(mapped[0])})
        site["targets"] = []
        for target, count in ordered:
            entry: dict = {"target": f"0x{target:08X}", "calls": count, "share": count / total}
            if program is not None:
                entry["function"] = program.entries.get(target)
            site["targets"].append(entry)
        sites.append(site)
    sites.sort(key=lambda site: (-site["calls"], site["return"]))
    summary: dict = {
        "calls": calls,
        "header_calls": census.header_calls,
        "dropped": census.dropped,
        "pairs": len(census.pairs),
        "sites": len(sites),
        "single_target_sites": sum(1 for site in sites if site["target_count"] == 1),
        "single_target_call_share": single_calls / calls if calls else 0.0,
        "top_target_coverage": {str(k): (value / calls if calls else 0.0)
                                for k, value in top_calls.items()},
    }
    if program is not None:
        summary["unmapped_sites"] = unmapped_sites
        summary["unmapped_calls"] = unmapped_calls
    return {"format": FORMAT, "inputs": census.files, "summary": summary, "sites": sites}


def summary_text(result: dict, top: int) -> str:
    summary = result["summary"]
    coverage = summary["top_target_coverage"]
    lines = [
        f"{len(result['inputs'])} census file(s): {summary['calls']} calls at {summary['sites']} "
        f"sites, {summary['pairs']} pairs, {summary['dropped']} dropped",
        f"single-target sites: {summary['single_target_sites']}, "
        f"{summary['single_target_call_share']:.1%} of calls",
        f"hottest 1/2/3 targets per site cover {coverage['1']:.1%} / {coverage['2']:.1%} / "
        f"{coverage['3']:.1%} of calls",
    ]
    if "unmapped_sites" in summary:
        lines.append(f"return addresses with no call site in the program: "
                     f"{summary['unmapped_sites']} ({summary['unmapped_calls']} calls)")
    lines.append(f"top {min(top, len(result['sites']))} sites by calls (N2 candidates):")
    for site in result["sites"][:top]:
        where = site["return"][2:]
        if site.get("site") is not None:
            where = f"{site['site'][2:]} {site.get('function') or '?'}"
        elif "site" in site:
            where = f"ret {site['return'][2:]} (no site) {site.get('function') or '?'}"
        targets = ", ".join(
            f"{target['target'][2:]}"
            f"{' ' + target['function'] if target.get('function') else ''} {target['share']:.0%}"
            for target in site["targets"][:3])
        more = f" +{site['target_count'] - 3}" if site["target_count"] > 3 else ""
        lines.append(f"  {where}: {site['calls']} ({site['share']:.1%}), "
                     f"{site['target_count']} target(s): {targets}{more}")
    return "\n".join(lines) + "\n"


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("inputs", nargs="+", type=Path,
                        help=".indirect.txt files or folders copied from sdmc:/switch/oot3dre/pgo")
    parser.add_argument("--program", type=Path, help="aot_program.json: name the call sites")
    parser.add_argument("--output", type=Path, help="write every site and target as JSON")
    parser.add_argument("--top", type=int, default=20, help="sites listed in the summary")
    args = parser.parse_args(argv)
    files = census_files(args.inputs)
    if not files:
        raise SystemExit("no oot3d_pgo_<N>.indirect.txt in " + ", ".join(map(str, args.inputs)))
    census = Census()
    for path in files:
        try:
            read_census(path, census)
        except ValueError as error:
            raise SystemExit(str(error)) from error
    program = load_program(args.program) if args.program is not None else None
    result = summarize(census, program)
    if args.output is not None:
        args.output.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    sys.stdout.write(summary_text(result, args.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
