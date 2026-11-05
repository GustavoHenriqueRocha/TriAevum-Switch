#!/usr/bin/env python3
"""Discover a title's A32 functions from code.bin + ExHeader alone (no manual inventory).

The whole-AOT flow analysis already follows direct calls, tail calls and code
addresses it finds in literal pools; seeded with just the ExHeader entry point
it reaches every instruction the reviewed OoT3D inventory reaches. Code
pointers stored in .rodata/.data (vtables, callback tables) are added as extra
roots where the flow decoded a block. The output CSV is a report (functions
the flow leaves unbounded have empty end/size). --program-output and
--selection-output write what generate_whole_aot_cpp.py reads: the program
manifest and a selection with every closed-CFG function (direct-call targets
left open become external functions).

  discover_functions.py --code code.bin --exheader exheader.bin --output inventory.csv
      [--compare reviewed_inventory.csv --profile eur_rev0]
      [--program-output aot_program.json --selection-output functions.json]

OoT3D EUR (2026-11-05): 14580 functions, 924525 instructions (same as the
reviewed inventory), 0 unclaimed blocks, 0 unresolved edges; 10307 of the
10388 reviewed entries are roots (the other 81 are never referenced).
"""
from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import whole_aot_program as program  # noqa: E402
from whole_aot_cpp import SELECTION_FORMAT  # noqa: E402

PAGE = 0x1000
INVENTORY_FIELDS = ("profile", "entry", "end", "size", "name", "status")
AUDIT_FIELDS = ("entry", "size", "name", "boundary_kind", "body_status", "abi_status",
                "classification", "evidence", "notes")


def code_segments(exheader: bytes) -> dict[str, tuple[int, int, int]]:
    """(address, file offset in the decompressed code.bin, size) of text, ro and data."""
    segments, offset = {}, 0
    for name, at in (("text", 0x10), ("ro", 0x20), ("data", 0x30)):
        address, pages, size = struct.unpack_from("<3I", exheader, at)
        segments[name] = (address, offset, size)
        offset += pages * PAGE
    return segments


def data_code_pointers(code: bytes, segments: dict[str, tuple[int, int, int]]) -> set[int]:
    text_address, _, text_size = segments["text"]
    pointers = set()
    for name in ("ro", "data"):
        _, offset, size = segments[name]
        for at in range(offset, min(offset + size, len(code)) - 3, 4):
            value = struct.unpack_from("<I", code, at)[0]
            if text_address <= value < text_address + text_size and value % 4 == 0:
                pointers.add(value)
    return pointers


def discover(code: bytes, exheader: bytes, profile: str) -> tuple[program.AotProgram, set[int]]:
    segments = code_segments(exheader)
    text_address, _, text_size = segments["text"]
    pointers = data_code_pointers(code, segments)
    with tempfile.TemporaryDirectory() as scratch:
        seed, audit = Path(scratch) / "seed.csv", Path(scratch) / "audit.csv"
        with seed.open("w", newline="") as output:
            writer = csv.DictWriter(output, INVENTORY_FIELDS)
            writer.writeheader()
            # crt0 runs straight into the startup routines; the flow bounds it.
            writer.writerow({"profile": profile, "entry": f"0x{text_address:08X}",
                             "end": f"0x{text_address + 0x27:08X}", "size": "40",
                             "name": "ctr_entry", "status": "seed"})
        with audit.open("w", newline="") as output:
            csv.DictWriter(output, AUDIT_FIELDS).writeheader()
        result = program.build_program(code, seed, audit, base=text_address,
                                       executable_size=text_size, extra_roots=pointers)
    return result, pointers


def closed_selection(manifest: dict) -> dict:
    """Every closed-CFG function; open direct-call targets become external."""
    records = {int(item["entry"]): item for item in manifest["functions"]}
    selected = {entry for entry, record in records.items() if record.get("closed_static_cfg")}
    external = {int(call["target"]) for entry in selected
                for call in records[entry].get("direct_calls", [])} - selected
    return {"format": SELECTION_FORMAT, "selection_mode": "all_lowerable",
            "functions": [{"entry": entry, "name": records[entry]["name"],
                           "reason": "discovered closed CFG"} for entry in sorted(selected)],
            "external_functions": [{"entry": entry, "reason": "open CFG"} for entry in sorted(external)],
            "performance_exclusions": [], "selection_conflicts": [], "closure_groups": []}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--code", type=Path, required=True)
    parser.add_argument("--exheader", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", default="title")
    parser.add_argument("--compare", type=Path, help="reviewed inventory to measure recall against")
    parser.add_argument("--program-output", type=Path, help="whole-AOT program manifest (JSON)")
    parser.add_argument("--selection-output", type=Path, help="whole-AOT function selection (JSON)")
    args = parser.parse_args()
    code = args.code.read_bytes()
    result, pointers = discover(code, args.exheader.read_bytes(), args.profile)
    with args.output.open("w", newline="") as output:
        writer = csv.DictWriter(output, INVENTORY_FIELDS)
        writer.writeheader()
        for function in sorted(result.functions, key=lambda f: f.entry):
            bounded = function.end is not None
            writer.writerow({"profile": args.profile, "entry": f"0x{function.entry:08X}",
                             "end": f"0x{function.end:08X}" if bounded else "",
                             "size": str(function.end - function.entry + 1) if bounded else "",
                             "name": function.name, "status": function.origin})
    manifest = result.to_manifest(False)
    counts = manifest["counts"]
    if args.program_output:
        args.program_output.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
    if args.selection_output:
        selection = closed_selection(manifest)
        args.selection_output.write_text(json.dumps(selection, indent=2) + "\n")
        print(f"seleção: {len(selection['functions'])} funções com CFG fechado, "
              f"{len(selection['external_functions'])} externas")
    print(f"{counts['functions']} funções, {counts['instructions']} instruções, "
          f"{counts['unclaimed_blocks']} blocos sem dono, {counts['unresolved_static_edges']} arestas "
          f"sem destino; {len(pointers)} ponteiros de código no ro/data -> {args.output}")
    if args.compare:
        reviewed = {int(row["entry"], 0) for row in csv.DictReader(args.compare.open())
                    if row.get("profile", args.profile) == args.profile}
        found = {function.entry for function in result.functions}
        print(f"inventário revisado: {len(reviewed & found)} de {len(reviewed)} entradas encontradas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
