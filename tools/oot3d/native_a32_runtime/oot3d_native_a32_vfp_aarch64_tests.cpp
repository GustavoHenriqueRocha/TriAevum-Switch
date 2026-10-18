// Differential test: the native AArch64 VFP operations of
// oot3d_native_a32_vfp_aarch64.h, run with FPCR loaded from the guest FPSCR
// exactly as generated code runs them, must produce the same value and the
// same cumulative flags (hardware FPSR plus any software fallback flags) as
// the software implementation, for every operation, in all 16 combinations of
// rounding mode, flush-to-zero and default-NaN.
//
// Only meaningful on AArch64 (run natively or under qemu-aarch64). On other
// hosts the entry points are the software operations and the test passes
// trivially.

#include "oot3d_native_a32_vfp_aarch64.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

namespace a32 = oot3d::recomp::a32;
using namespace Oot3dNativeGame;

int gFailures = 0;
std::uint64_t gChecks = 0;
// Hot-path coverage: add/multiply results under FZ=1 that the native
// instruction keeps (nonzero, non-NaN).
std::uint64_t gNativeKept = 0;
// Zero-aware coverage: zero results under FZ=1 that the Oot3dVfpZeroAware*
// entry points keep on the native path (exact zeros), per operation family.
std::uint64_t gZeroKept = 0;
// Zero results under FZ=1 that are NOT exact (flushed underflow, denormal
// operands) and must still be recomputed: the zero-aware criterion has to
// reject them, or the sign/flags would differ (they are checked like the rest).
std::uint64_t gZeroRejected = 0;

struct Observed {
    std::uint32_t value;
    std::uint32_t flags;
};

#if OOT3D_GUEST_FP_AARCH64
std::uint64_t gHostFpcr = 0;

void EnterMode(std::uint32_t fpscr) {
    Oot3dWriteFpcr(Oot3dGuestFpcr(gHostFpcr, fpscr));
}

void ClearFlags() {
    Oot3dWriteFpsr(0U);
}

std::uint32_t HardwareFlags() {
    return static_cast<std::uint32_t>(Oot3dReadFpsr()) & kOot3dGuestFpFlagMask;
}
#else
void EnterMode(std::uint32_t) {
}
void ClearFlags() {
}
std::uint32_t HardwareFlags() {
    return 0U;
}
#endif

void Expect(const char* name, Observed observed, a32::VfpBinary32Result reference,
            std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t mode) {
    ++gChecks;
    if (observed.value == reference.value &&
        observed.flags == reference.exception_flags) {
        return;
    }
    if (++gFailures <= 40) {
        std::printf("%s a=%08x b=%08x c=%08x fpscr=%08x native=%08x/%02x "
                    "software=%08x/%02x\n",
                    name, a, b, c, mode, observed.value, observed.flags,
                    reference.value, reference.exception_flags);
    }
}

template <typename Operation>
Observed RunBinary(Operation operation, std::uint32_t a, std::uint32_t b,
                   std::uint32_t mode) {
    std::uint32_t fpscr = mode;
    std::uint32_t destination = 0xDEADBEEFU;
    ClearFlags();
    operation(destination, a, b, fpscr);
    const std::uint32_t flags = HardwareFlags() | (fpscr & kOot3dGuestFpFlagMask);
    if ((fpscr & ~kOot3dGuestFpFlagMask) != mode) {
        return {0xBADBADU, 0xFFU};
    }
    return {destination, flags};
}

template <typename Operation>
Observed RunTernary(Operation operation, std::uint32_t accumulator, std::uint32_t a,
                    std::uint32_t b, std::uint32_t mode) {
    std::uint32_t fpscr = mode;
    std::uint32_t destination = 0xDEADBEEFU;
    ClearFlags();
    operation(destination, accumulator, a, b, fpscr);
    const std::uint32_t flags = HardwareFlags() | (fpscr & kOot3dGuestFpFlagMask);
    if ((fpscr & ~kOot3dGuestFpFlagMask) != mode) {
        return {0xBADBADU, 0xFFU};
    }
    return {destination, flags};
}

template <typename Operation>
Observed RunUnary(Operation operation, std::uint32_t a, std::uint32_t mode) {
    std::uint32_t fpscr = mode;
    std::uint32_t destination = 0xDEADBEEFU;
    ClearFlags();
    operation(destination, a, fpscr);
    const std::uint32_t flags = HardwareFlags() | (fpscr & kOot3dGuestFpFlagMask);
    if ((fpscr & ~kOot3dGuestFpFlagMask) != mode) {
        return {0xBADBADU, 0xFFU};
    }
    return {destination, flags};
}

