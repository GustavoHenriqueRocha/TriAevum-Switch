#!/usr/bin/env python3
"""config/deko3d_clear_draw: the constant-colour proof against the GLSL text.

gfx_deko3d_clear_draw.h's EvaluatePicaConstantTev predicts the bytes a
specialized fragment program writes when every TEV input it reads is a
constant, and refuses (ambiguous) when an 8-bit rounding sits near a boundary.
oot3d_deko3d_clear_draw_tests --emit writes random proven programs as GLSL built
with the generator's own expression helpers (TevExpressions) and the generator's
stage epilogue, with the predicted bytes. Here each text runs in binary32
(glsl_eval.py) in every arithmetic variant a GPU compiler may pick: fma fused or
not, mix as x+(y-x)*a or x*(1-a)+y*a, byteround dividing by 255 or multiplying
by its reciprocal. Every variant must store the predicted bytes.

    test_clear_draw_tev.py [path/to/oot3d_deko3d_clear_draw_tests]

Without an argument the binary is looked for in $HOST_BUILD
(default ~/oot3d-work/build-host-tests); missing, the test is skipped (exit 77).
"""
import itertools
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import glsl_eval  # noqa: E402
from glsl_eval import Program, Vec, f32  # noqa: E402

RECIPROCAL_255 = f32(1.0 / 255.0)


def find_binary():
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    root = Path(os.environ.get("HOST_BUILD", Path.home() / "oot3d-work/build-host-tests"))
    for path in root.rglob("oot3d_deko3d_clear_draw_tests"):
        if path.is_file() and os.access(path, os.X_OK):
            return path
    return None


def mix_products(self, x, y, a):
    one_minus = self.binary("-", 1.0, a)
    return self.binary("+", self.binary("*", x, one_minus), self.binary("*", y, a))


def run_case(case, fused, mix_lerp, reciprocal):
    text = case["glsl"]
    if reciprocal:
        text = text.replace("floor(biased)/255.0", f"floor(biased)*{RECIPROCAL_255!r}")
    program = Program(text)
    program.fused_fma = fused
    uniforms = program.globals["fragment_uniforms"]
    uniforms["tev_constants"] = [Vec([f32(v) for v in row]) for row in case["constants"]]
    uniforms["combiner_buffer_color"] = Vec([f32(v) for v in case["buffer"]])
    original = glsl_eval._Interpreter._b_mix
    if not mix_lerp:
        glsl_eval._Interpreter._b_mix = mix_products
    try:
        color = program.call("pica_constant_tev")
    finally:
        glsl_eval._Interpreter._b_mix = original
    # The render target is RGBA8 unorm: round to nearest.
    return [int(round(v * 255.0)) for v in color.c]


def main():
    binary = find_binary()
    if binary is None or not binary.exists():
        print("test_clear_draw_tev: oot3d_deko3d_clear_draw_tests not built; skipped")
        return 77
    with tempfile.TemporaryDirectory() as directory:
        cases_path = Path(directory) / "cases.jsonl"
        subprocess.run([str(binary), "--emit", str(cases_path)], check=True, stdout=subprocess.DEVNULL)
        cases = [json.loads(line) for line in cases_path.read_text().splitlines() if line.strip()]
    if len(cases) < 300:
        print(f"FAIL: only {len(cases)} programs emitted")
        return 1
    failures = 0
    for index, case in enumerate(cases):
        for fused, mix_lerp, reciprocal in itertools.product((True, False), repeat=3):
            got = run_case(case, fused, mix_lerp, reciprocal)
            if got != case["bytes"]:
                failures += 1
                if failures <= 5:
                    print(f"FAIL: program {index} (fused {fused}, lerp mix {mix_lerp}, reciprocal {reciprocal}): "
                          f"GLSL stores {got}, predicted {case['bytes']}")
    if failures:
        print(f"FAIL: {failures} mismatches")
        return 1
    print(f"test_clear_draw_tev: {len(cases)} constant TEV programs x 8 arithmetic variants: "
          "the GLSL stores the predicted bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
