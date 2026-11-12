#!/usr/bin/env python3
"""Compiles the deko3d backend shaders (GLSL -> DKSH with devkitPro's uam)
and writes the pack the game reads at sdmc:/switch/oot3dre/shadercache/deko3d.pack.

Inputs (any combination, repeatable):
  --src DIR     .src (PGS1) files from the GL shader cache: each holds the
                vertex and the fragment already translated to GL 4.3 (the same text
                the deko3d backend compiles);
  --glsl DIR    texts the game recorded because they were missing from the pack
                (logs/deko3d_shaders/{vert,frag}_<key>.glsl);
  --pack FILE   a previous pack (its entries are kept, except the ones of a
                text above compiled in the other pass-through mode).

  --keep-tev-passthrough   compiles every text as it is (deko3d_reference.pack,
                the config/deko3d_pack A/B). By default a fragment stage's
                pass-through TEV stages are compiled as copies (exact; see
                skip_tev_passthrough), each such entry has flag 1, and so has
                the header when any entry does. Keys are always the
                unmodified texts'.
  --lighting-variants TOOL   also builds the toon and per-pixel variants of every
                fragment text (config/lighting, gfx_deko3d_lighting.h) with TOOL
                (oot3d_deko3d_lighting_variants: the console's own mutation) and
                compiles them like the other texts.

Stages whose per-warp scratch exceeds the console queue's (8 KiB,
kPerWarpScratchBytes) are reported and left out: the game would refuse them.

The format and the key (FNV-1a 64 of the stage byte + text) are in
runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_shader_pack.h.

  tools/oot3d/deko3d_shader_pack.py --src shadercache --glsl logs/deko3d_shaders \\
      --pack deko3d.pack -o deko3d.pack [--uam /opt/devkitpro/tools/bin/uam] [-j 8]
"""
import argparse
import concurrent.futures
import os
import re
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
PER_WARP_SCRATCH_LIMIT = 8 << 10   # gfx_deko3d_shader_pack.h kPerWarpScratchBytes
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
    """Entries by key: (stage, DKSH, flags)."""
    data = path.read_bytes()
    magic, version, count, _ = struct.unpack_from("<IIII", data, 0)
    if magic != PACK_MAGIC or version != PACK_VERSION:
        raise SystemExit(f"{path}: not a DKP1 v1 pack")
    entries = {}
    for index in range(count):
        key, offset, size, stage, flags = struct.unpack_from("<QIIII", data, 16 + 24 * index)
        entries[key] = (stage, data[offset:offset + size], flags)
    return entries


def pack_flags(entries: dict) -> int:
    flags = 0
    for _, _, entry_flags in entries.values():
        flags |= entry_flags
    return flags


def write_pack(path: Path, entries: dict) -> None:
    keys = sorted(entries)
    header = struct.pack("<IIII", PACK_MAGIC, PACK_VERSION, len(keys), pack_flags(entries))
    offset = len(header) + 24 * len(keys)
    table = bytearray()
    blobs = bytearray()
    for key in keys:
        stage, blob, flags = entries[key]
        pad = (-(offset + len(blobs))) % 256
        blobs += b"\0" * pad
        table += struct.pack("<QIIII", key, offset + len(blobs), len(blob), stage, flags)
        blobs += blob
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(header + table + blobs)
    tmp.replace(path)


def dksh_scratch(blob: bytes):
    """Per-warp scratch of a DKSH's first program (DkshPerWarpScratchBytes)."""
    if len(blob) < 24:
        return None
    programs_off, programs = struct.unpack_from("<II", blob, 16)
    if programs == 0 or programs_off + 24 > len(blob):
        return None
    return struct.unpack_from("<I", blob, programs_off + 20)[0]


