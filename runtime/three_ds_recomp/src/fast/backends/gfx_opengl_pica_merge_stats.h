#pragma once

// Medicao (sem mudar nada do que e desenhado) de quantos draws PICA de um
// quadro poderiam virar um so, a tecnica do aurora/Dusklight: draws
// consecutivos com estado GL identico (programa, alvo, texturas, uniforms,
// formato de vertice, blend/depth/stencil/cull/viewport/scissor, topologia)
// concatenam vertices/indices numa chamada. O backend calcula uma assinatura
// de 64 bits por draw (so com config/gpu_timing) e o MergeTracker compara
// com a do draw imediatamente anterior. Livre de GL e sem alocacao: testado
// offline (tests/gfx_opengl_pica_merge_stats_tests.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Fast::PicaGlMergeStats {

// FNV-1a de 64 bits sobre valores e bytes.
struct Hasher {
    uint64_t Value = 14695981039346656037ULL;

    void Bytes(const void* data, size_t size) noexcept {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            Value = (Value ^ bytes[index]) * 1099511628211ULL;
        }
    }
    void Add(uint64_t value) noexcept {
        for (unsigned shift = 0U; shift < 64U; shift += 8U) {
            Value = (Value ^ ((value >> shift) & 0xFFU)) * 1099511628211ULL;
        }
    }
    // O tamanho entra antes dos bytes: {a}{bc} difere de {ab}{c}.
    void Block(const uint8_t* data, size_t size) noexcept {
        Add(size);
        if (size != 0U) Bytes(data, size);
    }
};

// Sequencias de draws de assinatura igual. Uma sequencia de N draws junta
// N-1 draws (mergeable) e tem comprimento N (run).
struct MergeTracker {
    uint64_t Last = 0U;
    bool Have = false;
    uint32_t Run = 0U;
    // Do quadro corrente (zerados por TakeFrame).
    uint32_t Draws = 0U;
    uint32_t Mergeable = 0U;
    uint32_t RunMax = 0U;

    // Um draw emitido com esta assinatura; true quando igual ao anterior.
    bool Feed(uint64_t signature) noexcept {
        ++Draws;
        const bool same = Have && signature == Last;
        Run = same ? Run + 1U : 1U;
        if (same) ++Mergeable;
        if (Run > RunMax) RunMax = Run;
        Last = signature;
        Have = true;
        return same;
    }
    // Algo que nao e draw PICA (limpeza, transfer, fim de quadro) quebra a
    // sequencia.
    void Break() noexcept {
        Have = false;
        Run = 0U;
    }
    void TakeFrame(uint32_t* draws, uint32_t* mergeable,
                   uint32_t* runMax) noexcept {
        *draws = Draws;
        *mergeable = Mergeable;
        *runMax = RunMax;
        Draws = Mergeable = RunMax = 0U;
        Break();
    }
};

} // namespace Fast::PicaGlMergeStats
