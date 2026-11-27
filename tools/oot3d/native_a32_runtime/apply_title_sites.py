"""Compila os sítios de título (MM3D: Restoration e dekui) nos blocos do C++ do whole-AOT.

    python -I apply_title_sites.py <pasta do C++ gerado> <guest_patch_supplemental.csv> [--remove]

Logo depois do Oot3dAotEnterBlock de cada bloco cujo PC está no CSV (sítio, PC + 4 ou alvo), o
prólogo chama Oot3dAotTitleSite (oot3d_native_whole_aot_runtime.h), que chama o mesmo callback de
função nativa que o despachante chamaria naquele PC. Recusou (ou o PC não é sítio): o bloco segue.
Substituiu: goto no rótulo da mesma função quando o PC seguinte é um bloco do CSV que ela tem (PC + 4,
alvo, o próprio PC de um sítio Soft), senão sai por valor ({Branch, pc}), sem exceção. Antes, o código
compilado só voltava ao despachante por exceção lançada no gancho de entrada, descartando a pilha C++
de quem chamou (docs/pesquisas/2026-11-24-mm3d-desempenho-sitios.md).

Cada shard processado ganha um registro estático (Oot3dRegisterTitleSiteShard): a janela só liga os
sítios compilados se houver shards registrados. Idempotente (marcadores "title-site") e --remove desfaz.
Recusa um bloco de sítio fora do formato esperado (nenhum sítio pode ficar de fora sem aviso).
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

BEGIN = "        // title-site begin"
END = "        // title-site end"
REGISTER = ("static const bool kOot3dTitleSiteShard = "
            "Oot3dNativeGame::Oot3dRegisterTitleSiteShard(); // title-site")
FUNCTION = re.compile(r"^Oot3dWholeAotFlow Execute_\w+\($")
LABEL = re.compile(r"^block_([0-9A-F]{8}):$")


def site_pcs(csv_text: str) -> set[int]:
    """Os PCs do CSV suplementar (primeira coluna, 0x...), sem o cabeçalho."""
    return {int(m, 16) for m in re.findall(r"^0x([0-9A-Fa-f]{8}),", csv_text, re.M)}


def enter_block(pc: int) -> list[str]:
    return [
        "    {",
        f"        if (!Oot3dAotEnterBlock(context, frame, state, 0x{pc:08X}U, commitState, reloadState)) {{",
        "            commitState();",
        f"            return Oot3dAotBlockLimit(context, 0x{pc:08X}U);",
        "        }",
    ]


def prologue(pc: int, gotos: list[int]) -> list[str]:
    lines = [
        BEGIN,
        "        {",
        "            uint32_t titleSiteNext = 0U;",
        f"            if (Oot3dAotTitleSite(context, frame, state, 0x{pc:08X}U, commitState, reloadState, titleSiteNext)) {{",
        "                switch (titleSiteNext) {",
    ]
    lines += [f"                case 0x{target:08X}U: goto block_{target:08X};" for target in gotos]
    lines += [
        "                default: break;",
        "                }",
        "                commitState();",
        "                return {Oot3dWholeAotFlowKind::Branch, titleSiteNext, 0U};",
        "            }",
        "        }",
        END,
    ]
    return lines


def strip(text: str) -> str:
    out: list[str] = []
    skipping = False
    for line in text.split("\n"):
        if line == BEGIN:
            skipping = True
            continue
        if line == END:
            skipping = False
            continue
        if not skipping and line != REGISTER:
            out.append(line)
    return "\n".join(out)


def apply(text: str, pcs: set[int]) -> tuple[str, int]:
    """(texto com os prólogos, quantos blocos receberam). Sem bloco de sítio, o texto volta igual."""
    lines = strip(text).split("\n")
    out: list[str] = []
    inserted = 0
    index = 0
    while index < len(lines):
        line = lines[index]
        if not FUNCTION.match(line):
            out.append(line)
            index += 1
            continue
        end = lines.index("}", index)
        body = lines[index:end + 1]
        labels = [int(m.group(1), 16) for m in map(LABEL.match, body) if m]
        gotos = sorted(pc for pc in labels if pc in pcs)
        at = 0
        while at < len(body):
            out.append(body[at])
            label = LABEL.match(body[at])
            if label and int(label.group(1), 16) in pcs:
                pc = int(label.group(1), 16)
                expected = enter_block(pc)
                if body[at + 1:at + 1 + len(expected)] != expected:
                    raise ValueError(f"bloco 0x{pc:08X} fora do formato do EnterBlock")
                out += expected
                out += prologue(pc, gotos)
                inserted += 1
                at += 1 + len(expected)
                continue
            at += 1
        index = end + 1
    result = "\n".join(out)
    if inserted:
        result = result.rstrip("\n") + "\n" + REGISTER + "\n"
    return result, inserted


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("cpp", type=Path)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--remove", action="store_true")
    args = parser.parse_args()
    pcs = site_pcs(args.csv.read_text())
    shards = blocks = changed = 0
    for path in sorted(args.cpp.glob("oot3d_whole_aot_shard_*.cpp")):
        original = path.read_text()
        if args.remove:
            text, count = strip(original), 0
        else:
            text, count = apply(original, pcs)
        if count:
            shards += 1
            blocks += count
        if text != original:
            path.write_text(text)
            changed += 1
    print(f"title-site: {len(pcs)} PCs, {shards} shards com prólogo, {blocks} blocos, {changed} arquivos mudaram")
    return 0


if __name__ == "__main__":
    sys.exit(main())