void CheckCompare(std::uint32_t a, std::uint32_t b, bool signalAllNans,
                  std::uint32_t mode) {
    // Preexisting NZCV bits must be replaced, other FPSCR bits kept.
    std::uint32_t fpscr = mode | 0x50000000U;
    ClearFlags();
    Oot3dVfpNativeCompare(a, b, signalAllNans, fpscr);
    const std::uint32_t flags = HardwareFlags() | (fpscr & kOot3dGuestFpFlagMask);
    const a32::VfpBinary32Result reference =
        a32::VfpBinary32Compare(a, b, signalAllNans);
    Observed observed{fpscr & 0xF0000000U, flags};
    if ((fpscr & 0x0FFFFF60U) != mode) {
        observed = {0xBADBADU, 0xFFU};
    }
    Expect(signalAllNans ? "vcmpe" : "vcmp", observed,
           {reference.value & 0xF0000000U, reference.exception_flags}, a, b, 0U, mode);
}

// The zero-aware entry points against the software model, every operation.
void CheckZeroAware(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t mode) {
    const bool flush = (mode & NativeVfpDetail::kFlushToZero) != 0U;
    const auto count = [&](a32::VfpBinary32Result reference, bool exact) {
        if (flush && NativeVfpDetail::ExactZero(reference.value)) {
            ++(exact ? gZeroKept : gZeroRejected);
        }
    };
    const a32::VfpBinary32Result add = a32::VfpBinary32Add(a, b, mode);
    count(add, NativeVfpDetail::ExactZeroSum(a, b));
    Expect("za_add", RunBinary(Oot3dVfpZeroAwareAdd, a, b, mode), add, a, b, c, mode);
    const a32::VfpBinary32Result sub = a32::VfpBinary32Subtract(a, b, mode);
    count(sub, NativeVfpDetail::ExactZeroSum(a, b ^ 0x80000000U));
    Expect("za_sub", RunBinary(Oot3dVfpZeroAwareSubtract, a, b, mode), sub, a, b, c, mode);
    const a32::VfpBinary32Result mul = a32::VfpBinary32Multiply(a, b, mode);
    count(mul, NativeVfpDetail::ExactZeroProduct(a, b));
    Expect("za_mul", RunBinary(Oot3dVfpZeroAwareMultiply, a, b, mode), mul, a, b, c, mode);
    Expect("za_nmul", RunBinary(Oot3dVfpZeroAwareNegativeMultiply, a, b, mode),
           a32::VfpBinary32NegativeMultiply(a, b, mode), a, b, c, mode);
    Expect("za_div", RunBinary(Oot3dVfpZeroAwareDivide, a, b, mode),
           a32::VfpBinary32Divide(a, b, mode), a, b, c, mode);
    const a32::VfpBinary32Result mla = a32::VfpBinary32MultiplyAccumulate(c, a, b, mode);
    const std::uint32_t product = a32::VfpBinary32Multiply(a, b, mode).value;
    count(mla, NativeVfpDetail::ExactZeroSum(c, product) &&
                   NativeVfpDetail::ProductAgrees(product, a, b));
    Expect("za_mla", RunTernary(Oot3dVfpZeroAwareMultiplyAccumulate, c, a, b, mode),
           mla, a, b, c, mode);
    const a32::VfpBinary32Result mls = a32::VfpBinary32MultiplySubtract(c, a, b, mode);
    count(mls, NativeVfpDetail::ExactZeroSum(c, product ^ 0x80000000U) &&
                   NativeVfpDetail::ProductAgrees(product, a, b));
    Expect("za_mls", RunTernary(Oot3dVfpZeroAwareMultiplySubtract, c, a, b, mode),
           mls, a, b, c, mode);
    Expect("za_nmla",
           RunTernary(Oot3dVfpZeroAwareNegativeMultiplyAccumulate, c, a, b, mode),
           a32::VfpBinary32NegativeMultiplyAccumulate(c, a, b, mode), a, b, c, mode);
    Expect("za_nmls",
           RunTernary(Oot3dVfpZeroAwareNegativeMultiplySubtract, c, a, b, mode),
           a32::VfpBinary32NegativeMultiplySubtract(c, a, b, mode), a, b, c, mode);
    Expect("za_sqrt", RunUnary(Oot3dVfpZeroAwareSquareRoot, a, mode),
           a32::VfpBinary32SquareRoot(a, mode), a, b, c, mode);
}

