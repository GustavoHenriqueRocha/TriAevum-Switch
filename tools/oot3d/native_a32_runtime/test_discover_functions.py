"""Regressão da descoberta de funções sem inventário (discover_functions.py).

Super Mario 3D Land (05/11/2026): 0x289EBC monta o callback 0x289EA8 com
"ldr r1, [pc, #imm]; add r1, pc, r1", sem o endereço aparecer como literal;
o boot parou com MissingBlock nele (e em 0x10CA24, que vem logo depois de um
literal). O mesmo padrão aponta para uma tabela de dados depois do literal
pool em 0x3A0DAC, que semeada como código derrubava 600 funções.
"""
from __future__ import annotations

import struct
import unittest

import discover_functions as discover

BASE = 0x100000


def image(words: list[int]) -> tuple[bytes, bytes]:
    code = struct.pack(f"<{len(words)}I", *words).ljust(0x1000, b"\0")
    exheader = bytearray(0x40)
    struct.pack_into("<3I", exheader, 0x10, BASE, 1, len(words) * 4)
    struct.pack_into("<3I", exheader, 0x20, BASE + 0x1000, 0, 0)
    struct.pack_into("<3I", exheader, 0x30, BASE + 0x1000, 0, 0)
    return code, bytes(exheader)


# 0x100000 entrada: guarda em [r0] um callback montado relativo ao PC (logo
# depois de um literal, como 0x10CA24) e salta (sem retorno) para 0x10001C,
# que lê uma tabela de dados endereçada do mesmo jeito.
WORDS = [
    0xE59F1008,  # 0x100000 ldr r1, [pc, #8]      ; literal 0x100010
    0xE08F1001,  # 0x100004 add r1, pc, r1        ; 0x10000C + 8 = 0x100014
    0xE5801000,  # 0x100008 str r1, [r0]
    0xEA000002,  # 0x10000C b 0x10001C
    0x00000008,  # 0x100010 literal
    0xE3A00001,  # 0x100014 callback: mov r0, #1
    0xE12FFF1E,  # 0x100018 bx lr
    0xE59F4008,  # 0x10001C ldr r4, [pc, #8]      ; literal 0x10002C
    0xE08F4004,  # 0x100020 add r4, pc, r4        ; 0x100028 + 8 = 0x100030
    0xE5940000,  # 0x100024 ldr r0, [r4]
    0xE12FFF1E,  # 0x100028 bx lr
    0x00000008,  # 0x10002C literal
    0x12345678,  # 0x100030 tabela de dados
    0x9ABCDEF0,
    # 0x100038: callback passado como argumento (r3) a uma chamada, como o
    # 0x10A470 que 0x10922C passa ao scanner 0x289EBC.
    0xE59F3008,  # 0x100038 ldr r3, [pc, #8]      ; literal 0x100048
    0xE08F3003,  # 0x10003C add r3, pc, r3        ; 0x100044 + 8 = 0x10004C
    0xEBFFFFF4,  # 0x100040 bl 0x100018
    0xE12FFF1E,  # 0x100044 bx lr
    0x00000008,  # 0x100048 literal
    0xE3A00002,  # 0x10004C callback: mov r0, #2
    0xE12FFF1E,  # 0x100050 bx lr
]


class DiscoverFunctionsTest(unittest.TestCase):
    def test_pc_relative_pattern_is_found(self) -> None:
        code, exheader = image(WORDS)
        pointers = discover.pc_relative_code_pointers(code, discover.code_segments(exheader))
        self.assertEqual(pointers, {0x100014, 0x10004C})

    def test_pc_relative_callback_becomes_function_but_data_table_does_not(self) -> None:
        code, exheader = image(WORDS)
        result, _ = discover.discover(code, exheader, "teste")
        entries = {function.entry: function.origin for function in result.functions}
        self.assertEqual(entries.get(0x100014), "pc_relative")
        self.assertNotIn(0x100030, entries)
        self.assertEqual(result.to_manifest(False)["counts"]["unclaimed_blocks"], 0)

    def test_manual_call_with_halfword_load_keeps_its_return(self) -> None:
        # 0x10C9E4 no Mario: "add lr, pc, #8", argumento via LDRH, "mov pc, r2".
        code, exheader = image([
            0xE28FE004,  # 0x100000 add lr, pc, #4    ; lr = 0x10000C
            0xE1DD00B8,  # 0x100004 ldrh r0, [sp, #8]
            0xE1A0F002,  # 0x100008 mov pc, r2
            0xE3A00003,  # 0x10000C retorno: mov r0, #3
            0xE12FFF1E,  # 0x100010 bx lr
        ])
        result, _ = discover.discover(code, exheader, "teste")
        blocks = result.to_manifest(False)["blocks"]
        self.assertIn(0x10000C, {int(block["pc"]) for block in blocks})


if __name__ == "__main__":
    unittest.main()
