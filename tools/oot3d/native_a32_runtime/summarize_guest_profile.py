"""Tabela das funções guest mais caras, para escolher substituições nativas.

Fontes aceitas (uma por execução):
  --switch-profile logs/guest_profile.txt   perfil do console (oot3d_native_guest_profile)
  --block-profile perfil.json               oot3d_a32_hot_block_profile_v1 (blocos amostrados)
  --coverage-profile perfil.json            oot3d_hot_source_coverage_v1 (por função)

Para cada função soma as amostras e junta dados estáticos do programa
estrutural (aot_program.json): instruções, blocos, instruções VFP, acessos à
memória, chamadas diretas/indiretas/de cauda, se é folha, e se já existe
substituição em guest_overrides.json.

--export-function-map grava o mapa entrada->nome que o perfil do console lê
(copiar para sdmc:/switch/oot3dre/config/guest_functions.txt).
"""

from __future__ import annotations

import argparse
import json
import re
from bisect import bisect_right
from pathlib import Path

HERE = Path(__file__).resolve().parent


def load_program(path: Path) -> tuple[list[dict], list[dict]]:
    program = json.loads(path.read_text(encoding="utf-8"))
    return program["functions"], program["blocks"]


def function_stats(function: dict, blocks: list[dict]) -> dict[str, int]:
    categories: dict[str, int] = {}
    instructions = 0
    for index in function["blocks"]:
        block = blocks[index]
        instructions += block["instruction_count"]
        for key, value in block["categories"].items():
            categories[key] = categories.get(key, 0) + value
    return {
        "instructions": instructions,
        "blocks": len(function["blocks"]),
        "vfp": categories.get("vfp_scalar", 0),
        "vfp_transport": categories.get("vfp_transport", 0),
        "memory": categories.get("core_memory", 0),
        "calls": len(function["direct_calls"]),
        "indirect": len(function["indirect_sites"]),
        "tail": len(function["tail_calls"]),
    }


def parse_switch_profile(path: Path) -> dict[int, float]:
    weights: dict[int, float] = {}
    section = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# funcoes"):
            section = "functions"
            continue
        if line.startswith("# blocos"):
            section = "blocks"
            continue
        if not line.startswith("0x"):
            continue
        fields = line.split()
        if section == "blocks":
            # pc | %tempo | ms | entradas | ns
            weights[int(fields[0], 16)] = weights.get(int(fields[0], 16), 0.0) + float(fields[2])
    return weights


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path, required=True, help="aot_program.json")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--switch-profile", type=Path)
    source.add_argument("--block-profile", type=Path)
    source.add_argument("--coverage-profile", type=Path)
    parser.add_argument("--manifest", type=Path, default=HERE / "guest_overrides.json")
    parser.add_argument("--top", type=int, default=30)
    parser.add_argument("--export-function-map", type=Path)
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args()

    functions, blocks = load_program(args.program)
    functions.sort(key=lambda item: item["entry"])
    entries = [item["entry"] for item in functions]
    if args.export_function_map:
        lines = ["# entrada nome (gerado por summarize_guest_profile.py)"]
        lines += ["%08X %s" % (item["entry"], re.sub(r"\s+", "_", item["name"])) for item in functions]
        args.export_function_map.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"{len(functions)} funções -> {args.export_function_map}")

    weights: dict[int, float] = {}
    unit = "amostras"
    if args.coverage_profile:
        data = json.loads(args.coverage_profile.read_text(encoding="utf-8"))
        for item in data["functions"]:
            weights[item["entry"]] = weights.get(item["entry"], 0.0) + item["samples"]
        total = float(data["profile_total_samples"])
    elif args.block_profile:
        data = json.loads(args.block_profile.read_text(encoding="utf-8"))
        for item in data["blocks"]:
            index = bisect_right(entries, item["pc"]) - 1
            if index >= 0:
                entry = entries[index]
                weights[entry] = weights.get(entry, 0.0) + item["samples"]
        total = float(data["total_samples"])
    elif args.switch_profile:
        unit = "ms"
        for pc, value in parse_switch_profile(args.switch_profile).items():
            index = bisect_right(entries, pc) - 1
            if index >= 0:
                weights[entries[index]] = weights.get(entries[index], 0.0) + value
        total = sum(weights.values())
    else:
        return 0

    overridden = set()
    if args.manifest.exists():
        overridden = {int(str(item["entry"]), 0)
                      for item in json.loads(args.manifest.read_text(encoding="utf-8"))["overrides"]}
    by_entry = {item["entry"]: item for item in functions}
    ranked = sorted(weights.items(), key=lambda item: -item[1])[: args.top]
    header = ("entrada", "nome", "%", unit, "instr", "blocos", "vfp", "mem",
              "chamadas", "ind", "cauda", "folha", "nativa")
    rows = []
    for entry, weight in ranked:
        function = by_entry.get(entry)
        if function is None:
            continue
        stats = function_stats(function, blocks)
        leaf = stats["calls"] == 0 and stats["indirect"] == 0 and stats["tail"] == 0
        rows.append((
            f"0x{entry:08X}", function["name"], f"{100.0 * weight / total:.2f}",
            f"{weight:.1f}" if unit == "ms" else f"{int(weight)}",
            str(stats["instructions"]), str(stats["blocks"]), str(stats["vfp"]),
            str(stats["memory"]), str(stats["calls"]), str(stats["indirect"]),
            str(stats["tail"]), "sim" if leaf else "não",
            "sim" if entry in overridden else "",
        ))
    if args.markdown:
        print("| " + " | ".join(header) + " |")
        print("|" + "---|" * len(header))
        for row in rows:
            print("| " + " | ".join(row) + " |")
    else:
        print("  ".join(header))
        for row in rows:
            print("  ".join(row))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
