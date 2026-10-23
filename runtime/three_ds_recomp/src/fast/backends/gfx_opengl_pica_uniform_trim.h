#pragma once

// Uniforms enxutos do caminho PICA (config/gl_uniform_trim, ligado por
// padrao; arquivo com "0" desliga; desktop: TRIAEVUM_GL_UNIFORM_TRIM).
//
// Cada draw manda dois blocos std140 ao anel de uniforms: o de vertice
// (1616 bytes: b, flip_viewport, i[4], f[96]) e o de fragmento (6112 bytes,
// pica_uniform_layout.h). Muitos programas so leem o comeco do bloco (um
// fragment shader sem iluminacao nem programa parametrico para em
// texture_lod_bias, byte 1184). Na criacao do programa, o GLSL final
// (depois de TranslatePicaShaderToOpenGl43) e varrido e guarda-se o prefixo
// [0, N) que ele pode ler; o draw copia ao anel so esse prefixo, mas reserva
// e liga (glBindBufferRange) o bloco inteiro como antes, entao o estado GL e
// o mesmo e os bytes que o shader le sao identicos aos do caminho antigo (o
// resto da faixa ligada fica com o que o anel tinha; ninguem le).
//
// Conservador: indice nao literal (ou array usado inteiro) vale o membro
// inteiro; o que o varredor nao entende (membro desconhecido, a instancia
// sem ".membro", #define, indice literal fora do array) vale o bloco
// inteiro. Comentarios contam como referencia (so superestima).
// Enderecamento relativo do PICA (a0/aL) aparece como get_offset_register()
// lendo uniforms.f[index]: chamado, vale f[] inteiro (o indice e
// (base + a) & 0x7F, qualquer registrador); so definido e nunca chamado, nao
// conta.
//
// Livre de GL e sem estado: testado offline
// (tests/gfx_opengl_pica_uniform_trim_tests.cpp).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "fast/oot3d/pica_uniform_layout.h"

namespace Fast::PicaGlUniformTrim {

// Le o bloco inteiro (ou nao foi possivel provar menos).
inline constexpr size_t kAll = static_cast<size_t>(-1);

// Membro de um bloco std140: Count == 0 e escalar/struct de Stride bytes;
// senao array de Count elementos de Stride bytes.
struct Member {
    std::string_view Name;
    size_t Offset = 0U;
    size_t Stride = 0U;
    size_t Count = 0U;

