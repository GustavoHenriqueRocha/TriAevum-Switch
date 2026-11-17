"""Regressão de switch_release.py com NRO e arquivos sintéticos (sem dados do jogo).

    python3 -m unittest tools/oot3d/test_switch_release.py
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import switch_release  # noqa: E402


def fake_nro(version: str, name: str = "Ocarina", author: str = "Gustavo") -> bytes:
    """NRO mínimo: cabeçalho NRO0 com o tamanho, depois ASET com a NACP."""
    body = bytearray(0x80)
    struct.pack_into("<4sII", body, 0x10, b"NRO0", 0, len(body))
    nacp = bytearray(0x4000)
    nacp[0:len(name)] = name.encode()
    nacp[0x200:0x200 + len(author)] = author.encode()
    nacp[0x3060:0x3060 + len(version)] = version.encode()
    aset = bytearray(0x38)
    struct.pack_into("<4sI", aset, 0, b"ASET", 0)
    struct.pack_into("<QQ", aset, 0x18, len(aset), len(nacp))  # nacp (icone vazio em 0x08)
    return bytes(body + aset + nacp)


class SwitchReleaseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.stage = self.root / "stage"
        for rel, data in {
            "oot3d_native_game.nro": fake_nro("1.0.0"),
            "shadercache/deko3d.pack": b"pack",
            "resources.o2r": b"o2r",
            "mods/ptbr/romfs/menu/06_EU_SPANISH/hud_all00.ctxb": b"a",
            "mods/ptbr/romfs/menu/07_US_SPANISH/hud_all00.ctxb": b"b",
            "config/renderer": b"deko3d\n",
        }.items():
            path = self.stage / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        self.out = self.root / "out"

    def tearDown(self):
        self.tmp.cleanup()

    def test_nacp(self):
        self.assertEqual(switch_release.nacp_of(fake_nro("1.2.3", "Nome", "Autor")), ("Nome", "Autor", "1.2.3"))
        with self.assertRaises(ValueError):
            switch_release.nacp_of(b"\0" * 0x40)

    def test_release_json_lists_only_updater_files(self):
        switch_release.build(self.stage, self.out, "1.0.0", 1, "test")
        manifest = json.loads((self.out / "release.json").read_text())
        self.assertEqual(manifest["format"], "oot3dre_switch_release_v1")
        self.assertEqual((manifest["version"], manifest["build"], manifest["channel"]), ("1.0.0", 1, "test"))
        paths = [f["path"] for f in manifest["files"]]
        self.assertEqual(paths, ["oot3d_native_game.nro", "shadercache/deko3d.pack", "resources.o2r"])
        pack = next(f for f in manifest["files"] if f["path"] == "shadercache/deko3d.pack")
        self.assertEqual((pack["size"], pack["sha256"]), (4, hashlib.sha256(b"pack").hexdigest()))
        # Cada arquivo do updater vira um asset com o nome do último componente.
        self.assertEqual((self.out / "deko3d.pack").read_bytes(), b"pack")

    def test_zip_has_the_whole_install_under_switch_oot3dre(self):
        switch_release.build(self.stage, self.out, "1.0.0", 1, "test")
        with zipfile.ZipFile(self.out / "oot3dre-1.0.0-switch.zip") as archive:
            names = set(archive.namelist())
        self.assertIn("switch/oot3dre/mods/ptbr/romfs/menu/06_EU_SPANISH/hud_all00.ctxb", names)
        self.assertIn("switch/oot3dre/mods/ptbr/romfs/menu/07_US_SPANISH/hud_all00.ctxb", names)
        self.assertIn("switch/oot3dre/config/renderer", names)
        self.assertIn("switch/oot3dre/oot3d_native_game.nro", names)

    def test_refuses_what_must_never_ship(self):
        # game/ (o jogo convertido), saves, a trava da importação e o que vem do TopScreen.
        for rel in ("game/romfs.bin", "savedata/save00.bin", "config/no_rom_import",
                    "config/atlas_overrides.o3tu", "config/font_coverage.zip", "config/update_token"):
            with self.subTest(rel=rel):
                path = self.stage / rel
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"x")
                with self.assertRaises(ValueError):
                    switch_release.build(self.stage, self.out, "1.0.0", 1, "test")
                path.unlink()

    def test_version_must_match_the_nro(self):
        with self.assertRaises(ValueError):
            switch_release.build(self.stage, self.out, "1.0.1", 1, "test")

    def test_build_and_channel(self):
        for build, channel in ((0, "test"), (1, "beta")):
            with self.assertRaises(ValueError):
                switch_release.build(self.stage, self.out, "1.0.0", build, channel)

    def test_promote_rewrites_only_the_channel(self):
        switch_release.build(self.stage, self.out, "1.0.0", 1, "test")
        before = json.loads((self.out / "release.json").read_text())
        switch_release.promote(self.out)
        after = json.loads((self.out / "release.json").read_text())
        self.assertEqual(after["channel"], "stable")
        self.assertEqual({**after, "channel": "test"}, before)
        sums = (self.out / "SHA256SUMS.txt").read_text()
        self.assertIn(f"{hashlib.sha256((self.out / 'release.json').read_bytes()).hexdigest()}  release.json", sums)


if __name__ == "__main__":
    unittest.main()
