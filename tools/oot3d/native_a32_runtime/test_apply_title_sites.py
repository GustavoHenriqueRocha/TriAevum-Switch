"""Testes do apply_title_sites.py com C++ sintético no formato do whole-AOT: python -I test_apply_title_sites.py"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import apply_title_sites as ts  # noqa: E402


def block(pc: int, body: str = "            r0 = r0 + 1U;") -> list[str]:
    return [f"block_{pc:08X}:"] + ts.enter_block(pc) + ["        {", body, "        }", "    }"]


def function(name: str, pcs: list[int]) -> list[str]:
    lines = [f"Oot3dWholeAotFlow Execute_{name}(", "    uint32_t entryPc) {"]
    for pc in pcs:
        lines += block(pc)
    return lines + ["    commitState();", "    return {Oot3dWholeAotFlowKind::Unsupported, 0U, 0U};", "}"]


def shard(*functions: list[str]) -> str:
    lines = ['#include "oot3d_whole_aot_generated_internal.h"', "", "namespace Oot3dNativeGame::GeneratedWholeAot {", ""]
    for f in functions:
        lines += f + [""]
    return "\n".join(lines + ["} // namespace Oot3dNativeGame::GeneratedWholeAot"]) + "\n"


class Aplicar(unittest.TestCase):
    def setUp(self):
        # f: sítio 0x100 e o PC seguinte 0x104 (rótulos dela); g: 0x104 (rótulo de outra função) e 0x200.
        self.original = shard(function("f", [0x100, 0x104, 0x108]), function("g", [0x104, 0x200]))
        self.pcs = {0x100, 0x104}

    def test_prologo_nos_blocos_de_sitio(self):
        text, count = ts.apply(self.original, self.pcs)
        self.assertEqual(count, 3)  # 0x100 e 0x104 em f, 0x104 em g
        self.assertEqual(text.count("Oot3dAotTitleSite(context, frame, state, 0x00000100U"), 1)
        self.assertEqual(text.count("Oot3dAotTitleSite(context, frame, state, 0x00000104U"), 2)
        self.assertNotIn("0x00000108U, commitState, reloadState, titleSiteNext", text)
        self.assertTrue(text.endswith(ts.REGISTER + "\n"))

    def test_prologo_logo_depois_do_enter_block(self):
        lines = ts.apply(self.original, self.pcs)[0].split("\n")
        at = lines.index("block_00000100:")
        self.assertEqual(lines[at + 1:at + 6], ts.enter_block(0x100))
        self.assertEqual(lines[at + 6], ts.BEGIN)

    def test_goto_so_para_rotulos_do_csv_na_mesma_funcao(self):
        text = ts.apply(self.original, self.pcs)[0]
        f, g = text.split("Oot3dWholeAotFlow Execute_g(")
        self.assertIn("case 0x00000104U: goto block_00000104;", f)
        self.assertIn("case 0x00000100U: goto block_00000100;", f)
        self.assertNotIn("goto block_00000108;", f)  # bloco da função, mas fora do CSV
        self.assertNotIn("goto block_00000100;", g)  # 0x100 não é rótulo de g
        self.assertIn("return {Oot3dWholeAotFlowKind::Branch, titleSiteNext, 0U};", g)

    def test_idempotente_e_remove(self):
        once = ts.apply(self.original, self.pcs)[0]
        self.assertEqual(ts.apply(once, self.pcs)[0], once)
        self.assertEqual(ts.strip(once), self.original)

    def test_sem_sitio_nao_muda(self):
        text, count = ts.apply(self.original, {0x999})
        self.assertEqual((text, count), (self.original, 0))

    def test_recusa_bloco_fora_do_formato(self):
        broken = self.original.replace("return Oot3dAotBlockLimit(context, 0x00000100U);", "return x;")
        with self.assertRaises(ValueError):
            ts.apply(broken, self.pcs)

    def test_pcs_do_csv(self):
        csv = "entry,size,name\n0x00104DA4,1780,a\n0x0011EE04,8,b\n"
        self.assertEqual(ts.site_pcs(csv), {0x00104DA4, 0x0011EE04})


if __name__ == "__main__":
    unittest.main()
