#pragma once

// Mtx3x4_Multiply (0x0036C174), a multiplicação de matrizes 3x4 mais chamada
// do jogo (~440 vezes por quadro na floresta Kokiri, sempre pela chamada
// externa de ExecuteMtx3x4Multiply em oot3d_native_compiled_functions.cpp).
//
// Semântica (a sequência do guest, VMUL/VMLA não fundidos): para cada linha r
// e coluna c de out = right x left (layout do guest, 3 linhas de 4 floats),
//   out[4r + c] = ((right[c] * left[4r] (+ left[4r + 3] se c == 3))
//                  + right[4 + c] * left[4r + 1]) + right[8 + c] * left[4r + 2]
// com cada produto arredondado antes da soma, e as flags acumuladas de todas
// as operações. A referência é Mtx3x4MultiplySoftware (VfpBinary32*, a
// implementação autoritativa); Mtx3x4MultiplyFast devolve exatamente o mesmo
// valor e as mesmas flags (oot3d_native_mtx3x4_kernel_tests.cpp, em x86 e sob
// qemu-aarch64):
//   - AArch64 com FPSCR.FZ = 1 (o modo do jogo): as instruções nativas de
//     oot3d_native_a32_vfp_aarch64.h (as mesmas do código gerado com
//     --native-vfp-aarch64) com o FPCR carregado localmente do FPSCR guest e
//     a mesma regra de volta ao software (resultado zero ou NaN), exceto os
//     zeros exatos nos dois modelos (ExactZeroProduct, cancelamento exato),
//     que em matrizes esparsas são a maioria das voltas ao software;
//   - senão, em arredondamento ao mais próximo: os caminhos rápidos de
//     oot3d_native_a32_vfp_fast.h (float do host + termo de erro exato);
//   - senão: o software.
// Só vale para FPSCR sem LEN/STRIDE e sem armadilhas habilitadas (o chamador
// usa o software nesses casos, como antes).

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
[[nodiscard]] constexpr bool ExactZero(std::uint32_t bits) noexcept {
    return (bits & 0x7FFFFFFFU) == 0U;
}

// Finito e não subnormal (zero incluído).
[[nodiscard]] constexpr bool FiniteNotDenormal(std::uint32_t bits) noexcept {
    const std::uint32_t exponent = bits & 0x7F800000U;
    return exponent != 0x7F800000U && (exponent != 0U || ExactZero(bits));
}

// O produto nativo deu zero sem nenhum arredondamento nem descarte para zero:
// um fator é zero e o outro é finito e não subnormal. Aí o sinal (o OU
// exclusivo dos sinais) e a ausência de flags são os do IEEE nos dois
// modelos; o descarte de subnormal (FZ), único caso em que o ARMv8 e o
// software diferem no sinal de um zero, não acontece.
[[nodiscard]] constexpr bool ExactZeroProduct(std::uint32_t left, std::uint32_t right) noexcept {
    return FiniteNotDenormal(left) && FiniteNotDenormal(right) &&
           (ExactZero(left) || ExactZero(right));
}

// VMUL nativo como Oot3dVfpNativeMultiply (FZ = 1), mas um zero exato
// (ExactZeroProduct) fica com o resultado nativo em vez de ir ao software.
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

// VMLA nativo como Oot3dVfpNativeMultiplyAccumulate (FZ = 1). Uma soma zero
// também fica com o resultado nativo quando é exata nos dois modelos: o
// acumulador não é subnormal e (a) o produto é não nulo (então normal: com
// FZ o produto nativo não é subnormal, e coincide com o do software) e é
// exatamente o oposto do acumulador, ou seja, cancelamento exato (uma soma
// de normais que só dá um subnormal descartado NÃO conta: aí o ARMv8 mantém
// o sinal e o software dá +0); ou (b) o produto é um zero exato
// (ExactZeroProduct) e o acumulador é zero. O sinal do zero segue a regra
// IEEE do modo de arredondamento nos dois modelos.
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

// Ordem das operações livre entre saídas (cada saída é uma cadeia
// independente e as flags são acumuladas por OU), fixa dentro de cada
// cadeia; as operações nativas são asm volatile, então o compilador não as
// funde nem as tira de dentro do escopo do FPCR guest.
inline void Native(const std::uint32_t* left, const std::uint32_t* right,
                   std::uint32_t fpscr, std::uint32_t* out, std::uint32_t& flags) noexcept {
    std::uint32_t work = fpscr & ~kMtx3x4FpscrFlagMask;
    // O que Oot3dGuestFpScope faz, sem o estado por thread (nada aqui chama
    // código que consulte gOot3dGuestFp): FPCR com o modo guest, FPSR zerado
    // para coletar as flags, e os dois do chamador restaurados no fim (se o
    // chamador já estava no modo guest, o FPSR dele volta intacto).
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

// VMUL/VMLA para código que já roda com o FPCR no modo guest (o C++ gerado e
// as substituições nativas): o mesmo valor e as mesmas flags que
// Oot3dVfpNativeMultiply / Oot3dVfpNativeMultiplyAccumulate (e portanto que o
// software), mas com FZ = 1 os zeros exatos (Mtx3x4Detail::Multiply e
// MultiplyAccumulate) ficam no caminho nativo.
OOT3D_AOT_HOT void Oot3dVfpZeroAwareMultiply(std::uint32_t& destination, std::uint32_t left,
                                             std::uint32_t right, std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    if ((fpscr & Mtx3x4Detail::kFlushToZero) != 0U) [[likely]] {
        Mtx3x4Detail::Multiply(destination, left, right, fpscr);
        return;
    }
#endif
    Oot3dVfpNativeMultiply(destination, left, right, fpscr);
}

OOT3D_AOT_HOT void Oot3dVfpZeroAwareMultiplyAccumulate(std::uint32_t& destination,
                                                       std::uint32_t accumulator,
                                                       std::uint32_t left, std::uint32_t right,
                                                       std::uint32_t& fpscr) noexcept {
#if OOT3D_GUEST_FP_AARCH64
    if ((fpscr & Mtx3x4Detail::kFlushToZero) != 0U) [[likely]] {
        Mtx3x4Detail::MultiplyAccumulate(destination, accumulator, left, right, fpscr);
        return;
    }
#endif
    Oot3dVfpNativeMultiplyAccumulate(destination, accumulator, left, right, fpscr);
}

// O mesmo resultado (out e flags) que Mtx3x4MultiplySoftware, mais rápido.
// out não pode se sobrepor a left nem a right (o chamador usa cópias locais).
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
