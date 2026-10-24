#!/usr/bin/env python3
"""Compila os shaders do backend deko3d (GLSL -> DKSH com o uam do devkitPro)
e grava o pacote que o jogo lê em sdmc:/switch/oot3dre/shadercache/deko3d.pack.

Entradas (qualquer combinação, repetíveis):
  --src DIR     arquivos .src (PGS1) do cache de shaders do GL: cada um traz o
                vertex e o fragment já traduzidos para GL 4.3 (o mesmo texto que
                o backend deko3d compila);
  --glsl DIR    textos que o jogo registrou por faltar no pacote
                (logs/deko3d_shaders/{vert,frag}_<chave>.glsl);
  --pack FILE   um pacote anterior (as entradas dele são mantidas).

O formato e a chave (FNV-1a 64 do byte do estágio + texto) estão em
runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_shader_pack.h.

  tools/oot3d/deko3d_shader_pack.py --src shadercache --glsl logs/deko3d_shaders \\
      --pack deko3d.pack -o deko3d.pack [--uam /opt/devkitpro/tools/bin/uam] [-j 8]
"""
import argparse
import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

PACK_MAGIC = 0x31504B44   # "DKP1"
PACK_VERSION = 1
DKSH_MAGIC = 0x48534B44   # "DKSH"
PGS1_MAGIC = 0x31534750   # "PGS1"
VERTEX, FRAGMENT = 0, 1
STAGE_NAMES = {VERTEX: "vert", FRAGMENT: "frag"}


def shader_text_key(stage: int, text: bytes) -> int:
    value = 1469598103934665603
    value ^= stage
    value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for byte in text:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return value


def read_pgs1(path: Path):
    data = path.read_bytes()
    if len(data) < 12:
        return None
    magic, vsize, fsize = struct.unpack_from("<III", data, 0)
    if magic != PGS1_MAGIC or 12 + vsize + fsize > len(data):
        return None
    return data[12:12 + vsize], data[12 + vsize:12 + vsize + fsize]


def read_pack(path: Path) -> dict:
    data = path.read_bytes()
    magic, version, count, _ = struct.unpack_from("<IIII", data, 0)
    if magic != PACK_MAGIC or version != PACK_VERSION:
        raise SystemExit(f"{path}: não é um pacote DKP1 v1")
    entries = {}
    for index in range(count):
        key, offset, size, stage, _ = struct.unpack_from("<QIIII", data, 16 + 24 * index)
        entries[key] = (stage, data[offset:offset + size])
    return entries


def write_pack(path: Path, entries: dict) -> None:
    keys = sorted(entries)
    header = struct.pack("<IIII", PACK_MAGIC, PACK_VERSION, len(keys), 0)
    offset = len(header) + 24 * len(keys)
    table = bytearray()
    blobs = bytearray()
    for key in keys:
        stage, blob = entries[key]
        pad = (-(offset + len(blobs))) % 256
        blobs += b"\0" * pad
        table += struct.pack("<QIIII", key, offset + len(blobs), len(blob), stage, 0)
        blobs += blob
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(header + table + blobs)
    tmp.replace(path)


def compile_stage(uam: str, stage: int, text: bytes):
    with tempfile.TemporaryDirectory() as work:
        source = Path(work) / f"s.{STAGE_NAMES[stage]}.glsl"
        output = Path(work) / "s.dksh"
        source.write_bytes(text)
        result = subprocess.run([uam, "-s", STAGE_NAMES[stage], "-o", str(output), str(source)],
                                capture_output=True, text=True)
        if result.returncode != 0 or not output.exists():
            return None, (result.stdout + result.stderr).strip()
        blob = output.read_bytes()
        if len(blob) < 4 or struct.unpack_from("<I", blob, 0)[0] != DKSH_MAGIC:
            return None, "uam não gerou DKSH"
        return blob, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", action="append", default=[], type=Path)
    parser.add_argument("--glsl", action="append", default=[], type=Path)
    parser.add_argument("--pack", action="append", default=[], type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--uam", default="/opt/devkitpro/tools/bin/uam")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--selftest", action="store_true", help="confere a chave com o valor do teste C++")
    args = parser.parse_args()

    if args.selftest:
        # Mesmo vetor de oot3d_deko3d_backend_tests (TestShaderPack).
        assert shader_text_key(FRAGMENT, b"abc") == 0x593522965D44E8B2, hex(shader_text_key(FRAGMENT, b"abc"))
        print("deko3d_shader_pack: selftest ok")
        return 0
    if args.output is None:
        parser.error("-o é obrigatório")

    entries = {}
    for pack in args.pack:
        if pack.exists():
            entries.update(read_pack(pack))
    texts = {}
    for directory in args.src:
        for path in sorted(directory.glob("*.src")):
            pair = read_pgs1(path)
            if pair is None:
                print(f"aviso: {path} não é PGS1", file=sys.stderr)
                continue
            for stage, text in ((VERTEX, pair[0]), (FRAGMENT, pair[1])):
                texts[shader_text_key(stage, text)] = (stage, text)
    for directory in args.glsl:
        for path in sorted(directory.glob("*.glsl")):
            name = path.stem
            stage = VERTEX if name.startswith("vert_") else FRAGMENT if name.startswith("frag_") else None
            if stage is None:
                continue
            text = path.read_bytes()
            key = shader_text_key(stage, text)
            if name.endswith(f"{key:016x}") is False:
                print(f"aviso: {path}: chave do nome difere do texto ({key:016x})", file=sys.stderr)
            texts[key] = (stage, text)
    pending = {key: value for key, value in texts.items() if key not in entries}
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(compile_stage, args.uam, stage, text): (key, stage)
                   for key, (stage, text) in pending.items()}
        for future in concurrent.futures.as_completed(futures):
            key, stage = futures[future]
            blob, error = future.result()
            if blob is None:
                failed += 1
                print(f"FALHA {STAGE_NAMES[stage]} {key:016x}: {error[:400]}", file=sys.stderr)
                continue
            entries[key] = (stage, blob)
    write_pack(args.output, entries)
    vertex = sum(1 for stage, _ in entries.values() if stage == VERTEX)
    print(f"{args.output}: {len(entries)} shaders ({vertex} vertex, {len(entries) - vertex} fragment), "
          f"{len(pending) - failed} compilados agora, {failed} falhas, {args.output.stat().st_size // 1024} KiB")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
