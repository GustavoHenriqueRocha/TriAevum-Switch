"""LDM/STM span lowering and PC-relative literal folding.

The span path must be indistinguishable from the word-by-word path it
replaces: the generated code is executed with the span helper accepting and
declining, including faults in the middle of a transfer.
"""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import whole_aot_cpp
from whole_aot_cpp import (
    PROGRAM_FORMAT,
    SELECTION_FORMAT,
    SOURCE_NAME,
    Instruction,
    LiteralImage,
    _emit_block_transfer,
    generate,
)


def words(*values: int) -> bytes:
    return b"".join(value.to_bytes(4, "little") for value in values)


class SpanExecutionTests(unittest.TestCase):
    def test_span_matches_word_by_word(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required for generated-code execution")
        transfers = {
            "Push": 0xE92D40F0,     # stmdb sp!, {r4-r7, lr}
            "Pop": 0xE8BD00F0,      # ldmia sp!, {r4-r7}
            "PopPc": 0xE8BD80F0,    # ldmia sp!, {r4-r7, pc}
            "StoreBase": 0xE8810006,  # stmia r1, {r1, r2}
            "LoadBase": 0xE8B10006,   # ldmia r1!, {r1, r2} (writeback after load)
            "Single": 0xE92D4000,   # stmdb sp!, {lr}: no span
        }
        functions = []
        for name, raw in transfers.items():
            body = "\n".join(_emit_block_transfer(Instruction(0x1000, raw)))
            functions.append(
                f"Flow {name}(Context& context, State& state) {{\n{body}\n"
                "return {0, 0};\n}")
        source = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

struct Flow { int Kind; uint32_t Value; };
struct Bus {
    std::array<uint8_t, 8192> Bytes{};
    uint32_t End = 8192;
    bool AllowSpan = true;
    unsigned Words = 0, Spans = 0;
    template<class T> bool ReadFast(uint32_t address, T* value) {
        ++Words;
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(value, Bytes.data() + address, sizeof(T)); return true;
    }
    template<class T> bool WriteFast(uint32_t address, T value) {
        ++Words;
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(Bytes.data() + address, &value, sizeof(T)); return true;
    }
    bool ReadWordsFast(uint32_t address, uint32_t* values, uint32_t count) {
        if (!AllowSpan || address > End || count * 4 > End - address) return false;
        ++Spans; std::memcpy(values, Bytes.data() + address, count * 4); return true;
    }
    bool WriteWordsFast(uint32_t address, const uint32_t* values, uint32_t count) {
        if (!AllowSpan || address > End || count * 4 > End - address) return false;
        ++Spans; std::memcpy(Bytes.data() + address, values, count * 4); return true;
    }
};
struct Context { Bus Memory; uint32_t FaultPc = 0, FaultAddress = 0; };
struct State { uint32_t R[16]{}; };
Flow Oot3dAotMemoryFault(Context& c, uint32_t pc, uint32_t address) {
    c.FaultPc = pc; c.FaultAddress = address; return {-1, address};
}
Flow Oot3dAotReturned(uint32_t value) { return {1, value}; }
""" + "\n".join(functions) + r"""
using Run = Flow (*)(Context&, State&);
struct Outcome { Flow flow; State state; std::array<uint8_t, 8192> bytes; uint32_t faultAddress; };
Outcome Execute(Run run, bool span, uint32_t end, unsigned* spans) {
    Context c; State s;
    for (unsigned i = 0; i < c.Memory.Bytes.size(); ++i) c.Memory.Bytes[i] = uint8_t(i * 13 + 7);
    for (unsigned r = 0; r < 15; ++r) s.R[r] = 0x01010101U * (r + 1);
    s.R[13] = 0x1000; s.R[1] = 0x0F00;
    c.Memory.End = end; c.Memory.AllowSpan = span;
    Flow flow = run(c, s);
    *spans += c.Memory.Spans;
    return {flow, s, c.Memory.Bytes, c.FaultAddress};
}
int main() {
    Run runs[] = {Push, Pop, PopPc, StoreBase, LoadBase, Single};
    unsigned spans = 0;
    for (Run run : runs) {
        // Ends inside every word of the transfers around 0x0F00/0x0FEC/0x1000.
        for (uint32_t end = 0x0EF0; end <= 0x1020; ++end) {
            unsigned ignored = 0;
            Outcome a = Execute(run, true, end, &spans);
            Outcome b = Execute(run, false, end, &ignored);
            assert(a.flow.Kind == b.flow.Kind && a.flow.Value == b.flow.Value);
            assert(std::memcmp(a.state.R, b.state.R, sizeof a.state.R) == 0);
            assert(a.bytes == b.bytes);
            assert(a.faultAddress == b.faultAddress);
        }
    }
    assert(spans != 0);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "spans.cpp"
            executable = root / ("spans.exe" if os.name == "nt" else "spans")
            cpp.write_text(source, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-O2", str(cpp), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=120)
            subprocess.run([str(executable)], check=True, capture_output=True, timeout=60)

    def test_single_register_keeps_word_path(self) -> None:
        lines = "\n".join(_emit_block_transfer(Instruction(0x1000, 0xE92D4000)))
        self.assertNotIn("WordsFast", lines)
        lines = "\n".join(_emit_block_transfer(Instruction(0x1000, 0xE92D40F0)))
        self.assertIn("WriteWordsFast(firstAddress, spanWords, 5U)", lines)


class LiteralFoldingTests(unittest.TestCase):
    # ldr r0, [pc, #4]; ldrb r1, [pc, #1]; bx lr; literal word
    CODE = words(0xE59F0004, 0xE5DF1001, 0xE12FFF1E, 0x12345678)

    def write_inputs(self, root: Path, executable_size: int | None) -> tuple[Path, Path, Path]:
        code_path = root / "code.bin"
        program_path = root / "program.json"
        selection_path = root / "selection.json"
        code_path.write_bytes(self.CODE)
        program = {
            "format": PROGRAM_FORMAT,
            "base": 0x1000,
            "code_sha256": hashlib.sha256(self.CODE).hexdigest(),
            "blocks": [{
                "id": 0, "pc": 0x1000, "end_pc": 0x100C,
                "successors": [{"site": 0x1008, "kind": "return"}],
            }],
            "functions": [{
                "entry": 0x1000, "name": "Literal", "blocks": [0],
                "closed_static_cfg": True, "direct_calls": [], "tail_calls": [],
                "indirect_sites": [],
            }],
        }
        if executable_size is not None:
            program["executable_size"] = executable_size
        program_path.write_text(json.dumps(program), encoding="utf-8")
        selection_path.write_text(json.dumps(
            {"format": SELECTION_FORMAT, "functions": [{"entry": 0x1000}]}), encoding="utf-8")
        return program_path, selection_path, code_path

    def test_text_literals_fold_to_code_bytes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            manifest = generate(*self.write_inputs(root, 16), root / "output")
            source = (root / "output" / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("const uint32_t value = 0x12345678U; // literal at 0x0000100C", source)
            self.assertIn("const uint8_t value = 0x56U; // literal at 0x0000100D", source)
            self.assertNotIn("ReadFast", source)
            self.assertTrue(manifest["literal_folding"])
            self.assertEqual(manifest["folded_literal_addresses"], 2)

    def test_opt_out_and_outside_text_keep_memory_reads(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            inputs = self.write_inputs(root, 16)
            manifest = generate(*inputs, root / "off", fold_literals=False)
            source = (root / "off" / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ReadFast<uint32_t>", source)
            self.assertFalse(manifest["literal_folding"])
            # The literal lies beyond a 12-byte text segment.
            generate(*self.write_inputs(root, 12), root / "short")
            source = (root / "short" / SOURCE_NAME).read_text(encoding="utf-8")
            self.assertIn("ReadFast<uint32_t>", source)
            self.assertNotIn("// literal at", source)

    def test_host_written_words_are_excluded(self) -> None:
        image = LiteralImage(0x1000, bytes(range(16)), excluded=((0x1008, 4),))
        self.assertEqual(image.word(0x1004, 4), 0x07060504)
        self.assertIsNone(image.word(0x1008, 4))
        self.assertIsNone(image.word(0x1006, 4))
        self.assertIsNone(image.word(0x100B, 1))
        self.assertIsNone(image.word(0x100E, 4))
        self.assertIn((0x003CC430, 4), whole_aot_cpp.HOST_WRITTEN_TEXT_RANGES)


if __name__ == "__main__":
    unittest.main()
