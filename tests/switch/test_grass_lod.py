#!/usr/bin/env python3
"""Host checks of the config/grass_lod shaders (SWITCH_PORT.md "Grass on deko3d: cost per tuft").

grass_lod_common.glsl runs on the CPU (glsl_eval.py, binary32) and these tests check:
  - no pop where a tuft changes draw: the 5-blade layout at morph 1 is the 3-blade one at
    morph 0, and the 3-blade one at morph 1 the 1-blade one; the blades that go away have no
    width left; the blades' summed width (the covered area) is the same at every morph;
  - the 5-blade layout at morph 0 is test.9's tuft (grass_vsh.glsl's formulas);
  - the level coordinate equals the C++ one the cull worker uses (GrassLodCoordinate, the
    same sample values as oot3d_grass_switch_tests) and is 0 near the camera;
  - the integer hash is lowbias32 (as MixGrassBits), the blade randoms are in [0, 1);
  - grass_lod_vsh.glsl's float division finds tuft and corner of every vertex of every
    draw (exhaustive: 65536 tufts of 5, 3 and 1 blades);
  - the tuft pass's lit root and tip colours, interpolated by the height, give
    grass_vsh.glsl's per-vertex colour (its lines run as they are in the file).

    python3 tests/switch/test_grass_lod.py
"""
import array
import random
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from glsl_eval import Program, Vec, f32  # noqa: E402

REPO = HERE.parent.parent
GRASS = REPO / "tools/oot3d/deko3d_grass"
PIXELS = (2.4, 1.8, 1.2, 0.9)  # kGrassLodPixels


# grass_lod_blade's out parameters as one vector (glsl_eval returns no out parameters).
LAYOUT = """
vec3 test_lod_blade(float blade, float from, float target, float to, float morph, vec3 random) {
    float centre;
    float halfBase;
    float tip;
    grass_lod_blade(blade, from, target, to, morph, random, centre, halfBase, tip);
    return vec3(centre, halfBase, tip);
}
"""


def common():
    # glsl_eval has no matrices; the tests call no function that reads this one.
    text = (GRASS / "grass_lod_common.glsl").read_text().replace("mat4 grass_position_to_clip;",
                                                                   "vec4 grass_position_to_clip[4];")
    return Program(text + LAYOUT)


def lowbias32(value):
    value &= 0xFFFFFFFF
    value ^= value >> 16
    value = (value * 0x7FEB352D) & 0xFFFFFFFF
    value ^= value >> 15
    value = (value * 0x846CA68B) & 0xFFFFFFFF
    value ^= value >> 16
    return value


def cpp_coordinate(focal, minimum, distance, half_width):
    """GrassLodCoordinate (grass_instance_stream.cpp) in binary32."""
    slot = f32(f32(f32(2.0 * half_width) * focal) / max(distance, 1.0))
    to_three = min(max(f32(f32(PIXELS[0] - slot) / f32(PIXELS[0] - PIXELS[1])), 0.0), 1.0)
    to_one = min(max(f32(f32(PIXELS[2] - slot) / f32(PIXELS[2] - PIXELS[3])), 0.0), 1.0)
    ramp = f32(2.0 / minimum)
    limit = f32(2.0 * min(max(f32(f32(distance - minimum) * ramp), 0.0), 1.0))
    return min(f32(to_three + to_one), limit)


