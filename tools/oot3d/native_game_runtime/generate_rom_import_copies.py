#!/usr/bin/env python3
"""Compila a receita COPY da USA do TriAevum em oot3d_rom_import_usa_copies.inc.

A receita (tools/triaevum_release/input_coverage/recipes/adapters/
oot3d-usa-ef210566-copies.json) só tem offsets e tamanhos da origem; a tabela
gerada guarda os mesmos pares como varints LEB128. Nenhum byte do jogo.

Uso: python3 tools/oot3d/native_game_runtime/generate_rom_import_copies.py
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
RECIPE = ROOT / "tools/triaevum_release/input_coverage/recipes/adapters/oot3d-usa-ef210566-copies.json"
RECIPE_SHA256 = "3a7771a365157c6a2a26ce6916a3a3437a15384a98c39c130cd16a98d2dd9519"
OUTPUT = Path(__file__).resolve().parent / "oot3d_rom_import_usa_copies.inc"


def varint(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def main() -> int:
    raw = RECIPE.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if digest != RECIPE_SHA256:
        raise SystemExit(f"receita inesperada {digest}")
    program = json.loads(raw)
    if program.get("format") != "triaevum_input_copy_adapter_v1":
        raise SystemExit("formato de receita inesperado")
    copies = program["copies"]
    written = 0
    stream = bytearray()
    for offset, length in copies:
        if offset < 0 or length <= 0 or offset + length > program["source"]["bytes"]:
            raise SystemExit("operação COPY fora da origem")
        written += length
        stream += varint(offset) + varint(length)
    if written != program["canonical"]["bytes"]:
        raise SystemExit("programa COPY incompleto")
    lines = [
        "// Gerado por generate_rom_import_copies.py; não editar.",
        f"// De {RECIPE.relative_to(ROOT).as_posix()}",
        f"// (sha256 {RECIPE_SHA256}): só offsets e tamanhos da origem, em pares LEB128.",
        f"// Code de origem {program['source']['sha256']} -> canônico {program['canonical']['sha256']}.",
        f"inline constexpr std::size_t kUsaCopyCount = {len(copies)};",
        f"inline constexpr std::size_t kUsaCopyOutputBytes = {program['canonical']['bytes']};",
        "inline constexpr std::uint8_t kUsaCopyProgram[] = {",
    ]
    for start in range(0, len(stream), 32):
        lines.append(",".join(str(b) for b in stream[start:start + 32]) + ",")
    lines.append("};")
    OUTPUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"{OUTPUT.name}: {len(copies)} cópias, {len(stream)} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
