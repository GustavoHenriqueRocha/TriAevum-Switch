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
#include <cstring>
#include <type_traits>

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
    kIdx004A022C,
    kIdx004A0338,
    kIdx00371738,
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
    {0x004A022CU, "AudioEffect_ProcessFourChannelDelay"},
    {0x004A0338U, "AudioEffect_ProcessStereoReverb"},
    {0x00371738U, "Runtime_Memcpy"},
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

// [a, a + alen) e [b, b + blen) se cruzam (aritmética de 64 bits).
inline bool Overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) noexcept {
    return uint64_t{a} < uint64_t{b} + blen && uint64_t{b} < uint64_t{a} + alen;
}

// Leitura de `size` bytes que cabe numa só página rápida (condição exata do
// caminho rápido de ReadFast<T> para T de `size` bytes).
inline bool SinglePageReadable(const NativeA32Memory& memory, uint32_t address,
                               uint32_t size) noexcept {
    return memory.SpanOnFastReadPage(address, size);
}

// Registradores guest em variáveis locais, com saída exata por limite de
// orçamento de blocos: Enter() faz o que Oot3dAotEnterBlock faria num bloco
// sem gancho; quando o orçamento acaba, BlockLimit() grava o estado como a
// tradução deixaria naquele ponto (registradores e flags preguiçosas) e
// devolve o mesmo fluxo de saída.
struct LocalGuest {
    Oot3dWholeAotContext& Context;
    Oot3dAotArchitecturalState& State;
    std::array<uint32_t, 15> R;
    Oot3dAotLazyFlags Flags;

    LocalGuest(Oot3dWholeAotContext& context, Oot3dAotArchitecturalState& state) noexcept
        : Context(context), State(state), R(state.R), Flags(state.Flags) {}

    bool Enter() noexcept {
        if (Context.BlocksRemaining == 0U) return false;
        --Context.BlocksRemaining;
        return true;
    }
    void Flush() noexcept {
        State.R = R;
        State.Flags = Flags;
    }
    Oot3dWholeAotFlow BlockLimit(uint32_t pc) noexcept {
        Flush();
        return Oot3dAotBlockLimit(Context, pc);
    }
};

// Acesso à tabela de páginas rápidas de NativeA32Memory sem mudar o cabeçalho
// (que é incluído por todos os shards: mudá-lo recompilaria os 256). Usa a
// regra padrão de que instanciações explícitas podem nomear membros privados.
// As constantes abaixo espelham as de oot3d_native_a32_memory.h e
// ResolveFast* reproduz FastPageFor + o teste do caminho rápido de
// ReadFast/WriteFast; CommitFastWrite reproduz a contabilidade de WriteFast.
template <class Tag, auto Member> struct ExposeMember {
    friend constexpr auto MemberOf(Tag) noexcept { return Member; }
};
struct FastGroupsTag { friend constexpr auto MemberOf(FastGroupsTag) noexcept; };
// mRangeGeneration = geração arquitetural + viés de certificado, num só
// contador: WriteFast faz ++ e carimba a página com o novo valor.
struct RangeGenerationTag { friend constexpr auto MemberOf(RangeGenerationTag) noexcept; };
struct RangeVersionsTag { friend constexpr auto MemberOf(RangeVersionsTag) noexcept; };
struct TraceTag { friend constexpr auto MemberOf(TraceTag) noexcept; };
template struct ExposeMember<FastGroupsTag, &NativeA32Memory::mFastPageGroups>;
template struct ExposeMember<RangeGenerationTag, &NativeA32Memory::mRangeGeneration>;
template struct ExposeMember<RangeVersionsTag, &NativeA32Memory::mRangeVersionsValid>;
template struct ExposeMember<TraceTag, &NativeA32Memory::mTraceWriteFingerprint>;

constexpr uint32_t kMemPageShift = 12U;     // NativeA32Memory::PageShift
constexpr uint32_t kMemGroupShift = 20U;    // NativeA32Memory::FastGroupShift
constexpr uint32_t kMemGroupPageMask = 0xFFU;
constexpr uint32_t kMemPageMask = 0xFFFU;
constexpr uint32_t kMemPageSize = 0x1000U;

// Página rápida de [address, address + size) (nula se cruza página).
inline auto* FastPageOf(NativeA32Memory& memory, uint32_t address, uint32_t size) noexcept {
    auto& groups = memory.*MemberOf(FastGroupsTag{});
    using Page = std::remove_reference_t<decltype(groups[0][0])>;
    const uint32_t offset = address & kMemPageMask;
    if (size == 0U || size > kMemPageSize - offset) return static_cast<Page*>(nullptr);
    auto& group = groups[address >> kMemGroupShift];
    return group ? &group[(address >> kMemPageShift) & kMemGroupPageMask]
                 : static_cast<Page*>(nullptr);
}

// Intervalo resolvido uma vez para os laços de áudio: leituras e escritas
// pelo ponteiro da página rápida, com a mesma geração/carimbo de WriteFast.
struct Span {
    uint8_t* Bytes = nullptr;
    uint64_t* Stamp = nullptr;
    uint32_t Base = 0U;
};

inline bool ResolveSpan(NativeA32Memory& memory, uint32_t base, uint32_t size,
                        Span& span) noexcept {
    if (memory.*MemberOf(TraceTag{})) return false;
    auto* page = FastPageOf(memory, base, size);
    if (page == nullptr || page->Write == nullptr || page->Bytes == nullptr ||
        page->Bytes != page->Write) {
        return false;
    }
    const uint32_t offset = base & kMemPageMask;
    span = {page->Write + offset, &page->WriteGeneration, base};
    return true;
}