void CheckAll(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t mode) {
    const auto kept = [&](std::uint32_t value) {
        if ((mode & NativeVfpDetail::kFlushToZero) != 0U &&
            !NativeVfpDetail::ZeroOrNan(value)) {
            ++gNativeKept;
        }
    };
    const a32::VfpBinary32Result add = a32::VfpBinary32Add(a, b, mode);
    kept(add.value);
    Expect("add", RunBinary(Oot3dVfpNativeAdd, a, b, mode), add, a, b, c, mode);
    Expect("sub", RunBinary(Oot3dVfpNativeSubtract, a, b, mode),
           a32::VfpBinary32Subtract(a, b, mode), a, b, c, mode);
    const a32::VfpBinary32Result mul = a32::VfpBinary32Multiply(a, b, mode);
    kept(mul.value);
    Expect("mul", RunBinary(Oot3dVfpNativeMultiply, a, b, mode), mul, a, b, c, mode);
    Expect("nmul", RunBinary(Oot3dVfpNativeNegativeMultiply, a, b, mode),
           a32::VfpBinary32NegativeMultiply(a, b, mode), a, b, c, mode);
    Expect("div", RunBinary(Oot3dVfpNativeDivide, a, b, mode),
           a32::VfpBinary32Divide(a, b, mode), a, b, c, mode);
    Expect("mla", RunTernary(Oot3dVfpNativeMultiplyAccumulate, c, a, b, mode),
           a32::VfpBinary32MultiplyAccumulate(c, a, b, mode), a, b, c, mode);
    Expect("mls", RunTernary(Oot3dVfpNativeMultiplySubtract, c, a, b, mode),
           a32::VfpBinary32MultiplySubtract(c, a, b, mode), a, b, c, mode);
    Expect("nmla", RunTernary(Oot3dVfpNativeNegativeMultiplyAccumulate, c, a, b, mode),
           a32::VfpBinary32NegativeMultiplyAccumulate(c, a, b, mode), a, b, c, mode);
    Expect("nmls", RunTernary(Oot3dVfpNativeNegativeMultiplySubtract, c, a, b, mode),
           a32::VfpBinary32NegativeMultiplySubtract(c, a, b, mode), a, b, c, mode);
    Expect("sqrt", RunUnary(Oot3dVfpNativeSquareRoot, a, mode),
           a32::VfpBinary32SquareRoot(a, mode), a, b, c, mode);
    Expect("from_signed", RunUnary(Oot3dVfpNativeFromSigned, a, mode),
           a32::VfpBinary32FromSigned(a, mode), a, b, c, mode);
    Expect("from_unsigned", RunUnary(Oot3dVfpNativeFromUnsigned, a, mode),
           a32::VfpBinary32FromUnsigned(a, mode), a, b, c, mode);
    Expect("to_signed", RunUnary(Oot3dVfpNativeToSigned, a, mode),
           a32::VfpBinary32ToSigned(a, mode), a, b, c, mode);
    Expect("to_unsigned", RunUnary(Oot3dVfpNativeToUnsigned, a, mode),
           a32::VfpBinary32ToUnsigned(a, mode), a, b, c, mode);
    CheckCompare(a, b, false, mode);
    CheckCompare(a, b, true, mode);
    CheckZeroAware(a, b, c, mode);
}

std::array<std::uint32_t, 16> Modes() {
    // RN, RP, RM, RZ, each with and without FZ and DN.
    std::array<std::uint32_t, 16> modes{};
    for (std::uint32_t i = 0; i < modes.size(); ++i) {
        modes[i] = ((i & 3U) << 22U) | (((i >> 2U) & 1U) << 24U) |
                   (((i >> 3U) & 1U) << 25U);
    }
    return modes;
}

