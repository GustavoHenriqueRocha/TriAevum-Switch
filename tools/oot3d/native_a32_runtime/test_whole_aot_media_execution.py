"""Executa as instruções de mídia ARMv6 emitidas (SIMD de 8/16 bits, SSAT16/
USAT16, SXTAB16/UXTAB16, SMLAD/SMLSD) contra um modelo de referência escrito
a partir do pseudocódigo do ARM ARM: valor, GE (CPSR[19:16]) e Q (CPSR[27]).

Regressão do Super Mario 3D Land: essas codificações faziam o lowering falhar
("media extension ... is unsupported") ou, no caso de SMLAD/SMLSD com Ra != 15,
caíam no caminho de STR/LDR e viravam um acesso à memória.
"""

from __future__ import annotations

import os
import random
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from whole_aot_cpp import Instruction, LoweringError, _emit_non_control

MASK32 = 0xFFFFFFFF
EDGES = (0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x0100, 0x7FFF, 0x8000,
         0x8001, 0xFF00, 0xFF7F, 0xFF80, 0xFFFF)


def _sext(value: int, bits: int) -> int:
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _lane(value: int, index: int, bits: int, signed: bool) -> int:
    raw = (value >> (index * bits)) & ((1 << bits) - 1)
    return _sext(raw, bits) if signed else raw


def reference_parallel(op1: int, op2: int, a: int, b: int) -> tuple[int, int | None]:
    """A8.8.* SADD16 ... UHSUB8: (Rd, GE ou None quando GE não muda)."""
    signed = op1 in {1, 2, 3}
    if op2 in {0, 1, 2, 3}:
        bits = 16
        pairs = {
            0: ((0, 0, 1), (1, 1, 1)),
            1: ((0, 1, -1), (1, 0, 1)),
            2: ((0, 1, 1), (1, 0, -1)),
            3: ((0, 0, -1), (1, 1, -1)),
        }[op2]
    else:
        bits = 8
        sign = 1 if op2 == 4 else -1
        pairs = tuple((lane, lane, sign) for lane in range(4))
    result = 0
    ge = 0
    for index, (left, right, sign) in enumerate(pairs):
        x = _lane(a, left, bits, signed) + sign * _lane(b, right, bits, signed)
        if op1 == 2:  # Q
            out = max(-(1 << (bits - 1)), min((1 << (bits - 1)) - 1, x))
        elif op1 == 6:  # UQ
            out = max(0, min((1 << bits) - 1, x))
        elif op1 in {3, 7}:  # SH / UH
            out = x >> 1
        else:
            out = x
            if op1 == 1:
                flag = x >= 0
            else:
                flag = x >= (1 << bits) if sign > 0 else x >= 0
            if flag:
                ge |= (0b11 << (2 * index)) if bits == 16 else (1 << index)
        result |= (out & ((1 << bits) - 1)) << (index * bits)
    return result, (ge if op1 in {1, 5} else None)