// O mesmo que WriteFast faz depois do memcpy num acerto do caminho rápido.
inline void CommitFastWrite(NativeA32Memory& memory, uint64_t* stamp) noexcept {
    auto& generation = memory.*MemberOf(RangeGenerationTag{});
    *stamp = ++generation;
    if (*stamp == 0U) [[unlikely]] memory.*MemberOf(RangeVersionsTag{}) = false;
}

inline uint32_t SpanLoad(const Span& span, uint32_t address) noexcept {
    uint32_t value;
    std::memcpy(&value, span.Bytes + (address - span.Base), 4U);
    return value;
}

inline void SpanStore(NativeA32Memory& memory, const Span& span, uint32_t address,
                      uint32_t value) noexcept {
    std::memcpy(span.Bytes + (address - span.Base), &value, 4U);
    CommitFastWrite(memory, span.Stamp);
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
            OOT3D_AOT_STAT(context.Stats.DirectCalls);
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
                OOT3D_AOT_STAT(context.Stats.DirectCalls);
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

// ---------------------------------------------------------------------------
// Efeitos de áudio do DSP (aritmética inteira, 160 amostras por canal).
// Mesmo contrato das demais, com uma diferença: o orçamento de blocos é
// respeitado bloco a bloco (LocalGuest::Enter). Se ele acabar no meio do
// laço, a função sai com BlockLimit no mesmo PC e com o mesmo estado que a
// tradução teria naquele ponto, e o despachante retoma pela tradução.
// Leituras e escritas continuam todas pelas mesmas chamadas ReadFast/WriteFast
// e na mesma ordem; a validação inicial garante que nenhuma falha e que as
// escritas nos buffers não alcançam a pilha nem os campos do objeto (de onde
// saem os ponteiros e coeficientes validados).

// 0x004A022C: atraso de até 4 canais. r0 = estado do efeito, r1 = 4 ponteiros
// de entrada/saída. Por amostra: d = atraso[i]; s = round0(d * ganho >> 7);
// y = (k1 * (x - s) + k2 * fb) >> 7; fb = atraso[i] = y; x = d.
bool Override_004A022C(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx004A022C;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    const uint32_t obj = state.R[0];
    const uint32_t list = state.R[1];
    const uint32_t sp0 = state.R[13];
    if (Hooked(context, 0x004A022CU, 0x004A0334U) ||
        !Readable(memory, obj + 0x56U, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t enabled = 0U;
    memory.ReadFast<uint8_t>(obj + 0x56U, &enabled);
    if (enabled != 0U) {
        // Validação de tudo que o caminho completo acessa.
        const uint32_t stackLow = sp0 - 52U;
        if (!Writable(memory, stackLow, 52U) || !Readable(memory, stackLow, 52U) ||
            !Readable(memory, list, 16U) || !Readable(memory, obj + 0x1CU, 0x3CU) ||
            !Writable(memory, obj + 0x2CU, 16U) || !Writable(memory, obj + 0x40U, 4U) ||
            !Readable(memory, obj + 0x55U, 1U, 1U) ||
            Overlaps(list, 16U, stackLow, 52U) || Overlaps(obj + 0x1CU, 0x3CU, stackLow, 52U)) {
            return Decline(kIdx);
        }
        uint8_t channels = 0U;
        memory.ReadFast<uint8_t>(obj + 0x55U, &channels);
        if (channels > 4U) return Decline(kIdx);
        const uint32_t frameIndex = Read32(memory, obj + 0x40U);
        const uint32_t base = (frameIndex + (frameIndex << 2U)) << 5U;
        for (uint32_t ch = 0; ch < channels; ++ch) {
            const uint32_t io = Read32(memory, list + 4U * ch);
            const uint32_t delay = Read32(memory, obj + 0x1CU + 4U * ch) + (base << 2U);
            for (const uint32_t buffer : {io, delay}) {
                if (!Writable(memory, buffer, 640U) || !Readable(memory, buffer, 640U) ||
                    Overlaps(buffer, 640U, stackLow, 52U) ||
                    Overlaps(buffer, 640U, obj + 0x1CU, 0x3CU)) {
                    return Decline(kIdx);
                }
            }
        }
    }
    LocalGuest g(context, state);
    auto& R = g.R;
    auto& flags = g.Flags;
    // 0x004A022C
    if (!g.Enter()) { flow = g.BlockLimit(0x004A022CU); return Accept(kIdx); }
    R[2] = enabled;
    flags.SetSubtract(R[2], 0U, Mask(0xFU));
    if (flags.Z()) {
        g.Flush();
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x004A0238: push {r4-r11, lr}; sub sp, #16; cópia dos 4 ponteiros
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0238U); return Accept(kIdx); }
    {
        const uint32_t pushed[9] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[14]};
        for (uint32_t i = 0; i < 9U; ++i) memory.WriteFast<uint32_t>(R[13] - 36U + 4U * i, pushed[i]);
        R[13] -= 36U + 16U;
    }
    R[7] = 0U;
    R[2] = Read32(memory, R[1]);
    memory.WriteFast<uint32_t>(R[13], R[2]);
    R[2] = Read32(memory, R[1] + 4U);
    memory.WriteFast<uint32_t>(R[13] + 4U, R[2]);
    R[2] = Read32(memory, R[1] + 8U);
    memory.WriteFast<uint32_t>(R[13] + 8U, R[2]);
    R[1] = Read32(memory, R[1] + 12U);
    memory.WriteFast<uint32_t>(R[13] + 12U, R[1]);
    R[1] = Read32(memory, R[0] + 0x40U);
    R[1] = R[1] + (R[1] << 2U);
    R[8] = R[1] << 5U;
    {
        uint8_t value = 0U;
        memory.ReadFast<uint8_t>(R[0] + 0x55U, &value);
        R[1] = value;
    }
    flags.SetSubtract(R[1], 0U, Mask(0x6U));
    if (flags.C() && !flags.Z()) {
        // 0x004A027C
        if (!g.Enter()) { flow = g.BlockLimit(0x004A027CU); return Accept(kIdx); }
        R[9] = R[13];
        while (true) {
            // 0x004A0280: preparação do canal r7
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0280U); return Accept(kIdx); }
            R[1] = R[0] + (R[7] << 2U);
            R[4] = Read32(memory, R[9] + (R[7] << 2U));
            R[5] = Read32(memory, R[1] + 0x1CU);
            R[6] = R[1] + 44U;
            R[2] = R[8];
            R[1] = 0U;
            // Caminho rápido do laço de amostras: buffers e campos do objeto
            // resolvidos uma vez (cada um numa só página rápida).
            Span delaySpan, ioSpan, objSpan;
            const bool fast =
                ResolveSpan(memory, R[5] + (R[2] << 2U), 640U, delaySpan) &&
                ResolveSpan(memory, R[4], 640U, ioSpan) &&
                ResolveSpan(memory, R[0] + 0x2CU, 0x24U, objSpan);
            while (true) {
                if (fast && context.BlocksRemaining >= 3U) {
                    // 0x004A0298, 0x004A02BC/0x004A02A4, 0x004A02C8
                    Consume(context, 3U);
                    R[3] = SpanLoad(delaySpan, R[5] + (R[2] << 2U));
                    flags.SetSubtract(R[3], 0U, Mask(0x9U));
                    R[10] = SpanLoad(objSpan, R[0] + 0x44U);
                    if (flags.N() == flags.V()) {
                        R[12] = Oot3dAotAsr(R[10] * R[3], 7U);
                    } else {
                        R[12] = 0U - Oot3dAotAsr(R[10] * (0U - R[3]), 7U);
                    }
                    R[10] = SpanLoad(ioSpan, R[4] + (R[1] << 2U));
                    R[11] = SpanLoad(objSpan, R[6]);
                    R[14] = SpanLoad(objSpan, R[0] + 0x48U);
                    R[12] = R[10] - R[12];
                    R[10] = SpanLoad(objSpan, R[0] + 0x4CU) * R[11];
                    R[12] = Oot3dAotAsr(R[14] * R[12] + R[10], 7U);
                    SpanStore(memory, objSpan, R[6], R[12]);
                    SpanStore(memory, delaySpan, R[5] + (R[2] << 2U), R[12]);
                    SpanStore(memory, ioSpan, R[4] + (R[1] << 2U), R[3]);
                    R[1] += 1U;
                    flags.SetSubtract(R[1], 0x000000A0U, Mask(0x2U));
                    R[2] += 1U;
                    if (flags.C()) break;
                    continue;
                }
                // 0x004A0298
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0298U); return Accept(kIdx); }
                R[3] = Read32(memory, R[5] + (R[2] << 2U));
                flags.SetSubtract(R[3], 0U, Mask(0x9U));
                if (flags.N() == flags.V()) {
                    // 0x004A02BC
                    if (!g.Enter()) { flow = g.BlockLimit(0x004A02BCU); return Accept(kIdx); }
                    R[10] = Read32(memory, R[0] + 0x44U);
                    R[12] = R[10] * R[3];
                    R[12] = Oot3dAotAsr(R[12], 7U);
                } else {
                    // 0x004A02A4
                    if (!g.Enter()) { flow = g.BlockLimit(0x004A02A4U); return Accept(kIdx); }
                    R[10] = Read32(memory, R[0] + 0x44U);
                    R[12] = 0U - R[3];
                    R[12] = R[10] * R[12];
                    R[12] = Oot3dAotAsr(R[12], 7U);
                    R[12] = 0U - R[12];
                }
                // 0x004A02C8
                if (!g.Enter()) { flow = g.BlockLimit(0x004A02C8U); return Accept(kIdx); }
                R[10] = Read32(memory, R[4] + (R[1] << 2U));
                R[11] = Read32(memory, R[6]);
                R[14] = Read32(memory, R[0] + 0x48U);
                R[12] = R[10] - R[12];
                R[10] = Read32(memory, R[0] + 0x4CU);
                R[10] = R[10] * R[11];
                R[12] = R[14] * R[12] + R[10];
                R[12] = Oot3dAotAsr(R[12], 7U);
                memory.WriteFast<uint32_t>(R[6], R[12]);
                memory.WriteFast<uint32_t>(R[5] + (R[2] << 2U), R[12]);
                memory.WriteFast<uint32_t>(R[4] + (R[1] << 2U), R[3]);
                R[1] += 1U;
                flags.SetSubtract(R[1], 0x000000A0U, Mask(0x2U));
                R[2] += 1U;
                if (flags.C()) break;
            }
            // 0x004A0304
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0304U); return Accept(kIdx); }
            {
                uint8_t value = 0U;
                memory.ReadFast<uint8_t>(R[0] + 0x55U, &value);
                R[1] = value;
            }
            R[7] += 1U;
            flags.SetSubtract(R[1], R[7], Mask(0x6U));
            if (!(flags.C() && !flags.Z())) break;
        }
    }
    // 0x004A0314: avança o índice circular de quadros e retorna
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0314U); return Accept(kIdx); }
    R[1] = Read32(memory, R[0] + 0x40U);
    R[1] += 1U;
    memory.WriteFast<uint32_t>(R[0] + 0x40U, R[1]);
    R[2] = Read32(memory, R[0] + 0x3CU);
    flags.SetSubtract(R[1], R[2], Mask(0xFU));
    if (flags.C()) {
        R[1] = 0U;
        memory.WriteFast<uint32_t>(R[0] + 0x40U, R[1]);
    }
    R[13] += 16U;
    uint32_t popped[9];
    for (uint32_t i = 0; i < 9U; ++i) popped[i] = Read32(memory, R[13] + 4U * i);
    for (uint32_t i = 0; i < 8U; ++i) R[4 + i] = popped[i];
    R[13] += 36U;
    g.Flush();
    flow = Oot3dAotReturned(popped[8]);
    return Accept(kIdx);
}

