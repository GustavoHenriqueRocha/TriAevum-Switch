#pragma once

// Native AArch64 execution of scalar binary32 VFP operations for generated
// whole-AOT code (whole_aot_cpp.py --native-vfp-aarch64).
//
// The guest FPSCR mode is held in FPCR and the guest's cumulative flags
// accumulate in FPSR (oot3d_guest_fp_environment.h). Each operation is one
// AArch64 instruction in a volatile asm statement, so the compiler can neither
// fuse (VMLA/VMLS are FMUL then FADD/FSUB, never FMADD), reorder across the
// FPCR/FPSR accesses, nor drop an operation whose only effect is a flag.
//
// The authoritative semantics remain the software implementation
// (oot3d_native_a32_vfp_ops.h). ARMv8 differs from it in a few places, all
// detected here and recomputed in software:
//
// - flush-to-zero: the software model flushes denormal operands and tiny
//   results to +0, ARMv8 keeps the sign. With equal flags, the values differ
//   only in the sign of a zero result, or of an infinite quotient (x / -denormal);
// - NaN propagation for VSUB with DN=0: the software model negates the second
//   operand before propagating it, ARMv8 does not;
// - underflow with FZ=0: ARMv8 detects tininess before rounding (UFC on a
//   result that rounds up to 2^-126), the software model does not. That flag
//   would already be in FPSR, so arithmetic with FZ=0 is not executed natively
//   at all (the title runs with FPSCR.FZ=1);
// - VCMP/VCMPE: the software model never flushes compare operands, ARMv8 does
//   with FZ=1 (and sets IDC); denormal operands go to software before FCMP.
//
// The fallbacks are value-only: whenever a result is recomputed, the native
// instruction has already raised exactly the flags of the software model
// (oot3d_native_a32_vfp_aarch64_tests.cpp checks value and flags of every
// path, including the fallbacks). The software result's flags are ORed again,
// which changes nothing.
//
// On other hosts every entry point is the software operation.

#include "oot3d_native_a32_vfp_ops.h"
#include "../native_game_runtime/oot3d_guest_fp_environment.h"

#include <bit>
#include <cstdint>

#ifndef OOT3D_AOT_HOT
#if defined(__GNUC__) && !defined(__clang__)
#define OOT3D_AOT_HOT inline __attribute__((always_inline))
#define OOT3D_AOT_COLD __attribute__((noinline, cold))
#else
#define OOT3D_AOT_HOT inline
#define OOT3D_AOT_COLD
#endif
#endif

