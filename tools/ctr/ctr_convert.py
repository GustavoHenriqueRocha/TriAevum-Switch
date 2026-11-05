#!/usr/bin/env python3
"""Converte um jogo de 3DS (cópia decriptada) em código nativo e roda o boot probe.

  ctr_convert.py <jogo.3ds> --work <pasta> [--jobs 2] [--run] [--vblanks 60]
                 [--complete-gpu-work] [--shaders] [--force]
                 [--window [--window-seconds 20]]

Etapas (cada uma pula se a saída já é mais nova que a entrada, salvo --force):
  1. extract   ncch_extract.py         -> <work>/extract/{exheader,code,romfs}.bin
  2. discover  discover_functions.py   -> <work>/extract/{inventory.csv,aot_program.json,functions.json}
  3. generate  generate_whole_aot_cpp.py -> <work>/cpp (256 shards)
  4. build     build_title_boot_probe.sh -> <work>/build/ctr_title_boot_probe (-O0)
     --run roda o probe: <work>/build/title/{report.json,stderr.log}
     --complete-gpu-work conclui o trabalho da GPU sem desenhar (o jogo não
               fica parado esperando a P3D do primeiro quadro)
  --window     em vez do probe, monta o executável com janela (oot3d_native_game,
               Vulkan, perfil de título pelo program id) com
               build_title_window.sh; com --run roda --window-seconds e grava
               PNGs do quadro apresentado em <work>/shots
  --shaders    extrai a RomFS e tira os registradores PICA dos materiais CGFX
               (<work>/shaders/regs.txt + .luz), quando o jogo usa CGFX.

Nada é específico de um título: o perfil de título (ctr_title_profile.h) vem
do program id do ExHeader. A descoberta precisa do capstone 5.0.7 no Python
que roda este script.
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
A32 = REPO / "tools/oot3d/native_a32_runtime"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import ncch_extract  # noqa: E402


def fresh(outputs: list[Path], inputs: list[Path]) -> bool:
    if not all(path.exists() for path in outputs):
        return False
    newest_input = max(path.stat().st_mtime for path in inputs)
    return min(path.stat().st_mtime for path in outputs) >= newest_input


def run(step: str, command: list[str], **kwargs) -> None:
    print(f"== {step}", flush=True)
    result = subprocess.run(["nice", *map(str, command)], **kwargs)
    if result.returncode:
        sys.exit(f"{step}: falhou (código {result.returncode})")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rom", type=Path)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--vblanks", type=int, default=60)
    parser.add_argument("--complete-gpu-work", action="store_true")
    parser.add_argument("--shaders", action="store_true")
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--window", action="store_true")
    parser.add_argument("--window-seconds", type=int, default=20)
    args = parser.parse_args()
    if importlib.util.find_spec("capstone") is None:
        sys.exit("a descoberta precisa do capstone (pip install capstone==5.0.7)")

    extract, cpp, build = args.work / "extract", args.work / "cpp", args.work / "build"
    rom_files = [extract / name for name in ("exheader.bin", "code.bin", "romfs.bin")]
    if args.force or not fresh(rom_files, [args.rom]):
        print("== extract", flush=True)
        try:
            info = ncch_extract.extract(args.rom, extract)
        except ncch_extract.ExtractError as error:
            sys.exit(f"extract: {error}")
        print(f"{info['product']} ({info['program_id']:016X}): code.bin {info['code_size']} bytes")
    exheader, code, romfs = rom_files

    program, selection = extract / "aot_program.json", extract / "functions.json"
    discovery_inputs = [code, exheader, A32 / "discover_functions.py", A32 / "whole_aot_program.py"]
    if args.force or not fresh([program, selection], discovery_inputs):
        run("discover", [sys.executable, "-E", "-s", A32 / "discover_functions.py",
                         "--code", code, "--exheader", exheader,
                         "--output", extract / "inventory.csv", "--profile", "title",
                         "--program-output", program, "--selection-output", selection])

    manifest = cpp / "oot3d_whole_aot_generated.cpp"
    if args.force or not fresh([manifest], [program, selection, A32 / "whole_aot_cpp.py"]):
        run("generate", [sys.executable, "-E", "-s", A32 / "generate_whole_aot_cpp.py",
                         "--program", program, "--selection", selection, "--code", code,
                         "--output", cpp, "--shards", "256"])

    if args.window:
        command = [REPO / "tools/ctr/build_title_window.sh", "--work", args.work,
                   "--jobs", str(args.jobs), "--seconds", str(args.window_seconds)]
        command += ["--run"] if args.run else []
        run("build janela" + (" + rodar" if args.run else ""), command)
        return 0

    command = [REPO / "tools/ctr/build_title_boot_probe.sh", "--shards", cpp,
               "--code-bin", code, "--exheader", exheader, "--romfs", romfs,
               "--build", build, "--jobs", str(args.jobs), "--vblanks", str(args.vblanks)]
    command += ["--run"] if args.run else []
    command += ["--complete-gpu-work"] if args.complete_gpu_work else []
    run("build" + (" + boot probe" if args.run else ""), command)

    if args.shaders:
        files, shaders = args.work / "romfs", args.work / "shaders"
        if args.force or not (files.exists() and fresh([files], [romfs])):
            run("romfs", [sys.executable, "-I", REPO / "tools/oot3d/romfs_extract.py", romfs, files])
        shaders.mkdir(parents=True, exist_ok=True)
        run("shaders", [sys.executable, "-I", REPO / "tools/oot3d/cgfx_tev_registers.py",
                        files, shaders / "regs.txt", "--census"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
