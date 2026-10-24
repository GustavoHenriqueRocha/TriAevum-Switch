#!/usr/bin/env python3
"""Compila os shaders do scanout do deko3d com o uam e gera o cabeçalho
runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_scanout_dksh.h.

Uso: python3 tools/oot3d/deko3d_scanout/gen.py [--uam /opt/devkitpro/tools/bin/uam]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = ROOT / "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_scanout_dksh.h"


def compile_stage(uam, stage, source, workdir):
    out = Path(workdir) / (source.stem + ".dksh")
    subprocess.run([uam, "-s", stage, "-o", str(out), str(source)], check=True)
    return out.read_bytes()


def array(name, data):
    lines = [f"inline constexpr uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--uam", default="/opt/devkitpro/tools/bin/uam")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as workdir:
        vsh = compile_stage(args.uam, "vert", HERE / "scanout_vsh.glsl", workdir)
        fsh = compile_stage(args.uam, "frag", HERE / "scanout_fsh.glsl", workdir)
    text = "\n".join([
        "#pragma once",
        "// Gerado por tools/oot3d/deko3d_scanout/gen.py (uam) a partir de",
        "// tools/oot3d/deko3d_scanout/scanout_{vsh,fsh}.glsl. Não editar à mão.",
        "#include <cstdint>",
        "",
        "namespace Fast::Deko3d {",
        array("kScanoutVertexDksh", vsh),
        array("kScanoutFragmentDksh", fsh),
        "} // namespace Fast::Deko3d",
        "",
    ])
    OUT.write_text(text)
    print(f"{OUT}: vertex {len(vsh)} B, fragment {len(fsh)} B")


if __name__ == "__main__":
    main()