# ---- TEV pass-through stages -------------------------------------------------
# The fragment generator (oot3d_native_pica_fragment_shader_gen.cpp) writes all
# six combiner stages; each rounds its colour and alpha to 8 bits
# (byteround3/byteround1: four floor per stage, F2F on Maxwell). A stage N >= 1
# whose colour (or alpha) is exactly (`precise` absent in texts older than
# lighting3, still in the reference pack)
#     precise vec3 color_output_N = byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)));
#     precise float alpha_output_N = byteround1(clamp(combiner_output.a, 0.0, 1.0));
# with scale 1 (`color_output_N * 1.0` in its combiner_output line) returns its
# input unchanged: that input is the previous stage's 8-bit result times 1, 2
# or 4, clamped to [0, 1], and rounding and clamping such a value again gives
# the same float (tests/switch/test_tev_passthrough.py proves it in binary32
# for every byte, scale, division and reciprocal form, fused or not). The pack
# compiles such a channel as a plain copy; the binary stays under the key of
# the unmodified text, so the runtime lookup, the lighting variants and the
# texts consoles record as missing are unchanged.
# The fork author's light uber shader skips trailing pass-through stages by the
# same rule: TriAevum-Switch commit 6884c113 by Gustavo Henrique Rocha
# (origin/deko3d, gfx_deko3d uber TEV; 4096 GPU cases, 0 differences),
# licence as the repository (LICENSE).
# Stage 0 is never rewritten: its "previous" source stands for another input
# (oot3d_native_pica_fragment_shader_gen.cpp, stage == 0 source remap).
PACK_FLAG_TEV_PASSTHROUGH_SKIP = 1   # header word 3 (gfx_deko3d_shader_pack.h kShaderPackFlagTevPassthroughSkip)

# The two byteround definitions the generator has written (TevExpressions::
# ByteRoundHelpers now, the unqualified one before): the proof covers both.
_BYTEROUND_HELPERS = (
    b"vec3 byteround3(vec3 value) { precise vec3 scaled=value*255.0; precise vec3 biased=scaled+0.5; "
    b"return floor(biased)/255.0; }\n"
    b"float byteround1(float value) { precise float scaled=value*255.0; precise float biased=scaled+0.5; "
    b"return floor(biased)/255.0; }\n",
    b"vec3 byteround3(vec3 value) { return floor(value * 255.0 + 0.5) / 255.0; }\n"
    b"float byteround1(float value) { return floor(value * 255.0 + 0.5) / 255.0; }\n",
)
_TEV_COLOR = re.compile(r"(?P<indent>[ \t]*)(?P<q>(?:precise )?)vec3 color_output_(?P<stage>\d+) = "
                        r"byteround3\(clamp\((?P<expr>.*), vec3\(0\.0\), vec3\(1\.0\)\)\);")
_TEV_ALPHA = re.compile(r"(?P<indent>[ \t]*)(?P<q>(?:precise )?)float alpha_output_(?P<stage>\d+) = "
                        r"byteround1\(clamp\((?P<expr>.*), 0\.0, 1\.0\)\);")
_TEV_OUTPUT = re.compile(r"(?P<indent>[ \t]*)combiner_output = vec4\(clamp\(color_output_(?P<stage>\d+) \* "
                         r"(?P<cs>[124])\.0, vec3\(0\.0\), vec3\(1\.0\)\), clamp\(alpha_output_(?P=stage) \* "
                         r"(?P<as>[124])\.0, 0\.0, 1\.0\)\);")
_TEV_BUFFER = re.compile(r"[ \t]*(combiner_buffer = next_combiner_buffer;|"
                         r"next_combiner_buffer\.rgb = combiner_output\.rgb;|"
                         r"next_combiner_buffer\.a = combiner_output\.a;)")


def _single_call(expr: str) -> bool:
    """True if no parenthesis of expr closes one it did not open (so the
    generator's byteround(clamp(expr, ...)) really wraps the whole stage)."""
    depth = 0
    for char in expr:
        depth += char == "("
        depth -= char == ")"
        if depth < 0:
            return False
    return depth == 0