// Biased toward exponent edges, denormals, zeros, the overflow edge,
// infinities, quiet and signaling NaNs, and integer-valued floats.
std::uint32_t Operand(std::mt19937_64& random) {
    const std::uint32_t sign = static_cast<std::uint32_t>(random() & 1U) << 31U;
    const std::uint32_t fraction = static_cast<std::uint32_t>(random()) & 0x007FFFFFU;
    static constexpr std::array<std::uint32_t, 22> kEdgeExponents = {
        0U, 1U, 2U, 3U, 23U, 24U, 25U, 26U, 100U, 126U, 127U, 128U,
        150U, 157U, 158U, 159U, 160U, 252U, 253U, 254U, 255U, 255U};
    std::uint32_t exponent = 0U;
    switch (random() % 10U) {
    case 0:
        return sign;
    case 1:
    case 2:
        exponent = kEdgeExponents[random() % kEdgeExponents.size()];
        break;
    case 3:
        // Short significands make exact results and cancellation frequent.
        return sign | ((static_cast<std::uint32_t>(96U + random() % 64U)) << 23U) |
               (fraction & 0x007F0000U);
    case 4: {
        // Small fractions: exact, tie and near-tie cases; NaN payload edges.
        const std::uint32_t small = static_cast<std::uint32_t>(random() % 8U);
        exponent = kEdgeExponents[random() % kEdgeExponents.size()];
        return sign | (exponent << 23U) |
               ((random() & 1U) != 0U ? small : (0x007FFFFFU - small));
    }
    case 5:
        // Raw integers for the integer-to-float conversions.
        return static_cast<std::uint32_t>(random());
    default:
        exponent = static_cast<std::uint32_t>(random() % 256U);
        break;
    }
    return sign | (exponent << 23U) | fraction;
}

struct Triple {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;
};

void RunCampaign(std::uint64_t seed, std::size_t iterations) {
    std::mt19937_64 random(seed);
    std::vector<Triple> triples;
    triples.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        std::uint32_t a = Operand(random);
        std::uint32_t b = Operand(random);
        std::uint32_t c = Operand(random);
        switch (random() % 8U) {
        case 0:
            b = a ^ 0x80000000U;  // exact cancellation
            break;
        case 1:
            c = a32::VfpBinary32Multiply(a, b, 0U).value ^
                ((random() & 1U) != 0U ? 0x80000000U : 0U);
            break;
        case 2:
            // Pairs whose sum or product lands near 2^-126.
            b = (b & 0x807FFFFFU) | ((static_cast<std::uint32_t>(random() % 8U)) << 23U);
            break;
        case 3:
            // Products near the denormal boundary and its rounding edge.
            b = (b & 0x807FFFFFU) |
                ((static_cast<std::uint32_t>(
                     ((a >> 23U) & 0xFFU) <= 127U
                         ? 127U - ((a >> 23U) & 0xFFU) + random() % 4U
                         : random() % 4U))
                 << 23U);
            break;
        case 4:
            b = a;  // x - x, x / x, equal compares
            break;
        case 5:
            // Zero factor with a zero accumulator of either sign (exact
            // zero products and sums), or a product cancelling the
            // accumulator under FZ (VMLS/VNMLS cancellation).
            if ((random() & 1U) != 0U) {
                a &= 0x80000000U;
                c &= 0x80000000U;
            } else {
                c = a32::VfpBinary32Multiply(a, b, 1U << 24U).value ^
                    ((random() & 1U) != 0U ? 0x80000000U : 0U);
            }
            break;
        default:
            break;
        }
        triples.push_back({a, b, c});
    }
    for (const std::uint32_t mode : Modes()) {
        EnterMode(mode);
        for (const Triple& triple : triples) {
            CheckAll(triple.a, triple.b, triple.c, mode);
        }
    }
}

// Every unary input in the bands where the operations change behavior,
// strided elsewhere, in every mode.
void RunUnarySweep() {
    for (const std::uint32_t mode : Modes()) {
        EnterMode(mode);
        for (std::uint32_t sign = 0; sign < 2U; ++sign) {
            for (std::uint32_t exponent = 0; exponent < 256U; ++exponent) {
                const bool dense = exponent <= 2U || exponent >= 253U ||
                                   (exponent >= 125U && exponent <= 160U);
                const std::uint32_t step = dense ? 4093U : 65521U;
                for (std::uint32_t fraction = 0; fraction < 0x00800000U;
                     fraction += step) {
                    for (const std::uint32_t edge : {fraction, 0x007FFFFFU - fraction}) {
                        const std::uint32_t value = (sign << 31U) | (exponent << 23U) | edge;
                        Expect("sqrt", RunUnary(Oot3dVfpNativeSquareRoot, value, mode),
                               a32::VfpBinary32SquareRoot(value, mode), value, 0, 0, mode);
                        Expect("za_sqrt", RunUnary(Oot3dVfpZeroAwareSquareRoot, value, mode),
                               a32::VfpBinary32SquareRoot(value, mode), value, 0, 0, mode);
                        Expect("to_signed", RunUnary(Oot3dVfpNativeToSigned, value, mode),
                               a32::VfpBinary32ToSigned(value, mode), value, 0, 0, mode);
                        Expect("to_unsigned", RunUnary(Oot3dVfpNativeToUnsigned, value, mode),
                               a32::VfpBinary32ToUnsigned(value, mode), value, 0, 0, mode);
                        Expect("from_signed", RunUnary(Oot3dVfpNativeFromSigned, value, mode),
                               a32::VfpBinary32FromSigned(value, mode), value, 0, 0, mode);
                        Expect("from_unsigned",
                               RunUnary(Oot3dVfpNativeFromUnsigned, value, mode),
                               a32::VfpBinary32FromUnsigned(value, mode), value, 0, 0, mode);
                    }
                }
            }
        }
    }
}

