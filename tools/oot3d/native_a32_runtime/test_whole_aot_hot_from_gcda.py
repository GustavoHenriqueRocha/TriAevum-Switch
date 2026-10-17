from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from whole_aot_hot_from_gcda import (
    TAG_ARCS,
    TAG_FUNCTION,
    TAG_TIME_PROFILER,
    gcc_profile_id,
    main,
    mangle,
    read_gcda,
)

SIGNATURE = (
    "    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,\n"
    "    Oot3dAotArchitecturalState& state,\n"
    "    uint32_t entryPc)"
)
SHARD = """#include "oot3d_whole_aot_generated_internal.h"

namespace Oot3dNativeGame::GeneratedWholeAot {

Oot3dWholeAotFlow Execute_Other_00200000(
    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
    Oot3dAotArchitecturalState& state,
    uint32_t entryPc);

Oot3dWholeAotFlow Execute_Hot_00100000(
    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
    Oot3dAotArchitecturalState& state,
    uint32_t entryPc) {
    return Execute_Other_00200000(frame, context, state, entryPc);
}

Oot3dWholeAotFlow Execute_Straight_00100100(
    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
    Oot3dAotArchitecturalState& state,
    uint32_t entryPc) {
    return {};
}

[[gnu::optimize("no-schedule-insns", "no-gcse-after-reload")]] Oot3dWholeAotFlow Execute_Cold_00100200(
    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
    Oot3dAotArchitecturalState& state,
    uint32_t entryPc) {
    return {};
}

}
"""


def _record(tag: int, words: list[int] | None = None, zero_counts: int = 0) -> bytes:
    if zero_counts:
        # Contadores todos zero: comprimento negativo e nenhum dado.
        return struct.pack("<Ii", tag, -8 * zero_counts)
    payload = b"".join(struct.pack("<q" if tag != TAG_FUNCTION else "<I", w) for w in words or [])
    return struct.pack("<Ii", tag, len(payload)) + payload


def _function(ident: int, arcs: list[int] | None, arc_zero: int, first_run: int) -> bytes:
    data = _record(TAG_FUNCTION, [ident, 0x1234, 0x5678])
    data += _record(TAG_ARCS, arcs) if arcs else _record(TAG_ARCS, zero_counts=arc_zero)
    data += (_record(TAG_TIME_PROFILER, [first_run]) if first_run
             else _record(TAG_TIME_PROFILER, zero_counts=1))
    return data


class HotFromGcdaTests(unittest.TestCase):
    def test_profile_id_matches_gcc_16(self) -> None:
        # idents gravados pelo aarch64-none-elf-g++ 16.1 nos .gcno dos shards 007 e 091.
        self.assertEqual(
            gcc_profile_id(mangle("Execute_EnHorse_Update_0014A5A8", SIGNATURE)),
            924984334)
        self.assertEqual(
            gcc_profile_id(mangle("Execute_FUN_0046c204_0046C204", SIGNATURE)),
            1027953132)

    def test_gcda_mode_lists_executed_functions(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            generated = root / "gen"
            generated.mkdir()
            (generated / "oot3d_whole_aot_shard_000.cpp").write_text(SHARD, encoding="utf-8")
            ident = {name: gcc_profile_id(mangle(name, SIGNATURE)) for name in (
                "Execute_Hot_00100000", "Execute_Straight_00100100",
                "Execute_Cold_00100200")}
            gcda = struct.pack("<4I", 0x67636461, 0x4236312A, 1, 2)
            gcda += struct.pack("<Ii", 0xA1000000, 8) + struct.pack("<II", 1, 0)
            gcda += _function(ident["Execute_Hot_00100000"], [5, 9], 0, 1)
            # Sem desvios: nenhum contador de arco; só o time_profiler diz que rodou.
            gcda += _function(ident["Execute_Straight_00100100"], None, 1, 2)
            gcda += _function(ident["Execute_Cold_00100200"], None, 2, 0)
            gcda += _function(987654, [77], 0, 3)  # helper inline do runtime
            profiles = root / "pgo"
            profiles.mkdir()
            name = ("CMakeFiles#oot3d_native_whole_aot.dir#home#x#gen#"
                    "oot3d_whole_aot_shard_000.gcda")
            (profiles / name).write_bytes(gcda)
            self.assertEqual(len(read_gcda(profiles / name)), 4)
            output = root / "hot.txt"
            self.assertEqual(main([
                "--gcda-dir", str(profiles), "--generated-dir", str(generated),
                "--output", str(output)]), 0)
            lines = output.read_text(encoding="utf-8").splitlines()
            self.assertEqual([line.split()[0] for line in lines], ["00100000", "00100100"])


if __name__ == "__main__":
    unittest.main()
