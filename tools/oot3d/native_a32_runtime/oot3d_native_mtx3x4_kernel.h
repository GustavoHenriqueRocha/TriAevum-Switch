#pragma once

// Mtx3x4_Multiply (0x0036C174), the most-called 3x4 matrix multiplication
// in the game (~440 times per frame in Kokiri Forest, always through the
// external call of ExecuteMtx3x4Multiply in oot3d_native_compiled_functions.cpp).
//
// Semantics (the guest sequence, unfused VMUL/VMLA): for each row r
// and column c of out = right x left (guest layout, 3 rows of 4 floats),
//   out[4r + c] = ((right[c] * left[4r] (+ left[4r + 3] if c == 3))
//                  + right[4 + c] * left[4r + 1]) + right[8 + c] * left[4r + 2]
// with each product rounded before the sum, and the flags accumulated from all
// operations. The reference is Mtx3x4MultiplySoftware (VfpBinary32*, the
// authoritative implementation); Mtx3x4MultiplyFast returns exactly the same
// value and the same flags (oot3d_native_mtx3x4_kernel_tests.cpp, on x86 and under
// qemu-aarch64):
//   - AArch64 with FPSCR.FZ = 1 (the game's mode): the native instructions of
//     oot3d_native_a32_vfp_aarch64.h (the same as generated code with
//     --native-vfp-aarch64) with the FPCR loaded locally from the guest FPSCR and
//     the same fall-back-to-software rule (zero or NaN result), except for
//     zeros that are exact in both models (ExactZeroProduct, exact cancellation),
//     which in sparse matrices are most of the fallbacks to software;
//   - otherwise, in round-to-nearest: the fast paths of
//     oot3d_native_a32_vfp_fast.h (host float + exact error term);
//   - otherwise: software.
// Only valid for FPSCR without LEN/STRIDE and without enabled traps (the caller
// uses software in those cases, as before).

#include "oot3d_native_a32_vfp_aarch64.h"
#include "oot3d_native_a32_vfp_fast.h"
#include "oot3d_native_a32_vfp_ops.h"

#include <cstdint>

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kMtx3x4FpscrFlagMask = 0x0000009FU;

inline void Mtx3x4MultiplySoftware(const std::uint32_t* left, const std::uint32_t* right,
                                   std::uint32_t fpscr, std::uint32_t* out,
                                   std::uint32_t& flags) noexcept {
    namespace a32 = oot3d::recomp::a32;
    std::uint32_t accumulated = 0U;
    for (std::uint32_t row = 0U; row < 3U; ++row) {
        const std::uint32_t base = row * 4U;
        for (std::uint32_t column = 0U; column < 4U; ++column) {
            a32::VfpBinary32Result value =
                column == 3U
                    ? a32::VfpBinary32MultiplyAccumulate(left[base + 3U], right[3U],
                                                         left[base], fpscr)
                    : a32::VfpBinary32Multiply(right[column], left[base], fpscr);
            accumulated |= value.exception_flags;
            value = a32::VfpBinary32MultiplyAccumulate(value.value, right[4U + column],
                                                       left[base + 1U], fpscr);
            accumulated |= value.exception_flags;
            value = a32::VfpBinary32MultiplyAccumulate(value.value, right[8U + column],
                                                       left[base + 2U], fpscr);
            accumulated |= value.exception_flags;
            out[base + column] = value.value;
        }
    }
    flags = accumulated;
}

namespace Mtx3x4Detail {

inline constexpr std::uint32_t kFlushToZero = 1U << 24U;
inline constexpr std::uint32_t kRoundingMode = 3U << 22U;

#if OOT3D_GUEST_FP_AARCH64
// Exact zeros: NativeVfpDetail::ExactZero, FiniteNotDenormal and
// ExactZeroProduct (oot3d_native_a32_vfp_aarch64.h, where the explanation is):
// the native product gave zero without rounding or flush-to-zero when
// one factor is zero and the other is finite and not subnormal.
using NativeVfpDetail::ExactZero;
using NativeVfpDetail::ExactZeroProduct;
using NativeVfpDetail::FiniteNotDenormal;

// Native VMUL like Oot3dVfpNativeMultiply (FZ = 1), but an exact zero
// (ExactZeroProduct) keeps the native result instead of going to software.
inline void Multiply(std::uint32_t& destination, std::uint32_t left, std::uint32_t right,
                     std::uint32_t& fpscr) noexcept {
    const std::uint32_t value = NativeVfpDetail::Multiply(left, right);
    if (!NativeVfpDetail::ZeroOrNan(value) || (ExactZero(value) && ExactZeroProduct(left, right)))
        [[likely]] {
        destination = value;
        return;
    }
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32Multiply(left, right, fpscr));
}

