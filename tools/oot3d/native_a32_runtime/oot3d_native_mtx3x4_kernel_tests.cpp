// Differential test of Mtx3x4MultiplyFast (oot3d_native_mtx3x4_kernel.h)
// against Mtx3x4MultiplySoftware (VfpBinary32*, the authoritative semantics):
// the 12 outputs and the accumulated flags must be bit-identical, across the 16
// rounding mode x FZ x DN modes, with game matrices, sparse ones (exact
// zeros, the case that forces software on the native path), with tiny
// values (subnormals and products that underflow), huge ones
// (overflow), infinities and NaNs.
//
// On x86 it exercises the host float paths (oot3d_native_a32_vfp_fast.h);
// on AArch64 (native or qemu-aarch64, see run_vfp_aarch64_tests_qemu.sh) the
// native path with the guest FPCR.
//
// Matrices per mode: OOT3D_MTX3X4_TRIALS (default 400000; the minimal qemu
// runtime passes no arguments).

#include "oot3d_native_mtx3x4_kernel.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

namespace {

using namespace Oot3dNativeGame;

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(std::uint64_t seed) : engine(seed) {}
    std::uint32_t U32() { return static_cast<std::uint32_t>(engine()); }
    std::uint32_t Below(std::uint32_t n) { return static_cast<std::uint32_t>(engine() % n); }
    bool Chance(std::uint32_t percent) { return Below(100U) < percent; }

    std::uint32_t FromFloat(float value) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, 4);
        return bits;
    }

    // One element according to the matrix "style".
    std::uint32_t Element(std::uint32_t style) {
        static constexpr std::uint32_t kSpecial[] = {
            0x00000000U, 0x80000000U, 0x3F800000U, 0xBF800000U, 0x7F800000U, 0xFF800000U,
            0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0x7FBFFFFFU, 0x00000001U, 0x807FFFFFU,
            0x00800000U, 0x80800000U, 0x7F7FFFFFU, 0xFF7FFFFFU, 0x00800001U, 0x1F800000U,
            0x9F800000U, 0x5F800000U, 0x20000000U, 0x1FFFFFFFU,
        };
        switch (style) {
        case 0: { // game matrix: rotation/scale/translation
            const std::uint32_t kind = Below(10U);
            if (kind < 2U) return Chance(50) ? 0x00000000U : 0x80000000U;
            if (kind < 3U) return Chance(50) ? 0x3F800000U : 0xBF800000U;
            if (kind < 7U) {
                const float value = static_cast<float>(static_cast<int>(Below(2000001U)) - 1000000) * 1.0e-6F;
                return FromFloat(value);
            }
            const float value = static_cast<float>(static_cast<int>(Below(200001U)) - 100000) * 0.05F;
            return FromFloat(value);
        }
        case 1: // sparse: many exact zeros
            if (Chance(55)) return Chance(70) ? 0x00000000U : 0x80000000U;
            return Element(0U);
        case 2: { // magnitudes near the lower limit (subnormals, underflow)
            const std::uint32_t exponent = Below(70U);
            const std::uint32_t sign = Chance(50) ? 0x80000000U : 0U;
            return sign | (exponent << 23U) | (U32() & 0x007FFFFFU);
        }
        case 3: { // magnitudes near overflow
            const std::uint32_t exponent = 190U + Below(65U);
            const std::uint32_t sign = Chance(50) ? 0x80000000U : 0U;
            return sign | (exponent << 23U) | (U32() & 0x007FFFFFU);
        }
        case 4: // specials
            return kSpecial[Below(sizeof(kSpecial) / sizeof(kSpecial[0]))];
        case 5: { // cancelling sums / large exponent difference
            const std::uint32_t exponent = 60U + Below(140U);
            const std::uint32_t sign = Chance(50) ? 0x80000000U : 0U;
            return sign | (exponent << 23U) | (Chance(30) ? 0U : (U32() & 0x007FFFFFU));
        }
        default:
            return U32();
        }
    }
};

