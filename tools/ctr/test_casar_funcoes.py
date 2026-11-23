"""Testes do casamento de funções (casar_funcoes.py) com programas A32 montados à mão."""
from __future__ import annotations

import csv
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import casar_funcoes as cf  # noqa: E402

BASE = 0x00100000
PUSH, POP, BX_LR = 0xE92D4010, 0xE8BD8010, 0xE12FFF1E


def bl(pc: int, target: int) -> int:
    return 0xEB000000 | (((target - pc - 8) >> 2) & 0x00FFFFFF)


def ldr_r0_pc(pc: int, literal: int) -> int:
    return 0xE59F0000 | (literal - pc - 8)


def program(padding: bool, field: int, literal: int, extra: bool):
    """Monta o mesmo "jogo" em duas versões.

    A versão de destino (padding=True) tem uma função nova no começo (desloca tudo),
    offsets de struct diferentes em f_y (field) e outro endereço no literal pool.
    Devolve (bytes, {nome: entrada}).
    """
    layout = []
    if padding:
        layout.append(("pad", 3))
    layout += [("main", 6), ("x", 4), ("y", 4), ("dup1", 2), ("dup2", 2)]
    if extra:
        layout.append(("so_origem", 2))
    entry, at = {}, BASE
    for name, words in layout:
        entry[name] = at
        at += words * 4
    words: list[int] = []
    for name, _ in layout:
        e = entry[name]
        if name == "pad":
            words += [0xE3A03009, 0xE2833001, BX_LR]
        elif name == "main":
            words += [PUSH, bl(e + 4, entry["x"]), bl(e + 8, entry["y"]),
                      ldr_r0_pc(e + 12, e + 20), POP, literal]
        elif name == "x":
            words += [0xE3A00001, 0xE2801005, bl(e + 8, entry["dup1"]), BX_LR]
        elif name == "y":
            words += [0xE5901000 | field, 0xE5801000 | (field + 4), bl(e + 8, entry["dup2"]), BX_LR]
        elif name in ("dup1", "dup2"):
            words += [0xE3A00000, BX_LR]
        elif name == "so_origem":
            words += [0xE3A02007, BX_LR]
    return struct.pack(f"<{len(words)}I", *words), entry


def image(code: bytes) -> cf.Image:
    return cf.Image(code, BASE, len(code), BASE + len(code) + 0x1000)


def sides():
    code_a, ea = program(padding=False, field=0x10, literal=BASE + 0x10, extra=True)
    code_b, eb = program(padding=True, field=0x18, literal=BASE + 0x0C, extra=False)
    src = cf.build_functions(image(code_a), {e: None for e in ea.values()})
    dst = cf.build_functions(image(code_b), {e: None for e in eb.values()})
    return src, dst, ea, eb


class NormalizeTest(unittest.TestCase):
    def test_branch_offset_and_image_address_are_masked(self):
        img = image(bytes(0x100))
        self.assertEqual(cf.normalize(bl(BASE, BASE + 0x40), img, False),
                         cf.normalize(bl(BASE, BASE + 0x80), img, False))
        self.assertEqual(cf.normalize(BASE + 0x10, img, False), cf.ADDRESS_TOKEN)
        self.assertEqual(cf.normalize(0x00000005, img, False), 0x00000005)  # constante pequena fica

    def test_struct_offset_only_masked_when_loose(self):
        img = image(bytes(0x100))
        self.assertNotEqual(cf.normalize(0xE5901010, img, False), cf.normalize(0xE5901018, img, False))
        self.assertEqual(cf.normalize(0xE5901010, img, True), cf.normalize(0xE5901018, img, True))

    def test_bl_target_decoding(self):
        self.assertEqual(cf.branch_target(BASE + 8, bl(BASE + 8, BASE)), BASE)
        self.assertEqual(cf.branch_target(BASE, bl(BASE, BASE + 0x1000)), BASE + 0x1000)


class MatchTest(unittest.TestCase):
    def setUp(self):
        self.src, self.dst, self.ea, self.eb = sides()
        self.matches = cf.match(self.src, self.dst)

    def pair(self, name):
        return self.matches.get(self.ea[name])

    def test_levels(self):
        self.assertEqual(self.pair("main"), (self.eb["main"], "exato"))
        self.assertEqual(self.pair("x"), (self.eb["x"], "exato"))
        # offset de struct diferente: não pode casar no exato, só no frouxo
        self.assertEqual(self.pair("y"), (self.eb["y"], "frouxo"))
        # corpos idênticos (não únicos) só se resolvem pela vizinhança no call graph
        self.assertEqual(self.pair("dup1"), (self.eb["dup1"], "vizinhanca"))
        self.assertEqual(self.pair("dup2"), (self.eb["dup2"], "vizinhanca"))
        self.assertIsNone(self.pair("so_origem"))

    def test_no_destination_used_twice(self):
        destinations = [b for b, _ in self.matches.values()]
        self.assertEqual(len(destinations), len(set(destinations)))
        self.assertEqual(cf.order_consistency(self.matches), 1.0)

    def test_hook_inside_function_moves_with_it(self):
        mapped = cf.map_address(self.ea["y"] + 4, self.src, self.dst, self.matches)
        self.assertEqual(mapped["destino"], f"0x{self.eb['y'] + 4:08X}")
        self.assertTrue(mapped["offset_confiavel"])
        self.assertEqual(cf.map_address(self.ea["so_origem"], self.src, self.dst, self.matches)["nivel"],
                         "sem_par")


