"""shrink_texture_pack.py: sizes, alpha edges, parts and languages, on synthetic packs."""
from __future__ import annotations

import io
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the suite skips without Pillow
    Image = None

import shrink_texture_pack as shrink

ROOT = "OOT3D 4K (1080p)/user/load/textures/0004000000033500/"


def png(width: int, height: int, colour=(10, 200, 30, 255)) -> bytes:
    out = io.BytesIO()
    Image.new("RGBA", (width, height), colour).save(out, format="PNG")
    return out.getvalue()


def size_of(path: Path) -> tuple[int, int]:
    with Image.open(path) as image:
        return image.size


@unittest.skipIf(Image is None, "needs Pillow")
class ShrinkTexturePackTest(unittest.TestCase):
    def setUp(self):
        self.work = Path(tempfile.mkdtemp())
        self.files = {
            ROOT + "pack.json": b'{"options": {"flip_png_files": true}}',
            ROOT + "UI/tex1_64x32_00000000000000A1_4_mip0.png": png(256, 128),        # 4x HUD
            ROOT + "UI/GERMAN/tex1_64x32_00000000000000A2_4_mip0.png": png(256, 128),
            ROOT + "MAPS/tex1_16x16_00000000000000A3_4_mip0.png": png(64, 64),
            ROOT + "ENVIRONMENT/tex1_32x32_00000000000000B1_0_mip0.png": png(128, 128),  # 4x world
            ROOT + "ENVIRONMENT/tex1_32x32_00000000000000B1_0_mip1.png": png(64, 64),
            ROOT + "ENVIRONMENT/readme.txt": b"not a texture",
            ROOT + "CHARACTERS/tex1_16x16_00000000000000C1_0_mip0.png": png(32, 32),     # already 2x
            "OOT3D 4K (1080p)/reshade-shaders/Textures/noise.png": png(16, 16),
        }

    def run_tool(self, *options: str, pack: Path | None = None) -> Path:
        if pack is None:
            pack = self.work / "pack.zip"
            with zipfile.ZipFile(pack, "w") as archive:
                for name, data in self.files.items():
                    archive.writestr(name, data)
        output = self.work / ("out" + str(len(list(self.work.glob("out*")))))
        self.assertEqual(shrink.main([str(pack), str(output), "--jobs", "2", *options]), 0)
        return output

    def test_default_sizes_world_2x_hud_3x(self):
        out = self.run_tool()
        self.assertEqual(size_of(out / "UI/tex1_64x32_00000000000000A1_4_mip0.png"), (192, 96))
        self.assertEqual(size_of(out / "MAPS/tex1_16x16_00000000000000A3_4_mip0.png"), (48, 48))
        self.assertEqual(size_of(out / "ENVIRONMENT/tex1_32x32_00000000000000B1_0_mip0.png"), (64, 64))
        # Within its size: the original bytes, not a re-encode.
        self.assertEqual((out / "CHARACTERS/tex1_16x16_00000000000000C1_0_mip0.png").read_bytes(),
                         self.files[ROOT + "CHARACTERS/tex1_16x16_00000000000000C1_0_mip0.png"])
        self.assertEqual((out / "pack.json").read_bytes(), self.files[ROOT + "pack.json"])
        for gone in ("ENVIRONMENT/tex1_32x32_00000000000000B1_0_mip1.png", "ENVIRONMENT/readme.txt",
                     "UI/GERMAN", "reshade-shaders"):
            self.assertFalse((out / gone).exists(), gone)

    def test_custom_scales(self):
        out = self.run_tool("--world", "1", "--hud", "2")
        self.assertEqual(size_of(out / "UI/tex1_64x32_00000000000000A1_4_mip0.png"), (128, 64))
        self.assertEqual(size_of(out / "ENVIRONMENT/tex1_32x32_00000000000000B1_0_mip0.png"), (32, 32))
        self.assertEqual(size_of(out / "CHARACTERS/tex1_16x16_00000000000000C1_0_mip0.png"), (16, 16))

    def test_parts(self):
        out = self.run_tool("--parts", "hud")
        self.assertTrue((out / "UI/tex1_64x32_00000000000000A1_4_mip0.png").exists())
        self.assertFalse((out / "ENVIRONMENT").exists())
        self.assertFalse((out / "CHARACTERS").exists())
        out = self.run_tool("--parts", "models")
        self.assertEqual(sorted(p.relative_to(out).as_posix() for p in out.rglob("*.png")),
                         ["CHARACTERS/tex1_16x16_00000000000000C1_0_mip0.png"])

    def test_language_folder_kept_and_wins(self):
        # The same texture in UI/ and UI/GERMAN/: the German art is the one kept.
        self.files[ROOT + "UI/GERMAN/tex1_64x32_00000000000000A1_4_mip0.png"] = png(256, 128, (1, 2, 3, 255))
        out = self.run_tool("--language", "german")
        self.assertEqual(size_of(out / "UI/GERMAN/tex1_64x32_00000000000000A2_4_mip0.png"), (192, 96))
        self.assertTrue((out / "UI/GERMAN/tex1_64x32_00000000000000A1_4_mip0.png").exists())
        self.assertFalse((out / "UI/tex1_64x32_00000000000000A1_4_mip0.png").exists())

    def test_alpha_edges_keep_their_colour(self):
        # Opaque red on the left 5 columns, transparent black on the right: the
        # mixed column must stay red (premultiplied), not darken towards black.
        image = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
        for x in range(5):
            for y in range(8):
                image.putpixel((x, y), (255, 0, 0, 255))
        data = io.BytesIO()
        image.save(data, format="PNG")
        self.files[ROOT + "ENVIRONMENT/tex1_2x2_00000000000000D1_0_mip0.png"] = data.getvalue()
        out = self.run_tool()
        with Image.open(out / "ENVIRONMENT/tex1_2x2_00000000000000D1_0_mip0.png") as reduced:
            self.assertEqual(reduced.size, (4, 4))
            partial = [reduced.getpixel((x, y)) for x in range(4) for y in range(4)
                       if 0 < reduced.getpixel((x, y))[3] < 255]
        self.assertTrue(partial)
        for r, g, b, _ in partial:
            self.assertGreaterEqual(r, 250)
            self.assertLessEqual(max(g, b), 2)

    def test_extracted_folder(self):
        folder = self.work / "extracted"
        for name, data in self.files.items():
            if name.startswith(ROOT):
                target = folder / name[len(ROOT):]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
        out = self.run_tool(pack=folder)
        self.assertEqual(size_of(out / "UI/tex1_64x32_00000000000000A1_4_mip0.png"), (192, 96))
        self.assertTrue((out / "pack.json").exists())

    def test_refuses_a_non_empty_output(self):
        output = self.work / "busy"
        output.mkdir()
        (output / "keep.txt").write_text("mine")
        pack = self.work / "pack.zip"
        with zipfile.ZipFile(pack, "w") as archive:
            for name, data in self.files.items():
                archive.writestr(name, data)
        with self.assertRaises(SystemExit):
            shrink.main([str(pack), str(output)])
        self.assertEqual((output / "keep.txt").read_text(), "mine")


if __name__ == "__main__":
    unittest.main()
