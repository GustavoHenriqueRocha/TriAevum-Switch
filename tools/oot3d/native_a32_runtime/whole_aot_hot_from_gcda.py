"""Lista as funções guest quentes a partir de uma coleta PGO do GCC (.gcda).

Saída: uma entrada hex por linha (com nome e contagem como comentário), no
formato que generate_whole_aot_cpp.py --hot-functions lê. Quente = função
``Execute_<nome>_<ENTRADA>`` executada pelo menos ``--min-count`` vezes na
coleta (padrão 1: toda função que rodou).

Dois modos (``--mode auto`` escolhe ``gcov`` se houver .gcno para os .gcda):

gcov  Usa aarch64-none-elf-gcov -j -t com os .gcno do build instrumentado
      (só existem se ele foi compilado com -ftest-coverage, o que
      -fprofile-generate sozinho NÃO faz). A contagem é a exata de entradas
      na função (``execution_count``).

gcda  Lê os .gcda direto, sem .gcno. Cada função do .gcda é identificada pelo
      ``profile_id`` que o GCC calcula (coverage_compute_profile_id): para um
      símbolo externo é crc32(nome mangled) & 0x7fffffff. As funções Execute_
      são externas (namespace Oot3dNativeGame::GeneratedWholeAot), então o
      script recalcula esse id a partir dos nomes do C++ gerado e casa com o
      .gcda. A contagem usada é o maior contador de arco da função; uma função
      sem desvios não tem contador de arco, e aí vale o time_profiler (ordem da
      primeira execução, > 0 se ela rodou). Para o critério "rodou ou não" os
      dois modos concordam; a contagem do modo gcda é só para ordenar.

Os .gcda têm o nome achatado com '#' (-fprofile-prefix-path), por exemplo
CMakeFiles#oot3d_native_whole_aot.dir#home#gustavo#oot3d-work#gen#cpp-lean#oot3d_whole_aot_shard_000.gcda;
o shard é deduzido do sufixo ``oot3d_whole_aot_shard_NNN``.
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
SHARD_RE = re.compile(r"(oot3d_whole_aot_shard_\d+)\.gcda$")
# Definições no C++ gerado (o atributo de função gigante, se houver, vem
# antes na mesma linha); as declarações terminam em ");" e ficam de fora.
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


# --- profile_id do GCC (coverage.cc / tree.cc) -----------------------------

def _crc32_byte_slow(chksum: int, byte: int) -> int:
    """crc32_unsigned_n(chksum, byte, 1) do GCC: CRC-32 0x04C11DB7, MSB primeiro."""
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
    """coverage_compute_profile_id para um símbolo TREE_PUBLIC."""
    chksum = 0
    # crc32_string inclui o NUL final.
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
    # Depois do primeiro uso, Oot3dNativeGame é a substituição S_ (NS_).
    return f"{NAMESPACE}{len(symbol)}{symbol}E{''.join(types)}"


def generated_functions(generated_dir: Path) -> dict[str, dict[int, tuple[int, str]]]:
    """Por shard: profile_id -> (entrada guest, símbolo)."""
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


# --- leitura direta do .gcda ------------------------------------------------

def read_gcda(path: Path) -> list[tuple[int, int, int]]:
    """[(profile_id, maior contador de arco, time_profiler)] de um .gcda."""
    data = path.read_bytes()
    magic, = struct.unpack_from("<I", data, 0)
    if magic != GCOV_MAGIC:
        raise ValueError(f"{path}: não é um .gcda little-endian")
    # magic, versão, stamp e (GCC >= 12) checksum.
    offset = 16
    functions: list[list[int]] = []
    while offset + 8 <= len(data):
        tag, length = struct.unpack_from("<Ii", data, offset)
        offset += 8
        # Comprimento negativo = contadores todos zero, sem dados gravados.
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
        raise SystemExit(f"nenhum shard gerado em {generated_dir}")
    counts: dict[int, tuple[str, int]] = {}
    notes: list[str] = []
    for path in gcda_files:
        shard = SHARD_RE.search(path.name)
        if shard is None:
            continue
        table = tables.get(shard.group(1))
        if table is None:
            notes.append(f"{path.name}: shard ausente no C++ gerado")
            continue
        matched = 0
        for ident, arcs, first_run in read_gcda(path):
            item = table.get(ident)
            if item is None:
                continue  # helper inline do runtime, não é função guest
            matched += 1
            entry, symbol = item
            count = arcs if arcs > 0 else (1 if first_run > 0 else 0)
            old = counts.get(entry, (symbol, 0))[1]
            counts[entry] = (symbol, max(old, count))
        if matched < len(table):
            # Uma função guest de um shard que foi compilado sempre aparece no
            # .gcda; se não casou, o .gcda é de outro C++ (outro shard/geração).
            notes.append(
                f"{path.name}: {len(table) - matched} de {len(table)} "
                "função(ões) do shard sem registro no .gcda")
    return counts, notes


# --- modo gcov (.gcno + .gcda) ----------------------------------------------

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
            notes.append(f"{gcda.name}: sem {gcno}")
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
            notes.append(f"{gcda.name}: gcov falhou: {process.stderr.strip()}")
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
                        help="diretório com os .gcda da coleta (nomes com '#')")
    parser.add_argument("--generated-dir", type=Path,
                        help="C++ gerado que foi compilado na coleta (modo gcda)")
    parser.add_argument("--build-dir", type=Path,
                        help="build instrumentado, com os .gcno (modo gcov)")
    parser.add_argument("--mode", choices=("auto", "gcov", "gcda"), default="auto")
    parser.add_argument("--gcov", default=DEFAULT_GCOV)
    parser.add_argument("--min-count", type=int, default=1,
                        help="contagem mínima para ser quente (padrão 1)")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    gcda_files = sorted(args.gcda_dir.glob("*.gcda"))
    if not any(SHARD_RE.search(path.name) for path in gcda_files):
        parser.error(f"nenhum .gcda de shard em {args.gcda_dir}")
    mode = args.mode
    if mode == "auto":
        has_notes = args.build_dir is not None and any(
            gcno_for(path, args.build_dir).is_file()
            for path in gcda_files if SHARD_RE.search(path.name))
        mode = "gcov" if has_notes else "gcda"
    if mode == "gcov":
        if args.build_dir is None:
            parser.error("--mode gcov precisa de --build-dir")
        counts, notes = counts_from_gcov(gcda_files, args.build_dir, args.gcov)
    else:
        if args.generated_dir is None:
            parser.error("sem .gcno: o modo gcda precisa de --generated-dir")
        counts, notes = counts_from_gcda(gcda_files, args.generated_dir)
    for note in notes:
        print("aviso:", note, file=sys.stderr)
    if not counts:
        raise SystemExit("nenhuma função guest casou com a coleta")
    hot = sorted(
        (entry for entry, (_, count) in counts.items() if count >= args.min_count),
        key=lambda entry: (-counts[entry][1], entry),
    )
    args.output.write_text(
        "".join(f"{entry:08X}  # {counts[entry][0]} {counts[entry][1]}\n"
                for entry in hot),
        encoding="utf-8",
    )
    print(f"modo {mode}: {len(counts)} função(ões) guest na coleta, "
          f"{len(hot)} quente(s) (contagem >= {args.min_count})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