int gFailures = 0;
std::uint64_t gChecks = 0;

void Report(const char* why, std::uint32_t fpscr, const std::uint32_t* left,
            const std::uint32_t* right, const std::uint32_t* a, std::uint32_t fa,
            const std::uint32_t* b, std::uint32_t fb) {
    if (++gFailures > 10) return;
    std::printf("FAIL %s fpscr=%08X\n  left:    ", why, fpscr);
    for (int i = 0; i < 12; ++i) std::printf(" %08X", left[i]);
    std::printf("\n  right:   ");
    for (int i = 0; i < 12; ++i) std::printf(" %08X", right[i]);
    std::printf("\n  fast:    ");
    for (int i = 0; i < 12; ++i) std::printf(" %08X", a[i]);
    std::printf(" flags=%02X\n  software:", fa);
    for (int i = 0; i < 12; ++i) std::printf(" %08X", b[i]);
    std::printf(" flags=%02X\n", fb);
}

// Standalone VMUL/VMLA (Oot3dVfpZeroAwareMultiply/MultiplyAccumulate) in the mode
// generated code runs in: FPCR with the guest mode, flags in FPSR plus those
// from fallbacks to software. Accumulators chosen to cancel the product and
// zeros forced in some of the cases.
std::uint64_t gOpChecks = 0;

void OperationTests(Rng& rng, std::uint64_t perMode) {
    namespace a32 = oot3d::recomp::a32;
    for (std::uint32_t mode = 0U; mode < 16U; ++mode) {
        const std::uint32_t control = ((mode & 3U) << 22U) | (((mode >> 2U) & 1U) << 24U) |
                                      (((mode >> 3U) & 1U) << 25U);
        for (std::uint64_t trial = 0; trial < perMode; ++trial) {
            const std::uint32_t style = rng.Below(8U);
            std::uint32_t left = rng.Element(style), right = rng.Element(rng.Chance(80) ? style : rng.Below(7U));
            if (rng.Chance(15)) (rng.Chance(50) ? left : right) = rng.Chance(50) ? 0x00000000U : 0x80000000U;
            const auto product = a32::VfpBinary32Multiply(left, right, control);
            std::uint32_t accumulator = rng.Element(rng.Below(7U));
            switch (rng.Below(5U)) {
            case 0: accumulator = product.value ^ 0x80000000U; break;        // cancels
            case 1: accumulator = rng.Chance(50) ? 0x00000000U : 0x80000000U; break;
            case 2: accumulator = product.value ^ 0x80000000U ^ (rng.Below(3U)); break; // almost cancels
            default: break;
            }
            std::uint32_t mulValue = 0U, macValue = 0U;
            std::uint32_t mulFlags = control, macFlags = control;
            std::uint32_t mulHardware = 0U, macHardware = 0U;
#if OOT3D_GUEST_FP_AARCH64
            const std::uint64_t host = Oot3dReadFpcr();
            Oot3dWriteFpcr(Oot3dGuestFpcr(host, control));
            Oot3dWriteFpsr(0U);
            Oot3dVfpZeroAwareMultiply(mulValue, left, right, mulFlags);
            mulHardware = static_cast<std::uint32_t>(Oot3dReadFpsr());
            Oot3dWriteFpsr(0U);
            Oot3dVfpZeroAwareMultiplyAccumulate(macValue, accumulator, left, right, macFlags);
            macHardware = static_cast<std::uint32_t>(Oot3dReadFpsr());
            Oot3dWriteFpcr(host);
            Oot3dWriteFpsr(0U);
#else
            Oot3dVfpZeroAwareMultiply(mulValue, left, right, mulFlags);
            Oot3dVfpZeroAwareMultiplyAccumulate(macValue, accumulator, left, right, macFlags);
#endif
            const auto mac = a32::VfpBinary32MultiplyAccumulate(accumulator, left, right, control);
            gOpChecks += 2U;
            const bool mulOk = mulValue == product.value &&
                ((mulFlags | mulHardware) & kMtx3x4FpscrFlagMask) ==
                    (product.exception_flags & kMtx3x4FpscrFlagMask);
            const bool macOk = macValue == mac.value &&
                ((macFlags | macHardware) & kMtx3x4FpscrFlagMask) ==
                    (mac.exception_flags & kMtx3x4FpscrFlagMask);
            if ((!mulOk || !macOk) && ++gFailures <= 10) {
                std::printf("FAIL operation %s fpscr=%08X acc=%08X l=%08X r=%08X: "
                            "fast %08X/%08X flags %02X/%02X, software %08X/%08X flags %02X/%02X\n",
                            mulOk ? "vmla" : "vmul", control, accumulator, left, right, mulValue,
                            macValue, (mulFlags | mulHardware) & 0x9FU, (macFlags | macHardware) & 0x9FU,
                            product.value, mac.value, product.exception_flags & 0x9FU,
                            mac.exception_flags & 0x9FU);
            }
        }
    }
}

} // namespace