// 0x004A0338: reverberação estéreo (2 canais x 160 amostras), cinco linhas de
// atraso circulares (índices em 0x9C/0xA4/0xB0/0xB4/0xC4, limites em
// 0x98/0xA0/0xA8/0xAC/0xC0), filtros com arredondamento simétrico (o sinal é
// tirado antes do >> 7) e realimentação; a saída é gravada no próprio buffer
// de entrada de cada canal.
bool Override_004A0338(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx004A0338;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    const uint32_t obj = state.R[0];
    const uint32_t list = state.R[1];
    const uint32_t sp0 = state.R[13];
    if (Hooked(context, 0x004A0338U, 0x004A0570U) ||
        !Readable(memory, obj + 0x100U, 1U, 1U)) {
        return Decline(kIdx);
    }
    uint8_t enabled = 0U;
    memory.ReadFast<uint8_t>(obj + 0x100U, &enabled);
    constexpr uint32_t kObjLow = 0x38U, kObjSize = 0x104U - 0x38U;
    if (enabled != 0U) {
        const uint32_t stackLow = sp0 - 72U;
        if (!Writable(memory, stackLow, 72U) || !Readable(memory, stackLow, 72U) ||
            !Readable(memory, list, 16U) || !Readable(memory, obj + kObjLow, kObjSize - 4U) ||
            !Writable(memory, obj + 0x98U, 0x30U) || !Writable(memory, obj + 0xCCU, 8U) ||
            !SinglePageReadable(memory, obj + 0xB0U, 8U) ||
            Overlaps(list, 16U, stackLow, 72U) ||
            Overlaps(obj + kObjLow, kObjSize, stackLow, 72U)) {
            return Decline(kIdx);
        }
        const uint32_t i3 = Read32(memory, obj + 0x9CU), i12 = Read32(memory, obj + 0xA4U);
        const uint32_t i4 = Read32(memory, obj + 0xB0U), i5 = Read32(memory, obj + 0xB4U);
        const uint32_t i6 = Read32(memory, obj + 0xC4U);
        for (uint32_t ch = 0; ch < 2U; ++ch) {
            const uint32_t buffers[6] = {
                Read32(memory, obj + 0x38U + 4U * ch) + (i3 << 2U),
                Read32(memory, obj + 0x48U + 4U * ch) + (i12 << 2U),
                Read32(memory, obj + 0x58U + 8U * ch) + (i4 << 2U),
                Read32(memory, obj + 0x5CU + 8U * ch) + (i5 << 2U),
                Read32(memory, obj + 0x78U + 4U * ch) + (i6 << 2U),
                Read32(memory, list + 4U * ch),
            };
            for (const uint32_t buffer : buffers) {
                if (!Writable(memory, buffer, 640U) || !Readable(memory, buffer, 640U) ||
                    Overlaps(buffer, 640U, stackLow, 72U) ||
                    Overlaps(buffer, 640U, obj + kObjLow, kObjSize)) {
                    return Decline(kIdx);
                }
            }
        }
    }
    LocalGuest g(context, state);
    auto& R = g.R;
    auto& flags = g.Flags;
    // 0x004A0338
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0338U); return Accept(kIdx); }
    R[2] = enabled;
    flags.SetSubtract(R[2], 0U, Mask(0xFU));
    if (flags.Z()) {
        g.Flush();
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x004A0344: push {r4-r11, lr}; sub sp, #36; cópia dos 4 ponteiros
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0344U); return Accept(kIdx); }
    {
        const uint32_t pushed[9] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[11], R[14]};
        for (uint32_t i = 0; i < 9U; ++i) memory.WriteFast<uint32_t>(R[13] - 36U + 4U * i, pushed[i]);
        R[13] -= 36U + 36U;
    }
    R[10] = 0U;
    R[2] = Read32(memory, R[1]);
    memory.WriteFast<uint32_t>(R[13] + 0x10U, R[2]);
    R[2] = Read32(memory, R[1] + 4U);
    memory.WriteFast<uint32_t>(R[13] + 0x14U, R[2]);
    R[2] = Read32(memory, R[1] + 8U);
    memory.WriteFast<uint32_t>(R[13] + 0x18U, R[2]);
    R[1] = Read32(memory, R[1] + 12U);
    memory.WriteFast<uint32_t>(R[13] + 0x1CU, R[1]);
    while (true) {
        // 0x004A0370: preparação do canal r10
        if (!g.Enter()) { flow = g.BlockLimit(0x004A0370U); return Accept(kIdx); }
        R[3] = Read32(memory, R[0] + 0x9CU);
        {
            uint64_t pair = 0U;
            memory.ReadFast<uint64_t>(R[0] + 0xB0U, &pair);
            R[4] = static_cast<uint32_t>(pair);
            R[5] = static_cast<uint32_t>(pair >> 32U);
        }
        R[9] = R[0] + (R[10] << 2U);
        R[6] = Read32(memory, R[0] + 0xC4U);
        R[2] = Read32(memory, R[9] + 0x38U);
        R[12] = Read32(memory, R[0] + 0xA4U);
        R[7] = Read32(memory, R[9] + 0x48U);
        memory.WriteFast<uint32_t>(R[13] + 0xCU, R[2]);
        R[2] = R[0] + (R[10] << 3U);
        R[1] = 0U;
        memory.WriteFast<uint32_t>(R[13] + 0x20U, R[2]);
        memory.WriteFast<uint32_t>(R[13] + 0x8U, R[7]);
        Span stackSpan, objSpan, aSpan, bSpan, cSpan, dSpan, eSpan, ioSpan;
        bool fast = ResolveSpan(memory, R[13], 0x24U, stackSpan) &&
                    ResolveSpan(memory, R[0] + 0x38U, 0xB4U, objSpan);
        if (fast) {
            const uint32_t pointerC = SpanLoad(objSpan, R[0] + (R[10] << 3U) + 0x58U);
            const uint32_t pointerD = SpanLoad(objSpan, R[0] + (R[10] << 3U) + 0x5CU);
            const uint32_t pointerE = SpanLoad(objSpan, R[9] + 0x78U);
            const uint32_t pointerIo = SpanLoad(stackSpan, R[13] + 0x10U + (R[10] << 2U));
            const uint32_t pointerA = SpanLoad(stackSpan, R[13] + 0xCU);
            const uint32_t pointerB = SpanLoad(stackSpan, R[13] + 0x8U);
            fast = ResolveSpan(memory, pointerA + (R[3] << 2U), 640U, aSpan) &&
                   ResolveSpan(memory, pointerB + (R[12] << 2U), 640U, bSpan) &&
                   ResolveSpan(memory, pointerC + (R[4] << 2U), 640U, cSpan) &&
                   ResolveSpan(memory, pointerD + (R[5] << 2U), 640U, dSpan) &&
                   ResolveSpan(memory, pointerE + (R[6] << 2U), 640U, eSpan) &&
                   ResolveSpan(memory, pointerIo, 640U, ioSpan);
        }
        while (true) {
            if (fast && context.BlocksRemaining >= 9U) {
                // Mesmo corpo de 0x004A03A0..0x004A04B4 pelos intervalos resolvidos.
                uint32_t blocks = 7U;
                const auto stack = [&](uint32_t offset) { return SpanLoad(stackSpan, R[13] + offset); };
                const auto field = [&](uint32_t offset) { return SpanLoad(objSpan, R[0] + offset); };
                R[2] = R[13] + 0x10U;
                R[7] = stack(0xCU);
                R[2] = stack(0x10U + (R[10] << 2U));
                R[11] = field(0xDCU);
                R[8] = SpanLoad(aSpan, R[7] + (R[3] << 2U));
                R[2] = SpanLoad(ioSpan, R[2] + (R[1] << 2U));
                SpanStore(memory, aSpan, R[7] + (R[3] << 2U), R[2]);
                R[7] = stack(0x8U);
                R[14] = stack(0x8U);
                R[11] = R[8] * R[11];
                R[7] = SpanLoad(bSpan, R[7] + (R[12] << 2U));
                SpanStore(memory, bSpan, R[14] + (R[12] << 2U), R[2]);
                R[2] = stack(0x20U);
                R[8] = SpanLoad(objSpan, R[2] + 0x58U);
                R[2] = SpanLoad(cSpan, R[8] + (R[4] << 2U));
                SpanStore(memory, stackSpan, R[13] + 0x4U, R[11]);
                flags.SetSubtract(R[2], 0U, Mask(0x9U));
                if (flags.N() == flags.V()) {
                    R[11] = Oot3dAotAsr(field(0xB8U) * R[2], 7U);
                } else {
                    R[14] = field(0xB8U);
                    R[11] = 0U - Oot3dAotAsr(R[14] * (0U - R[2]), 7U);
                }
                R[11] = R[11] + R[7];
                SpanStore(memory, cSpan, R[8] + (R[4] << 2U), R[11]);
                SpanStore(memory, stackSpan, R[13], R[2]);
                R[2] = stack(0x20U);
                R[8] = SpanLoad(objSpan, R[2] + 0x5CU);
                R[2] = SpanLoad(dSpan, R[8] + (R[5] << 2U));
                flags.SetSubtract(R[2], 0U, Mask(0x9U));
                if (flags.N() == flags.V()) {
                    R[11] = Oot3dAotAsr(field(0xBCU) * R[2], 7U);
                } else {
                    R[14] = field(0xBCU);
                    R[11] = 0U - Oot3dAotAsr(R[14] * (0U - R[2]), 7U);
                }
                R[7] = R[7] + R[11];
                SpanStore(memory, dSpan, R[8] + (R[5] << 2U), R[7]);
                R[7] = stack(0x0U);
                R[11] = SpanLoad(objSpan, R[9] + 0x78U);
                R[8] = R[7] - R[2];
                R[2] = field(0xC8U);
                R[7] = SpanLoad(eSpan, R[11] + (R[6] << 2U));
                R[14] = R[7];
                flags.SetSubtract(R[7], 0U, Mask(0x9U));
                if (flags.N() == flags.V()) {
                    R[14] = Oot3dAotAsr(R[14] * R[2], 7U);
                } else {
                    ++blocks; // 0x004A0480
                    R[14] = 0U - Oot3dAotAsr((0U - R[14]) * R[2], 7U);
                }
                flags.SetAdd(R[8], R[14], Mask(0x8U));
                R[8] = R[8] + R[14];
                SpanStore(memory, eSpan, R[11] + (R[6] << 2U), R[8]);
                if (!flags.N()) {
                    R[2] = Oot3dAotAsr(R[8] * R[2], 7U);
                } else {
                    ++blocks; // 0x004A04A4
                    R[8] = 0U - R[8];
                    R[2] = 0U - Oot3dAotAsr(R[8] * R[2], 7U);
                }
                R[8] = SpanLoad(objSpan, R[9] + 0xCCU);
                R[2] = R[7] - R[2];
                R[7] = field(0xE8U);
                R[8] = R[8] + R[2];
                R[3] += 1U;
                R[12] += 1U;
                R[7] = R[7] * R[8];
                R[4] += 1U;
                R[5] += 1U;
                R[6] += 1U;
                R[2] = R[2] - Oot3dAotAsr(R[7], 7U);
                SpanStore(memory, objSpan, R[9] + 0xCCU, R[2]);
                R[8] = field(0xE0U);
                R[7] = stack(0x4U);
                R[2] = R[2] * R[8] + R[7];
                R[7] = R[13] + 0x10U;
                R[7] = stack(0x10U + (R[10] << 2U));
                R[2] = Oot3dAotAsr(R[2], 7U);
                SpanStore(memory, ioSpan, R[7] + (R[1] << 2U), R[2]);
                R[1] += 1U;
                flags.SetSubtract(R[1], 0x000000A0U, Mask(0x9U));
                Consume(context, blocks);
                if (flags.N() == flags.V()) break;
                continue;
            }
            // 0x004A03A0
            if (!g.Enter()) { flow = g.BlockLimit(0x004A03A0U); return Accept(kIdx); }
            R[2] = R[13] + 0x10U;
            R[7] = Read32(memory, R[13] + 0xCU);
            R[2] = Read32(memory, R[2] + (R[10] << 2U));
            R[11] = Read32(memory, R[0] + 0xDCU);
            R[8] = Read32(memory, R[7] + (R[3] << 2U));
            R[2] = Read32(memory, R[2] + (R[1] << 2U));
            memory.WriteFast<uint32_t>(R[7] + (R[3] << 2U), R[2]);
            R[7] = Read32(memory, R[13] + 0x8U);
            R[14] = Read32(memory, R[13] + 0x8U);
            R[11] = R[8] * R[11];
            R[7] = Read32(memory, R[7] + (R[12] << 2U));
            memory.WriteFast<uint32_t>(R[14] + (R[12] << 2U), R[2]);
            R[2] = Read32(memory, R[13] + 0x20U);
            R[8] = Read32(memory, R[2] + 0x58U);
            R[2] = Read32(memory, R[8] + (R[4] << 2U));
            memory.WriteFast<uint32_t>(R[13] + 0x4U, R[11]);
            flags.SetSubtract(R[2], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                // 0x004A0400
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0400U); return Accept(kIdx); }
                R[11] = Read32(memory, R[0] + 0xB8U);
                R[11] = R[11] * R[2];
                R[11] = Oot3dAotAsr(R[11], 7U);
            } else {
                // 0x004A03E8
                if (!g.Enter()) { flow = g.BlockLimit(0x004A03E8U); return Accept(kIdx); }
                R[14] = Read32(memory, R[0] + 0xB8U);
                R[11] = 0U - R[2];
                R[11] = R[14] * R[11];
                R[11] = Oot3dAotAsr(R[11], 7U);
                R[11] = 0U - R[11];
            }
            // 0x004A040C
            if (!g.Enter()) { flow = g.BlockLimit(0x004A040CU); return Accept(kIdx); }
            R[11] = R[11] + R[7];
            memory.WriteFast<uint32_t>(R[8] + (R[4] << 2U), R[11]);
            memory.WriteFast<uint32_t>(R[13], R[2]);
            R[2] = Read32(memory, R[13] + 0x20U);
            R[8] = Read32(memory, R[2] + 0x5CU);
            R[2] = Read32(memory, R[8] + (R[5] << 2U));
            flags.SetSubtract(R[2], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                // 0x004A0444
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0444U); return Accept(kIdx); }
                R[11] = Read32(memory, R[0] + 0xBCU);
                R[11] = R[11] * R[2];
                R[11] = Oot3dAotAsr(R[11], 7U);
            } else {
                // 0x004A042C
                if (!g.Enter()) { flow = g.BlockLimit(0x004A042CU); return Accept(kIdx); }
                R[14] = Read32(memory, R[0] + 0xBCU);
                R[11] = 0U - R[2];
                R[11] = R[14] * R[11];
                R[11] = Oot3dAotAsr(R[11], 7U);
                R[11] = 0U - R[11];
            }
            // 0x004A0450
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0450U); return Accept(kIdx); }
            R[7] = R[7] + R[11];
            memory.WriteFast<uint32_t>(R[8] + (R[5] << 2U), R[7]);
            R[7] = Read32(memory, R[13]);
            R[11] = Read32(memory, R[9] + 0x78U);
            R[8] = R[7] - R[2];
            R[2] = Read32(memory, R[0] + 0xC8U);
            R[7] = Read32(memory, R[11] + (R[6] << 2U));
            R[14] = R[7];
            flags.SetSubtract(R[7], 0U, Mask(0x9U));
            if (flags.N() == flags.V()) {
                R[14] = R[14] * R[2];
                R[14] = Oot3dAotAsr(R[14], 7U);
            } else {
                // 0x004A0480
                if (!g.Enter()) { flow = g.BlockLimit(0x004A0480U); return Accept(kIdx); }
                R[14] = 0U - R[14];
                R[14] = R[14] * R[2];
                R[14] = Oot3dAotAsr(R[14], 7U);
                R[14] = 0U - R[14];
            }
            // 0x004A0490
            if (!g.Enter()) { flow = g.BlockLimit(0x004A0490U); return Accept(kIdx); }
            flags.SetAdd(R[8], R[14], Mask(0x8U));
            R[8] = R[8] + R[14];
            memory.WriteFast<uint32_t>(R[11] + (R[6] << 2U), R[8]);
            if (!flags.N()) {
                R[2] = R[8] * R[2];
                R[2] = Oot3dAotAsr(R[2], 7U);
            } else {
                // 0x004A04A4
                if (!g.Enter()) { flow = g.BlockLimit(0x004A04A4U); return Accept(kIdx); }
                R[8] = 0U - R[8];
                R[2] = R[8] * R[2];
                R[2] = Oot3dAotAsr(R[2], 7U);
                R[2] = 0U - R[2];
            }
            // 0x004A04B4
            if (!g.Enter()) { flow = g.BlockLimit(0x004A04B4U); return Accept(kIdx); }
            R[8] = Read32(memory, R[9] + 0xCCU);
            R[2] = R[7] - R[2];
            R[7] = Read32(memory, R[0] + 0xE8U);
            R[8] = R[8] + R[2];
            R[3] += 1U;
            R[12] += 1U;
            R[7] = R[7] * R[8];
            R[4] += 1U;
            R[5] += 1U;
            R[6] += 1U;
            R[2] = R[2] - Oot3dAotAsr(R[7], 7U);
            memory.WriteFast<uint32_t>(R[9] + 0xCCU, R[2]);
            R[8] = Read32(memory, R[0] + 0xE0U);
            R[7] = Read32(memory, R[13] + 0x4U);
            R[2] = R[2] * R[8] + R[7];
            R[7] = R[13] + 0x10U;
            R[7] = Read32(memory, R[7] + (R[10] << 2U));
            R[2] = Oot3dAotAsr(R[2], 7U);
            memory.WriteFast<uint32_t>(R[7] + (R[1] << 2U), R[2]);
            R[1] += 1U;
            flags.SetSubtract(R[1], 0x000000A0U, Mask(0x9U));
            if (flags.N() == flags.V()) break;
        }
        // 0x004A050C
        if (!g.Enter()) { flow = g.BlockLimit(0x004A050CU); return Accept(kIdx); }
        R[10] += 1U;
        flags.SetSubtract(R[10], 0x00000002U, Mask(0x9U));
        if (flags.N() == flags.V()) break;
    }
    // 0x004A0518: grava os índices circulares (com volta ao limite) e retorna
    if (!g.Enter()) { flow = g.BlockLimit(0x004A0518U); return Accept(kIdx); }
    R[2] = Read32(memory, R[0] + 0x98U);
    R[1] = 0U;
    const auto wrap = [&](uint32_t limitOffset, uint32_t index, uint32_t fieldOffset,
                          uint8_t mask) {
        R[2] = Read32(memory, R[0] + limitOffset);
        flags.SetSubtract(R[2], index, Mask(mask));
        memory.WriteFast<uint32_t>(R[0] + fieldOffset,
                                   flags.C() && !flags.Z() ? index : R[1]);
    };
    flags.SetSubtract(R[2], R[3], Mask(0x6U));
    memory.WriteFast<uint32_t>(R[0] + 0x9CU, flags.C() && !flags.Z() ? R[3] : R[1]);
    wrap(0xA0U, R[12], 0xA4U, 0x6U);
    wrap(0xA8U, R[4], 0xB0U, 0x6U);
    wrap(0xACU, R[5], 0xB4U, 0x6U);
    wrap(0xC0U, R[6], 0xC4U, 0xFU);
    R[13] += 36U;
    uint32_t popped[9];
    for (uint32_t i = 0; i < 9U; ++i) popped[i] = Read32(memory, R[13] + 4U * i);
    for (uint32_t i = 0; i < 8U; ++i) R[4 + i] = popped[i];
    R[13] += 36U;
    g.Flush();
    flow = Oot3dAotReturned(popped[8]);
    return Accept(kIdx);
}