// Exact operands the random campaign could miss: signed zeros, denormals of
// both signs, minimum normal, maximum finite, infinities, quiet/signaling NaNs.
void RunSpecialGrid() {
    static constexpr std::array<std::uint32_t, 24> kSpecial = {
        0x00000000U, 0x80000000U, 0x00000001U, 0x80000001U, 0x007FFFFFU,
        0x807FFFFFU, 0x00400000U, 0x00800000U, 0x80800000U, 0x00800001U,
        0x3F800000U, 0xBF800000U, 0x3F800001U, 0x4B000001U, 0x4F000000U,
        0xCF000000U, 0x7F7FFFFFU, 0xFF7FFFFFU, 0x7F800000U, 0xFF800000U,
        0x7FC00000U, 0xFFC00001U, 0x7F800001U, 0xFFA00005U};
    for (const std::uint32_t mode : Modes()) {
        EnterMode(mode);
        for (const std::uint32_t a : kSpecial) {
            for (const std::uint32_t b : kSpecial) {
                for (const std::uint32_t c : kSpecial) {
                    CheckAll(a, b, c, mode);
                }
            }
        }
    }
}

} // namespace

int main() {
#if OOT3D_GUEST_FP_AARCH64
    gHostFpcr = Oot3dReadFpcr();
#endif
    RunSpecialGrid();
#if defined(OOT3D_VFP_TEST_QUICK)
    // Só para as mutações (run_vfp_zero_aware_mutations.sh): grade especial
    // e uma campanha curta; o teste completo é o de baixo.
    RunCampaign(0x0A32F00DULL, 20000U);
#else
    RunUnarySweep();
    RunCampaign(0x0A32F00DULL, 400000U);
    RunCampaign(0xA64E5ULL, 200000U);
#endif
#if OOT3D_GUEST_FP_AARCH64
    Oot3dWriteFpcr(gHostFpcr);
#if defined(OOT3D_VFP_TEST_QUICK)
    if (gNativeKept < 100000U) {
#else
    if (gNativeKept < 1000000U) {
#endif
        std::printf("native results kept too rarely: %llu\n",
                    static_cast<unsigned long long>(gNativeKept));
        return 1;
    }
#if defined(OOT3D_VFP_TEST_QUICK)
    if (gZeroKept < 1000U || gZeroRejected < 100U) {
#else
    if (gZeroKept < 100000U || gZeroRejected < 10000U) {
#endif
        std::printf("zero-aware coverage too low: kept %llu, rejected %llu\n",
                    static_cast<unsigned long long>(gZeroKept),
                    static_cast<unsigned long long>(gZeroRejected));
        return 1;
    }
#endif
    if (gFailures != 0) {
        std::printf("%d of %llu native VFP checks differ\n", gFailures,
                    static_cast<unsigned long long>(gChecks));
        return 1;
    }
    std::printf("native AArch64 VFP matches the software implementation "
                "(%llu checks, %llu add/mul results on the native path, "
                "%llu exact zeros kept native by the zero-aware entry points, "
                "%llu inexact zeros recomputed, native=%d)\n",
                static_cast<unsigned long long>(gChecks),
                static_cast<unsigned long long>(gNativeKept),
                static_cast<unsigned long long>(gZeroKept),
                static_cast<unsigned long long>(gZeroRejected),
                OOT3D_GUEST_FP_AARCH64);
    return 0;
}