def skip_tev_passthrough(text: bytes):
    """The text with every pass-through colour/alpha of a stage >= 1 written as
    a copy, and (colour channels, alpha channels, whole stages) rewritten.

    Line by line: `rgb_ok`/`alpha_ok` say combiner_output's channel holds a
    stage's 8-bit result times 1, 2 or 4, clamped (it was last written by a
    generator combiner_output line whose operands were byteround outputs or
    copies of such a channel). Only the generator's buffer lines may follow
    it; any other line clears both, so nothing a lighting or toon variant
    inserts is ever trusted. Texts without one of the generator's byteround
    definitions (each exactly once) are returned unchanged."""
    if sum(text.count(helper) for helper in _BYTEROUND_HELPERS) != 1 or text.count(b"vec3 byteround3(") != 1 \
            or text.count(b"float byteround1(") != 1:
        return text, (0, 0, 0)
    lines = text.decode("latin-1").split("\n")
    rgb_ok = alpha_ok = False
    color = alpha = None   # (stage, line index, pass-through, qualifier) of the stage being read
    colors = alphas = stages = 0
    for index, line in enumerate(lines):
        match = _TEV_COLOR.fullmatch(line)
        if match and _single_call(match["expr"]):
            stage = int(match["stage"])
            color = (stage, index, stage >= 1 and rgb_ok and match["expr"] == "combiner_output.rgb", match["q"])
            alpha = None
            continue
        match = _TEV_ALPHA.fullmatch(line)
        if match and _single_call(match["expr"]) and color is not None and color[1] == index - 1 \
                and int(match["stage"]) == color[0]:
            stage = int(match["stage"])
            alpha = (stage, index, stage >= 1 and alpha_ok and match["expr"] == "combiner_output.a", match["q"])
            continue
        match = _TEV_OUTPUT.fullmatch(line)
        if match and color is not None and alpha is not None and alpha[1] == index - 1 \
                and int(match["stage"]) == color[0]:
            stage = color[0]
            indent = match["indent"]
            color_copy = color[2] and match["cs"] == "1"
            alpha_copy = alpha[2] and match["as"] == "1"
            # The copies are not `precise`: a precise copy would make the
            # earlier stages' clamps exact through combiner_output, and uam
            # then stops folding them into the multiply's saturate (two
            # FMNMX per channel instead; measured +24 instructions on some
            # texts). Every transformation left to it here is exact anyway
            # (x * 1.0, clamp -> .sat, powers of two times 1/255).
            if color_copy:
                lines[color[1]] = f"{indent}vec3 color_output_{stage} = combiner_output.rgb;"
                colors += 1
            if alpha_copy:
                lines[alpha[1]] = f"{indent}float alpha_output_{stage} = combiner_output.a;"
                alphas += 1
            stages += color_copy and alpha_copy
            if color_copy or alpha_copy:
                rgb = f"color_output_{stage}" if color_copy else \
                    f"clamp(color_output_{stage} * {match['cs']}.0, vec3(0.0), vec3(1.0))"
                a = f"alpha_output_{stage}" if alpha_copy else \
                    f"clamp(alpha_output_{stage} * {match['as']}.0, 0.0, 1.0)"
                lines[index] = f"{indent}combiner_output = vec4({rgb}, {a});"
            rgb_ok = alpha_ok = True
            color = alpha = None
            continue
        if _TEV_BUFFER.fullmatch(line) and color is None:
            continue
        rgb_ok = alpha_ok = False
        color = alpha = None
    return "\n".join(lines).encode("latin-1"), (colors, alphas, stages)


