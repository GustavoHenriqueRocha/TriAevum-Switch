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
    draw (exhaustive: 65536 tufts of 5, 3 and 1 blades, and of the near draw's segmented blades);
  - the near draw's single blades are the PC's curved blade (grass_blade_shape.h with the PC
    preset of graphics_settings_persistence.cpp) in GRASS_NEAR_SEGMENTS segments: their vertices
    cover each segment once, root and tip are where test.9's blade had them (plus the curve), and
    a tuft drawn by the near program keeps test.9's triangle corners;
  - the tuft pass's lit root and tip colours, interpolated by the height, give
    grass_vsh.glsl's per-vertex colour (its lines run as they are in the file).

    python3 tests/switch/test_grass_lod.py
"""
import array
import math
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


def near_segments():
    """GRASS_NEAR_SEGMENTS of grass_lod_common.glsl and kGrassNearBladeSegments of the C++."""
    shader = int(re.search(r"const uint GRASS_NEAR_SEGMENTS = (\d+)u;",
                           (GRASS / "grass_lod_common.glsl").read_text()).group(1))
    header = (REPO / "runtime/three_ds_recomp/include/fast/oot3d/grass_instance_stream.h").read_text()
    cpp = int(re.search(r"kGrassNearBladeSegments = (\d+)U;", header).group(1))
    return shader, cpp


def far_segments():
    """GRASS_FAR_SEGMENTS of grass_lod_common.glsl and kGrassFarBladeSegments of the C++."""
    shader = re.search(r"const uint GRASS_FAR_SEGMENTS = (\d+)u;", (GRASS / "grass_lod_common.glsl").read_text())
    header = (REPO / "runtime/three_ds_recomp/include/fast/oot3d/grass_instance_stream.h").read_text()
    cpp = re.search(r"kGrassFarBladeSegments = (\d+)U;", header)
    return (int(shader.group(1)) if shader else None), (int(cpp.group(1)) if cpp else None)


class GrassLodVertexIndex(unittest.TestCase):
    def test_the_shader_and_the_draw_agree_on_the_near_segments(self):
        shader, cpp = near_segments()
        self.assertEqual(shader, cpp)
        self.assertGreaterEqual(shader, 2)

    def test_the_shader_and_the_draw_agree_on_the_far_segments(self):
        # The far tufts' blades were one straight triangle each: pointed from a distance next to
        # the curved near blades (Gustavo, 12/11). They are curved in segments too.
        shader, cpp = far_segments()
        self.assertEqual(shader, cpp)
        self.assertGreaterEqual(shader or 0, 2)
        vsh = (GRASS / "grass_lod_vsh.glsl").read_text()
        self.assertIn("const uint bladeVertices = 6u * GRASS_FAR_SEGMENTS - 3u;", vsh)

    def test_every_vertex_finds_its_tuft_and_corner(self):
        # grass_lod_vsh.glsl: index = uint((float(v) + 0.5) * (1 / (vertices per tuft))) in
        # binary32; float(v) and + 0.5 are exact (v < 2^23), the product of two binary32 values
        # is exact in a double, so one rounding to binary32 (array('f')) is the GPU's FMUL.
        # Every draw: 6 x segments - 3 vertices per blade, 5, 3 or 1 blades far, 5 near.
        near = 5 * (6 * near_segments()[0] - 3)
        far = 6 * (far_segments()[0] or 1) - 3
        for per in (5 * far, 3 * far, far, near):
            inverse = array.array("f", [1.0 / per])[0]
            vertices = 65536 * per
            products = array.array("f", ((v + 0.5) * inverse for v in range(vertices)))
            for v, product in enumerate(products):
                if int(product) != v // per:
                    self.fail(f"vertex {v} of {per}-vertex tufts: {int(product)} != {v // per}")


# The PC preset's blade (graphics_settings_persistence.cpp, default "Grass" "Appearance").
def pc_preset_shape():
    text = (REPO / "runtime/three_ds_recomp/src/fast/oot3d/graphics_settings_persistence.cpp").read_text()
    grass = text[text.index('"Grass": {\n    "Appearance"'):]
    value = lambda name: float(re.search(rf'"{name}": ([-0-9.]+)', grass).group(1))
    return (value("BladeCurvature"), value("BladeDroop"), value("ShapeVariation"),
            value("BladeTwistDegrees") * math.pi / 180.0)


def pc_blade_shape(t, phase, signed_random, axis, shape):
    """grass_blade_shape of grass_blade_shape.h (the PC blade) with its random given."""
    angle = signed_random * shape[2] * math.pi
    c, s = math.cos(angle), math.sin(angle)
    direction = (c * axis[0] - s * axis[1], s * axis[0] + c * axis[1])
    side = (-direction[1], direction[0])
    curve = shape[0] * (1.0 + signed_random * shape[2])
    sway = 0.35 * shape[2] * math.sin(math.pi * t + phase) * t * t
    offset = [direction[i] * curve * t * t + side[i] * sway for i in range(2)]
    return (offset[0], t - shape[1] * t * t * t, offset[1]), shape[3] * signed_random * t * t


class GrassNearBlade(unittest.TestCase):
    """The near draw's single blades (GRASS_LOD_NEAR in grass_lod_vsh.glsl)."""

    @classmethod
    def setUpClass(cls):
        cls.program = common()
        cls.segments = near_segments()[0]

    def row_side(self, k, segments):
        return tuple(self.program.call("grass_row_side", k, segments).c)

    def test_the_vertices_cover_every_segment_once(self):
        for segments in (self.segments, far_segments()[0]):
            with self.subTest(segments=segments):
                self.check_rows(segments)
        for k in range(6 * self.segments - 3):
            self.assertEqual(tuple(self.program.call("grass_near_row_side", k).c), self.row_side(k, self.segments))

    def check_rows(self, segments):
        per = 6 * segments - 3
        rows = [r / segments for r in range(segments + 1)]
        triangles = []
        for first in range(0, per, 3):
            corners = [self.row_side(first + c, segments) for c in range(3)]
            for t, side in corners:
                self.assertIn(min(rows, key=lambda r: abs(r - t)), rows)
                self.assertAlmostEqual(t, min(rows, key=lambda r: abs(r - t)), delta=1.0e-6)
                self.assertIn(side, (-1.0, 0.0, 1.0))
            # Twice the signed area in (side, t): not degenerate, all the same winding.
            (t0, s0), (t1, s1), (t2, s2) = corners
            area = (s1 - s0) * (t2 - t0) - (s2 - s0) * (t1 - t0)
            self.assertGreater(abs(area), 1.0e-6)
            triangles.append((corners, area))
        windings = {area > 0 for _, area in triangles}
        self.assertEqual(len(windings), 1, "mixed winding")
        # Each segment's area (side from -1 to 1, so width 2 x its height) is covered once:
        # a quad for every segment but the last, a triangle to the tip for the last.
        covered = [0.0] * segments
        for corners, area in triangles:
            low = min(t for t, _ in corners)
            covered[round(low * segments)] += abs(area) / 2.0
        height = 1.0 / segments
        for index, value in enumerate(covered):
            want = 2.0 * height if index < segments - 1 else height
            self.assertAlmostEqual(value, want, delta=1.0e-6, msg=index)
        # The tip is one point: side 0 at t = 1, and nothing else reaches t = 1.
        tips = [c for corners, _ in triangles for c in corners if c[0] == 1.0]
        self.assertTrue(tips)
        self.assertTrue(all(side == 0.0 for _, side in tips))

    def test_the_shape_is_the_pc_preset(self):
        source = (GRASS / "grass_lod_common.glsl").read_text()
        got = [float(v) for v in re.search(r"const vec4 GRASS_NEAR_SHAPE = vec4\(([^)]*)\);", source)
               .group(1).split(",")]
        for a, b in zip(got, pc_preset_shape()):
            self.assertAlmostEqual(a, b, delta=1.0e-5)

    def test_the_curve_is_the_pc_blades(self):
        shape = pc_preset_shape()
        rng = random.Random(14)
        for _ in range(300):
            t = f32(rng.choice([0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0, rng.random()]))
            phase = f32(rng.uniform(0.0, 6.2831853))
            signed_random = f32(rng.uniform(-1.0, 1.0))
            angle = rng.uniform(0.0, 2.0 * math.pi)
            axis = (f32(math.cos(angle)), f32(math.sin(angle)))
            want, want_twist = pc_blade_shape(t, phase, signed_random, axis, shape)
            got = self.program.call("grass_near_blade_shape", t, phase, signed_random, Vec(list(axis))).c
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, delta=2.0e-5)
            self.assertAlmostEqual(self.program.call("grass_near_twist", t, signed_random), want_twist, delta=2.0e-5)

    def test_root_and_tip_are_test9s_blade_ends(self):
        # grass_lod_vsh.glsl before the segments: the root corners at root -+ 1.32 x half width
        # along the facing, the tip at root + (lean.x h, h (1 - 0.3 |lean|^2), lean.y h).
        # Now the root is the same, and the tip the same plus the PC curve at t = 1.
        shape = pc_preset_shape()
        rng = random.Random(15)
        for _ in range(200):
            root = Vec([f32(rng.uniform(-500, 500)), f32(rng.uniform(-50, 50)), f32(rng.uniform(-500, 500))])
            angle = rng.uniform(0.0, 2.0 * math.pi)
            facing = (f32(math.cos(angle)), f32(math.sin(angle)))
            half_width, height = f32(rng.uniform(1.0, 2.0)), f32(rng.uniform(10.0, 30.0))
            lean = (f32(rng.uniform(-0.4, 0.4)), f32(rng.uniform(-0.4, 0.4)))
            phase, signed_random = f32(rng.uniform(0.0, 6.28)), f32(rng.uniform(-1.0, 1.0))
            args = (root, Vec(list(facing)), half_width, height, Vec(list(lean)), phase, signed_random)
            for side in (-1.0, 1.0):
                got = self.program.call("grass_near_blade_point", *args, 0.0, side).c
                want = [root.c[0] + facing[0] * side * 1.32 * half_width, root.c[1],
                        root.c[2] + facing[1] * side * 1.32 * half_width]
                for a, b in zip(got, want):
                    self.assertAlmostEqual(a, b, delta=1.0e-3)
            curve, _ = pc_blade_shape(1.0, phase, signed_random, facing, shape)
            got = self.program.call("grass_near_blade_point", *args, 1.0, 0.0).c
            want = [root.c[0] + (lean[0] + curve[0]) * height,
                    root.c[1] + height * (1.0 - 0.3 * (lean[0] ** 2 + lean[1] ** 2)) - shape[1] * height,
                    root.c[2] + (lean[1] + curve[2]) * height]
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, delta=1.0e-3)
            # Both sides meet at the tip.
            left = self.program.call("grass_near_blade_point", *args, 1.0, -1.0).c
            right = self.program.call("grass_near_blade_point", *args, 1.0, 1.0).c
            for a, b in zip(left, right):
                self.assertAlmostEqual(a, b, delta=1.0e-4)

    def test_a_tuft_blade_is_the_pc_curve_on_its_card(self):
        # Every tuft blade (far draws and the near draw's ring) was grass_lod_blade's straight
        # triangle: pointed from a distance (12/11). Now the root keeps that triangle's corners
        # (centre -+ half base) and the rows follow the PC blade seen on the card: the centre line
        # from the centre to the tip bent sideways by `curve` t^2, the height drooping as the PC's
        # (t - droop t^3), the width narrowing as the PC's (1 - 0.75 t), one point at the tip.
        droop = pc_preset_shape()[1]
        rng = random.Random(16)
        for _ in range(200):
            centre, half_base, tip = f32(rng.random()), f32(rng.uniform(0.0, 0.1)), f32(rng.random())
            tip_y, curve = f32(rng.uniform(0.7, 1.0)), f32(rng.uniform(-0.5, 0.5))
            t = f32(rng.choice([0.0, 0.5, 1.0 / 3.0, 2.0 / 3.0, 1.0, rng.random()]))
            for side in (-1.0, 0.0, 1.0):
                got = self.program.call("grass_tuft_xy", centre, half_base, tip, tip_y, curve, t, side).c
                width = 0.0 if t >= 1.0 else 1.0 - 0.75 * t
                want = (centre + (tip - centre) * t + curve * t * t + side * half_base * width,
                        tip_y * (t - droop * t * t * t))
                self.assertAlmostEqual(got[0], want[0], delta=2.0e-6)
                self.assertAlmostEqual(got[1], want[1], delta=2.0e-6)
        vsh = (GRASS / "grass_lod_vsh.glsl").read_text()
        self.assertNotIn("corner == 2u ? tipX", vsh)
        self.assertEqual(vsh.count("grass_tuft_xy("), 1, "one curved tuft for the far and near programs")


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