def reference_saturate16(signed: bool, imm: int, value: int) -> tuple[int, bool]:
    bits = imm + 1 if signed else imm
    low, high = ((-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed
                 else (0, (1 << bits) - 1))
    result, saturated = 0, False
    for index in range(2):
        x = _lane(value, index, 16, True)
        out = max(low, min(high, x))
        saturated |= out != x
        result |= (out & 0xFFFF) << (16 * index)
    return result, saturated


def reference_extend_add16(signed: bool, rotate: int, base: int, value: int) -> int:
    rotated = ((value >> rotate) | (value << (32 - rotate))) & MASK32 if rotate else value
    low = (base + _lane(rotated, 0, 8, signed)) & 0xFFFF
    high = ((base >> 16) + _lane(rotated, 2, 8, signed)) & 0xFFFF
    return low | (high << 16)


def reference_dual_multiply(subtract: bool, exchange: bool, accumulate: int | None,
                            n: int, m: int) -> tuple[int, bool]:
    if exchange:
        m = ((m >> 16) | (m << 16)) & MASK32
    p1 = _lane(n, 0, 16, True) * _lane(m, 0, 16, True)
    p2 = _lane(n, 1, 16, True) * _lane(m, 1, 16, True)
    result = p1 - p2 if subtract else p1 + p2
    if accumulate is not None:
        result += _sext(accumulate, 32)
    return result & MASK32, result != _sext(result, 32)


HARNESS = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace a32 { constexpr uint32_t kFlagC = 1U << 29U; }
struct Frame { struct { uint32_t cpsr = 0; } Guest; };
struct Flags {
    bool Zero = false;
    bool N() const { return false; } bool Z() const { return Zero; }
    bool C() const { return false; } bool V() const { return false; }
};
struct State {
    std::array<uint32_t, 15> R{};
    uint32_t Preserved = 0;
    Flags Flags;
    void SetPreservedCpsrBits(uint32_t mask, uint32_t value) {
        Preserved = (Preserved & ~mask) | (value & mask);
    }
};
inline uint32_t Oot3dAotRor(uint32_t value, uint32_t amount) {
    amount &= 31U;
    return amount == 0U ? value : (value >> amount) | (value << (32U - amount));
}
inline void Oot3dAotSetSaturationFlag(Frame& frame, bool saturated) {
    if (saturated) frame.Guest.cpsr |= 1U << 27U;
}
struct Case { uint32_t n, m, a, value, ge; bool q; };
using Run = void (*)(Frame&, State&);
static int failures = 0;
// GE parte de 0xA (sentinela): instruções sem GE precisam preservá-lo.
static void Check(const char* name, Run run, const Case* cases, size_t count,
                  bool writesGe, bool skip) {
    for (size_t index = 0; index < count; ++index) {
        const Case& c = cases[index];
        Frame frame; State state;
        state.Preserved = 0x000A0000U;  // Z = 0: a forma EQ não executa.
        state.R[1] = c.n; state.R[2] = c.m; state.R[3] = c.a;
        state.R[4] = 0xDEADBEEFU;
        run(frame, state);
        const uint32_t ge = (state.Preserved >> 16U) & 0xFU;
        const bool q = (frame.Guest.cpsr & (1U << 27U)) != 0U;
        const uint32_t wantValue = skip ? 0xDEADBEEFU : c.value;
        const uint32_t wantGe = (writesGe && !skip) ? c.ge : 0xAU;
        const bool wantQ = skip ? false : c.q;
        if (state.R[4] != wantValue || ge != wantGe || q != wantQ ||
            state.R[1] != c.n || state.R[2] != c.m || state.R[3] != c.a) {
            std::printf("%s n=%08X m=%08X a=%08X: r4=%08X/%08X ge=%X/%X q=%d/%d\n",
                        name, c.n, c.m, c.a, state.R[4], wantValue,
                        ge, wantGe, q, wantQ);
            ++failures;
        }
    }
}
"""


def _values(rng: random.Random, count: int) -> list[int]:
    values = [(hi << 16) | lo for hi in EDGES for lo in EDGES[::3]]
    values += [int.from_bytes(bytes(rng.choice((0x00, 0x01, 0x7F, 0x80, 0xFF))
                                    for _ in range(4)), "little") for _ in range(16)]
    values += [rng.getrandbits(32) for _ in range(count)]
    return values


class WholeAotMediaExecutionTests(unittest.TestCase):
    def test_generated_media_instructions_match_reference(self) -> None:
        compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required for generated-code execution")
        rng = random.Random(0x3D1A)
        values = _values(rng, 24)
        pairs = [(rng.choice(values), rng.choice(values)) for _ in range(160)]
        # (nome, raw, escreve GE?, casos (n, m, a, valor, ge, q))
        specs: list[tuple[str, int, bool, list[tuple[int, int, int, int, int, bool]]]] = []
        prefixes = {1: "S", 2: "Q", 3: "SH", 5: "U", 6: "UQ", 7: "UH"}
        names = {0: "ADD16", 1: "ASX", 2: "SAX", 3: "SUB16", 4: "ADD8", 7: "SUB8"}
        for op1, prefix in prefixes.items():
            for op2, suffix in names.items():
                # <op> r4, r1, r2
                raw = 0xE6000F10 | (op1 << 20) | (1 << 16) | (4 << 12) | (op2 << 5) | 2
                cases = []
                for n, m in pairs:
                    value, ge = reference_parallel(op1, op2, n, m)
                    cases.append((n, m, 0, value, 0 if ge is None else ge, False))
                specs.append((prefix + suffix, raw, op1 in {1, 5}, cases))
        for signed in (False, True):
            for imm in (0, 1, 7, 8, 15):
                # SSAT16/USAT16 r4, #sat, r1
                raw = (0xE6A00F30 if signed else 0xE6E00F30) | (imm << 16) | (4 << 12) | 1
                cases = []
                for n, _ in pairs:
                    value, q = reference_saturate16(signed, imm, n)
                    cases.append((n, 0, 0, value, 0, q))
                specs.append((f"{'S' if signed else 'U'}SAT16#{imm + signed}", raw, False, cases))
        for signed in (False, True):
            for rotation in range(4):
                # SXTAB16/UXTAB16 r4, r1, r2, ROR #(8*rotation)
                raw = ((0xE6800070 if signed else 0xE6C00070) | (1 << 16) | (4 << 12)
                       | (rotation << 10) | 2)
                cases = [(n, m, 0, reference_extend_add16(signed, rotation * 8, n, m), 0, False)
                         for n, m in pairs]
                specs.append((f"{'S' if signed else 'U'}XTAB16/{rotation}", raw, False, cases))
        accumulators = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF] + values[:8]
        for subtract in (False, True):
            for exchange in (False, True):
                for accumulate in (False, True):
                    # SMLAD/SMLSD[X] r4, r1, r2, r3 (SMUAD/SMUSD com Ra = 15)
                    raw = (0xE7000010 | (4 << 16) | ((3 if accumulate else 15) << 12)
                           | (2 << 8) | (int(subtract) << 6) | (int(exchange) << 5) | 1)
                    cases = []
                    for index, (n, m) in enumerate(pairs + [(0x80008000, 0x80008000)]):
                        a = accumulators[index % len(accumulators)]
                        value, q = reference_dual_multiply(
                            subtract, exchange, a if accumulate else None, n, m)
                        cases.append((n, m, a, value, 0, q))
                    name = ("SMLSD" if subtract else "SMLAD") if accumulate else (
                        "SMUSD" if subtract else "SMUAD")
                    specs.append((name + ("X" if exchange else ""), raw, False, cases))

        source = [HARNESS]
        checks = []
        for index, (name, raw, writes_ge, cases) in enumerate(specs):
            for skip in (False, True):
                encoded = (raw & 0x0FFFFFFF) | ((0x0 if skip else 0xE) << 28)  # EQ com Z=0
                body = "\n".join(_emit_non_control(Instruction(0x1000, encoded)))
                source.append(
                    f"static void Run{index}_{int(skip)}(Frame& frame, State& state) {{\n"
                    f"{body}\n}}"
                )
                checks.append(
                    f"Check(\"{name}\", Run{index}_{int(skip)}, Cases{index}, "
                    f"{len(cases)}, {str(writes_ge).lower()}, {str(skip).lower()});"
                )
            rows = ",\n".join(
                f"{{0x{n:08X}U, 0x{m:08X}U, 0x{a:08X}U, 0x{value:08X}U, 0x{ge:X}U, "
                f"{str(q).lower()}}}"
                for n, m, a, value, ge, q in cases
            )
            source.append(f"static const Case Cases{index}[] = {{\n{rows}\n}};")
        source.append("int main() {\n" + "\n".join(checks)
                      + "\nif (failures) std::printf(\"%d failures\\n\", failures);"
                      + "\nreturn failures == 0 ? 0 : 1;\n}\n")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cpp = root / "media.cpp"
            executable = root / ("media.exe" if os.name == "nt" else "media")
            cpp.write_text("\n".join(source), encoding="utf-8")
            build = subprocess.run(
                [compiler, "-std=c++20", "-O1", str(cpp), "-o", str(executable)],
                capture_output=True, text=True, timeout=180)
            self.assertEqual(build.returncode, 0, build.stderr[-4000:])
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout[-4000:])

    def test_reference_model_known_values(self) -> None:
        # Valores conferidos à mão com o ARM ARM, independentes do gerador.
        self.assertEqual(reference_parallel(2, 0, 0x7FFF8000, 0x00018000), (0x7FFF8000, None))
        self.assertEqual(reference_parallel(5, 4, 0x01FF80FF, 0x01010101), (0x02008100, 0b0101))
        self.assertEqual(reference_parallel(1, 3, 0x00010000, 0x00000001), (0x0001FFFF, 0b1100))
        self.assertEqual(reference_parallel(7, 4, 0xFF000000, 0xFF000000), (0xFF000000, None))
        self.assertEqual(reference_saturate16(False, 8, 0x0123FF00), (0x00FF0000, True))
        self.assertEqual(reference_saturate16(True, 7, 0x00800010), (0x007F0010, True))
        self.assertEqual(reference_dual_multiply(False, False, None, 0x80008000, 0x80008000),
                         (0x80000000, True))

    def test_pc_operands_and_coprocessor_fallback(self) -> None:
        for raw in (0xE6E8FF33, 0xE623FF17, 0xE70FF612, 0xE6C6C07F):
            with self.assertRaises(LoweringError, msg=hex(raw)):
                _emit_non_control(Instruction(0x1000, raw))
        # CDP p15 alcançado depois de uma chamada que não retorna (SM3DL 0x3A0D14).
        lines = _emit_non_control(Instruction(0x3A0D14, 0x3EFFFFED))
        self.assertEqual(lines[0], "if (!(state.Flags.C())) {")
        self.assertIn("Oot3dWholeAotFlowKind::Unsupported, 0x003A0D14U, 0x3EFFFFEDU", lines[1])


if __name__ == "__main__":
    unittest.main()
