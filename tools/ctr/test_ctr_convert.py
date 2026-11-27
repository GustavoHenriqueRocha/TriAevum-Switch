"""ctr_convert repassa os fatos do título à descoberta (sem ROM: run é trocado)."""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ctr_convert  # noqa: E402


def convert(scratch: str, extra: list[str], fresh: bool = False) -> list[tuple[str, list[str]]]:
    """Roda o main com o run trocado; devolve (etapa, comando) de cada etapa."""
    work, rom = Path(scratch) / "w", Path(scratch) / "jogo.3ds"
    extract = work / "extract"
    extract.mkdir(parents=True, exist_ok=True)
    for name in ("exheader.bin", "code.bin", "romfs.bin"):
        (extract / name).write_bytes(b"")
    rom.write_bytes(b"")
    calls: list[tuple[str, list[str]]] = []
    argv = ["ctr_convert.py", "--work", str(work), *extra, str(rom)]
    with mock.patch.object(sys, "argv", argv), \
            mock.patch.object(ctr_convert, "fresh", return_value=fresh), \
            mock.patch.object(ctr_convert.ncch_extract, "extract",
                              return_value={"product": "t", "program_id": 0, "code_size": 0}), \
            mock.patch.object(ctr_convert, "run",
                              side_effect=lambda step, cmd, **_: calls.append((step, list(map(str, cmd))))):
        ctr_convert.main()
    return calls


class CtrConvertTest(unittest.TestCase):
    # O MM3D saía só com --shards/--shard-strategy (25/11): sem o VFP nativo, o
    # zero exato e a pilha enxuta que o OoT3D usa no Switch.
    def test_generator_options_reach_generate(self) -> None:
        options = ["--native-vfp-aarch64", "--vfp-zero-aware", "--lean-stack"]
        with tempfile.TemporaryDirectory() as scratch:
            calls = convert(scratch, options)
        generate = next(cmd for step, cmd in calls if step == "generate")
        for option in options:
            self.assertIn(option, generate)

    # O C++ em dia pelas datas mas gerado com outras opções tem de ser regerado.
    def test_changed_generator_options_regenerate(self) -> None:
        with tempfile.TemporaryDirectory() as scratch:
            convert(scratch, [])
            calls = convert(scratch, ["--lean-stack"], fresh=True)
            self.assertIn("generate", [step for step, _ in calls])
            calls = convert(scratch, ["--lean-stack"], fresh=True)
            self.assertNotIn("generate", [step for step, _ in calls])


if __name__ == "__main__":
    unittest.main()