namespace Oot3dNativeGame {

namespace NativeVfpDetail {

inline constexpr std::uint32_t kFlushToZero = 1U << 24U;

// +/-0 or NaN.
[[nodiscard]] constexpr bool ZeroOrNan(std::uint32_t bits) noexcept {
    return ((bits & 0x7FFFFFFFU) - 1U) >= 0x7F800000U;
}

// +/-0, +/-infinity or NaN.
[[nodiscard]] constexpr bool ZeroInfinityOrNan(std::uint32_t bits) noexcept {
    return ((bits & 0x7FFFFFFFU) - 1U) >= 0x7F7FFFFFU;
}

[[nodiscard]] constexpr bool Denormal(std::uint32_t bits) noexcept {
    return ((bits & 0x7FFFFFFFU) - 1U) < 0x007FFFFFU;
}

inline void Commit(std::uint32_t& destination, std::uint32_t& fpscr,
                   oot3d::recomp::a32::VfpBinary32Result result) noexcept {
    destination = result.value;
    fpscr |= result.exception_flags;
}

#if OOT3D_GUEST_FP_AARCH64

[[nodiscard]] inline float AsFloat(std::uint32_t bits) noexcept {
    return std::bit_cast<float>(bits);
}

[[nodiscard]] inline std::uint32_t AsBits(float value) noexcept {
    return std::bit_cast<std::uint32_t>(value);
}

#define OOT3D_NATIVE_VFP_BINARY(name, instruction)                              \
    [[nodiscard]] OOT3D_AOT_HOT std::uint32_t name(                            \
        std::uint32_t left, std::uint32_t right) noexcept {                    \
        float result;                                                          \
        asm volatile(instruction " %s0, %s1, %s2"                              \
                     : "=w"(result)                                            \
                     : "w"(AsFloat(left)), "w"(AsFloat(right)));               \
        return AsBits(result);                                                 \
    }

OOT3D_NATIVE_VFP_BINARY(Add, "fadd")
OOT3D_NATIVE_VFP_BINARY(Subtract, "fsub")
OOT3D_NATIVE_VFP_BINARY(Multiply, "fmul")
OOT3D_NATIVE_VFP_BINARY(NegativeMultiply, "fnmul")
OOT3D_NATIVE_VFP_BINARY(Divide, "fdiv")

#undef OOT3D_NATIVE_VFP_BINARY

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t SquareRoot(std::uint32_t value) noexcept {
    float result;
    asm volatile("fsqrt %s0, %s1" : "=w"(result) : "w"(AsFloat(value)));
    return AsBits(result);
}

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t FromSigned(std::uint32_t value) noexcept {
    float result;
    asm volatile("scvtf %s0, %w1" : "=w"(result) : "r"(value));
    return AsBits(result);
}

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t FromUnsigned(std::uint32_t value) noexcept {
    float result;
    asm volatile("ucvtf %s0, %w1" : "=w"(result) : "r"(value));
    return AsBits(result);
}

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t ToSigned(std::uint32_t value) noexcept {
    std::uint32_t result;
    asm volatile("fcvtzs %w0, %s1" : "=r"(result) : "w"(AsFloat(value)));
    return result;
}

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t ToUnsigned(std::uint32_t value) noexcept {
    std::uint32_t result;
    asm volatile("fcvtzu %w0, %s1" : "=r"(result) : "w"(AsFloat(value)));
    return result;
}

// NZCV in bits 31:28, as VCMP writes FPSCR.
[[nodiscard]] OOT3D_AOT_HOT std::uint32_t Compare(
    std::uint32_t left, std::uint32_t right) noexcept {
    std::uint64_t nzcv;
    asm volatile("fcmp %s1, %s2\n\tmrs %0, nzcv"
                 : "=r"(nzcv)
                 : "w"(AsFloat(left)), "w"(AsFloat(right))
                 : "cc");
    return static_cast<std::uint32_t>(nzcv);
}

[[nodiscard]] OOT3D_AOT_HOT std::uint32_t CompareSignaling(
    std::uint32_t left, std::uint32_t right) noexcept {
    std::uint64_t nzcv;
    asm volatile("fcmpe %s1, %s2\n\tmrs %0, nzcv"
                 : "=r"(nzcv)
                 : "w"(AsFloat(left)), "w"(AsFloat(right))
                 : "cc");
    return static_cast<std::uint32_t>(nzcv);
}

#endif

} // namespace NativeVfpDetail

// Binary arithmetic: native under FPSCR.FZ=1, software otherwise or when the
// result is a zero or NaN (and, for VDIV, an infinity).
#if OOT3D_GUEST_FP_AARCH64
#define OOT3D_NATIVE_VFP_ARITHMETIC(guard, native, software)                    \
    if ((fpscr & NativeVfpDetail::kFlushToZero) != 0U) [[likely]] {             \
        const std::uint32_t nativeResult = (native);                            \
        if (!NativeVfpDetail::guard(nativeResult)) [[likely]] {                 \
            destination = nativeResult;                                         \
            return;                                                             \
        }                                                                       \
    }                                                                           \
    NativeVfpDetail::Commit(destination, fpscr, (software))
#else
#define OOT3D_NATIVE_VFP_ARITHMETIC(guard, native, software)                    \
    NativeVfpDetail::Commit(destination, fpscr, (software))
#endif

OOT3D_AOT_HOT void Oot3dVfpNativeAdd(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan, NativeVfpDetail::Add(left, right),
        oot3d::recomp::a32::VfpBinary32Add(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpNativeSubtract(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan, NativeVfpDetail::Subtract(left, right),
        oot3d::recomp::a32::VfpBinary32Subtract(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpNativeMultiply(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan, NativeVfpDetail::Multiply(left, right),
        oot3d::recomp::a32::VfpBinary32Multiply(left, right, fpscr));
}

// VNMUL: -(left * right), one rounding (AArch64 FNMUL is not fused).
OOT3D_AOT_HOT void Oot3dVfpNativeNegativeMultiply(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan, NativeVfpDetail::NegativeMultiply(left, right),
        oot3d::recomp::a32::VfpBinary32NegativeMultiply(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpNativeDivide(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroInfinityOrNan, NativeVfpDetail::Divide(left, right),
        oot3d::recomp::a32::VfpBinary32Divide(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpNativeSquareRoot(
    std::uint32_t& destination, std::uint32_t value,
    std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan, NativeVfpDetail::SquareRoot(value),
        oot3d::recomp::a32::VfpBinary32SquareRoot(value, fpscr));
}

// Multiply-accumulate family: the product is rounded (and its flags raised)
// before the separate addition, exactly as VMLA/VMLS/VNMLA/VNMLS. The
// identities below hold for every non-NaN operand under every rounding mode;
// NaN results are recomputed in software.
// VMLA: accumulator + product.
OOT3D_AOT_HOT void Oot3dVfpNativeMultiplyAccumulate(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan,
        NativeVfpDetail::Add(accumulator, NativeVfpDetail::Multiply(left, right)),
        oot3d::recomp::a32::VfpBinary32MultiplyAccumulate(
            accumulator, left, right, fpscr));
}

// VMLS: accumulator + -(product) == accumulator - product.
OOT3D_AOT_HOT void Oot3dVfpNativeMultiplySubtract(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan,
        NativeVfpDetail::Subtract(
            accumulator, NativeVfpDetail::Multiply(left, right)),
        oot3d::recomp::a32::VfpBinary32MultiplySubtract(
            accumulator, left, right, fpscr));
}

// VNMLA: -accumulator + -(product) == -accumulator - product.
OOT3D_AOT_HOT void Oot3dVfpNativeNegativeMultiplyAccumulate(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan,
        NativeVfpDetail::Subtract(
            accumulator ^ 0x80000000U, NativeVfpDetail::Multiply(left, right)),
        oot3d::recomp::a32::VfpBinary32NegativeMultiplyAccumulate(
            accumulator, left, right, fpscr));
}

// VNMLS: -accumulator + product == product - accumulator.
OOT3D_AOT_HOT void Oot3dVfpNativeNegativeMultiplySubtract(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_NATIVE_VFP_ARITHMETIC(
        ZeroOrNan,
        NativeVfpDetail::Subtract(
            NativeVfpDetail::Multiply(left, right), accumulator),
        oot3d::recomp::a32::VfpBinary32NegativeMultiplySubtract(
            accumulator, left, right, fpscr));
}

#undef OOT3D_NATIVE_VFP_ARITHMETIC

// Zero-aware variants (whole_aot_cpp.py --vfp-zero-aware, the native
// overrides, the Mtx3x4 kernel). The same value and the same flags as the
// Oot3dVfpNative* entry points above (and therefore as the software model),
// but under FPSCR.FZ=1 a zero result that is EXACT in both models stays on
// the native path instead of being recomputed in software. Zeros of this
// kind (x * 0, x - x, accumulator - product cancelling exactly) are frequent
// in rotation/scale matrices, and each one used to cost a software operation
// plus the spills around its call.
//
// A native zero is exact when no rounding and no flush to zero happened:
// every operand that reaches the final add/sub is finite and not subnormal
// (a subnormal operand is flushed to a signed zero by ARMv8 and to +0 by the
// software model) and the exact result is zero (x + y with y == -x, or two
// zeros; a product with a zero factor and a finite non-subnormal other
// factor). Then the value is the IEEE signed zero of the rounding mode in
// both models and no flag is raised. NOT exact, and so still recomputed:
// a sum or product of normals whose exact result is a nonzero subnormal that
// FZ discards (ARMv8 keeps the sign there, the software model gives +0, and
// UFC/IXC are raised), any denormal input, NaN results.
namespace NativeVfpDetail {

[[nodiscard]] constexpr bool ExactZero(std::uint32_t bits) noexcept {
    return (bits & 0x7FFFFFFFU) == 0U;
}

// Finite and not subnormal (zero included).
[[nodiscard]] constexpr bool FiniteNotDenormal(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = bits & 0x7F800000U;
    return exponent != 0x7F800000U && (exponent != 0U || ExactZero(bits));
}

// left * right is an exact (signed) zero in both models.
[[nodiscard]] constexpr bool ExactZeroProduct(std::uint32_t left,
                                              std::uint32_t right) noexcept {
    return FiniteNotDenormal(left) && FiniteNotDenormal(right) &&
           (ExactZero(left) || ExactZero(right));
}

// left + right is an exact (signed) zero in both models: exact cancellation
// of two finite non-subnormal operands, or two zeros. Callers pass operands
// that are bit-identical in both models (guest registers, or a native
// product already known to be the software product).
[[nodiscard]] constexpr bool ExactZeroSum(std::uint32_t left,
                                          std::uint32_t right) noexcept {
    return FiniteNotDenormal(left) && FiniteNotDenormal(right) &&
           ((left ^ right) == 0x80000000U || (ExactZero(left) && ExactZero(right)));
}

// The native product under FZ=1 is the software product (value and flags)
// whenever it is nonzero (then it is normal or infinite) or an exact zero.
// A zero product from a flushed underflow is not.
[[nodiscard]] constexpr bool ProductAgrees(std::uint32_t product, std::uint32_t left,
                                           std::uint32_t right) noexcept {
    return !ExactZero(product) || ExactZeroProduct(left, right);
}

// Hot/cold split: the inline part is the same as Oot3dVfpNative* (one native
// instruction, one test of the result); a zero or NaN result calls the cold
// ZeroAwareSlow, which keeps the native zero when it is exact and otherwise
// runs the software operation as before. The generated code therefore grows
// by nothing in the hot path, and an exact zero costs a short out-of-line
// check instead of a software operation.
enum class ZeroAwareOperation : std::uint8_t {
    Add, Subtract, Multiply, NegativeMultiply, Divide, SquareRoot,
    MultiplyAccumulate, MultiplySubtract, NegativeMultiplyAccumulate,
    NegativeMultiplySubtract,
};

#if OOT3D_GUEST_FP_AARCH64
// Cold half, reached under FZ=1 with a native result that is a zero or NaN
// (for VDIV also an infinity). `nativeResult` is that result; its flags are
// already in FPSR. For the multiply-accumulate family the native product is
// recomputed here: same operands, same FPCR, so the same value, and its flags
// (sticky, already raised once) change nothing.
template <ZeroAwareOperation Operation>
OOT3D_AOT_COLD std::uint32_t ZeroAwareSlow(
    std::uint32_t nativeResult, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    namespace a32 = oot3d::recomp::a32;
    bool exact = false;
    if (ExactZero(nativeResult)) {
        if constexpr (Operation == ZeroAwareOperation::Add) {
            exact = ExactZeroSum(left, right);
        } else if constexpr (Operation == ZeroAwareOperation::Subtract) {
            exact = ExactZeroSum(left, right ^ 0x80000000U);
        } else if constexpr (Operation == ZeroAwareOperation::Multiply || Operation == ZeroAwareOperation::NegativeMultiply) {
            exact = ExactZeroProduct(left, right);
        } else if constexpr (Operation == ZeroAwareOperation::Divide) {
            // 0 / y (y nonzero, not subnormal, possibly infinite) and
            // x / infinity; infinite quotients still go to software.
            exact = FiniteNotDenormal(left) &&
                    (ExactZero(left) ? !Denormal(right) && !ExactZero(right)
                                     : (right & 0x7FFFFFFFU) == 0x7F800000U);
        } else if constexpr (Operation == ZeroAwareOperation::SquareRoot) {
            exact = ExactZero(left);
        } else {
            const std::uint32_t product = NativeVfpDetail::Multiply(left, right);
            if constexpr (Operation == ZeroAwareOperation::MultiplyAccumulate) {
                exact = ExactZeroSum(accumulator, product);
            } else if constexpr (Operation == ZeroAwareOperation::MultiplySubtract) {
                exact = ExactZeroSum(accumulator, product ^ 0x80000000U);
            } else if constexpr (Operation == ZeroAwareOperation::NegativeMultiplyAccumulate) {
                exact = ExactZeroSum(accumulator ^ 0x80000000U, product ^ 0x80000000U);
            } else {
                static_assert(Operation == ZeroAwareOperation::NegativeMultiplySubtract);
                exact = ExactZeroSum(product, accumulator ^ 0x80000000U);
            }
            exact = exact && ProductAgrees(product, left, right);
        }
    }
    if (exact) {
        return nativeResult;
    }
    a32::VfpBinary32Result result{};
    if constexpr (Operation == ZeroAwareOperation::Add) {
        result = a32::VfpBinary32Add(left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::Subtract) {
        result = a32::VfpBinary32Subtract(left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::Multiply) {
        result = a32::VfpBinary32Multiply(left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::NegativeMultiply) {
        result = a32::VfpBinary32NegativeMultiply(left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::Divide) {
        result = a32::VfpBinary32Divide(left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::SquareRoot) {
        result = a32::VfpBinary32SquareRoot(left, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::MultiplyAccumulate) {
        result = a32::VfpBinary32MultiplyAccumulate(accumulator, left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::MultiplySubtract) {
        result = a32::VfpBinary32MultiplySubtract(accumulator, left, right, fpscr);
    } else if constexpr (Operation == ZeroAwareOperation::NegativeMultiplyAccumulate) {
        result = a32::VfpBinary32NegativeMultiplyAccumulate(accumulator, left, right, fpscr);
    } else {
        result = a32::VfpBinary32NegativeMultiplySubtract(accumulator, left, right, fpscr);
    }
    fpscr |= result.exception_flags;
    return result.value;
}
#endif

} // namespace NativeVfpDetail

#if OOT3D_GUEST_FP_AARCH64
#define OOT3D_ZERO_AWARE_VFP(operation, guard, nativeValue, accumulator, left, right, software) \
    if ((fpscr & NativeVfpDetail::kFlushToZero) != 0U) [[likely]] {             \
        const std::uint32_t nativeResult = (nativeValue);                       \
        if (!NativeVfpDetail::guard(nativeResult)) [[likely]] {                 \
            destination = nativeResult;                                         \
            return;                                                             \
        }                                                                       \
        destination = NativeVfpDetail::ZeroAwareSlow<                           \
            NativeVfpDetail::ZeroAwareOperation::operation>(                    \
            nativeResult, (accumulator), (left), (right), fpscr);               \
        return;                                                                 \
    }                                                                           \
    NativeVfpDetail::Commit(destination, fpscr, (software))
#else
#define OOT3D_ZERO_AWARE_VFP(operation, guard, nativeValue, accumulator, left, right, software) \
    NativeVfpDetail::Commit(destination, fpscr, (software))
#endif

OOT3D_AOT_HOT void Oot3dVfpZeroAwareAdd(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(Add, ZeroOrNan, NativeVfpDetail::Add(left, right), 0U, left, right,
                         oot3d::recomp::a32::VfpBinary32Add(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareSubtract(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(Subtract, ZeroOrNan, NativeVfpDetail::Subtract(left, right), 0U,
                         left, right,
                         oot3d::recomp::a32::VfpBinary32Subtract(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareMultiply(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(Multiply, ZeroOrNan, NativeVfpDetail::Multiply(left, right), 0U,
                         left, right,
                         oot3d::recomp::a32::VfpBinary32Multiply(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareNegativeMultiply(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(NegativeMultiply, ZeroOrNan,
                         NativeVfpDetail::NegativeMultiply(left, right), 0U, left, right,
                         oot3d::recomp::a32::VfpBinary32NegativeMultiply(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareDivide(
    std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(Divide, ZeroInfinityOrNan, NativeVfpDetail::Divide(left, right), 0U,
                         left, right,
                         oot3d::recomp::a32::VfpBinary32Divide(left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareSquareRoot(
    std::uint32_t& destination, std::uint32_t value,
    std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(SquareRoot, ZeroOrNan, NativeVfpDetail::SquareRoot(value), 0U,
                         value, 0U,
                         oot3d::recomp::a32::VfpBinary32SquareRoot(value, fpscr));
}

// Multiply-accumulate family (same native sequences as Oot3dVfpNative*).
OOT3D_AOT_HOT void Oot3dVfpZeroAwareMultiplyAccumulate(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(
        MultiplyAccumulate, ZeroOrNan,
        NativeVfpDetail::Add(accumulator, NativeVfpDetail::Multiply(left, right)),
        accumulator, left, right,
        oot3d::recomp::a32::VfpBinary32MultiplyAccumulate(accumulator, left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareMultiplySubtract(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(
        MultiplySubtract, ZeroOrNan,
        NativeVfpDetail::Subtract(accumulator, NativeVfpDetail::Multiply(left, right)),
        accumulator, left, right,
        oot3d::recomp::a32::VfpBinary32MultiplySubtract(accumulator, left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareNegativeMultiplyAccumulate(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(
        NegativeMultiplyAccumulate, ZeroOrNan,
        NativeVfpDetail::Subtract(accumulator ^ 0x80000000U,
                                  NativeVfpDetail::Multiply(left, right)),
        accumulator, left, right,
        oot3d::recomp::a32::VfpBinary32NegativeMultiplyAccumulate(
            accumulator, left, right, fpscr));
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareNegativeMultiplySubtract(
    std::uint32_t& destination, std::uint32_t accumulator, std::uint32_t left,
    std::uint32_t right, std::uint32_t& fpscr) noexcept {
    OOT3D_ZERO_AWARE_VFP(
        NegativeMultiplySubtract, ZeroOrNan,
        NativeVfpDetail::Subtract(NativeVfpDetail::Multiply(left, right), accumulator),
        accumulator, left, right,
        oot3d::recomp::a32::VfpBinary32NegativeMultiplySubtract(
            accumulator, left, right, fpscr));
}

#undef OOT3D_ZERO_AWARE_VFP

// Conversions match the software model in every FPSCR mode.
OOT3D_AOT_HOT void Oot3dVfpNativeFromSigned(
    std::uint32_t& destination, std::uint32_t value, std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    static_cast<void>(fpscr);
    destination = NativeVfpDetail::FromSigned(value);
#else
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32FromSigned(value, fpscr));
#endif
}

OOT3D_AOT_HOT void Oot3dVfpNativeFromUnsigned(
    std::uint32_t& destination, std::uint32_t value, std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    static_cast<void>(fpscr);
    destination = NativeVfpDetail::FromUnsigned(value);
#else
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32FromUnsigned(value, fpscr));
#endif
}

// VCVT.S32.F32 / VCVT.U32.F32 (round toward zero).
OOT3D_AOT_HOT void Oot3dVfpNativeToSigned(
    std::uint32_t& destination, std::uint32_t value, std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    static_cast<void>(fpscr);
    destination = NativeVfpDetail::ToSigned(value);
#else
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32ToSigned(value, fpscr));
#endif
}

OOT3D_AOT_HOT void Oot3dVfpNativeToUnsigned(
    std::uint32_t& destination, std::uint32_t value, std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    static_cast<void>(fpscr);
    destination = NativeVfpDetail::ToUnsigned(value);
#else
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32ToUnsigned(value, fpscr));
#endif
}

// VCMP (signalAllNans=false) / VCMPE: FPSCR.NZCV and IOC.
OOT3D_AOT_HOT void Oot3dVfpNativeCompare(
    std::uint32_t left, std::uint32_t right, bool signalAllNans,
    std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    if (!NativeVfpDetail::Denormal(left) && !NativeVfpDetail::Denormal(right))
        [[likely]] {
        const std::uint32_t nzcv =
            signalAllNans ? NativeVfpDetail::CompareSignaling(left, right)
                          : NativeVfpDetail::Compare(left, right);
        fpscr = (fpscr & ~0xF0000000U) | (nzcv & 0xF0000000U);
        return;
    }
#endif
    const auto comparison =
        oot3d::recomp::a32::VfpBinary32Compare(left, right, signalAllNans);
    fpscr = (fpscr & ~0xF0000000U) | (comparison.value & 0xF0000000U);
    fpscr |= comparison.exception_flags;
}

} // namespace Oot3dNativeGame
