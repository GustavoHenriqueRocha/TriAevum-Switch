#!/usr/bin/env python3
"""The pack tool's TEV pass-through rewrite is exact (tools/oot3d/deko3d_shader_pack.py,
skip_tev_passthrough; SWITCH_PORT.md "Pass-through TEV stages skipped in the pack").

A generated fragment text rounds every combiner stage to 8 bits. A stage N >= 1
whose colour or alpha is byteround(clamp(combiner_output, 0, 1)) with scale 1
is compiled as a plain copy. These tests check:
  - binary32, exhaustively: every byte 0-255 a previous stage can produce, times
    each scale (1, 2, 4) and clamped, comes back unchanged from byteround(clamp()),
    with `/ 255.0` and with the reciprocal uam emits (0x3B808081), the multiply
    and add separate or fused;
  - the rewrite itself: stage 0, scales other than 1 and anything a variant
    inserts between stages are left alone; only TEV lines change;
  - sample texts (tests/switch/data/tev_passthrough/, the boot/title/Kokiri
    profiles, and TEV_PASSTHROUGH_TEXTS=<dir of *.glsl/*.frag> if set): the
    stages of the original and the rewritten text, run by glsl_eval.py on random
    inputs with each byteround form, give the same bits;
  - the pack: entries keep the unmodified text's key, carry flag 1 when
    rewritten (and the header with them), and --keep-tev-passthrough builds the
    reference pack from the unmodified texts.

    python3 tests/switch/test_tev_passthrough.py
"""
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO / "tools/oot3d"))
from glsl_eval import Program, Vec, f32  # noqa: E402
import deko3d_shader_pack as pack  # noqa: E402

SAMPLES = HERE / "data/tev_passthrough"
PROFILES = REPO / "tools/oot3d/native_pica_frontend/profiles"
RECIPROCAL_BITS = 0x3B808081   # fl(1/255): the constant uam multiplies by (fmul32i ... 0x3b808081)


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def from_bits(word):
    return struct.unpack("<f", struct.pack("<I", word))[0]


def clamp01(value):
    return min(max(value, 0.0), 1.0)