def recompiled(registers: tuple[int, int], extra: bool, decoy: bool):
    """Mesma função "compilada" com outros registradores; traços: string, float e imediatos.

    O texto ocupa 0x40 bytes; a string "Majora" fica logo depois (fora do .text).
    """
    rl, rm = registers
    text = BASE + 0x40
    body = [0xE59F0000 | rl << 12 | 16,       # ldr rl, [pc, #16] -> ponteiro da string
            0xED9F0A04 | rl << 12,            # vldr s(2*rl), [pc, #16] -> 0.5f
            0xE3A00B01 | rm << 12,            # mov rm, #0x400
            0xE2800A01 | rm << 16 | rm << 12,  # add rm, rm, #0x1000
            BX_LR, 0xE320F000, text, 0x3F000000]
    words = list(body)
    entries = [BASE]
    if extra:  # instrução a mais no fim do corpo não muda os traços
        words[5] = 0xE1A03000
    if decoy:  # só a string e o 0x400 em comum, float diferente
        entries.append(BASE + 4 * len(words))
        words += [0xE59F0008, 0xE3A01B01, BX_LR, 0xE320F000, text, 0x40000000]
    words += [0xE320F000] * ((0x40 - 4 * len(words)) // 4)
    code = struct.pack(f"<{len(words)}I", *words) + b"Majora\0\0"
    img = cf.Image(code, BASE, 0x40, BASE + len(code) + 0x100)
    return cf.build_functions(img, {e: None for e in entries})


class SimilarityTest(unittest.TestCase):
    def test_recompiled_function_matches_by_features_not_hash(self):
        src = recompiled((0, 1), extra=False, decoy=False)
        dst = recompiled((4, 2), extra=True, decoy=True)
        self.assertEqual(src[BASE].features, dst[BASE].features)
        self.assertNotEqual(src[BASE].loose, dst[BASE].loose)
        self.assertIn("s:Majora", src[BASE].features)
        self.assertIn("f:3f000000", src[BASE].features)
        self.assertIn("i:400", src[BASE].features)
        self.assertEqual(cf.match(src, dst), {BASE: (BASE, "semelhanca")})

    def test_hook_offset_not_trusted_across_recompilation(self):
        # Mesmo tamanho não basta: recompilado, o offset do meio cai em outra instrução
        # (no MM3D, 0x003FD3A8 do OoT3D + mesmo offset caiu num literal pool).
        src = recompiled((0, 1), extra=False, decoy=False)
        dst = recompiled((4, 2), extra=False, decoy=False)
        matches = cf.match(src, dst)
        self.assertEqual(matches, {BASE: (BASE, "semelhanca")})
        self.assertEqual(src[BASE].size, dst[BASE].size)
        self.assertFalse(cf.map_address(BASE + 4, src, dst, matches)["offset_confiavel"])
        self.assertTrue(cf.map_address(BASE, src, dst, matches)["offset_confiavel"])

    def test_too_few_features_never_match(self):
        src = recompiled((0, 1), extra=False, decoy=False)
        dst = recompiled((4, 2), extra=True, decoy=False)
        for func in (*src.values(), *dst.values()):
            func.features = cf.Counter({"s:Majora": 1})
        self.assertEqual(cf.match(src, dst), {})


class CliTest(unittest.TestCase):
    def test_end_to_end_with_csv_inventories(self):
        code_a, ea = program(padding=False, field=0x10, literal=BASE + 0x10, extra=True)
        code_b, eb = program(padding=True, field=0x18, literal=BASE + 0x0C, extra=False)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = []
            for side, code, entries in (("origem", code_a, ea), ("destino", code_b, eb)):
                exheader = bytearray(0x800)
                struct.pack_into("<3I", exheader, 0x10, BASE, 1, len(code))
                (root / f"{side}.bin").write_bytes(code)
                (root / f"{side}.exh").write_bytes(exheader)
                with (root / f"{side}.csv").open("w", newline="") as handle:
                    writer = csv.writer(handle)
                    writer.writerow(("profile", "entry", "end", "size", "name", "status"))
                    writer.writerow(("title", f"0x{BASE + 4:08X}", "", "", "dado", "data_pointer"))
                    for name, at in entries.items():
                        writer.writerow(("title", f"0x{at:08X}", "", "", name, "direct_target"))
                args += [f"--{side}-code", root / f"{side}.bin", f"--{side}-exheader", root / f"{side}.exh",
                         f"--{side}-inventario", root / f"{side}.csv"]
            with (root / "alvos.csv").open("w", newline="") as handle:
                handle.write(f"address,item,name\n0x{ea['y'] + 4:08X},gancho,teste\n")
            result = subprocess.run([sys.executable, "-I", Path(__file__).with_name("casar_funcoes.py"), *args,
                                     "--alvos", root / "alvos.csv", "--saida-alvos", root / "out.csv"],
                                    check=True, capture_output=True, text=True)
            summary = json.loads(result.stdout)
            self.assertEqual(summary["pares"], 5)
            self.assertEqual(summary["alvos_por_nivel"], {"frouxo": 1})
            with (root / "out.csv").open(newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(rows[0]["destino"], f"0x{eb['y'] + 4:08X}")


if __name__ == "__main__":
    unittest.main()
