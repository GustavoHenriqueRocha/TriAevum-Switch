// Substituições nativas das funções guest mais quentes. Contrato e garantias
// em oot3d_native_guest_overrides.h; cada função abaixo espelha a tradução
// gerada (mesma ordem de acessos à memória, mesmas chamadas de VFP, mesmas
// máscaras de flags), só que sem a entrada de bloco por bloco, sem o teste de
// FPSCR por instrução e com os registradores em variáveis locais.
//
// Regras seguidas em todas:
//  1. Toda recusa (return Decline(...)) acontece antes do primeiro efeito
//     colateral: nenhuma escrita na memória guest, no estado ou no contexto.
//  2. Antes do primeiro efeito, valida-se que cada acesso que vem depois cai
//     no caminho rápido da memória (página rápida, alinhado, sem cruzar
//     página); a seguir os acessos são feitos com as MESMAS chamadas
//     ReadFast/WriteFast da tradução, na mesma ordem, de modo que gerações de
//     escrita, carimbos de página e contadores de diagnóstico ficam iguais.
//  3. Literais de VLDR no segmento de texto (somente leitura, igual ao
//     dobramento de literais do gerador) viram constantes.
//  4. As operações de VFP usam os mesmos auxiliares do código gerado
//     (Oot3dVfp*, a32::VfpBinary32*) com o FPSCR evoluindo na mesma ordem;
//     nenhuma aritmética de ponto flutuante do host é usada diretamente.

#include "oot3d_native_guest_overrides.h"

#include "oot3d_native_a32_memory.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Oot3dNativeGame {

bool gOot3dGuestOverridesEnabled = true;

void SetOot3dGuestOverridesEnabled(bool enabled) noexcept {
    gOot3dGuestOverridesEnabled = enabled;
}

namespace {

namespace a32 = oot3d::recomp::a32;

enum OverrideIndex : size_t {
    kIdx00372070,
    kIdx002CDE90,
    kIdx003094CC,
    kIdx002F9430,
    kIdx0036E168,
    kIdx003325F0,
    kIdx00307C94,
    kIdx003727F0,
    kOverrideCount,
};

Oot3dGuestOverrideInfo gOverrides[kOverrideCount] = {
    {0x00372070U, "oot3d_translate_mtx3x4_local"},
    {0x002CDE90U, "FUN_002cde90"},
    {0x003094CCU, "FUN_003094cc"},
    {0x002F9430U, "PauseRenderBuffer_SetVec2Range"},
    {0x0036E168U, "Math_SmoothStepToF"},
    {0x003325F0U, "Math3D_CylOutsideCylDist"},
    {0x00307C94U, "PicaCommandWriter_UploadVertexFloatUniforms"},
    {0x003727F0U, "Math_SinF"},
};

constexpr uint32_t kFpscrTrapMask = 0x00379F00U; // LEN/STRIDE e habilitação de exceções
constexpr uint32_t kPageSize = 0x1000U;

inline bool Decline(size_t index) noexcept {
    ++gOverrides[index].Declined;
    return false;
}

inline bool Accept(size_t index) noexcept {
    ++gOverrides[index].Calls;
    return true;
}

// Algum gancho de entrada de bloco (ou o modo "todos os blocos") cobre
// [first, last]? Igual a NativeA32UnobservedMemory::Observed.
inline bool Hooked(const Oot3dWholeAotContext& context, uint32_t first,
                   uint32_t last) noexcept {
    if (context.BlockEntry == nullptr) return false;
    if (context.BlockEntryPcCount == 0U) return true;
    const uint32_t* end = context.BlockEntryPcs + context.BlockEntryPcCount;
    const uint32_t* hook = std::lower_bound(context.BlockEntryPcs, end, first);
    return hook != end && *hook <= last;
}

inline bool Budget(const Oot3dWholeAotContext& context,
                   uint32_t blocks) noexcept {
    return context.BlocksRemaining >= blocks;
}

inline void Consume(Oot3dWholeAotContext& context, uint32_t blocks) noexcept {
    context.BlocksRemaining -= blocks;
    context.BlocksConsumed += blocks;
}

inline bool VfpDefault(uint32_t fpscr) noexcept {
    return (fpscr & kFpscrTrapMask) == 0U;
}

// [address, address + size) alinhado a `align`, sem dar a volta no espaço de
// 32 bits e coberto por páginas rápidas (de escrita, se `write`). Garante que
// ReadFast/WriteFast de tamanho <= align nesse intervalo não falham.
bool FastRange(const NativeA32Memory& memory, uint32_t address, uint32_t size,
               uint32_t align, bool write) noexcept {
    if (size == 0U) return true;
    if ((address & (align - 1U)) != 0U) return false;
    if (static_cast<uint64_t>(address) + size > (uint64_t{1} << 32U)) {
        return false;
    }
    while (size != 0U) {
        const uint32_t room = kPageSize - (address & (kPageSize - 1U));
        const uint32_t chunk = std::min(room, size);
        const bool fast = write ? memory.SpanOnFastWritePage(address, chunk)
                                : memory.SpanOnFastReadPage(address, chunk);
        if (!fast) return false;
        address += chunk;
        size -= chunk;
    }
    return true;
}

inline bool Readable(const NativeA32Memory& memory, uint32_t address,
                     uint32_t size, uint32_t align = 4U) noexcept {
    return FastRange(memory, address, size, align, false);
}

inline bool Writable(const NativeA32Memory& memory, uint32_t address,
                     uint32_t size, uint32_t align = 4U) noexcept {
    return FastRange(memory, address, size, align, true);
}

// Acumula o resultado de uma operação VFP como Oot3dAotCommitVfp.
inline uint32_t Commit(uint32_t& fpscr, a32::VfpBinary32Result result) noexcept {
    fpscr |= result.exception_flags;
    return result.value;
}

// VCMP/VCMPE seguido de VMRS APSR_nzcv: NZCV do FPSCR e das flags.
inline void Compare(uint32_t& fpscr, Oot3dAotLazyFlags& flags, uint32_t left,
                    uint32_t right, bool signaling) noexcept {
    const auto comparison = a32::VfpBinary32Compare(left, right, signaling);
    fpscr = (fpscr & ~0xF0000000U) | (comparison.value & 0xF0000000U);
    fpscr |= comparison.exception_flags;
    flags.AssignPackedNzcv(fpscr);
}

inline uint32_t Read32(NativeA32Memory& memory, uint32_t address) noexcept {
    uint32_t value = 0U;
    memory.ReadFast<uint32_t>(address, &value);
    return value;
}

inline Oot3dAotFlagMask Mask(uint8_t bits) noexcept {
    return static_cast<Oot3dAotFlagMask>(bits);
}

} // namespace