// Native VMLA like Oot3dVfpNativeMultiplyAccumulate (FZ = 1). A zero sum
// also keeps the native result when it is exact in both models: the
// accumulator is not subnormal and (a) the product is nonzero (hence normal: with
// FZ the native product is not subnormal, and matches the software one) and is
// exactly the negation of the accumulator, i.e. exact cancellation (a sum
// of normals that only yields a flushed subnormal does NOT count: there ARMv8 keeps
// the sign and software gives +0); or (b) the product is an exact zero
// (ExactZeroProduct) and the accumulator is zero. The sign of the zero follows the
// IEEE rule for the rounding mode in both models.
inline void MultiplyAccumulate(std::uint32_t& destination, std::uint32_t accumulator,
                               std::uint32_t left, std::uint32_t right,
                               std::uint32_t& fpscr) noexcept {
    const std::uint32_t product = NativeVfpDetail::Multiply(left, right);
    const std::uint32_t value = NativeVfpDetail::Add(accumulator, product);
    if (!NativeVfpDetail::ZeroOrNan(value)) [[likely]] {
        destination = value;
        return;
    }
    if (ExactZero(value) && FiniteNotDenormal(accumulator) &&
        (!ExactZero(product) ? accumulator == (product ^ 0x80000000U)
                             : ExactZeroProduct(left, right) && ExactZero(accumulator))) {
        destination = value;
        return;
    }
    NativeVfpDetail::Commit(destination, fpscr,
                            oot3d::recomp::a32::VfpBinary32MultiplyAccumulate(
                                accumulator, left, right, fpscr));
}

// Operation order is free across outputs (each output is an independent
// chain and the flags are accumulated by OR), fixed within each
// chain; the native operations are asm volatile, so the compiler neither fuses
// them nor moves them out of the guest FPCR scope.
inline void Native(const std::uint32_t* left, const std::uint32_t* right,
                   std::uint32_t fpscr, std::uint32_t* out, std::uint32_t& flags) noexcept {
    std::uint32_t work = fpscr & ~kMtx3x4FpscrFlagMask;
    // What Oot3dGuestFpScope does, without the per-thread state (nothing here calls
    // code that consults gOot3dGuestFp): FPCR with the guest mode, FPSR zeroed
    // to collect the flags, and both of the caller's restored at the end (if the
    // caller was already in guest mode, its FPSR comes back intact).
    const std::uint64_t callerFpcr = Oot3dReadFpcr();
    const std::uint64_t callerFpsr = Oot3dReadFpsr();
    const std::uint64_t guestFpcr = Oot3dGuestFpcr(callerFpcr, fpscr);
    if (guestFpcr != callerFpcr) Oot3dWriteFpcr(guestFpcr);
    Oot3dWriteFpsr(0U);
    {
        for (std::uint32_t row = 0U; row < 3U; ++row) {
            const std::uint32_t base = row * 4U;
            std::uint32_t value[4];
            Multiply(value[0], right[0], left[base], work);
            Multiply(value[1], right[1], left[base], work);
            Multiply(value[2], right[2], left[base], work);
            MultiplyAccumulate(value[3], left[base + 3U], right[3], left[base], work);
            for (std::uint32_t column = 0U; column < 4U; ++column) {
                MultiplyAccumulate(value[column], value[column], right[4U + column],
                                   left[base + 1U], work);
            }
            for (std::uint32_t column = 0U; column < 4U; ++column) {
                MultiplyAccumulate(value[column], value[column], right[8U + column],
                                   left[base + 2U], work);
                out[base + column] = value[column];
            }
        }
    }
    Oot3dGuestFpFoldFlags(work);
    if (guestFpcr != callerFpcr) Oot3dWriteFpcr(callerFpcr);
    Oot3dWriteFpsr(callerFpsr);
    flags = work & kMtx3x4FpscrFlagMask;
}
#endif

inline void HostFloat(const std::uint32_t* left, const std::uint32_t* right,
                      std::uint32_t fpscr, std::uint32_t* out, std::uint32_t& flags) noexcept {
    std::uint32_t accumulated = 0U;
    for (std::uint32_t row = 0U; row < 3U; ++row) {
        const std::uint32_t base = row * 4U;
        for (std::uint32_t column = 0U; column < 4U; ++column) {
            oot3d::recomp::a32::VfpBinary32Result value =
                column == 3U ? Oot3dVfpMultiplyAccumulate(left[base + 3U], right[3U],
                                                          left[base], fpscr)
                             : Oot3dVfpMultiply(right[column], left[base], fpscr);
            accumulated |= value.exception_flags;
            value = Oot3dVfpMultiplyAccumulate(value.value, right[4U + column],
                                               left[base + 1U], fpscr);
            accumulated |= value.exception_flags;
            value = Oot3dVfpMultiplyAccumulate(value.value, right[8U + column],
                                               left[base + 2U], fpscr);
            accumulated |= value.exception_flags;
            out[base + column] = value.value;
        }
    }
    flags = accumulated;
}

} // namespace Mtx3x4Detail

// Oot3dVfpZeroAwareMultiply / Oot3dVfpZeroAwareMultiplyAccumulate (standalone
// VMUL and VMLA, with the same exact-zero criterion) live in
// oot3d_native_a32_vfp_aarch64.h, alongside the other operations.

// The same result (out and flags) as Mtx3x4MultiplySoftware, faster.
// out must not overlap left or right (the caller uses local copies).
inline void Mtx3x4MultiplyFast(const std::uint32_t* left, const std::uint32_t* right,
                               std::uint32_t fpscr, std::uint32_t* out,
                               std::uint32_t& flags) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    if ((fpscr & Mtx3x4Detail::kFlushToZero) != 0U) [[likely]] {
        Mtx3x4Detail::Native(left, right, fpscr, out, flags);
        return;
    }
#endif
    if ((fpscr & Mtx3x4Detail::kRoundingMode) == 0U) {
        Mtx3x4Detail::HostFloat(left, right, fpscr, out, flags);
        return;
    }
    Mtx3x4MultiplySoftware(left, right, fpscr, out, flags);
}

} // namespace Oot3dNativeGame