#ifndef OOT3D_MTX3X4_TRIALS
#define OOT3D_MTX3X4_TRIALS 400000
#endif

int main() {
    const std::uint64_t perMode = OOT3D_MTX3X4_TRIALS;
    Rng rng(0x3A7C174ULL);
    for (std::uint32_t mode = 0U; mode < 16U; ++mode) {
        const std::uint32_t control = ((mode & 3U) << 22U) | (((mode >> 2U) & 1U) << 24U) |
                                      (((mode >> 3U) & 1U) << 25U);
        for (std::uint64_t trial = 0; trial < perMode; ++trial) {
            std::uint32_t left[12], right[12], fast[12], soft[12];
            const std::uint32_t styleLeft = rng.Below(8U), styleRight = rng.Below(8U);
            for (int i = 0; i < 12; ++i) {
                // Sometimes an element of another style mixed in.
                left[i] = rng.Element(rng.Chance(90) ? styleLeft : rng.Below(7U));
                right[i] = rng.Element(rng.Chance(90) ? styleRight : rng.Below(7U));
            }
            if (rng.Chance(10)) { // sums that cancel exactly
                const int i = static_cast<int>(rng.Below(3U)) * 4;
                left[i + 3] = rng.Element(0U);
                right[3] = left[i + 3] ^ 0x80000000U;
                left[i] = 0x3F800000U;
            }
            std::uint32_t fpscr = control;
            if (rng.Chance(30)) fpscr |= rng.U32() & 0x9FU;       // already-accumulated flags
            if (rng.Chance(30)) fpscr |= rng.U32() & 0xF0000000U; // NZCV
            std::uint32_t fastFlags = 0xFFFFFFFFU, softFlags = 0xFFFFFFFFU;
            Mtx3x4MultiplyFast(left, right, fpscr, fast, fastFlags);
            Mtx3x4MultiplySoftware(left, right, fpscr, soft, softFlags);
            ++gChecks;
            if (std::memcmp(fast, soft, sizeof(fast)) != 0) {
                Report("value", fpscr, left, right, fast, fastFlags, soft, softFlags);
            } else if ((fastFlags & kMtx3x4FpscrFlagMask) != (softFlags & kMtx3x4FpscrFlagMask)) {
                Report("flags", fpscr, left, right, fast, fastFlags, soft, softFlags);
            }
        }
    }
    OperationTests(rng, perMode * 4U);
    std::printf("%s mtx3x4: %llu matrices (%llu operations) + %llu standalone operations, %d failures\n",
                gFailures == 0 ? "OK" : "FAILED", static_cast<unsigned long long>(gChecks),
                static_cast<unsigned long long>(gChecks * 36U),
                static_cast<unsigned long long>(gOpChecks), gFailures);
    return gFailures == 0 ? 0 : 1;
}