def byteround(value, reciprocal, fused):
    """byteround1 as compiled: floor(value * 255 + 0.5), then / 255 or * reciprocal."""
    if fused:
        biased = f32(value * 255.0 + 0.5)   # value * 255 is exact in binary64: one rounding
    else:
        biased = f32(f32(value * 255.0) + 0.5)
    whole = float(int(biased // 1))
    return f32(whole / 255.0) if reciprocal is None else f32(whole * reciprocal)


def pass_through_failures(reciprocal):
    failures = []
    for fused in (False, True):
        for scale in (1.0, 2.0, 4.0):
            for byte in range(256):
                stage_result = f32(byte / 255.0) if reciprocal is None else f32(byte * reciprocal)
                value = clamp01(f32(stage_result * scale))     # the next stage's input
                if bits(byteround(clamp01(value), reciprocal, fused)) != bits(value) or \
                        bits(clamp01(f32(value * 1.0))) != bits(value):
                    failures.append((fused, scale, byte))
    return failures


class ExhaustiveBinary32(unittest.TestCase):
    def test_reciprocal_is_the_correctly_rounded_one(self):
        self.assertEqual(bits(f32(1.0 / 255.0)), RECIPROCAL_BITS)

    def test_division_form(self):
        self.assertEqual(pass_through_failures(None), [])

    def test_reciprocal_form(self):
        self.assertEqual(pass_through_failures(from_bits(RECIPROCAL_BITS)), [])

    def test_a_wrong_reciprocal_would_be_caught(self):
        # The proof depends on the constant: one ulp off, 255 * r != 1.
        self.assertNotEqual(pass_through_failures(from_bits(RECIPROCAL_BITS + 1)), [])
        self.assertNotEqual(pass_through_failures(from_bits(RECIPROCAL_BITS - 1)), [])


HELPERS = pack._BYTEROUND_HELPERS[0].decode()


def stage(n, color, alpha, color_scale=1, alpha_scale=1, buffer_rgb=False, buffer_a=False, precise="precise "):
    lines = [f"    {precise}vec3 color_output_{n} = byteround3(clamp({color}, vec3(0.0), vec3(1.0)));",
             f"    {precise}float alpha_output_{n} = byteround1(clamp({alpha}, 0.0, 1.0));",
             f"    combiner_output = vec4(clamp(color_output_{n} * {color_scale}.0, vec3(0.0), vec3(1.0)), "
             f"clamp(alpha_output_{n} * {alpha_scale}.0, 0.0, 1.0));",
             "    combiner_buffer = next_combiner_buffer;"]
    if buffer_rgb:
        lines.append("    next_combiner_buffer.rgb = combiner_output.rgb;")
    if buffer_a:
        lines.append("    next_combiner_buffer.a = combiner_output.a;")
    return lines


def synthetic(stages, helpers=HELPERS):
    body = [line for lines in stages for line in lines]
    return ("#version 430\n" + helpers + "void main() {\n    vec4 combiner_output = vec4(0.0);\n" +
            "\n".join(body) + "\n    pica_color = combiner_output;\n}\n").encode()


PASS = ("combiner_output.rgb", "combiner_output.a")
FIRST = ("rounded_primary_color.rgb * pica_sample_texture0(pica_texcoord0).rgb",
         "rounded_primary_color.a * pica_sample_texture0(pica_texcoord0).a")


class Rewrite(unittest.TestCase):
    def test_passthrough_stages_become_copies(self):
        text = synthetic([stage(0, *FIRST, color_scale=2), stage(1, *PASS), stage(2, *PASS)])
        new, counts = pack.skip_tev_passthrough(text)
        self.assertEqual(counts, (2, 2, 2))
        self.assertIn(b"    vec3 color_output_1 = combiner_output.rgb;\n", new)
        self.assertIn(b"    float alpha_output_2 = combiner_output.a;\n", new)
        self.assertIn(b"    combiner_output = vec4(color_output_2, alpha_output_2);\n", new)
        self.assertEqual(text.count(b"\n"), new.count(b"\n"))

    def test_stage_zero_is_never_rewritten(self):
        text = synthetic([stage(0, *PASS), stage(1, *PASS)])
        new, counts = pack.skip_tev_passthrough(text)
        # Stage 0's input is not a stage result; stage 1 after it is.
        self.assertIn(b"color_output_0 = byteround3(clamp(combiner_output.rgb", new)
        self.assertEqual(counts, (1, 1, 1))

    def test_scale_other_than_one_stays(self):
        text = synthetic([stage(0, *FIRST), stage(1, *PASS, color_scale=2, alpha_scale=4)])
        self.assertEqual(pack.skip_tev_passthrough(text), (text, (0, 0, 0)))

    def test_one_channel(self):
        text = synthetic([stage(0, *FIRST), stage(1, "combiner_output.rgb * fragment_uniforms.tev_constants[1].rgb",
                                                    "combiner_output.a")])
        new, counts = pack.skip_tev_passthrough(text)
        self.assertEqual(counts, (0, 1, 0))
        self.assertIn(b"combiner_output = vec4(clamp(color_output_1 * 1.0, vec3(0.0), vec3(1.0)), alpha_output_1);",
                      new)

    def test_buffer_lines_keep_the_chain(self):
        text = synthetic([stage(0, *FIRST, buffer_rgb=True, buffer_a=True), stage(1, *PASS)])
        self.assertEqual(pack.skip_tev_passthrough(text)[1], (1, 1, 1))

    def test_an_inserted_line_breaks_the_chain(self):
        chain = [stage(0, *FIRST), ["    combiner_output.rgb = oot3d_apply_toon(combiner_output.rgb);"],
                 stage(1, *PASS), stage(2, *PASS)]
        new, counts = pack.skip_tev_passthrough(synthetic(chain))
        self.assertEqual(counts, (1, 1, 1))   # stage 2 only: stage 1 re-rounds the toon output
        self.assertIn(b"color_output_1 = byteround3(clamp(combiner_output.rgb", new)

    def test_unknown_byteround_definition_leaves_the_text(self):
        helpers = HELPERS.replace("floor(biased)/255.0", "floor(biased)/256.0")
        text = synthetic([stage(0, *FIRST), stage(1, *PASS)], helpers)
        self.assertEqual(pack.skip_tev_passthrough(text), (text, (0, 0, 0)))

    def test_older_unqualified_texts(self):
        text = synthetic([stage(0, *FIRST, precise=""), stage(1, *PASS, precise="")],
                         pack._BYTEROUND_HELPERS[1].decode())
        new, counts = pack.skip_tev_passthrough(text)
        self.assertEqual(counts, (1, 1, 1))
        self.assertIn(b"    vec3 color_output_1 = combiner_output.rgb;", new)


# ---- the stages of real texts in glsl_eval ------------------------------------
TEV_LINE = re.compile(r"[ \t]*((precise )?(vec3 color_output_\d+|float alpha_output_\d+) = .*;|"
                      r"combiner_output = vec4\(.*\);|combiner_buffer = next_combiner_buffer;|"
                      r"next_combiner_buffer\.(rgb|a) = combiner_output\.(rgb|a);)")
CHAIN_START = re.compile(r"[ \t]*(precise )?vec3 color_output_0 = ")
UNIFORMS = re.compile(r"layout\([^)]*\) uniform PicaFragmentUniforms \{.*?\} fragment_uniforms;", re.S)
RECIPROCAL = repr(from_bits(RECIPROCAL_BITS))
# byteround as the text has it, and the forms the compiler may make of it.
FORMS = {
    "text": None,
    "reciprocal": "vec3 byteround3(vec3 value) { precise vec3 b = value * 255.0 + 0.5; return floor(b) * R; }\n"
                  "float byteround1(float value) { precise float b = value * 255.0 + 0.5; return floor(b) * R; }\n",
    "fused": "vec3 byteround3(vec3 value) { return floor(fma(value, vec3(255.0), vec3(0.5))) / 255.0; }\n"
             "float byteround1(float value) { return floor(fma(value, 255.0, 0.5)) / 255.0; }\n",
    "fused reciprocal": "vec3 byteround3(vec3 value) { return floor(fma(value, vec3(255.0), vec3(0.5))) * R; }\n"
                        "float byteround1(float value) { return floor(fma(value, 255.0, 0.5)) * R; }\n",
}


def chains(lines):
    """(first, end) line ranges of every TEV chain."""
    found, index = [], 0
    while index < len(lines):
        if CHAIN_START.match(lines[index]):
            end = index
            while end < len(lines) and TEV_LINE.fullmatch(lines[end]):
                end += 1
            found.append((index, end))
            index = end
        else:
            index += 1
    return found


TRACE_SLOTS = 8


def traced(chain_lines):
    """The chain with each stage's combiner_output saved (tev_trace<k>), so
    every intermediate value is compared, not only the 8-bit end result."""
    lines, slot = [], 0
    for line in chain_lines:
        lines.append(line)
        if line.lstrip().startswith("combiner_output = vec4(") and slot < TRACE_SLOTS:
            lines.append(f"    tev_trace{slot} = combiner_output;")
            slot += 1
    return lines


def harness(text, chain_lines, form):
    uniforms = UNIFORMS.search(text).group(0)
    helpers = next(h.decode() for h in pack._BYTEROUND_HELPERS if h.decode() in text) if FORMS[form] is None \
        else FORMS[form].replace("R", RECIPROCAL)
    return (uniforms + "\n" + helpers +
            "vec2 pica_texcoord0;\nvec2 pica_texcoord1;\nvec2 pica_texcoord2;\n"
            "vec4 tev_texture0;\nvec4 tev_texture1;\nvec4 tev_texture2;\nvec4 tev_output;\nvec4 tev_buffer;\n" +
            "".join(f"vec4 tev_trace{slot};\n" for slot in range(TRACE_SLOTS)) +
            "vec4 pica_sample_texture0(vec2 coord) { return tev_texture0; }\n"
            "vec4 pica_sample_texture1(vec2 coord) { return tev_texture1; }\n"
            "vec4 pica_sample_texture2(vec2 coord) { return tev_texture2; }\n"
            "void tev(vec4 rounded_primary_color, vec4 primary_fragment_color, vec4 secondary_fragment_color) {\n"
            "    vec4 combiner_buffer = vec4(0.0);\n"
            "    vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;\n"
            "    vec4 combiner_output = vec4(0.0);\n" + "\n".join(traced(chain_lines)) + "\n"
            "    tev_output = combiner_output;\n    tev_buffer = next_combiner_buffer;\n}\n")


def random_inputs(rng):
    def byte():
        return f32(rng.randrange(256) / 255.0)

    def unit():
        roll = rng.random()
        return 0.0 if roll < 0.05 else 1.0 if roll < 0.1 else f32(rng.random())

    def vec(make):
        return Vec([make() for _ in range(4)])
    return dict(primary=vec(byte), fragment=vec(unit), secondary=vec(unit),
                textures=[vec(rng.choice((byte, unit))) for _ in range(3)],
                constants=[vec(byte) for _ in range(6)], buffer=vec(byte))


def run(program, inputs):
    uniforms = program.globals["fragment_uniforms"]
    for index, value in enumerate(inputs["constants"]):
        uniforms["tev_constants"][index] = Vec(list(value.c))
    uniforms["combiner_buffer_color"] = Vec(list(inputs["buffer"].c))
    for unit, value in enumerate(inputs["textures"]):
        program.globals[f"tev_texture{unit}"] = Vec(list(value.c))
    program.call("tev", inputs["primary"], inputs["fragment"], inputs["secondary"])
    names = ["tev_output", "tev_buffer"] + [f"tev_trace{slot}" for slot in range(TRACE_SLOTS)]
    return [bits(v) for name in names for v in program.globals[name].c]


def sample_texts():
    texts = {}
    for path in sorted(SAMPLES.glob("*.glsl")):
        texts[path.name] = path.read_bytes()
    for path in sorted(PROFILES.glob("*.json")):
        for entry in json.loads(path.read_text())["shaders"]:
            if entry["stage"] == "fragment":
                texts[f"{path.stem}:{entry['source_id']}"] = entry["source"].encode()
    extra = os.environ.get("TEV_PASSTHROUGH_TEXTS")
    if extra:
        for path in sorted(Path(extra).iterdir()):
            if path.suffix in (".glsl", ".frag"):
                texts[path.name] = path.read_bytes()
    return texts


class SampleTexts(unittest.TestCase):
    INPUTS = int(os.environ.get("TEV_PASSTHROUGH_INPUTS", "12"))

    def test_samples_give_the_same_bits(self):
        rng = random.Random(0x7E5)
        texts = sample_texts()
        cases = {}   # (original chain, rewritten chain, uniform block, helpers) -> name
        rewritten_texts = 0
        for name, text in texts.items():
            new, counts = pack.skip_tev_passthrough(text)
            old_lines, new_lines = text.decode("latin-1").split("\n"), new.decode("latin-1").split("\n")
            self.assertEqual(len(old_lines), len(new_lines), name)
            ranges = chains(old_lines)
            inside = {i for first, end in ranges for i in range(first, end)}
            for i, (a, b) in enumerate(zip(old_lines, new_lines)):
                if i not in inside:
                    self.assertEqual(a, b, f"{name}: line {i + 1} outside the stages changed")
            if new == text:
                continue
            rewritten_texts += 1
            uniforms = UNIFORMS.search(text.decode("latin-1")).group(0)
            for first, end in ranges:
                old_chain, new_chain = tuple(old_lines[first:end]), tuple(new_lines[first:end])
                if old_chain != new_chain:
                    helper = next(h for h in pack._BYTEROUND_HELPERS if h in text)
                    cases.setdefault((old_chain, new_chain, uniforms, helper), name)
        self.assertGreater(rewritten_texts, 100)
        for (old_chain, new_chain, _, _), name in cases.items():
            source = texts[name].decode("latin-1")
            for form in FORMS:
                old = Program(harness(source, old_chain, form))
                new = Program(harness(source, new_chain, form))
                for _ in range(self.INPUTS):
                    inputs = random_inputs(rng)
                    self.assertEqual(run(old, inputs), run(new, inputs), f"{name} ({form})")
        print(f"tev pass-through: {rewritten_texts} sample texts rewritten, {len(cases)} distinct stage chains "
              f"x {len(FORMS)} byteround forms x {self.INPUTS} inputs: same bits", file=sys.stderr)

    def test_the_comparison_catches_a_wrong_copy(self):
        # Control: a stage 1 that rounds its input but doubles it (scale 2) is
        # not a pass-through; written as a copy anyway, the comparison must see it.
        text = synthetic([stage(0, *FIRST), stage(1, *PASS, color_scale=2, alpha_scale=2)])
        source = text.decode().replace("void main", UNIFORM_BLOCK + "\nvoid main")
        lines = source.split("\n")
        (first, end), = chains(lines)
        old_chain = lines[first:end]
        wrong = [line.replace("byteround3(clamp(combiner_output.rgb, vec3(0.0), vec3(1.0)))", "combiner_output.rgb")
                 .replace("clamp(color_output_1 * 2.0, vec3(0.0), vec3(1.0))", "color_output_1")
                 for line in old_chain]
        rng = random.Random(1)
        old, bad = Program(harness(source, old_chain, "text")), Program(harness(source, wrong, "text"))
        differ = sum(run(old, inputs) != run(bad, inputs) for inputs in (random_inputs(rng) for _ in range(16)))
        self.assertGreater(differ, 8)


UNIFORM_BLOCK = ("layout(binding=4, std140) uniform PicaFragmentUniforms {\n    vec4 tev_constants[6];\n"
                 "    vec4 combiner_buffer_color;\n} fragment_uniforms;")


# ---- the pack --------------------------------------------------------------------
FAKE_UAM = r"""#!{python}
# uam stand-in: a DKSH whose code is the GLSL it was given.
import struct, sys
args = sys.argv[1:]
out, source = args[args.index("-o") + 1], args[-1]
text = open(source, "rb").read()
header = struct.pack("<6I", 0x48534B44, 24, 0, len(text), 24, 1)
program = struct.pack("<6I", 0, 0, 8, 0, 0, 0)
open(out, "wb").write(header + program + text)
"""


class Pack(unittest.TestCase):
    def build(self, work, *extra):
        uam = work / "uam"
        uam.write_text(FAKE_UAM.replace("{python}", sys.executable))
        uam.chmod(0o755)
        result = subprocess.run([sys.executable, str(REPO / "tools/oot3d/deko3d_shader_pack.py"), "--glsl",
                                 str(work / "glsl"), "--uam", str(uam), "-j", "2", *extra],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_keys_flags_and_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "glsl").mkdir()
            rewritten = synthetic([stage(0, *FIRST), stage(1, *PASS)])
            plain = synthetic([stage(0, *FIRST), stage(1, *FIRST)])
            keys = {}
            for text in (rewritten, plain):
                key = pack.shader_text_key(pack.FRAGMENT, text)
                keys[key] = text
                (work / "glsl" / f"frag_{key:016x}.glsl").write_bytes(text)
            rewritten_key = pack.shader_text_key(pack.FRAGMENT, rewritten)

            out = self.build(work, "-o", str(work / "deko3d.pack"))
            self.assertIn("tev pass-through: skipped in 1 of 2 fragment texts", out)
            entries = pack.read_pack(work / "deko3d.pack")
            self.assertEqual(set(entries), set(keys))
            for key, (stage_kind, blob, flags) in entries.items():
                compiled = blob[48:]
                if key == rewritten_key:
                    self.assertEqual(flags, pack.PACK_FLAG_TEV_PASSTHROUGH_SKIP)
                    self.assertEqual(compiled, pack.skip_tev_passthrough(keys[key])[0])
                else:
                    self.assertEqual((flags, compiled), (0, keys[key]))
            header = struct.unpack_from("<4I", (work / "deko3d.pack").read_bytes())
            self.assertEqual(header[3], pack.PACK_FLAG_TEV_PASSTHROUGH_SKIP)

            out = self.build(work, "--keep-tev-passthrough", "-o", str(work / "deko3d_reference.pack"))
            self.assertIn("kept in all 2 fragment texts", out)
            reference = pack.read_pack(work / "deko3d_reference.pack")
            self.assertEqual(set(reference), set(keys))
            for key, (_, blob, flags) in reference.items():
                self.assertEqual((flags, blob[48:]), (0, keys[key]))
            self.assertEqual(struct.unpack_from("<4I", (work / "deko3d_reference.pack").read_bytes())[3], 0)

            # A previous pack in the other mode: its entry of a known text is recompiled.
            out = self.build(work, "--pack", str(work / "deko3d_reference.pack"), "-o", str(work / "again.pack"))
            self.assertIn("1 compiled now", out)
            self.assertEqual(pack.read_pack(work / "again.pack"), entries)


if __name__ == "__main__":
    unittest.main()