def lighting_variants(tool: str, texts: dict) -> dict:
    """Runs the variant tool over every fragment text; returns the variants by key."""
    with tempfile.TemporaryDirectory() as work:
        source = Path(work) / "in"
        output = Path(work) / "out"
        source.mkdir()
        for key, (stage, text) in texts.items():
            if stage == FRAGMENT:
                (source / f"frag_{key:016x}.glsl").write_bytes(text)
        result = subprocess.run([tool, str(source), str(output)], capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"{tool} failed: {(result.stdout + result.stderr).strip()[:400]}")
        summary = [line for line in result.stdout.splitlines() if line.startswith("lighting variants:")]
        for line in result.stdout.splitlines():
            if line.startswith("unsupported "):
                print(f"lighting: {line}", file=sys.stderr)
        variants = {}
        for path in sorted(output.glob("frag_*.glsl")):
            text = path.read_bytes()
            variants[shader_text_key(FRAGMENT, text)] = (FRAGMENT, text)
        print(summary[-1] if summary else f"lighting variants: {len(variants)}")
        return variants


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
            return None, "uam did not produce DKSH"
        return blob, ""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", action="append", default=[], type=Path)
    parser.add_argument("--glsl", action="append", default=[], type=Path)
    parser.add_argument("--pack", action="append", default=[], type=Path)
    parser.add_argument("--lighting-variants", metavar="TOOL")
    parser.add_argument("--keep-tev-passthrough", action="store_true",
                        help="compile the texts unmodified (the reference pack)")
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--uam", default="/opt/devkitpro/tools/bin/uam")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--selftest", action="store_true", help="checks the key against the C++ test value")
    args = parser.parse_args()

    if args.selftest:
        # Same vector as oot3d_deko3d_backend_tests (TestShaderPack).
        assert shader_text_key(FRAGMENT, b"abc") == 0x593522965D44E8B2, hex(shader_text_key(FRAGMENT, b"abc"))
        print("deko3d_shader_pack: selftest ok")
        return 0
    if args.output is None:
        parser.error("-o is required")

    entries = {}
    for pack in args.pack:
        if pack.exists():
            entries.update(read_pack(pack))
    texts = {}
    for directory in args.src:
        for path in sorted(directory.glob("*.src")):
            pair = read_pgs1(path)
            if pair is None:
                print(f"warning: {path} is not PGS1", file=sys.stderr)
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
                print(f"warning: {path}: name key differs from text ({key:016x})", file=sys.stderr)
            texts[key] = (stage, text)
    if args.lighting_variants:
        variants = lighting_variants(args.lighting_variants, texts)
        texts.update(variants)
    # What uam compiles for each key, and the entry flags that gives.
    compiled = {}
    rewritten = [0, 0, 0, 0]   # texts, colour channels, alpha channels, whole stages
    for key, (stage, text) in texts.items():
        source, flags = text, 0
        if stage == FRAGMENT and not args.keep_tev_passthrough:
            source, (colors, alphas, stages) = skip_tev_passthrough(text)
            if source != text:
                flags = PACK_FLAG_TEV_PASSTHROUGH_SKIP
                rewritten = [rewritten[0] + 1, rewritten[1] + colors, rewritten[2] + alphas, rewritten[3] + stages]
        compiled[key] = (stage, source, flags)
    fragments = sum(1 for stage, _ in texts.values() if stage == FRAGMENT)
    if fragments and args.keep_tev_passthrough:
        print(f"tev pass-through: kept in all {fragments} fragment texts (reference pack)")
    elif fragments:
        print(f"tev pass-through: skipped in {rewritten[0]} of {fragments} fragment texts "
              f"({rewritten[1]} colour and {rewritten[2]} alpha channels, {rewritten[3]} whole stages)")
    pending = {key: value for key, value in compiled.items()
               if key not in entries or entries[key][2] != value[2]}
    failed = 0
    scratch = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = {pool.submit(compile_stage, args.uam, stage, source): (key, stage, flags)
                   for key, (stage, source, flags) in pending.items()}
        for future in concurrent.futures.as_completed(futures):
            key, stage, flags = futures[future]
            blob, error = future.result()
            if blob is None:
                # A previous entry of the key (the other pass-through mode,
                # equally exact) stays.
                failed += 1
                print(f"FAILED {STAGE_NAMES[stage]} {key:016x}: {error[:400]}", file=sys.stderr)
                continue
            need = dksh_scratch(blob)
            if need is None or need > PER_WARP_SCRATCH_LIMIT:
                failed += 1
                print(f"FAILED {STAGE_NAMES[stage]} {key:016x}: needs {need} bytes of scratch per warp "
                      f"(limit {PER_WARP_SCRATCH_LIMIT})", file=sys.stderr)
                continue
            scratch.append(need)
            entries[key] = (stage, blob, flags)
    write_pack(args.output, entries)
    vertex = sum(1 for stage, _, _ in entries.values() if stage == VERTEX)
    flagged = sum(1 for _, _, flags in entries.values() if flags & PACK_FLAG_TEV_PASSTHROUGH_SKIP)
    print(f"{args.output}: {len(entries)} shaders ({vertex} vertex, {len(entries) - vertex} fragment), "
          f"{len(pending) - failed} compiled now, {failed} failures, {args.output.stat().st_size // 1024} KiB, "
          f"flags {pack_flags(entries)} ({flagged} with pass-through stages skipped)")
    if scratch:
        print(f"scratch per warp of the stages compiled now: max {max(scratch)} bytes, "
              f"{sum(1 for need in scratch if need > 0)} use some (limit {PER_WARP_SCRATCH_LIMIT})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
