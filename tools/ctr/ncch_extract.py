#!/usr/bin/env python3
"""Extrai exheader.bin, code.bin (descomprimido), romfs.bin e exefs/{icon,banner,logo}
de um jogo de 3DS.

Aceita imagem de cartão (NCSD: .3ds/.cci, usa a partição 0) ou um NCCH solto
(.cxi). O conteúdo precisa estar sem criptografia (flag NoCrypto, como sai do
GodMode9 com "Decrypt"); CIA ainda não é aceito.

  ncch_extract.py <jogo.3ds> <pasta de saída>
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

MEDIA_UNIT = 0x200


class ExtractError(Exception):
    pass


def blz_decompress(data: bytes) -> bytes:
    """Descompressão "backwards LZ" do .code do ExeFS."""
    buffer_top, extra = struct.unpack_from("<II", data, len(data) - 8)
    out = bytearray(data) + bytes(extra)
    header_size, compressed_size = buffer_top >> 24, buffer_top & 0xFFFFFF
    source = len(data) - header_size
    destination = len(out)
    limit = len(data) - compressed_size
    while source > limit:
        source -= 1
        flags = out[source]
        for _ in range(8):
            if source <= limit:
                break
            if flags & 0x80:
                source -= 2
                value = out[source] | out[source + 1] << 8
                count = ((value >> 12) & 0xF) + 3
                distance = (value & 0xFFF) + 3
                for _ in range(count):
                    destination -= 1
                    out[destination] = out[destination + distance]
            else:
                source -= 1
                destination -= 1
                out[destination] = out[source]
            flags = (flags << 1) & 0xFF
    return bytes(out)


def ncch_offset(image) -> int:
    image.seek(0)
    header = image.read(0x200)
    if header[0x100:0x104] == b"NCSD":
        return struct.unpack_from("<I", header, 0x120)[0] * MEDIA_UNIT
    if header[0x100:0x104] == b"NCCH":
        return 0
    raise ExtractError("não é NCSD (.3ds/.cci) nem NCCH (.cxi); CIA ainda não é aceito")


# Seções do ExeFS que o jogo pode abrir pelo archive SelfNCCH (Azahar
# file_sys/archive_selfncch.cpp: icon, banner e logo; o .code não).
EXEFS_SECTIONS = (b"icon", b"banner", b"logo")


def extract(source: Path, output: Path) -> dict[str, object]:
    output.mkdir(parents=True, exist_ok=True)
    with source.open("rb") as image:
        base = ncch_offset(image)
        image.seek(base)
        header = image.read(0x200)
        if header[0x100:0x104] != b"NCCH":
            raise ExtractError("partição 0 sem cabeçalho NCCH")
        if not header[0x18F] & 0x4:
            raise ExtractError("conteúdo criptografado: gere uma cópia decriptada (GodMode9)")
        exefs_offset = struct.unpack_from("<I", header, 0x1A0)[0] * MEDIA_UNIT
        romfs_offset, romfs_size = (value * MEDIA_UNIT for value in struct.unpack_from("<2I", header, 0x1B0))
        image.seek(base + 0x200)
        exheader = image.read(0x800)
        image.seek(base + exefs_offset)
        exefs = image.read(0x200)
        code = None
        sections: dict[str, bytes] = {}
        for index in range(10):
            name, offset, size = struct.unpack_from("<8sII", exefs, index * 16)
            name = name.rstrip(b"\0")
            if name == b".code":
                image.seek(base + exefs_offset + 0x200 + offset)
                code = image.read(size)
            elif name in EXEFS_SECTIONS and size:
                image.seek(base + exefs_offset + 0x200 + offset)
                sections[name.decode()] = image.read(size)
        if code is None:
            raise ExtractError("ExeFS sem .code")
        if exheader[0xD] & 1:
            code = blz_decompress(code)
        (output / "exheader.bin").write_bytes(exheader)
        (output / "code.bin").write_bytes(code)
        # Seções servidas pelo archive SelfNCCH (id 3) do host: <saída>/exefs/<nome>.
        exefs_dir = output / "exefs"
        if sections:
            exefs_dir.mkdir(exist_ok=True)
            for stale in exefs_dir.iterdir():
                if stale.name not in sections:
                    stale.unlink()
            for name, data in sections.items():
                (exefs_dir / name).write_bytes(data)
        image.seek(base + romfs_offset)
        with (output / "romfs.bin").open("wb") as romfs:
            left = romfs_size
            while left:
                chunk = image.read(min(left, 1 << 24))
                if not chunk:
                    raise ExtractError("RomFS truncada")
                romfs.write(chunk)
                left -= len(chunk)
    return {
        "product": header[0x150:0x160].rstrip(b"\0").decode(errors="replace"),
        "program_id": struct.unpack_from("<Q", header, 0x118)[0],
        "code_size": len(code),
        "romfs_size": romfs_size,
        "exefs_sections": sorted(sections),
    }


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 64
    try:
        info = extract(Path(sys.argv[1]), Path(sys.argv[2]))
    except ExtractError as error:
        print(f"erro: {error}", file=sys.stderr)
        return 1
    print(f"{info['product']} ({info['program_id']:016X}): code.bin {info['code_size']} bytes, "
          f"romfs {info['romfs_size']} bytes -> {sys.argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
