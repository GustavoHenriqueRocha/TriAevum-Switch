#!/usr/bin/env python3
"""Compila os shaders da UI do deko3d (TopScreen e ImGui, pacote 6) com o uam e gera
runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_ui_dksh.h.

Uso: python3 tools/oot3d/deko3d_ui/gen.py [--uam /opt/devkitpro/tools/bin/uam]
"""
import argparse
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "deko3d_scanout"))
from gen import array, compile_stage  # noqa: E402

ROOT = HERE.parents[2]
OUT = ROOT / "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_ui_dksh.h"
STAGES = [
    ("kUiSolidVertexDksh", "vert", "ui_solid_vsh.glsl"),
    ("kUiSolidFragmentDksh", "frag", "ui_solid_fsh.glsl"),
    ("kUiTexturedVertexDksh", "vert", "ui_textured_vsh.glsl"),
    ("kUiTexturedFragmentDksh", "frag", "ui_textured_fsh.glsl"),
]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--uam", default="/opt/devkitpro/tools/bin/uam")
    args = parser.parse_args()
    arrays = []
    with tempfile.TemporaryDirectory() as workdir:
        for name, stage, source in STAGES:
            arrays.append(array(name, compile_stage(args.uam, stage, HERE / source, workdir)))
    OUT.write_text("\n".join([
        "#pragma once",
        "// Gerado por tools/oot3d/deko3d_ui/gen.py (uam) a partir de",
        "// tools/oot3d/deko3d_ui/ui_*.glsl. Não editar à mão.",
        "#include <cstdint>",
        "",
        "namespace Fast::Deko3d {",
        *arrays,
        "} // namespace Fast::Deko3d",
        "",
    ]))
    print(f"{OUT}: {len(STAGES)} estágios")


if __name__ == "__main__":
    main()