// ---------------------------------------------------------------------------
// 0x00371738 Runtime_Memcpy (folha; cópia de palavras com src/dst alinhados
// pelo chamador): push {r4-r10, lr}; voltas de 32 bytes (ldm/ldm/stm/stm,
// 1 bloco por volta); depois 16 e 8 bytes conforme os bits 4 e 3 de r2, pop,
// 4 bytes (bit 2) e, se sobrar, 2 e 1 bytes no bloco 0x00371794.
// O laço vira cópias por trecho de página: a tradução escreve palavra a
// palavra em ordem crescente de endereço (geração +1 por palavra, página
// carimbada com a geração da última palavra escrita nela), e isso é
// reproduzido trecho a trecho. Exige src e dst alinhados a 4, sem
// sobreposição entre origem e destino (cada região do mapa tem bytes
// próprios, então intervalos guest disjuntos são disjuntos no host) e tudo
// em páginas rápidas; o resto (cabeçalho e cauda) usa as mesmas chamadas
// ReadWordsFast/WriteWordsFast/ReadFast/WriteFast da tradução.
bool Override_00371738(Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
                       Oot3dAotArchitecturalState& state,
                       Oot3dWholeAotFlow& flow) noexcept {
    constexpr size_t kIdx = kIdx00371738;
    static_cast<void>(frame);
    auto& memory = context.Memory;
    auto& R = state.R;
    auto& flags = state.Flags;
    const uint32_t dst = R[0], src = R[1], len = R[2], sp = R[13];
    if (Hooked(context, 0x00371738U, 0x003717A8U)) return Decline(kIdx);
    const uint32_t loops = len >> 5U;
    // 0x00371738 [+ 0x00371744 + voltas em 0x00371750] + 0x0037176C [+ 0x00371794]
    const uint32_t blocks = 2U + (loops != 0U ? 1U + loops : 0U) + ((len & 3U) != 0U ? 1U : 0U);
    if (len > 0x00100000U || ((src | dst) & 3U) != 0U || !Budget(context, blocks) ||
        !Writable(memory, sp - 32U, 32U) || !Readable(memory, sp - 32U, 32U) ||
        !Readable(memory, src, len) || !Writable(memory, dst, len) ||
        Overlaps(src, len, dst, len)) {
        return Decline(kIdx);
    }
    auto& generation = memory.*MemberOf(RangeGenerationTag{});
    // Nenhum carimbo pode dar a volta (a tradução invalidaria as versões).
    if (generation > UINT64_MAX - 64U - len) return Decline(kIdx);
    Consume(context, blocks);
    // 0x00371738: push {r4-r10, lr}; subs r2, r2, #0x20
    {
        const uint32_t firstAddress = sp - 32U;
        const uint32_t words[8] = {R[4], R[5], R[6], R[7], R[8], R[9], R[10], R[14]};
        if (!memory.WriteWordsFast(firstAddress, words, 8U)) {
            for (uint32_t i = 0; i < 8U; ++i) memory.WriteFast<uint32_t>(firstAddress + 4U * i, words[i]);
        }
        R[13] = firstAddress;
    }
    flags.SetSubtract(R[2], 0x20U, Mask(0x3U));
    R[2] -= 0x20U;
    if (loops != 0U) {
        // 0x00371744 + 0x00371750 x voltas: copia 32 * voltas bytes.
        const uint32_t bytes = loops << 5U;
        uint32_t done = 0U;
        while (done < bytes) {
            const uint32_t from = src + done, to = dst + done;
            const uint32_t chunk = std::min({bytes - done, kMemPageSize - (from & kMemPageMask),
                                             kMemPageSize - (to & kMemPageMask)});
            auto* readPage = FastPageOf(memory, from, chunk);
            auto* writePage = FastPageOf(memory, to, chunk);
            std::memcpy(writePage->Write + (to & kMemPageMask),
                        readPage->Bytes + (from & kMemPageMask), chunk);
            generation += chunk >> 2U;
            writePage->WriteGeneration = generation;
            done += chunk;
        }
        // Registradores da última volta (r3-r10 = as 8 palavras finais).
        const uint32_t last = src + bytes - 32U;
        for (uint32_t i = 0; i < 8U; ++i) R[3 + i] = Read32(memory, last + 4U * i);
        R[0] = dst + bytes;
        R[1] = src + bytes;
        flags.SetSubtract(len - bytes, 0x20U, Mask(0x3U));
        R[2] = len - bytes - 0x20U;
    }
    // 0x0037176C
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 28U, flags.C());
        R[12] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[12], shifted.Carry, Mask(0xAU));
    }
    if (flags.C()) {
        uint32_t words[4];
        if (!memory.ReadWordsFast(R[1], words, 4U)) {
            for (uint32_t i = 0; i < 4U; ++i) memory.ReadFast<uint32_t>(R[1] + 4U * i, &words[i]);
        }
        R[3] = words[0];
        R[4] = words[1];
        R[12] = words[2];
        R[14] = words[3];
        R[1] += 16U;
        const uint32_t out[4] = {R[3], R[4], R[12], R[14]};
        if (!memory.WriteWordsFast(R[0], out, 4U)) {
            for (uint32_t i = 0; i < 4U; ++i) memory.WriteFast<uint32_t>(R[0] + 4U * i, out[i]);
        }
        R[0] += 16U;
    }
    if (flags.N()) {
        uint32_t words[2];
        if (!memory.ReadWordsFast(R[1], words, 2U)) {
            for (uint32_t i = 0; i < 2U; ++i) memory.ReadFast<uint32_t>(R[1] + 4U * i, &words[i]);
        }
        R[3] = words[0];
        R[4] = words[1];
        R[1] += 8U;
        if (!memory.WriteWordsFast(R[0], words, 2U)) {
            for (uint32_t i = 0; i < 2U; ++i) memory.WriteFast<uint32_t>(R[0] + 4U * i, words[i]);
        }
        R[0] += 8U;
    }
    {
        uint32_t words[8];
        if (!memory.ReadWordsFast(R[13], words, 8U)) {
            for (uint32_t i = 0; i < 8U; ++i) memory.ReadFast<uint32_t>(R[13] + 4U * i, &words[i]);
        }
        for (uint32_t i = 0; i < 7U; ++i) R[4 + i] = words[i];
        R[14] = words[7];
        R[13] += 32U;
    }
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 30U, flags.C());
        R[12] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[12], shifted.Carry, Mask(0xEU));
    }
    if (flags.C()) {
        R[3] = Read32(memory, R[1]);
        R[1] += 4U;
        memory.WriteFast<uint32_t>(R[0], R[3]);
        R[0] += 4U;
    }
    if (flags.Z()) {
        flow = Oot3dAotReturned(R[14]);
        return Accept(kIdx);
    }
    // 0x00371794
    {
        const auto shifted = Oot3dAotShiftImmediate(R[2], 0U, 31U, flags.C());
        R[2] = shifted.Value;
        Oot3dAotSetLogicalFlags(flags, R[2], shifted.Carry, Mask(0xEU));
    }
    if (flags.C()) {
        uint16_t value = 0U;
        memory.ReadFast<uint16_t>(R[1], &value);
        R[3] = value;
        R[1] += 2U;
    }
    if (flags.N()) {
        uint8_t value = 0U;
        memory.ReadFast<uint8_t>(R[1], &value);
        R[2] = value;
        R[1] += 1U;
    }
    if (flags.C()) {
        memory.WriteFast<uint16_t>(R[0], static_cast<uint16_t>(R[3]));
        R[0] += 2U;
    }
    if (flags.N()) {
        memory.WriteFast<uint8_t>(R[0], static_cast<uint8_t>(R[2]));
        R[0] += 1U;
    }
    flow = Oot3dAotReturned(R[14]);
    return Accept(kIdx);
}

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
