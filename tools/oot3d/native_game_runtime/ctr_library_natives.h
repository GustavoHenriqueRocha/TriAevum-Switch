#pragma once

// Funções de biblioteca do compilador (RVCT/armcc) reconhecidas pelos bytes,
// não pelo endereço: servem a qualquer título CTR montado com a mesma
// biblioteca.
//
// Algumas rotinas da biblioteca de ponto flutuante do RVCT chamam um helper
// de exceção (__fpl_*) que lê uma palavra literal logo depois do BL
// ("ldr r4, [lr], #4") e volta por uma tabela de slots depois dela. A
// descoberta de funções vê a palavra literal como instrução e marca o CFG
// como aberto: a função fica fora do whole-AOT (external_functions) e, sem
// interpretador, o processo para em MissingBlock ao chamá-la. Super Mario 3D
// Land: __aeabi_f2ulz em 0x003A0D38, chamado no boot da janela.
//
// Cada entrada aqui é a sequência exata de palavras A32 do início da rotina
// (até antes do primeiro BL, cujo deslocamento depende do endereço) e uma
// implementação nativa com a mesma semântica, inclusive a do caminho do
// helper (saturação), conferida na tabela de retorno do próprio binário.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

enum class CtrLibraryNative : uint8_t {
    // unsigned long long __aeabi_f2ulz(float): r0 = bits do float; volta
    // r0:r1 (baixo:alto). NaN e negativos -> 0; acima de 2^64 e +inf ->
    // 0xFFFFFFFFFFFFFFFF (slots 0 e 3 da tabela do helper __fpl).
    AeabiF2ulz,
};

struct CtrLibraryNativeSignature {
    CtrLibraryNative Kind;
    const char* Name;
    std::span<const uint32_t> Words;
};

inline constexpr std::array<uint32_t, 19> kRvctAeabiF2ulzWords{
    0xE1B02BC0U, // asrs r2, r0, #23
    0xE1A03400U, // lsl r3, r0, #8
    0x13833102U, // orrne r3, r3, #0x80000000
    0x4A000007U, // bmi negativo
    0xE27220BEU, // rsbs r2, r2, #0xbe
    0x3A00000BU, // blo helper (estouro, NaN)
    0xE1A01233U, // lsr r1, r3, r2
    0xE252C020U, // subs ip, r2, #0x20
    0x21A00C33U, // lsrhs r0, r3, ip
    0x3262C020U, // rsblo ip, r2, #0x20
    0x31A00C13U, // lsllo r0, r3, ip
    0xE12FFF1EU, // bx lr
    0xE1A0C080U, // lsl ip, r0, #1
    0xE35C047FU, // cmp ip, #0x7f000000
    0x2A000002U, // bhs helper
    0xE3A00000U, // mov r0, #0
    0xE3A01000U, // mov r1, #0
    0xE12FFF1EU, // bx lr
    0xE92D4010U, // push {r4, lr}  (segue BL __fpl_* + literal)
};

inline constexpr std::array<CtrLibraryNativeSignature, 1> kCtrLibraryNativeSignatures{{
    {CtrLibraryNative::AeabiF2ulz, "__aeabi_f2ulz", kRvctAeabiF2ulzWords},
}};

inline uint64_t CtrAeabiF2ulz(uint32_t bits) noexcept {
    const uint32_t exponent = (bits >> 23U) & 0xFFU;
    if (exponent == 0xFFU && (bits & 0x7FFFFFU) != 0U) {
        return 0U; // NaN: slot 0 (r0 = r1 = 0)
    }
    if ((bits & 0x80000000U) != 0U) {
        return 0U; // (-1, 0) direto; <= -1 e -inf pelo slot 3 (~(r0 asr 31))
    }
    if (exponent > 0xBEU) {
        return ~uint64_t{0}; // >= 2^64 e +inf: slot 3
    }
    const uint32_t mantissa =
        (bits << 8U) | (exponent != 0U ? 0x80000000U : 0U);
    const uint32_t shift = 0xBEU - exponent;
    return shift >= 64U ? 0U : (uint64_t{mantissa} << 32U) >> shift;
}

struct CtrLibraryNativeEntry {
    uint32_t Pc = 0;
    CtrLibraryNative Kind = CtrLibraryNative::AeabiF2ulz;
    friend bool operator<(const CtrLibraryNativeEntry& a,
                          const CtrLibraryNativeEntry& b) noexcept {
        return a.Pc < b.Pc;
    }
};

// Procura as assinaturas num segmento executável (palavras alinhadas).
inline void FindCtrLibraryNatives(uint32_t baseAddress,
                                  std::span<const uint8_t> bytes,
                                  std::vector<CtrLibraryNativeEntry>& out) {
    const size_t words = bytes.size() / 4U;
    const auto word = [&](size_t index) {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + index * 4U, 4U);
        return value; // host little-endian, como o ARM11
    };
    for (const auto& signature : kCtrLibraryNativeSignatures) {
        const size_t length = signature.Words.size();
        for (size_t index = 0; index + length <= words; ++index) {
            if (word(index) != signature.Words[0]) {
                continue;
            }
            bool match = true;
            for (size_t offset = 1; offset < length && match; ++offset) {
                match = word(index + offset) == signature.Words[offset];
            }
            if (match) {
                out.push_back({baseAddress + static_cast<uint32_t>(index * 4U),
                               signature.Kind});
            }
        }
    }
    std::sort(out.begin(), out.end());
}

inline const CtrLibraryNativeEntry* FindCtrLibraryNative(
    std::span<const CtrLibraryNativeEntry> entries, uint32_t pc) noexcept {
    const auto it = std::lower_bound(entries.begin(), entries.end(),
                                     CtrLibraryNativeEntry{pc});
    return it != entries.end() && it->Pc == pc ? &*it : nullptr;
}

// Executa a rotina com os registradores do guest (r0..r3 entram/saem) e volta
// para lr. Devolve o PC de retorno.
template <typename GuestState>
uint32_t ExecuteCtrLibraryNative(CtrLibraryNative kind, GuestState& state) {
    switch (kind) {
    case CtrLibraryNative::AeabiF2ulz: {
        const uint64_t value = CtrAeabiF2ulz(state.r[0]);
        state.r[0] = static_cast<uint32_t>(value);
        state.r[1] = static_cast<uint32_t>(value >> 32U);
        break;
    }
    }
    state.r[15] = state.r[14];
    return state.r[15];
}

} // namespace Oot3dNativeGame
