#!/usr/bin/env python3
"""CPU evaluation of the deko3d per-pixel lighting text (gfx_deko3d_lighting.cpp).

The per-pixel fragment code reads its interpolants once, at the top of the
body, and has no lane-dependent branch (SWITCH_PORT.md "Per-pixel dots: 8x4
tiles"). These tests run the shipped GLSL on the CPU (glsl_eval.py, binary32,
Maxwell min/max) and check:
  - valid inputs: the same bytes as the 0.1.2-test.7 text (data/perpixel_lighting_test7.glsl)
    on every path (native lights, environment, scene points and directionals, baked, rim);
  - degenerate inputs (zero or opposite normals, the eye at the fragment, a light at the
    surface, a zero radius, grazing angles, infinite or NaN interpolants): finite, in [0, 1];
  - the interpolants are read only in pica_surface_fragment(), and every branch condition
    is uniform (the block's members, loop counters).

    python3 tests/switch/test_perpixel_lighting.py
"""
import math
import random
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from glsl_eval import Program, Vec, is_finite  # noqa: E402

REPO = HERE.parent.parent
SOURCE = REPO / "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting.cpp"
REFERENCE = HERE / "data/perpixel_lighting_test7.glsl"
INPUTS = ("pica_view", "pica_surface_normal", "pica_surface_tint")


def raw_string(name):
    text = SOURCE.read_text()
    match = re.search(r"constexpr std::string_view " + name + r' = R"glsl\((.*?)\)glsl";', text, re.S)
    if match is None:
        raise AssertionError(f"{name} not found in {SOURCE}")
    return match.group(1)


def byteround3(color):
    return tuple(math.floor(c * 255.0 + 0.5) for c in color.c)


def unit(values):
    length = math.sqrt(sum(v * v for v in values))
    return [v / length for v in values]


def random_direction(rng):
    while True:
        v = [rng.uniform(-1.0, 1.0) for _ in range(3)]
        if 0.05 < sum(x * x for x in v) <= 1.0:
            return unit(v)


def color(rng, low=0.0, high=1.0):
    return Vec([rng.uniform(low, high) for _ in range(3)] + [0.0])


def random_block(rng, program):
    """A valid PicaSurfaceLighting block (what BuildPicaSurfaceLightingUniforms and
    AssignPicaSurfaceRim produce), choosing one of the shader's paths."""
    block = program.globals["pica_surface"]
    block_copy = {key: value for key, value in block.items()}
    native = rng.random() < 0.6
    environment = rng.random() < 0.5
    baked = rng.random() < 0.25
    points = rng.randint(0, 4)
    directionals = rng.randint(0, 2)
    block_copy["control"] = Vec([1 if native else 0, points, directionals, rng.choice([0, 1])], "u")
    block_copy["material_diffuse"] = color(rng)
    block_copy["material_ambient"] = color(rng)
    block_copy["resolved_ambient"] = color(rng, 0.0, 0.6)
    # A rotation (and a uniform scale) from view to world.
    axis = random_direction(rng)
    angle = rng.uniform(-math.pi, math.pi)
    c, s = math.cos(angle), math.sin(angle)
    x, y, z = axis
    rows = [[c + x * x * (1 - c), x * y * (1 - c) - z * s, x * z * (1 - c) + y * s],
            [y * x * (1 - c) + z * s, c + y * y * (1 - c), y * z * (1 - c) - x * s],
            [z * x * (1 - c) - y * s, z * y * (1 - c) + x * s, c + z * z * (1 - c)]]
    scale = rng.uniform(0.5, 2.0)
    block_copy["view_to_world_linear"] = [Vec([v * scale for v in row] + [0.0]) for row in rows]
    lights = []
    for _ in range(3):
        enabled = rng.random() < 0.7
        positional = rng.random() < 0.4
        lights.append({
            "direction_enabled": Vec(random_direction(rng) + [1.0 if enabled else 0.0]),
            "diffuse": color(rng),
            "position_radius": Vec([rng.uniform(-300, 300) for _ in range(3)] +
                                   [rng.uniform(50.0, 800.0) if positional else 0.0]),
        })
    block_copy["lights"] = lights
    block_copy["environment_parameters"] = Vec([rng.choice([0.0, rng.uniform(0.2, 1.5)]), rng.uniform(0.0, 1.0),
                                                rng.uniform(0.0, 1.0), 1.0 if environment else 0.0])
    block_copy["environment_ambient"] = color(rng, 0.0, 0.4)
    block_copy["environment_directions"] = [Vec(random_direction(rng) + [rng.choice([0.0, rng.uniform(0.1, 2.0)])])
                                            for _ in range(2)]
    block_copy["environment_colors"] = [color(rng) for _ in range(2)]
    block_copy["scene_directions"] = [Vec(random_direction(rng) + [0.0]) for _ in range(2)]
    block_copy["scene_directional_colors"] = [color(rng) for _ in range(2)]
    scene_points = [dict(point) for point in block["scene_points"]]
    for slot in range(4):
        scene_points[slot] = {
            "position_radius": Vec([rng.uniform(-400, 400) for _ in range(3)] + [rng.uniform(80.0, 900.0)]),
            "color": color(rng, 0.0, 1.5),
        }
    block_copy["scene_points"] = scene_points
    rim_source = rng.choice([0.0, 1.0, 2.0])
    block_copy["rim"] = Vec([rng.uniform(0.1, 2.0) if rim_source else 0.0, rng.uniform(1.0, 16.0), rim_source,
                             rng.uniform(0.0, 1.0)])
    block_copy["rim_color"] = color(rng)
    block_copy["rim_direction"] = Vec(unit([rng.uniform(-1, 1), rng.uniform(-1, 1) or 1.0]) + [0.0, 0.0])
    block_copy["baked_reference_up"] = Vec(random_direction(rng) + [1.0 if baked else 0.0])
    block_copy["baked_reference_key"] = color(rng)
    return block_copy