    [[nodiscard]] constexpr size_t End() const noexcept {
        return Offset + Stride * (Count == 0U ? 1U : Count);
    }
};

namespace detail {

constexpr size_t kVec4 = Oot3d::kPicaPackedVec4Bytes;

inline constexpr std::array<Member, 4> kVertexMembers{{
    {"b", Oot3d::kPicaPackedVertexBooleanMaskOffset, 4U, 0U},
    {"flip_viewport", Oot3d::kPicaPackedVertexFlipViewportOffset, 4U, 0U},
    {"i", Oot3d::kPicaPackedVertexIntegerOffset, kVec4, 4U},
    {"f", Oot3d::kPicaPackedVertexFloatOffset, kVec4, 96U},
}};

inline constexpr std::array<Member, 25> kFragmentMembers{{
    {"tev_constants", Oot3d::kPicaPackedFragmentTevConstantsOffset, kVec4, 6U},
    {"combiner_buffer_color", Oot3d::kPicaPackedFragmentCombinerBufferOffset, kVec4, 0U},
    {"alpha_reference", Oot3d::kPicaPackedFragmentAlphaReferenceOffset, 4U, 0U},
    {"depth_scale", Oot3d::kPicaPackedFragmentDepthScaleOffset, 4U, 0U},
    {"depth_offset", Oot3d::kPicaPackedFragmentDepthOffsetOffset, 4U, 0U},
    {"w_buffering", Oot3d::kPicaPackedFragmentWBufferingOffset, 4U, 0U},
    {"fog_color", Oot3d::kPicaPackedFragmentFogColorOffset, kVec4, 0U},
    {"fog_lut", Oot3d::kPicaPackedFragmentFogLutOffset, kVec4, 64U},
    {"texture_lod_bias", Oot3d::kPicaPackedFragmentTextureLodBiasOffset, kVec4, 0U},
    {"lighting_specular0", Oot3d::kPicaPackedFragmentLightingSpecular0Offset, kVec4, 8U},
    {"lighting_specular1", Oot3d::kPicaPackedFragmentLightingSpecular1Offset, kVec4, 8U},
    {"lighting_diffuse", Oot3d::kPicaPackedFragmentLightingDiffuseOffset, kVec4, 8U},
    {"lighting_ambient", Oot3d::kPicaPackedFragmentLightingAmbientOffset, kVec4, 8U},
    {"lighting_position", Oot3d::kPicaPackedFragmentLightingPositionOffset, kVec4, 8U},
    {"lighting_spot_direction", Oot3d::kPicaPackedFragmentLightingSpotDirectionOffset, kVec4, 8U},
    {"lighting_attenuation", Oot3d::kPicaPackedFragmentLightingAttenuationOffset, kVec4, 8U},
    {"lighting_global_ambient", Oot3d::kPicaPackedFragmentLightingGlobalAmbientOffset, kVec4, 0U},
    {"shadow_texture_bias", Oot3d::kPicaPackedFragmentShadowTextureBiasOffset, 4U, 0U},
    {"shadow_orthographic", Oot3d::kPicaPackedFragmentShadowOrthographicOffset, 4U, 0U},
    {"shadow_bias_constant", Oot3d::kPicaPackedFragmentShadowBiasConstantOffset, 4U, 0U},
    {"shadow_bias_linear", Oot3d::kPicaPackedFragmentShadowBiasLinearOffset, 4U, 0U},
    // Programa parametrico: structs (tamanho = distancia ao proximo membro).
    {"tev_program", Oot3d::kPicaPackedFragmentTevProgramOffset,
     Oot3d::kPicaPackedFragmentLightingProgramOffset - Oot3d::kPicaPackedFragmentTevProgramOffset, 0U},
    {"lighting_program", Oot3d::kPicaPackedFragmentLightingProgramOffset,
     Oot3d::kPicaPackedFragmentControlOffset - Oot3d::kPicaPackedFragmentLightingProgramOffset, 0U},
    {"fragment_control", Oot3d::kPicaPackedFragmentControlOffset,
     Oot3d::kPicaPackedFragmentProcTexOffset - Oot3d::kPicaPackedFragmentControlOffset, 0U},
    {"proctex_program", Oot3d::kPicaPackedFragmentProcTexOffset,
     Oot3d::kPicaPackedFragmentUniformSize - Oot3d::kPicaPackedFragmentProcTexOffset, 0U},
}};

constexpr bool IsIdent(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
constexpr bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}
inline size_t SkipSpace(std::string_view s, size_t at) noexcept {
    while (at < s.size() && IsSpace(s[at])) ++at;
    return at;
}
// Posicao de `word` como token inteiro a partir de `from`, ou npos.
inline size_t FindToken(std::string_view s, std::string_view word,
                        size_t from) noexcept {
    while (true) {
        const size_t at = s.find(word, from);
        if (at == std::string_view::npos) return at;
        const bool begins = at == 0U || !IsIdent(s[at - 1U]);
        const size_t after = at + word.size();
        const bool ends = after >= s.size() || !IsIdent(s[after]);
        if (begins && ends) return at;
        from = at + 1U;
    }
}

// Faixa [Begin, End) do corpo de uma funcao definida e nunca chamada, ou
// vazia. A definicao e "<tipo> nome(...) {": o token anterior ao nome e um
// identificador.
struct Range {
    size_t Begin = 0U;
    size_t End = 0U;
};
inline Range UncalledHelperBody(std::string_view s, std::string_view name) {
    if (name.empty()) return {};
    size_t definition = std::string_view::npos;
    size_t bodyBegin = 0U, bodyEnd = 0U;
    size_t uses = 0U;
    for (size_t at = FindToken(s, name, 0U); at != std::string_view::npos;
         at = FindToken(s, name, at + name.size())) {
        if (definition != std::string_view::npos && at >= bodyBegin &&
            at < bodyEnd) {
            ++uses; // recursao: conservador, conta como chamada
            continue;
        }
        size_t before = at;
        while (before > 0U && IsSpace(s[before - 1U])) --before;
        const bool typed = before > 0U && IsIdent(s[before - 1U]);
        size_t open = SkipSpace(s, at + name.size());
        if (definition == std::string_view::npos && typed && open < s.size() &&
            s[open] == '(') {
            const size_t close = s.find(')', open);
            const size_t brace =
                close == std::string_view::npos ? close : SkipSpace(s, close + 1U);
            if (brace != std::string_view::npos && brace < s.size() &&
                s[brace] == '{') {
                size_t depth = 0U;
                size_t cursor = brace;
                for (; cursor < s.size(); ++cursor) {
                    if (s[cursor] == '{') ++depth;
                    if (s[cursor] == '}' && --depth == 0U) break;
                }
                if (cursor >= s.size()) return {}; // corpo sem fim: conta tudo
                definition = at;
                bodyBegin = brace;
                bodyEnd = cursor + 1U;
                continue;
            }
        }
        ++uses;
    }
    if (definition == std::string_view::npos || uses != 0U) return {};
    return {bodyBegin, bodyEnd};
}

// Indice literal decimal "[ 12 ]" (sufixo u aceito) a partir de `at` (que
// aponta para '['). Devolve false quando nao e literal.
inline bool LiteralIndex(std::string_view s, size_t at, size_t* index) {
    size_t cursor = SkipSpace(s, at + 1U);
    size_t value = 0U;
    size_t digits = 0U;
    while (cursor < s.size() && s[cursor] >= '0' && s[cursor] <= '9') {
        value = value * 10U + static_cast<size_t>(s[cursor] - '0');
        if (++digits > 6U) return false;
        ++cursor;
    }
    if (digits == 0U) return false;
    if (cursor < s.size() && (s[cursor] == 'u' || s[cursor] == 'U')) ++cursor;
    cursor = SkipSpace(s, cursor);
    if (cursor >= s.size() || s[cursor] != ']') return false;
    *index = value;
    return true;
}

} // namespace detail

// Bytes [0, N) do bloco de instancia `instance` que `source` pode ler; kAll
// quando nao da para provar. `helper`: funcao cujo corpo nao conta se ela
// nunca for chamada.
inline size_t UsedBytes(std::string_view source, std::string_view instance,
                        std::span<const Member> members,
                        std::string_view helper = {}) {
    using namespace detail;
    const Range skip = UncalledHelperBody(source, helper);
    size_t used = 0U;
    for (size_t at = FindToken(source, instance, 0U);
         at != std::string_view::npos;
         at = FindToken(source, instance, at + instance.size())) {
        if (at >= skip.Begin && at < skip.End) continue;
        size_t cursor = SkipSpace(source, at + instance.size());
        if (cursor < source.size() && source[cursor] == ';') {
            // A declaracao "} instance;" (nada mais vem antes de ';').
            size_t before = at;
            while (before > 0U && IsSpace(source[before - 1U])) --before;
            if (before > 0U && source[before - 1U] == '}') continue;
            return kAll;
        }
        if (cursor >= source.size() || source[cursor] != '.') return kAll;
        cursor = SkipSpace(source, cursor + 1U);
        size_t nameEnd = cursor;
        while (nameEnd < source.size() && IsIdent(source[nameEnd])) ++nameEnd;
        const std::string_view name = source.substr(cursor, nameEnd - cursor);
        const Member* member = nullptr;
        for (const Member& candidate : members) {
            if (candidate.Name == name) {
                member = &candidate;
                break;
            }
        }
        if (member == nullptr) return kAll;
        size_t end = member->End();
        if (member->Count != 0U) {
            const size_t open = SkipSpace(source, nameEnd);
            size_t index = 0U;
            if (open < source.size() && source[open] == '[' &&
                LiteralIndex(source, open, &index)) {
                if (index >= member->Count) return kAll;
                end = member->Offset + member->Stride * (index + 1U);
            }
        }
        used = std::max(used, end);
    }
    return used;
}

// GLSL de vertice (bloco "uniforms").
inline size_t VertexUsedBytes(std::string_view source) {
    return UsedBytes(source, "uniforms", detail::kVertexMembers,
                     "get_offset_register");
}

// GLSL de fragmento (bloco "fragment_uniforms").
inline size_t FragmentUsedBytes(std::string_view source) {
    return UsedBytes(source, "fragment_uniforms", detail::kFragmentMembers);
}

// Prefixos de um programa (vertice + fragmento): cada bloco vale o maior
// prefixo lido por qualquer dos dois estagios (hoje cada estagio so declara
// o seu bloco; a uniao so protege contra um estagio que passe a ler o outro).
struct ProgramUsage {
    size_t Vertex = kAll;
    size_t Fragment = kAll;
};
[[nodiscard]] inline ProgramUsage ProgramUsedBytes(std::string_view vertex,
                                                   std::string_view fragment) {
    const auto unite = [](size_t a, size_t b) {
        return a == kAll || b == kAll ? kAll : std::max(a, b);
    };
    return {unite(VertexUsedBytes(vertex), VertexUsedBytes(fragment)),
            unite(FragmentUsedBytes(vertex), FragmentUsedBytes(fragment))};
}

// Bytes a copiar de um bloco de `blockSize` bytes: o prefixo usado em
// multiplos de vec4, nunca mais que o bloco; trim desligado: o bloco inteiro.
[[nodiscard]] constexpr size_t SendBytes(bool trim, size_t used,
                                         size_t blockSize) noexcept {
    if (!trim || used == kAll) return blockSize;
    const size_t rounded = (used + 15U) & ~static_cast<size_t>(15U);
    return std::min(rounded, blockSize);
}

// O que o anel ja tem de um bloco: os bytes copiados (prefixo), o tamanho
// do bloco ligado e onde (geracao do anel, deslocamento).
struct LastUpload {
    std::vector<uint8_t> Bytes;
    size_t BlockSize = 0U;
    uint64_t Generation = 0U;
    uint64_t Offset = 0U;