class GrassLodLayout(unittest.TestCase):
    """grass_lod_blade: the levels' layouts and the morphs between them."""

    @classmethod
    def setUpClass(cls):
        cls.program = common()
        cls.randoms = [Vec([f32(random.Random(seed).random()) for _ in range(3)]) for seed in range(40)]

    def layout(self, b, blades, target, morph, rnd):
        return tuple(self.program.call("test_lod_blade", f32(b), f32(1.0 / blades), f32(target),
                                       f32(1.0 / target), f32(morph), rnd).c)

    def test_a_level_change_shows_the_same_blades(self):
        for rnd in self.randoms:
            for blades, target in ((5, 3), (3, 1)):
                for b in range(blades):
                    end = self.layout(b, blades, target, 1.0, rnd)
                    if b < target:
                        start = self.layout(b, target, 1, 0.0, rnd)
                        for got, want in zip(end, start):
                            self.assertAlmostEqual(got, want, delta=1.0e-6, msg=(blades, target, b))
                    else:
                        centre, half_base, tip = end
                        self.assertEqual(half_base, 0.0)  # no width: no pixel
                        self.assertEqual(tip, centre)

    def test_the_covered_width_stays_the_same_through_a_morph(self):
        for rnd in self.randoms[:10]:
            for blades, target in ((5, 3), (3, 1)):
                for step in range(11):
                    morph = step / 10.0
                    total = sum(self.layout(b, blades, target, morph, rnd)[1] for b in range(blades))
                    self.assertAlmostEqual(total, 0.42, delta=2.0e-6, msg=(blades, target, morph))

    def test_a_morph_moves_the_blades_continuously(self):
        for rnd in self.randoms[:10]:
            for blades, target in ((5, 3), (3, 1)):
                for b in range(blades):
                    before = self.layout(b, blades, target, 0.0, rnd)
                    for step in range(1, 101):
                        now = self.layout(b, blades, target, step / 100.0, rnd)
                        for x, y in zip(before, now):
                            self.assertLess(abs(x - y), 0.01)  # at most 1% of the card per 1% of morph
                        before = now

    def test_five_blades_unmorphed_are_test9s_tuft(self):
        # grass_vsh.glsl: centre = (b + 0.5 + (r0 - 0.5) * 0.5) / n, halfBase = 0.42 / n,
        # tip = centre + (r1 - 0.5) * 0.6 / n.
        for rnd in self.randoms:
            r0, r1 = rnd.c[0], rnd.c[1]
            for b in range(5):
                centre, half_base, tip = self.layout(b, 5, 5, 0.0, rnd)
                want_centre = f32(f32(f32(b + 0.5) + f32(f32(r0 - 0.5) * 0.5)) / 5.0)
                self.assertAlmostEqual(centre, want_centre, delta=1.0e-6)
                self.assertAlmostEqual(half_base, 0.42 / 5.0, delta=1.0e-7)
                self.assertAlmostEqual(tip, want_centre + (r1 - 0.5) * 0.6 / 5.0, delta=1.0e-6)
                # The near draw (5 blades, target 5) ignores the morph.
                self.assertEqual(self.layout(b, 5, 5, 0.7, rnd), (centre, half_base, tip))


class GrassLodCoordinate(unittest.TestCase):
    # (focal, minimum distance, distance, half width) -> GrassLodCoordinate, as in
    # oot3d_grass_switch_tests (LevelCoordinateSamples).
    SAMPLES = [(600.0, 800.0, 700.0, 1.4), (600.0, 800.0, 900.0, 1.4), (600.0, 800.0, 1150.0, 1.75),
               (600.0, 800.0, 1300.0, 1.4), (600.0, 800.0, 1500.0, 1.4), (600.0, 800.0, 6000.0, 1.0),
               (900.0, 800.0, 1500.0, 2.5)]

    @classmethod
    def setUpClass(cls):
        cls.program = common()

    def glsl(self, focal, minimum, distance, half_width):
        block = self.program.globals
        block["grass_lod_scale"] = Vec([f32(focal), f32(minimum), f32(2.0 / minimum), 0.0])
        block["grass_lod_px"] = Vec([f32(p) for p in PIXELS])
        return self.program.call("grass_lod_coordinate", f32(distance), f32(half_width))

    def test_glsl_and_the_cull_worker_agree(self):
        for sample in self.SAMPLES:
            self.assertAlmostEqual(self.glsl(*sample), cpp_coordinate(*sample), delta=1.0e-6, msg=sample)
        rng = random.Random(3)
        for _ in range(300):
            sample = (rng.uniform(300.0, 1500.0), 800.0, rng.uniform(1.0, 20000.0), rng.uniform(0.5, 6.0))
            self.assertAlmostEqual(self.glsl(*sample), cpp_coordinate(*sample), delta=1.0e-5, msg=sample)

    def test_near_the_camera_every_tuft_has_five_blades(self):
        for distance in (1.0, 300.0, 600.0, 799.0, 800.0):
            for half_width in (0.1, 1.0, 5.0):
                self.assertEqual(self.glsl(5000.0, 800.0, distance, half_width), 0.0)

    def test_the_coordinate_grows_with_distance(self):
        before = 0.0
        for step in range(1, 400):
            now = self.glsl(600.0, 800.0, step * 25.0, 1.4)
            self.assertGreaterEqual(now, before)
            before = now
        self.assertEqual(before, 2.0)


