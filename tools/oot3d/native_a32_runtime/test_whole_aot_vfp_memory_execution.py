"""Execute generated VFP memory operations, not just their source spelling."""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import whole_aot_cpp
from whole_aot_cpp import Instruction, _emit_vfp


class WholeAotVfpMemoryExecutionTests(unittest.TestCase):
    def test_generated_transfers(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required for generated-code execution")

        functions = []
        cases = []
        for lane in range(32):
            for double in (False, True):
                if double and lane % 2:
                    continue
                for positive in (False, True):
                    for load in (False, True):
                        raw = (0x1D020A03 | (int(positive) << 23) | (int(load) << 20)
                               | ((lane // 2) << 12) | (int(double) << 8))
                        if not double:
                            raw |= (lane % 2) << 22
                        index = len(functions)
                        body = "\n".join(_emit_vfp(Instruction(0x1000, raw)))
                        functions.append(f"int Run{index}(Frame& frame, Context& context, State& state) {{\n{body}\nreturn 0;\n}}")
                        cases.append(f"Check(Run{index}, {lane}, {str(double).lower()}, {str(positive).lower()}, {str(load).lower()});")

        literal = "\n".join(_emit_vfp(Instruction(0x1000, 0xED9FFB03)))
        source = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

struct Bus {
    std::array<uint8_t, 8192> Bytes{};
    uint32_t End = Bytes.size();
    unsigned Transfers = 0, Width = 0;
    template<class T> bool ReadFast(uint32_t address, T* value) {
        ++Transfers; Width = sizeof(T);
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(value, Bytes.data() + address, sizeof(T)); return true;
    }
    template<class T> bool WriteFast(uint32_t address, T value) {
        ++Transfers; Width = sizeof(T);
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(Bytes.data() + address, &value, sizeof(T)); return true;
    }
};
struct Frame { struct { std::array<uint32_t, 32> vfp; } Guest; };
struct Context { Bus Memory; uint32_t FaultPc = 0, FaultAddress = 0; };
struct State {
    uint32_t R[16]{};
    struct { bool Zero = false; bool Z() const { return Zero; } } Flags;
};
int Oot3dAotMemoryFault(Context& c, uint32_t pc, uint32_t address) {
    c.FaultPc = pc; c.FaultAddress = address; return -1;
}
using Run = int (*)(Frame&, Context&, State&);
void Check(Run run, unsigned lane, bool isDouble, bool positive, bool load) {
    constexpr uint32_t address = 100; // 4-byte aligned, deliberately not 8-byte aligned.
    constexpr uint32_t low = 0x89ABCDEF, high = 0x400921FB, sentinel = 0xDEADC0DE;
    for (bool fault : {false, true}) for (bool skip : {false, true}) {
        Frame f; f.Guest.vfp.fill(sentinel);
        Context c; c.Memory.Bytes.fill(0xA5);
        State s; s.R[2] = positive ? address - 12 : address + 12; s.Flags.Zero = skip;
        std::memcpy(c.Memory.Bytes.data() + address, &low, 4);
        std::memcpy(c.Memory.Bytes.data() + address + 4, &high, 4);
        if (!load) { f.Guest.vfp[lane] = low; if (isDouble) f.Guest.vfp[lane + 1] = high; }
        auto before = f.Guest.vfp;
        auto memoryBefore = c.Memory.Bytes;
        if (fault) c.Memory.End = address + (isDouble ? 4 : 0);
        assert(run(f, c, s) == (fault && !skip ? -1 : 0));
        assert(c.Memory.Transfers == (skip ? 0U : 1U));
        if (!skip) assert(c.Memory.Width == (isDouble ? 8U : 4U));
        if (fault || skip) {
            assert(f.Guest.vfp == before); assert(c.Memory.Bytes == memoryBefore);
            if (fault && !skip) { assert(c.FaultPc == 0x1000); assert(c.FaultAddress == address); }
        } else if (load) {
            before[lane] = low; if (isDouble) before[lane + 1] = high;
            assert(f.Guest.vfp == before); assert(c.Memory.Bytes == memoryBefore);
        } else {
            assert(f.Guest.vfp == before); assert(c.Memory.Bytes == memoryBefore);
        }
        assert(s.R[2] == (positive ? address - 12 : address + 12));
    }
}
"""
        source += "\n".join(functions)
        source += f"\nint Literal(Frame& frame, Context& context, State& state) {{\n{literal}\nreturn 0;\n}}\n"
        source += "int main() {\n" + "\n".join(cases)
        source += r"""
    Frame f{}; Context c; State s;
    const uint64_t expected = 0x400921FB89ABCDEFULL;
    assert(c.Memory.WriteFast<uint64_t>(0x1014, expected));
    assert(Literal(f, c, s) == 0);
    assert(f.Guest.vfp[30] == 0x89ABCDEF && f.Guest.vfp[31] == 0x400921FB);
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "vfp_memory.cpp"
            executable = root / ("vfp_memory.exe" if os.name == "nt" else "vfp_memory")
            cpp.write_text(source, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-O2", str(cpp), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=60)
            subprocess.run([str(executable)], check=True, capture_output=True, timeout=15)

    def test_generated_multiple_transfers_match_word_by_word(self) -> None:
        """VLDM/VSTM/VPUSH/VPOP: the single-page span path gives the same lanes,
        bytes, base register, faults and store bookkeeping (one generation step
        per word, the last page stamped) as the word-by-word path, inside a
        page, across a page boundary and with a fault part way through."""
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required for generated-code execution")
        raws = []
        for condition in (0xE, 0x0):
            for double, words in ((False, 1), (False, 2), (False, 5), (True, 2), (True, 8), (True, 32)):
                size = 0xB if double else 0xA
                vd = 2 if words < 32 else 0
                first = (vd << 12) | (size << 8) | words
                for opcode in (0x0D2D0000, 0x0CBD0000,   # VPUSH, VPOP
                               0x0CB20000, 0x0D320000,   # VLDMIA r2!, VLDMDB r2!
                               0x0C820000, 0x0D220000,   # VSTMIA r2, VSTMDB r2!
                               0x0C920000):              # VLDMIA r2
                    raws.append((condition << 28) | opcode | first)
        functions = []
        for index, raw in enumerate(raws):
            body = "\n".join(_emit_vfp(Instruction(0x1000, raw)))
            functions.append(
                f"int Run{index}(Frame& frame, Context& context, State& state) {{\n{body}\nreturn 0;\n}}")
        source = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Fast pages are 4 KiB pages wholly below End; Span=false disables the
// multi-word entry points, which leaves the word-by-word path.
struct Bus {
    std::array<uint8_t, 16384> Bytes{};
    uint32_t End = 16384;
    bool Span = true;
    uint64_t Stores = 0, SpanCalls = 0;
    int64_t LastStorePage = -1;
    template<class T> bool ReadFast(uint32_t address, T* value) {
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(value, Bytes.data() + address, sizeof(T)); return true;
    }
    template<class T> bool WriteFast(uint32_t address, T value) {
        if (address > End || sizeof(T) > End - address) return false;
        std::memcpy(Bytes.data() + address, &value, sizeof(T));
        ++Stores; LastStorePage = address >> 12; return true;
    }
    bool FastSpan(uint32_t address, uint32_t count) const {
        const uint32_t offset = address & 4095U, bytes = count * 4U;
        return Span && bytes <= 4096U && offset <= 4096U - bytes &&
               uint64_t{address - offset} + 4096U <= End;
    }
    bool ReadWordsFast(uint32_t address, uint32_t* values, uint32_t count) {
        if (!FastSpan(address, count)) return false;
        ++SpanCalls; std::memcpy(values, Bytes.data() + address, count * 4U); return true;
    }
    bool WriteWordsFast(uint32_t address, const uint32_t* values, uint32_t count) {
        if (!FastSpan(address, count)) return false;
        ++SpanCalls; std::memcpy(Bytes.data() + address, values, count * 4U);
        Stores += count; LastStorePage = address >> 12; return true;
    }
};
struct Frame { struct { std::array<uint32_t, 32> vfp; uint32_t r[16]; } Guest{}; };
struct Context { Bus Memory; uint32_t FaultPc = 0, FaultAddress = 0; };
struct State {
    uint32_t R[16]{};
    struct { bool Zero = false; bool Z() const { return Zero; } } Flags;
};
int Oot3dAotMemoryFault(Context& c, uint32_t pc, uint32_t address) {
    c.FaultPc = pc; c.FaultAddress = address; return -1;
}
using Run = int (*)(Frame&, Context&, State&);
struct Outcome {
    int Result; std::array<uint32_t, 32> Vfp; std::array<uint8_t, 16384> Bytes;
    uint32_t Sp, R2, FaultAddress; uint64_t Stores; int64_t LastStorePage;
    bool operator==(const Outcome& o) const {
        return Result == o.Result && Vfp == o.Vfp && Bytes == o.Bytes && Sp == o.Sp &&
               R2 == o.R2 && FaultAddress == o.FaultAddress && Stores == o.Stores &&
               LastStorePage == o.LastStorePage;
    }
};
Outcome Execute(Run run, bool span, uint32_t base, uint32_t end, bool skip, uint64_t* spanCalls) {
    Frame f; Context c; State s;
    for (uint32_t i = 0; i < 32; ++i) f.Guest.vfp[i] = 0x51000000U + i;
    for (uint32_t i = 0; i < c.Memory.Bytes.size(); ++i) c.Memory.Bytes[i] = uint8_t(i * 7U + 3U);
    c.Memory.Span = span; c.Memory.End = end;
    f.Guest.r[13] = base; s.R[2] = base; s.Flags.Zero = !skip;
    const int result = run(f, c, s);
    *spanCalls += c.Memory.SpanCalls;
    return {result, f.Guest.vfp, c.Memory.Bytes, f.Guest.r[13], s.R[2],
            c.FaultAddress, c.Memory.Stores, c.Memory.LastStorePage};
}
int failures = 0;
uint64_t spanCalls = 0;
void Check(Run run, int index) {
    // Inside one page, ending at / starting from a page boundary, crossing
    // it, and an end of memory that faults part way through (both sides).
    const uint32_t bases[] = {0x1100U, 0x2000U, 0x1FF8U, 0x1F80U, 0x2F00U};
    const uint32_t ends[] = {16384U, 0x2004U, 0x2F10U, 0x1F00U};
    for (uint32_t base : bases) for (uint32_t end : ends) for (bool skip : {false, true}) {
        uint64_t ignored = 0;
        const Outcome words = Execute(run, false, base, end, skip, &ignored);
        const Outcome spans = Execute(run, true, base, end, skip, &spanCalls);
        if (!(words == spans)) {
            std::printf("mismatch: run %d base %08X end %08X skip %d\n", index, base, end, skip);
            ++failures;
        }
    }
}
"""
        source += "\n".join(functions)
        source += "\nint main() {\n"
        source += "\n".join(f"    Check(Run{index}, {index});" for index in range(len(raws)))
        source += r"""
    if (spanCalls == 0) { std::puts("the span path never ran"); return 1; }
    return failures == 0 ? 0 : 1;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "vfp_multiple.cpp"
            executable = root / ("vfp_multiple.exe" if os.name == "nt" else "vfp_multiple")
            cpp.write_text(source, encoding="utf-8")
            subprocess.run([compiler, "-std=c++17", "-O1", str(cpp), "-o", str(executable)],
                           check=True, capture_output=True, text=True, timeout=120)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_no_vfp_word_span_restores_the_word_by_word_lowering(self) -> None:
        # Regression lead of 12/11 (guest +1-2 ms after the english merge): the
        # VFP word span of d0c42b4d changed 2938 of the 2972 functions that
        # differ from p7e. --no-vfp-word-span must give back the old lowering.
        cases = (0xED2D8B04, 0xECBD8B04, 0xEC900A04, 0xEC800A04)  # VPUSH/VPOP {d8-d9}, VLDM/VSTM {s0-s3}
        spanned = ["\n".join(_emit_vfp(Instruction(0x1000, raw))) for raw in cases]
        for text in spanned:
            self.assertIn("WordsFast(firstAddress, spanWords", text)
        whole_aot_cpp._VFP_WORD_SPAN = False
        try:
            plain = ["\n".join(_emit_vfp(Instruction(0x1000, raw))) for raw in cases]
        finally:
            whole_aot_cpp._VFP_WORD_SPAN = True
        for text in plain:
            self.assertNotIn("spanWords", text)
            self.assertNotIn("WordsFast", text)


if __name__ == "__main__":
    unittest.main()
