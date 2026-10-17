"""Lista as funções guest quentes a partir de perfis do jogo.

Saída: uma entrada hex por linha (com o nome e o % como comentário), no
formato que generate_whole_aot_cpp.py --hot-functions lê. As funções listadas
nunca recebem o atributo de função gigante fria, de modo que o caminho quente continua com
todas as otimizações de -O3.

Entradas aceitas (repetíveis, misturáveis):
  --profile guest_profile.txt   linhas "0xPC  N.NN%  ..." (triaevum guest_profile v1)
  --entries arquivo             entradas hex já conhecidas (uma por linha)

Um perfil cobre só as cenas jogadas; por isso o padrão é conservador: toda
função que aparece em qualquer perfil (--min-percent 0) conta como quente.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
from pathlib import Path
from typing import Sequence

PROFILE_LINE = re.compile(r"^0x([0-9A-Fa-f]+)\s+([\d.]+)%")


def block_owners(program: dict[str, object]) -> tuple[list[int], list[tuple[int, int, int]]]:
    """Intervalos [pc, end_pc) de cada bloco -> entrada da função dona."""
    blocks = {int(block["id"]): block for block in program["blocks"]}
    ranges: list[tuple[int, int, int]] = []
    for function in program["functions"]:
        entry = int(function["entry"])
        for block_id in function["blocks"]:
            block = blocks[int(block_id)]
            ranges.append((int(block["pc"]), int(block["end_pc"]), entry))
    ranges.sort()
    return [item[0] for item in ranges], ranges


def owner_of(pc: int, starts: list[int], ranges: list[tuple[int, int, int]]) -> int | None:
    # Blocos compartilhados aparecem uma vez por função; a busca para nos
    # 64 intervalos que começam antes do PC (blocos são curtos).
    index = bisect.bisect_right(starts, pc) - 1
    for candidate in range(index, max(-1, index - 64), -1):
        start, end, entry = ranges[candidate]
        if start <= pc < end:
            return entry
    return None


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--program", type=Path, required=True,
                        help="aot_program.json (blocos e funções)")
    parser.add_argument("--profile", type=Path, action="append", default=[])
    parser.add_argument("--entries", type=Path, action="append", default=[])
    parser.add_argument("--min-percent", type=float, default=0.0)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    program = json.loads(args.program.read_text(encoding="utf-8"))
    names = {int(f["entry"]): str(f.get("name", "")) for f in program["functions"]}
    starts, ranges = block_owners(program)
    percent: dict[int, float] = {}
    unknown = 0.0
    for path in args.profile:
        for line in path.read_text(encoding="utf-8").splitlines():
            match = PROFILE_LINE.match(line)
            if not match:
                continue
            pc, share = int(match.group(1), 16), float(match.group(2))
            entry = owner_of(pc, starts, ranges)
            if entry is None:
                unknown += share
                continue
            percent[entry] = max(percent.get(entry, 0.0), 0.0) + share
    for path in args.entries:
        for line in path.read_text(encoding="utf-8").splitlines():
            token = line.split("#", 1)[0].strip()
            if token:
                percent.setdefault(int(token.split()[0], 16), 0.0)
    hot = sorted(
        (entry for entry, share in percent.items()
         if share >= args.min_percent or share == 0.0),
        key=lambda entry: (-percent[entry], entry),
    )
    args.output.write_text(
        "".join(
            f"{entry:08X}  # {names.get(entry, '?')} {percent[entry]:.2f}%\n"
            for entry in hot
        ),
        encoding="utf-8",
    )
    print(f"{len(hot)} função(ões) quente(s); {unknown:.2f}% sem função dona")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