    void Forget() noexcept {
        Bytes.clear();
        BlockSize = 0U;
        Generation = 0U;
    }
};

// Dedup: o bloco anterior pode ser religado quando tem o mesmo tamanho, a
// mesma geracao do anel e os `send` primeiros bytes (tudo que o programa
// le) iguais. Com send == block.size() e a regra antiga (bloco igual).
[[nodiscard]] inline bool Reusable(const LastUpload& last, uint64_t generation,
                                   std::span<const uint8_t> block,
                                   size_t send) noexcept {
    return last.Generation != 0U && last.Generation == generation &&
           last.BlockSize == block.size() && last.Bytes.size() >= send &&
           send <= block.size() &&
           (send == 0U ||
            std::memcmp(last.Bytes.data(), block.data(), send) == 0);
}

// Decisao de um bloco num draw (o backend e o teste usam a mesma):
// Reuse religa last.Offset; senao copiar Send bytes a uma faixa nova de
// block.size() bytes. trim: config/gl_uniform_trim e o bloco vai ao anel;
// dedup: config/gl_lean e o bloco vai ao anel.
struct Plan {
    bool Reuse = false;
    size_t Send = 0U;
};
[[nodiscard]] inline Plan PlanUpload(bool trim, bool dedup,
                                     const LastUpload& last,
                                     uint64_t generation,
                                     std::span<const uint8_t> block,
                                     size_t used) noexcept {
    Plan plan;
    plan.Send = SendBytes(trim, used, block.size());
    plan.Reuse = dedup && Reusable(last, generation, block, plan.Send);
    return plan;
}
// Depois de copiar `send` bytes de `block` para `offset` (com dedup).
inline void Remember(LastUpload& last, std::span<const uint8_t> block,
                     size_t send, uint64_t generation, uint64_t offset) {
    last.Bytes.assign(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(send));
    last.BlockSize = block.size();
    last.Generation = generation;
    last.Offset = offset;
}

} // namespace Fast::PicaGlUniformTrim