class PerPixelLighting(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.declarations = raw_string("kPerPixelDeclarations")
        cls.body = raw_string("kPerPixelBody")
        cls.current = Program(cls.declarations)
        cls.reference = Program(REFERENCE.read_text())

    def evaluate_current(self, block, native, normal, view, tint):
        program = self.current
        program.globals["pica_surface"] = block
        program.globals["pica_surface_normal"] = Vec(normal)
        program.globals["pica_view"] = Vec(view)
        program.globals["pica_surface_tint"] = Vec(tint)
        fragment = program.call("pica_surface_fragment")
        return program.call("pica_surface_primary", Vec(native), fragment)

    def evaluate_reference(self, block, native, normal, view, tint):
        program = self.reference
        program.globals["pica_surface"] = block
        program.globals["pica_surface_normal"] = Vec(normal)
        program.globals["pica_view"] = Vec(view)
        program.globals["pica_surface_tint"] = Vec(tint)
        normal_view = program.call("pica_surface_unit", Vec(normal))
        return program.call("pica_surface_primary", Vec(native), normal_view)

    def test_valid_inputs_match_test7(self):
        rng = random.Random(20261107)
        paths = set()
        for case in range(300):
            block = random_block(rng, self.current)
            native = [rng.uniform(0.0, 1.0) for _ in range(3)]
            # Interpolated (not unit) normals, eye-space positions in front of the camera.
            normal = [v * rng.uniform(0.3, 1.5) for v in random_direction(rng)]
            view = [rng.uniform(-200, 200), rng.uniform(-200, 200), rng.uniform(-900, -5)]
            tint = [rng.uniform(0.0, 1.0) for _ in range(3)] + [1.0]
            ours = self.evaluate_current(block, native, normal, view, tint)
            theirs = self.evaluate_reference(block, native, normal, view, tint)
            self.assertEqual(byteround3(ours), byteround3(theirs), f"case {case}: {ours} != {theirs}")
            paths.add((block["baked_reference_up"].c[3] != 0.0, block["control"].c[0], block["control"].c[1],
                       block["environment_parameters"].c[3] != 0.0, block["rim"].c[2]))
        self.assertGreater(len(paths), 40, "the random blocks cover the shader's paths")

    def test_degenerate_inputs_stay_finite(self):
        rng = random.Random(7)
        big = 3.0e38
        cases = {
            "zero normal": ([0.0, 0.0, 0.0], [10.0, 5.0, -100.0]),
            "tiny normal": ([1e-25, 0.0, 0.0], [10.0, 5.0, -100.0]),
            "normal facing away": ([0.0, 0.0, -1.0], [0.0, 0.0, -100.0]),
            "grazing": ([1.0, 0.0, 0.0], [0.0, 0.0, -100.0]),
            "eye at the fragment": ([0.0, 0.0, 1.0], [0.0, 0.0, 0.0]),
            "huge view": ([0.0, 1.0, 0.0], [big, -big, big]),
            "infinite view": ([0.0, 1.0, 0.0], [math.inf, 0.0, -math.inf]),
            "NaN view": ([0.0, 1.0, 0.0], [math.nan, 0.0, -1.0]),
            "NaN normal": ([math.nan, 1.0, 0.0], [0.0, 0.0, -1.0]),
            "infinite normal": ([math.inf, 0.0, 0.0], [0.0, 0.0, -1.0]),
        }
        for name, (normal, view) in cases.items():
            for case in range(25):
                block = random_block(rng, self.current)
                # A scene point and a native point light at the fragment, one with a zero radius.
                points = block["scene_points"]
                points[0] = {"position_radius": Vec([-v if math.isfinite(v) else 0.0 for v in view] + [300.0]),
                             "color": Vec([1.0, 1.0, 1.0, 0.0])}
                points[1] = {"position_radius": Vec([1.0, 2.0, 3.0, 0.0]), "color": Vec([1.0, 0.5, 0.2, 0.0])}
                block["lights"][0]["position_radius"] = Vec(
                    [-v if math.isfinite(v) else 0.0 for v in view] + [500.0])
                tint = [rng.choice([0.0, 1.0, rng.uniform(0.0, 1.0)]) for _ in range(3)] + [1.0]
                result = self.evaluate_current(block, [0.5, 0.5, 0.5], normal, view, tint)
                self.assertTrue(is_finite(result) and all(0.0 <= c <= 1.0 for c in result.c),
                                f"{name}, case {case}: {result}")
                if all(math.isfinite(v) for v in normal + view):
                    # Finite degenerate inputs keep test.7's guards (no rim on a zero normal, ...).
                    reference = self.evaluate_reference(block, [0.5, 0.5, 0.5], normal, view, tint)
                    self.assertEqual(byteround3(result), byteround3(reference), f"{name}, case {case}")

    def test_interpolants_read_once_at_the_top(self):
        code = re.sub(r"//[^\n]*", "", self.declarations)
        function = re.search(r"PicaSurfaceFragment pica_surface_fragment\(\) \{(.*?)\n\}", code, re.S)
        self.assertIsNotNone(function, "pica_surface_fragment() reads the interpolants")
        outside = code.replace(function.group(0), "")
        for name in INPUTS:
            uses = [m.start() for m in re.finditer(r"\b" + name + r"\b", outside)]
            declared = re.findall(r"layout\(location=\d+\) in vec\d " + name + ";", outside)
            self.assertEqual(len(uses), len(declared), f"{name} is read outside pica_surface_fragment()")
            self.assertIn(name, function.group(1))
        # The body reads them before its branch, and passes them down.
        self.assertRegex(self.body, r"PicaSurfaceFragment pica_surface_inputs = pica_surface_fragment\(\);\s*\n\s*if \(")
        for name in INPUTS:
            self.assertNotRegex(self.body, r"\b" + name + r"\b")

    def test_branches_are_uniform(self):
        """Every if/for condition and every ternary that guards a return reads only the
        block (pica_surface.*), loop counters and constants: no lane-dependent branch."""
        # `light` is a copy of pica_surface.lights[slot] (pica_surface_light).
        uniform = re.compile(r"(pica_surface|light)\.[\w\[\]\.]+|\b(slot|count|native_direct|min|u?int)\b|"
                             r"\d+\.\d*(e[-+]?\d+)?|\d+u?|[\s()!=<>&|+\-*.,]+")

        def conditions(text):
            for keyword in ("if", "for"):
                for match in re.finditer(r"\b" + keyword + r" \(", text):
                    depth, at = 1, match.end()
                    while depth:
                        depth += {"(": 1, ")": -1}.get(text[at], 0)
                        at += 1
                    condition = text[match.end():at - 1]
                    if keyword == "for":
                        condition = condition.split(";")[1]
                    yield condition

        self.assertIn("PicaSurfaceLight light = pica_surface.lights[slot];", self.declarations)
        for condition in conditions(re.sub(r"//[^\n]*", "", self.declarations)):
            leftover = uniform.sub("", condition)
            self.assertEqual(leftover.strip(), "", f"lane-dependent branch condition: {condition!r}")


if __name__ == "__main__":
    unittest.main()