class GrassLodHash(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.program = common()

    def test_the_mix_is_lowbias32(self):
        for value in (0, 1, 2, 0x12345678, 0xFFFFFFFF, 0x9E3779B9):
            self.assertEqual(self.program.call("grass_mix_bits", value), lowbias32(value))

    def test_blade_randoms_are_in_unit_range_and_per_blade(self):
        rng = random.Random(5)
        sums = [0.0, 0.0, 0.0]
        count = 0
        for _ in range(300):
            seed = rng.getrandbits(32)
            seen = set()
            for b in range(5):
                values = self.program.call("grass_blade_random", seed, b).c
                bits = lowbias32((seed + b * 0x9E3779B9) & 0xFFFFFFFF)
                self.assertEqual(values, [(bits & 0x3FF) / 1024.0, ((bits >> 10) & 0x7FF) / 2048.0,
                                          (bits >> 21) / 2048.0])
                for i, v in enumerate(values):
                    self.assertGreaterEqual(v, 0.0)
                    self.assertLess(v, 1.0)
                    sums[i] += v
                count += 1
                seen.add(tuple(values))
            self.assertEqual(len(seen), 5)
        for total in sums:
            self.assertAlmostEqual(total / count, 0.5, delta=0.03)


class GrassLodVertexIndex(unittest.TestCase):
    def test_every_vertex_finds_its_tuft_and_corner(self):
        # grass_lod_vsh.glsl: index = uint((float(v) + 0.5) * (1 / (3 n))) in binary32; float(v)
        # and + 0.5 are exact (v < 2^23), the product of two binary32 values is exact in a
        # double, so one rounding to binary32 (array('f')) is the GPU's FMUL.
        for blades in (5, 3, 1):
            per = 3 * blades
            inverse = array.array("f", [1.0 / per])[0]
            vertices = 65536 * per
            products = array.array("f", ((v + 0.5) * inverse for v in range(vertices)))
            for v, product in enumerate(products):
                if int(product) != v // per:
                    self.fail(f"vertex {v} of {blades}-blade tufts: {int(product)} != {v // per}")


class GrassLodColour(unittest.TestCase):
    def test_root_and_tip_interpolated_give_test9s_colour(self):
        old = (GRASS / "grass_vsh.glsl").read_text()
        tuft = (GRASS / "grass_tuft_vsh.glsl").read_text()
        old_lines = "\n".join(re.search(rf"^\s*vec3 {name} = .*;$", old, re.M).group(0)
                              for name in ("authored", "fromGround", "colour"))
        new_lines = "\n".join(re.search(rf"^\s*vec3 {name} = .*;$", tuft, re.M).group(0) for name in ("root", "tip"))
        blocks = re.search(r"layout\(std140, binding = 1\) uniform GrassFrame \{.*?\};", old, re.S).group(0)
        text = blocks + f"""
vec3 old_colour(vec4 ground, float y) {{
{old_lines}
    return colour;
}}
vec3 new_colour(vec4 ground, float y) {{
    vec3 light = vec3(1.0);
{new_lines}
    return mix(root, tip, y);
}}
"""
        program = Program(text)
        rng = random.Random(9)
        for _ in range(200):
            program.globals["grass_root_color"] = Vec([f32(rng.random()) for _ in range(3)] + [f32(rng.random())])
            program.globals["grass_tip_color"] = Vec([f32(rng.random()) for _ in range(3)] + [0.0])
            program.globals["grass_brightness"] = Vec([f32(rng.uniform(0.5, 1.5)), f32(rng.uniform(0.5, 1.5)), 0.0, 0.0])
            ground = Vec([f32(rng.random()) for _ in range(4)])
            for y in (0.0, 0.7, f32(rng.uniform(0.7, 1.0)), 1.0):
                old_value = program.call("old_colour", ground, f32(y)).c
                new_value = program.call("new_colour", ground, f32(y)).c
                for a, b in zip(old_value, new_value):
                    self.assertAlmostEqual(a, b, delta=4.0e-7 * 255.0 / 255.0 + 1.0e-6)


if __name__ == "__main__":
    unittest.main()