size_t Oot3dGuestOverrideCount() noexcept { return kOverrideCount; }

const Oot3dGuestOverrideInfo* Oot3dGuestOverrideTable() noexcept {
    return gOverrides;
}

namespace GuestOverrides {

// ---------------------------------------------------------------------------
// 0x00372070 oot3d_translate_mtx3x4_local (folha, 1 bloco)
//   vldmia r1, {s0-s11}; vldmia r2, {s12-s14}
//   s3/s7/s11 += linha · v (VMLA não fundido, ordem coluna a coluna)
//   vstmia r0, {s0-s11}; bx lr
bool Override_00372070(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00372070;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0], r1 = state.R[1], r2 = state.R[2];
    if (!Budget(context, 1U) || Hooked(context, 0x00372070U, 0x003720A0U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, r1, 48U) ||
        !Readable(memory, r2, 12U) || !Writable(memory, r0, 48U)) {
        return Decline(kIdx);
    }
    uint32_t m[12];
    uint32_t v[3];
    for (uint32_t i = 0; i < 12U; ++i) m[i] = Read32(memory, r1 + 4U * i);
    for (uint32_t i = 0; i < 3U; ++i) v[i] = Read32(memory, r2 + 4U * i);
    uint32_t fpscr = guest.fpscr;
    const auto mla = [&](uint32_t& acc, uint32_t a, uint32_t b) {
        acc = Commit(fpscr, Oot3dVfpMultiplyAccumulate(acc, a, b, fpscr));
    };
    mla(m[3], m[0], v[0]);
    mla(m[7], m[4], v[0]);
    mla(m[11], m[8], v[0]);
    mla(m[3], m[1], v[1]);
    mla(m[7], m[5], v[1]);
    mla(m[11], m[9], v[1]);
    mla(m[3], m[2], v[2]);
    mla(m[7], m[6], v[2]);
    mla(m[11], m[10], v[2]);
    for (uint32_t i = 0; i < 12U; ++i) {
        memory.WriteFast<uint32_t>(r0 + 4U * i, m[i]);
    }
    std::copy(m, m + 12, guest.vfp.begin());
    std::copy(v, v + 3, guest.vfp.begin() + 12);
    guest.fpscr = fpscr;
    Consume(context, 1U);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002CDE90 (folha): if (*(u8*)r0 == 0 && *(f32*)(r0+16) == 0.0f) return s0=0;
//                     else s0 = *(f32*)(r0+4) * 0.1f
bool Override_002CDE90(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002CDE90;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    if (!Budget(context, 3U) || Hooked(context, 0x002CDE90U, 0x002CDEB8U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, r0, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t byte = 0U;
    memory.ReadFast<uint8_t>(r0, &byte);
    if (byte == 0U) {
        if (!Readable(memory, r0 + 16U, 4U)) return Decline(kIdx);
    } else if (!Readable(memory, r0 + 4U, 4U)) {
        return Decline(kIdx);
    }
    auto& flags = state.Flags;
    uint32_t fpscr = guest.fpscr;
    state.R[1] = byte;
    flags.SetSubtract(byte, 0U, Mask(0xFU));
    uint32_t blocks = 2U; // 0x002CDE90 e 0x002CDEB8
    bool equal = false;
    if (byte == 0U) {
        const uint32_t s1 = Read32(memory, r0 + 16U);
        guest.vfp[1] = s1;
        guest.vfp[0] = 0x00000000U; // literal em 0x002CDEBC
        Compare(fpscr, flags, s1, 0x00000000U, false);
        equal = flags.Z();
    }
    if (!equal) {
        ++blocks; // 0x002CDEAC
        const uint32_t s0 = Read32(memory, r0 + 4U);
        guest.vfp[1] = 0x3DCCCCCDU; // literal em 0x002CDEC0 (0.1f)
        guest.vfp[0] = Commit(fpscr, Oot3dVfpMultiply(s0, 0x3DCCCCCDU, fpscr));
    }
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003094CC (folha): r1 = r0 + r1*4; s0 = max(s0, 0) (VCMPE/vmovlo);
//   if (*(f32*)(r1+72) != s0) { *(f32*)(r1+72) = s0; *(u16*)(r0+0x20) |= 8; }
bool Override_003094CC(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003094CC;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    const uint32_t slot = r0 + (state.R[1] << 2U);
    if (!Budget(context, 3U) || Hooked(context, 0x003094CCU, 0x00309500U) ||
        !VfpDefault(guest.fpscr) || !Readable(memory, slot + 72U, 4U)) {
        return Decline(kIdx);
    }
    Oot3dAotLazyFlags flags = state.Flags; // local: ainda pode recusar
    uint32_t fpscr = guest.fpscr;
    uint32_t s0 = guest.vfp[0];
    Compare(fpscr, flags, s0, 0x00000000U, true); // literal 0.0f em 0x00309504
    if (!flags.C()) s0 = 0x00000000U;
    const uint32_t current = Read32(memory, slot + 72U);
    Compare(fpscr, flags, current, s0, false);
    const bool store = !flags.Z();
    if (store && (!Writable(memory, slot + 72U, 4U) ||
                  !Writable(memory, r0 + 0x20U, 2U, 2U))) {
        return Decline(kIdx);
    }
    // Daqui em diante não há mais recusa.
    state.Flags = flags;
    state.R[1] = slot;
    guest.vfp[0] = s0;
    guest.vfp[1] = current;
    uint32_t blocks = 2U; // 0x003094CC e 0x00309500
    if (store) {
        ++blocks; // 0x003094F0
        memory.WriteFast<uint32_t>(slot + 72U, s0);
        uint16_t half = 0U;
        memory.ReadFast<uint16_t>(r0 + 0x20U, &half);
        const uint32_t r1 = static_cast<uint32_t>(half) | 0x00000008U;
        state.R[1] = r1;
        memory.WriteFast<uint16_t>(r0 + 0x20U, static_cast<uint16_t>(r1));
    }
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x002F9430 PauseRenderBuffer_SetVec2Range (folha, laço)
//   copia n = r2 pares de floats de r1 para buffer(*(r0+0x1C)) + 8*r3 - 4,
//   com a leitura antecipada do primeiro float do par seguinte (inclusive na
//   última volta, como no original).
bool Override_002F9430(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx002F9430;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    auto& flags = state.Flags;
    uint32_t r0 = state.R[0], r1 = state.R[1];
    uint32_t r2 = state.R[2] << 1U;
    uint32_t r3 = state.R[3] << 1U;
    if (Hooked(context, 0x002F9430U, 0x002F9480U)) return Decline(kIdx);
    const bool positive = static_cast<int32_t>(r2) > 0;
    if (!positive) {
        if (!Budget(context, 2U)) return Decline(kIdx);
        flags.SetSubtract(r2, 0U, Mask(0xFU));
        state.R[2] = r2;
        state.R[3] = r3;
        Consume(context, 2U);
        flow = Oot3dAotReturned(state.R[14]);
        return Accept(kIdx);
    }
    const uint32_t count = static_cast<uint32_t>(static_cast<int32_t>(r2) >> 1);
    // 0x002F9430 + 0x002F9440 + count voltas + 0x002F9480
    if (count > 0x00FFFFFFU || !Budget(context, count + 3U) ||
        !Readable(memory, r0 + 0x1CU, 4U)) {
        return Decline(kIdx);
    }
    const uint32_t base = Read32(memory, r0 + 0x1CU);
    const uint32_t destination = base + (0xFFFFFFFCU + (r3 << 2U));
    // Leituras em [r1, r1 + 8n + 4), escritas em [destino + 4, destino + 8n + 4).
    if (!Readable(memory, r1, 8U * count + 4U) ||
        !Writable(memory, destination + 4U, 8U * count)) {
        return Decline(kIdx);
    }
    uint32_t s0 = Read32(memory, r1); // [r1 - 4 + 4]
    uint32_t s1 = guest.vfp[1];
    r1 -= 4U;
    r0 = destination;
    r3 = destination;
    r2 = count;
    while (true) {
        r1 += 8U;
        r3 = r0;
        s1 = Read32(memory, r1);
        r0 += 8U;
        memory.WriteFast<uint32_t>(r3 + 4U, s0);
        s0 = Read32(memory, r1 + 4U);
        flags.SetSubtract(r2, 1U, Mask(0xFU));
        r2 -= 1U;
        memory.WriteFast<uint32_t>(r0, s1);
        if (flags.Z()) break;
    }
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = 0xFFFFFFFCU;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    Consume(context, count + 3U);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x0036E168 Math_SmoothStepToF (nome do inventário; é um passo limitado em
// direção ao alvo, escalado pela taxa de atualização global R_UPDATE_RATE).
//   s4 = *pValue; if (s4 == target) -> 0x0036E26C
//   rate = (s16)*(*(ctx**)0x0051B2F4 + 0x110); s3/s1/s2 *= rate/3
//   passo = (alvo - s4) * fração (|alvo - s4| < 1e-5 usa o resto inteiro),
//   limitado por máximo/mínimo, gravado em *pValue; s0 = |alvo - *pValue|
bool Override_0036E168(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx0036E168;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    if (!Budget(context, 8U) || Hooked(context, 0x0036E168U, 0x0036E278U) ||
        !VfpDefault(guest.fpscr) || !Writable(memory, r0, 4U) ||
        !Readable(memory, r0, 4U) || !Readable(memory, 0x0051B2F4U, 4U)) {
        return Decline(kIdx);
    }
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags; // local: ainda pode recusar
    uint32_t s0 = guest.vfp[0], s1 = guest.vfp[1], s2 = guest.vfp[2];
    uint32_t s3 = guest.vfp[3], s5 = guest.vfp[5], s6 = guest.vfp[6];
    uint32_t r1 = state.R[1], r2 = state.R[2];
    const uint32_t s4 = Read32(memory, r0);
    Compare(fpscr, flags, s4, s0, false);
    uint32_t blocks = 2U; // 0x0036E168 e 0x0036E26C
    if (!flags.Z()) {
        blocks += 1U; // 0x0036E178
        const uint32_t globalContext = Read32(memory, 0x0051B2F4U);
        if (!Readable(memory, globalContext + 0x110U, 2U, 2U)) return Decline(kIdx);
        uint16_t rate16 = 0U;
        memory.ReadFast<uint16_t>(globalContext + 0x110U, &rate16);
        r2 = 0x3727C5ACU; // 1e-5
        r1 = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(rate16)));
        s6 = r1;
        s5 = r1;
        s6 = Commit(fpscr, Oot3dVfpFromSigned(s6, fpscr));
        s5 = Commit(fpscr, Oot3dVfpFromSigned(s5, fpscr));
        s3 = Commit(fpscr, Oot3dVfpMultiply(s6, s3, fpscr));
        s6 = r1;
        s1 = Commit(fpscr, Oot3dVfpMultiply(s5, s1, fpscr));
        s5 = 0x3EAAAAABU; // literal em 0x0036E280 (1/3)
        s6 = Commit(fpscr, Oot3dVfpFromSigned(s6, fpscr));
        s3 = Commit(fpscr, Oot3dVfpMultiply(s3, s5, fpscr));
        s1 = Commit(fpscr, Oot3dVfpMultiply(s1, s5, fpscr));
        s2 = Commit(fpscr, Oot3dVfpMultiply(s6, s2, fpscr));
        s2 = Commit(fpscr, Oot3dVfpMultiply(s2, s5, fpscr));
        s5 = Commit(fpscr, Oot3dVfpSubtract(s0, s4, fpscr));
        s6 = s5 & 0x7FFFFFFFU;
        s1 = Commit(fpscr, Oot3dVfpMultiply(s5, s1, fpscr));
        r1 = s6;
        flags.SetSubtract(r1, r2, Mask(0x9U));
        if (flags.N() != flags.V()) s1 = s5;
        Compare(fpscr, flags, s1, s3, true);
        bool toClamp = flags.N() == flags.V(); // ge -> 0x0036E1F4
        if (!toClamp) {
            blocks += 1U; // 0x0036E1E4
            s5 = s3 ^ 0x80000000U;
            Compare(fpscr, flags, s1, s5, true);
            toClamp = !(flags.C() && !flags.Z()); // hi -> 0x0036E21C
        }
        // Daqui em diante não há mais recusa (r0 já validado para escrita).
        if (toClamp) {
            blocks += 1U; // 0x0036E1F4
            Compare(fpscr, flags, s1, s2, true);
            if (!flags.Z() && flags.N() == flags.V()) s1 = s2;
            s2 = s2 ^ 0x80000000U;
            Compare(fpscr, flags, s1, s2, true);
            if (!flags.C()) s1 = s2;
            s1 = Commit(fpscr, Oot3dVfpAdd(s4, s1, fpscr));
            memory.WriteFast<uint32_t>(r0, s1);
        } else {
            blocks += 1U; // 0x0036E21C
            Compare(fpscr, flags, s1, s3, true);
            if (!flags.C()) {
                blocks += 1U; // 0x0036E228
                s1 = s3;
                s2 = Commit(fpscr, Oot3dVfpAdd(s4, s1, fpscr));
                memory.WriteFast<uint32_t>(r0, s2);
                Compare(fpscr, flags, s2, s0, true);
                if (!flags.Z() && flags.N() == flags.V()) s2 = s0;
                memory.WriteFast<uint32_t>(r0, s2);
            }
            blocks += 1U; // 0x0036E244
            Compare(fpscr, flags, s1, s5, true);
            if (!flags.Z() && flags.N() == flags.V()) {
                blocks += 1U; // 0x0036E250
                s1 = Read32(memory, r0);
                s1 = Commit(fpscr, Oot3dVfpAdd(s1, s5, fpscr));
                memory.WriteFast<uint32_t>(r0, s1);
                Compare(fpscr, flags, s1, s0, true);
                if (flags.Z() || flags.N() != flags.V()) s1 = s0;
                memory.WriteFast<uint32_t>(r0, s1);
            }
        }
    }
    // 0x0036E26C
    s1 = Read32(memory, r0);
    s0 = Commit(fpscr, Oot3dVfpSubtract(s0, s1, fpscr));
    s0 &= 0x7FFFFFFFU;
    state.Flags = flags;
    state.R[1] = r1;
    state.R[2] = r2;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[4] = s4;
    guest.vfp[5] = s5;
    guest.vfp[6] = s6;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003325F0 Math3D_CylOutsideCylDist (folha)
//   Copia os dois cilindros para o rascunho global 0x00569830..0x0056985F,
//   dist = sqrt(dx*dx + dz*dz) -> *r3; se houver sobreposição em raio e em
//   altura, *r2 = (raio0 + raio1) - dist e r0 = 1, senão r0 = 0.
bool Override_003325F0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003325F0;
    constexpr uint32_t kScratch = 0x00569830U; // r4 depois do sub
    constexpr uint32_t kSecond = kScratch + 0x24U; // r7 = 0x00569854
    constexpr uint32_t kCopy = kSecond - 12U;     // r0 = 0x00569848
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t in0 = state.R[0], in1 = state.R[1];
    const uint32_t out5 = state.R[2], out6 = state.R[3];
    const uint32_t sp = state.R[13];
    if (!Budget(context, 5U) || Hooked(context, 0x003325F0U, 0x003326E8U) ||
        !VfpDefault(guest.fpscr) || !Writable(memory, sp - 16U, 16U) ||
        !Readable(memory, sp - 16U, 16U) || !Writable(memory, kScratch, 0x30U) ||
        !Readable(memory, kScratch, 0x30U) || !Readable(memory, in0, 24U) ||
        !Readable(memory, in1, 24U) || !Writable(memory, out6, 4U) ||
        !Writable(memory, out5, 4U)) {
        return Decline(kIdx);
    }
    // push {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        memory.WriteFast<uint32_t>(sp - 16U + 4U * i, state.R[4 + i]);
    }
    // ldm r0+12 / stm 0x0056983C
    uint32_t r3 = Read32(memory, in0 + 12U);
    uint32_t r7 = Read32(memory, in0 + 16U);
    uint32_t r12 = Read32(memory, in0 + 20U);
    memory.WriteFast<uint32_t>(kScratch + 12U, r3);
    memory.WriteFast<uint32_t>(kScratch + 16U, r7);
    memory.WriteFast<uint32_t>(kScratch + 20U, r12);
    uint32_t s0 = Read32(memory, in0);
    memory.WriteFast<uint32_t>(kScratch, s0);
    s0 = Read32(memory, in0 + 8U);
    memory.WriteFast<uint32_t>(kScratch + 8U, s0);
    s0 = Read32(memory, in0 + 4U);
    memory.WriteFast<uint32_t>(kScratch + 4U, s0);
    // ldm r1+12 / stm 0x00569854
    const uint32_t r2 = Read32(memory, in1 + 12U);
    r3 = Read32(memory, in1 + 16U);
    r12 = Read32(memory, in1 + 20U);
    memory.WriteFast<uint32_t>(kSecond, r2);
    memory.WriteFast<uint32_t>(kSecond + 4U, r3);
    memory.WriteFast<uint32_t>(kSecond + 8U, r12);
    s0 = Read32(memory, in1);
    memory.WriteFast<uint32_t>(kCopy, s0);
    s0 = Read32(memory, in1 + 8U);
    memory.WriteFast<uint32_t>(kCopy + 8U, s0);
    s0 = Read32(memory, in1 + 4U);
    memory.WriteFast<uint32_t>(kCopy + 4U, s0);
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t s1 = Read32(memory, kScratch + 12U);
    s0 = Read32(memory, kCopy + 12U);
    uint32_t s2 = Read32(memory, kCopy + 20U);
    s1 = Commit(fpscr, Oot3dVfpSubtract(s1, s0, fpscr));
    s0 = Read32(memory, kScratch + 20U);
    s0 = Commit(fpscr, Oot3dVfpSubtract(s0, s2, fpscr));
    s1 = Commit(fpscr, Oot3dVfpMultiply(s1, s1, fpscr));
    s1 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s1, s0, s0, fpscr));
    s0 = Commit(fpscr, a32::VfpBinary32SquareRoot(s1, fpscr));
    memory.WriteFast<uint32_t>(out6, s0);
    s1 = Read32(memory, kScratch);
    s2 = Read32(memory, kCopy);
    s1 = Commit(fpscr, Oot3dVfpAdd(s1, s2, fpscr));
    Compare(fpscr, flags, s1, s0, true);
    uint32_t s3 = guest.vfp[3], s4 = guest.vfp[4], s5 = guest.vfp[5];
    uint32_t result = 0U;
    uint32_t blocks = 3U; // 0x003325F0, 0x003326E4 e (0x003326E0 ou 0x003326C0)
    if (flags.C()) {
        blocks += 1U; // 0x00332694
        s2 = Read32(memory, kScratch + 16U);
        s3 = Read32(memory, kScratch + 8U);
        s5 = Read32(memory, kCopy + 8U);
        s2 = Commit(fpscr, Oot3dVfpAdd(s2, s3, fpscr));
        s3 = Read32(memory, kScratch + 4U);
        s4 = Commit(fpscr, Oot3dVfpAdd(s2, s3, fpscr));
        s3 = Read32(memory, kCopy + 16U);
        s3 = Commit(fpscr, Oot3dVfpAdd(s3, s5, fpscr));
        Compare(fpscr, flags, s4, s3, true);
        if (flags.C()) {
            // 0x003326C0
            s4 = Read32(memory, kCopy + 4U);
            s3 = Commit(fpscr, Oot3dVfpAdd(s3, s4, fpscr));
            Compare(fpscr, flags, s2, s3, true);
            if (flags.Z() || flags.N() != flags.V()) {
                s0 = Commit(fpscr, Oot3dVfpSubtract(s1, s0, fpscr));
                result = 1U;
                memory.WriteFast<uint32_t>(out5, s0);
            } else {
                blocks += 1U; // 0x003326E0
            }
        }
    }
    // pop {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        state.R[4 + i] = Read32(memory, sp - 16U + 4U * i);
    }
    state.R[0] = result;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = r12;
    state.Flags = flags;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[4] = s4;
    guest.vfp[5] = s5;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00307C94 PicaCommandWriter_UploadVertexFloatUniforms (folha, laço)
//   r0 = escritor (lista em *(r0+8)), r1 = registrador de config, r2 = n vec4,
//   r3 = origem. Escreve o cabeçalho de configuração, o cabeçalho de dados e os
//   n vetores com os componentes invertidos (w, z, y, x), termina a lista com
//   um zero e avança *(r0+8).
bool Override_00307C94(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00307C94;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t r0 = state.R[0];
    const uint32_t count = state.R[2];
    if (Hooked(context, 0x00307C94U, 0x00307D80U)) return Decline(kIdx);
    if (static_cast<int32_t>(count) < 1) {
        // cmp r2, #1; bxlt lr
        if (!Budget(context, 1U)) return Decline(kIdx);
        state.Flags.SetSubtract(count, 1U, Mask(0xFU));
        Consume(context, 1U);
        flow = Oot3dAotReturned(state.R[14]);
        return Accept(kIdx);
    }
    const uint32_t sp = state.R[13];
    const uint32_t source = state.R[3];
    const uint32_t blocks = count == 1U ? 3U : count + 4U;
    if (count > 0x00FFFFFFU || !Budget(context, blocks) ||
        !Writable(memory, sp - 16U, 16U) || !Readable(memory, sp - 16U, 16U) ||
        !Writable(memory, r0 + 8U, 4U) || !Readable(memory, r0 + 8U, 4U) ||
        (r0 + 8U >= sp - 16U && r0 + 8U < sp)) {
        return Decline(kIdx);
    }
    const uint32_t list = Read32(memory, r0 + 8U);
    const uint32_t listBytes = 28U + 16U * (count - 1U) + 4U;
    if (!Writable(memory, list, listBytes) || !Readable(memory, list, listBytes) ||
        !Readable(memory, source, 16U * count)) {
        return Decline(kIdx);
    }
    // push {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        memory.WriteFast<uint32_t>(sp - 16U + 4U * i, state.R[4 + i]);
    }
    uint32_t r12 = Read32(memory, r0 + 8U); // o mesmo valor de `list`
    const uint32_t r2x4 = count << 2U;
    const uint32_t r7 = source + 16U;
    memory.WriteFast<uint32_t>(r12, state.R[1] | 0x80000000U);
    memory.WriteFast<uint32_t>(r12 + 4U, 0x000F02C0U);
    uint32_t s0 = Read32(memory, source + 12U);
    memory.WriteFast<uint32_t>(r12 + 8U, s0);
    memory.WriteFast<uint32_t>(r12 + 12U, ((r2x4 << 20U) - 0x00100000U) | 0x000F02C1U);
    s0 = Read32(memory, source + 8U);
    memory.WriteFast<uint32_t>(r12 + 16U, s0);
    uint32_t r1 = 0xFFFFFFFDU - (r2x4 - 4U);
    uint32_t r2 = Oot3dAotAsr(r1, 31U);
    s0 = Read32(memory, source + 4U);
    memory.WriteFast<uint32_t>(r12 + 20U, s0);
    r1 = r1 + Oot3dAotLsr(r2, 30U);
    s0 = Read32(memory, source);
    const uint32_t r6 = Oot3dAotAsr(r1, 2U);
    uint32_t r3 = 0U - r6;
    Oot3dAotLazyFlags flags = state.Flags;
    flags.SetSubtract(r3, 0U, Mask(0xFU));
    uint32_t r5 = r12 + 28U;
    memory.WriteFast<uint32_t>(r12 + 24U, s0);
    r12 += 20U;
    if (!flags.Z() && flags.N() == flags.V()) {
        r1 = r7 + 12U;
        r2 = r5;
        while (true) {
            s0 = Read32(memory, r1);
            r12 = r2;
            const uint32_t r4 = r1;
            memory.WriteFast<uint32_t>(r12, s0);
            s0 = Read32(memory, r4 - 4U);
            flags.SetSubtract(r3, 1U, Mask(0xFU));
            r3 -= 1U;
            memory.WriteFast<uint32_t>(r12 + 4U, s0);
            s0 = Read32(memory, r4 - 8U);
            r1 += 16U;
            memory.WriteFast<uint32_t>(r12 + 8U, s0);
            s0 = Read32(memory, r4 - 12U);
            r2 += 16U;
            memory.WriteFast<uint32_t>(r12 + 12U, s0);
            if (flags.Z()) break;
        }
        r5 -= r6 << 4U;
    }
    memory.WriteFast<uint32_t>(r5, 0U);
    r1 = r5 + 4U;
    memory.WriteFast<uint32_t>(r0 + 8U, r1);
    // pop {r4-r7}
    for (uint32_t i = 0; i < 4U; ++i) {
        state.R[4 + i] = Read32(memory, sp - 16U + 4U * i);
    }
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[3] = r3;
    state.R[12] = r12;
    state.Flags = flags;
    guest.vfp[0] = s0;
    Consume(context, blocks);
    flow = Oot3dAotReturned(state.R[14]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x003727F0 Math_SinF
//   Caminhos nativos: |x| < 12867.57 (redução por 2/pi com Cody-Waite em
//   quatro constantes e polinômios de seno/cosseno) e x minúsculo (inclusive
//   as chamadas a 0x002C0B34, classificação, e 0x002C0B24, que levanta
//   underflow). Recusa |x| >= 12867.57, NaN e infinito (chamada à redução
//   0x002C0B68) e lr dentro da própria função. O retorno é ldr pc, [sp], #4:
//   como na tradução, sai com Branch(lr).
static void Classify002C0B34(Oot3dAotArchitecturalState& state,
                             Oot3dAotLazyFlags& flags, uint32_t& s0) noexcept {
    uint32_t r0 = state.R[0];
    s0 = r0; // vmov s0, r0
    r0 = s0; // vmov r0, s0
    uint32_t r1 = Oot3dAotLsl(r0, 1U);
    auto shifted = Oot3dAotShiftImmediate(r1, 0U, 8U, flags.C());
    r0 = shifted.Value;
    flags.SetLogical(r0, shifted.Carry, Mask(0x4U));
    if (!flags.Z()) r0 = 0x00000004U;
    shifted = Oot3dAotShiftImmediate(r1, 1U, 24U, flags.C());
    uint32_t r2 = shifted.Value;
    flags.SetLogical(r2, shifted.Carry, Mask(0x6U));
    if (!flags.Z()) r0 |= 0x00000001U;
    r2 = 0xFF000000U;
    shifted = Oot3dAotShiftImmediate(r1, 0U, 0U, flags.C());
    r1 = r2 & ~shifted.Value;
    flags.SetLogical(r1, shifted.Carry, Mask(0x4U));
    if (flags.Z()) r0 |= 0x00000002U;
    flags.SetSubtract(r0, 0x00000001U, Mask(0xFU));
    if (flags.Z()) r0 = 0x00000005U;
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
}

bool Override_003727F0(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx003727F0;
    auto& guest = frame.Guest;
    auto& memory = context.Memory;
    const uint32_t x = guest.vfp[0];
    const uint32_t lr = state.R[14];
    const uint32_t sp = state.R[13];
    const uint32_t twice = x << 1U;
    const bool small = twice < 0x7E921FB6U; // |x| < pi/4
    const bool reduceCall = !small && (x & 0x7FFFFFFFU) >= 0x46490E49U;
    if (reduceCall || (lr >= 0x003727F0U && lr <= 0x00372974U) ||
        !Budget(context, 9U) || Hooked(context, 0x003727F0U, 0x00372974U) ||
        Hooked(context, 0x002C0B24U, 0x002C0B64U) || !VfpDefault(guest.fpscr) ||
        !Writable(memory, sp - 16U, 16U) || !Readable(memory, sp - 16U, 16U)) {
        return Decline(kIdx);
    }
    uint32_t fpscr = guest.fpscr;
    Oot3dAotLazyFlags flags = state.Flags;
    uint32_t s0 = x, s1 = guest.vfp[1], s2 = guest.vfp[2], s3 = guest.vfp[3];
    uint32_t r0 = 0x7E921FB6U;
    const uint32_t r1In = x;
    uint32_t r1 = r1In;
    uint32_t r2 = state.R[2];
    uint32_t r14 = lr;
    uint32_t blocks = 1U; // 0x003727F0
    // str lr, [sp, #-4]!; vpush {d8}; cmp r0, r1, lsl #1; sub sp, sp, #4
    memory.WriteFast<uint32_t>(sp - 4U, lr);
    memory.WriteFast<uint32_t>(sp - 12U, guest.vfp[16]);
    memory.WriteFast<uint32_t>(sp - 8U, guest.vfp[17]);
    flags.SetSubtract(r0, r1In << 1U, Mask(0x6U));
    const uint32_t frameSp = sp - 16U;
    if (flags.C() && !flags.Z()) {
        // 0x0037280C: |x| < pi/4; r0 = -1 se |x| minúsculo
        blocks += 1U;
        r0 = 0x73000000U;
        flags.SetSubtract(r0, r1In << 1U, Mask(0x6U));
        if (!flags.C() || flags.Z()) r0 = 0x00000000U;
        if (flags.C() && !flags.Z()) r0 = 0xFFFFFFFFU;
        memory.WriteFast<uint32_t>(frameSp, r0);
    } else {
        // 0x00372824 e 0x00372834 (o caso da redução grande foi recusado)
        blocks += 2U;
        r2 = 0x46490E49U;
        r0 = r1In & ~0x80000000U;
        flags.SetSubtract(r0, r2, Mask(0xFU));
        flags.SetLogical(r1In & 0x80000000U, true, Mask(0x4U));
        s1 = 0x3F22F983U; // 2/pi
        s2 = 0x4B000000U; // 2^23
        s3 = 0x39FDA000U;
        s1 = Commit(fpscr, Oot3dVfpMultiply(s0, s1, fpscr));
        if (!flags.Z()) s1 = Commit(fpscr, Oot3dVfpSubtract(s1, s2, fpscr));
        s1 = Commit(fpscr, Oot3dVfpAdd(s1, s2, fpscr));
        if (flags.Z()) s1 = Commit(fpscr, Oot3dVfpSubtract(s1, s2, fpscr));
        s2 = Commit(fpscr, a32::VfpBinary32ToSigned(s1, fpscr));
        r0 = s2;
        s2 = 0x3FC90000U;
        r0 &= 0x00000003U;
        s0 = Commit(fpscr, Oot3dVfpMultiplySubtract(s0, s1, s2, fpscr));
        s2 = 0x33A22000U;
        memory.WriteFast<uint32_t>(frameSp, r0);
        s0 = Commit(fpscr, Oot3dVfpMultiplySubtract(s0, s1, s3, fpscr));
        s3 = 0x2C34611AU;
        s0 = Commit(fpscr, Oot3dVfpMultiplySubtract(s0, s1, s2, fpscr));
        s0 = Commit(fpscr, Oot3dVfpMultiplySubtract(s0, s1, s3, fpscr));
    }
    // 0x0037288C
    blocks += 1U;
    r0 = Read32(memory, frameSp);
    uint32_t s16 = s0;
    uint32_t s17 = guest.vfp[17];
    flags.SetSubtract(r0, 0U, Mask(0xBU));
    uint32_t returnSp = frameSp + 4U; // onde começa o vpop {d8} final
    bool negate = false;
    if (flags.N() != flags.V()) {
        // 0x0037289C: quadrante -1, só gravado por 0x0037280C com |x| < 2^-12
        // (2|x| < 0x73000000). Então 2|x| < 0xFF000000, C fica limpo e o
        // ramo 0x003728CC (NaN/inf) não é alcançável por aqui.
        blocks += 1U;
        r0 = Oot3dAotLsl(s16, 1U);
        flags.SetSubtract(r0, 0xFF000000U, Mask(0xFU));
        {
            // 0x003728AC: bl 0x002C0B34; 0x003728B4: cmp r0, #4; bleq 0x002C0B24
            blocks += 3U;
            r0 = s16;
            ++context.Stats.DirectCalls;
            r14 = 0x003728B4U;
            state.R[0] = r0;
            state.R[1] = r1;
            state.R[2] = r2;
            Classify002C0B34(state, flags, s0);
            r0 = state.R[0];
            r1 = state.R[1];
            r2 = state.R[2];
            flags.SetSubtract(r0, 0x00000004U, Mask(0xFU));
            if (flags.Z()) {
                blocks += 1U; // 0x002C0B24
                ++context.Stats.DirectCalls;
                r14 = 0x003728BCU;
                s0 = 0x10000000U; // literal em 0x002C0B30
                s0 = Commit(fpscr, Oot3dVfpMultiply(s0, s0, fpscr));
            }
            blocks += 1U; // 0x003728BC
            s0 = s16;
        }
    } else {
        // 0x003728F8: tst r0, #1
        blocks += 1U;
        flags.SetLogical(r0 & 0x00000001U, flags.C(), Mask(0x6U));
        if (flags.Z()) {
            // 0x00372900: quadrantes pares (polinômio de seno)
            blocks += 1U;
            s0 = Commit(fpscr, Oot3dVfpMultiply(s16, s16, fpscr));
            flags.SetLogical(r0 & 0x00000002U, flags.C(), Mask(0xEU));
            s1 = 0x394C6D33U;
            s2 = 0x3C0882DAU;
            s2 = Commit(fpscr, Oot3dVfpMultiplySubtract(s2, s0, s1, fpscr));
            s1 = 0xBE2AAAA0U;
            s1 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s1, s0, s2, fpscr));
            s1 = Commit(fpscr, Oot3dVfpMultiply(s1, s0, fpscr));
            s0 = s16;
            s0 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s0, s16, s1, fpscr));
            if (!flags.Z()) {
                blocks += 2U; // 0x00372934 (b) e 0x00372968
                negate = true;
            }
        } else {
            // 0x00372938: quadrantes ímpares (polinômio de cosseno)
            blocks += 1U;
            s1 = Commit(fpscr, Oot3dVfpMultiply(s16, s16, fpscr));
            s0 = 0xBAB23AB9U;
            flags.SetLogical(r0 & 0x00000002U, flags.C(), Mask(0xEU));
            s3 = 0x3D2A9FCAU;
            s2 = 0xBEFFFFDDU;
            const bool early = flags.Z();
            if (early) {
                // vpopeq {d8} antes das contas: só leituras, mesma ordem.
                s16 = Read32(memory, frameSp + 4U);
                s17 = Read32(memory, frameSp + 8U);
            }
            s3 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s3, s1, s0, fpscr));
            s0 = 0x3F800000U;
            s2 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s2, s1, s3, fpscr));
            s0 = Commit(fpscr, Oot3dVfpMultiplyAccumulate(s0, s1, s2, fpscr));
            if (early) {
                const uint32_t target = Read32(memory, frameSp + 12U);
                state.R[0] = r0;
                state.R[1] = r1;
                state.R[2] = r2;
                state.R[13] = frameSp + 16U;
                state.Flags = flags;
                guest.vfp[0] = s0;
                guest.vfp[1] = s1;
                guest.vfp[2] = s2;
                guest.vfp[3] = s3;
                guest.vfp[16] = s16;
                guest.vfp[17] = s17;
                guest.fpscr = fpscr;
                Consume(context, blocks);
                flow = Oot3dAotBranch(target);
                return Accept(kIdx);
            }
            blocks += 1U; // 0x00372968
            negate = true;
        }
    }
    // vpop {d8} (+ vneg.f32 s0, s0 em 0x00372968); ldr pc, [sp], #4
    s16 = Read32(memory, returnSp);
    s17 = Read32(memory, returnSp + 4U);
    if (negate) s0 ^= 0x80000000U;
    const uint32_t target = Read32(memory, returnSp + 8U);
    state.R[0] = r0;
    state.R[1] = r1;
    state.R[2] = r2;
    state.R[13] = returnSp + 12U;
    state.R[14] = r14;
    state.Flags = flags;
    guest.vfp[0] = s0;
    guest.vfp[1] = s1;
    guest.vfp[2] = s2;
    guest.vfp[3] = s3;
    guest.vfp[16] = s16;
    guest.vfp[17] = s17;
    guest.fpscr = fpscr;
    Consume(context, blocks);
    flow = Oot3dAotBranch(target);
    return Accept(kIdx);
}

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
