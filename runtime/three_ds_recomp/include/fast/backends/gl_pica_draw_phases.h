#pragma once

// Fases de CPU de um draw PICA no backend GL (glperf step_draw_<fase>_ms).
// Livre de GL e sem alocacao; so a render thread escreve. O acumulador e
// "sequencial": Lap(fase) cobra o tempo desde a marca anterior a fase dada,
// entao cada instante do SubmitPicaDraw cai em exatamente uma fase (uma
// leitura de relogio por marca). Relogio: contador de sistema do ARM no
// Switch (19,2 MHz), steady_clock no host. Ativo so com config/gpu_timing;
// desligado, quem chama nem le o relogio (veja GlPerf::Enabled()).
//
// Alem das fases por draw ha "secoes" (tempo de parede de trechos da render
// thread fora do draw, para decompor step_cpu_other) e contadores por quadro.

#include <array>
#include <chrono>
#include <cstdint>

namespace Fast::PicaDrawPhases {

enum class Phase : uint8_t {
    Validate,   // checagens, composicao, inicio de quadro, marca de etapa
    Program,    // GetPicaGlProgram (chave + busca no mapa; compilacao se nova)
    Target,     // alvo, fills pendentes, sombra
    State,      // binds (FBO/VAO/programa/texturas/imagem) e estado de raster
    Uniforms,   // memcmp + copia para o anel + bind range
    Vertices,   // validacao, registro de geometria persistente, anel, atribs
    TexHash,    // hash de conteudo de textura (so quando o quadro nao o trouxe)
    TexLookup,  // chave + busca no mapa de texturas (inclui LUT de iluminacao)
    TexUpload,  // criacao de textura nova (decode + glTexImage)
    Indices,    // copia dos indices para o anel
    PerfSig,    // assinatura de mesclagem (SO MEDICAO: custo da propria medicao)
    Draw,       // glDrawElements/Arrays (incl. passes de canto)
    Rest,       // o que sobrar ate o fim do SubmitPicaDraw
    Count
};
inline constexpr size_t kPhaseCount = static_cast<size_t>(Phase::Count);

inline constexpr std::array<const char*, kPhaseCount> kPhaseNames{
    "validate", "program", "target", "state", "uniforms", "vertices",
    "tex_hash", "tex_lookup", "tex_upload", "indices", "perf_sig", "draw",
    "rest"};

// Trechos da render thread fora do draw (tempo de parede, podem aninhar).
enum class Section : uint8_t {
    ResolvePlans,  // planos diferidos dos quadros fechados
    PinCopy,       // PinGpuCopyResources
    SceneEnv,      // ambiente de cena / publicacao de luz
    Execute,       // picaPresentationScheduler.Execute (inclui os draws)
    VisualSample,  // ExecuteVisualSample inteiro (inclui PinCopy e Execute)
    Count
};
inline constexpr size_t kSectionCount = static_cast<size_t>(Section::Count);

inline constexpr std::array<const char*, kSectionCount> kSectionNames{
    "resolve_plans", "pin_copy", "scene_env", "execute", "visual_sample"};

enum class Count : uint8_t {
    Draws,            // draws medidos
    TexLookups,       // buscas de textura
    TexHashes,        // hashes de conteudo calculados nesta render thread
    TexHashBytes,     // bytes hasheados
    TexUploads,       // texturas criadas (decode + upload)
    TexUploadBytes,   // bytes de GPU das texturas criadas
    UniformSent,      // blocos de uniform enviados
    UniformSentBytes, // bytes de uniform enviados
    UniformSkipped,   // blocos iguais ao anterior (nao enviados)
    UniformSkippedBytes,
    VertexBytes,      // bytes de vertice copiados (anel/buffer proprio)
    IndexBytes,       // bytes de indice copiados
    Count
};
inline constexpr size_t kCountCount = static_cast<size_t>(Count::Count);

inline constexpr std::array<const char*, kCountCount> kCountNames{
    "draws", "tex_lookups", "tex_hashes", "tex_hash_bytes", "tex_uploads",
    "tex_upload_bytes", "uniform_sent", "uniform_sent_bytes",
    "uniform_skipped", "uniform_skipped_bytes", "vertex_bytes", "index_bytes"};

// Relogio. Ticks() e barato; TicksToNs() converte uma soma.
inline uint64_t Ticks() noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    uint64_t value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}
inline uint64_t TicksToNs(uint64_t ticks) noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    constexpr uint64_t kFrequency = 19200000ULL;  // CNTFRQ do Switch
    return ticks / kFrequency * 1000000000ULL +
           ticks % kFrequency * 1000000000ULL / kFrequency;
#else
    return ticks;
#endif
}

struct FrameTotals {
    std::array<uint64_t, kPhaseCount> PhaseTicks{};
    std::array<uint64_t, kSectionCount> SectionTicks{};
    std::array<uint64_t, kCountCount> Counts{};
    uint64_t SubmitTicks = 0U;  // Begin -> End de todos os draws
};

class Tracker {
public:
    // Inicio de um draw: tempo desde a ultima marca nao e cobrado.
    void Begin(uint64_t now) noexcept {
        mStart = mLast = now;
        mOpen = true;
    }
    // Cobra o tempo desde a marca anterior a `phase`. Fora de um draw
    // (Begin nao chamado) nao faz nada.
    void Lap(Phase phase, uint64_t now) noexcept {
        if (!mOpen) return;
        if (now > mLast) {
            mFrame.PhaseTicks[static_cast<size_t>(phase)] += now - mLast;
        }
        mLast = now;
    }
    // Fim do draw: o resto vai para Rest e o total do draw e somado.
    void End(uint64_t now) noexcept {
        if (!mOpen) return;
        Lap(Phase::Rest, now);
        if (now > mStart) mFrame.SubmitTicks += now - mStart;
        ++mFrame.Counts[static_cast<size_t>(Count::Draws)];
        mOpen = false;
    }
    bool Open() const noexcept { return mOpen; }

    void Add(Count what, uint64_t value = 1U) noexcept {
        mFrame.Counts[static_cast<size_t>(what)] += value;
    }
    void AddSection(Section section, uint64_t ticks) noexcept {
        mFrame.SectionTicks[static_cast<size_t>(section)] += ticks;
    }
    // Totais do quadro; zera para o proximo.
    FrameTotals TakeFrame() noexcept {
        const FrameTotals result = mFrame;
        mFrame = {};
        mOpen = false;
        return result;
    }

private:
    FrameTotals mFrame{};
    uint64_t mStart = 0U;
    uint64_t mLast = 0U;
    bool mOpen = false;
};

// Soma em janela (varios quadros) -> ns por fase; usado pelo relatorio.
struct WindowTotals {
    std::array<uint64_t, kPhaseCount> PhaseNs{};
    std::array<uint64_t, kSectionCount> SectionNs{};
    std::array<uint64_t, kCountCount> Counts{};
    uint64_t SubmitNs = 0U;

    void Accumulate(const FrameTotals& frame) noexcept {
        for (size_t i = 0; i < kPhaseCount; ++i) {
            PhaseNs[i] += TicksToNs(frame.PhaseTicks[i]);
        }
        for (size_t i = 0; i < kSectionCount; ++i) {
            SectionNs[i] += TicksToNs(frame.SectionTicks[i]);
        }
        for (size_t i = 0; i < kCountCount; ++i) Counts[i] += frame.Counts[i];
        SubmitNs += TicksToNs(frame.SubmitTicks);
    }
};

// Unica instancia (render thread).
inline Tracker gTracker;

} // namespace Fast::PicaDrawPhases
