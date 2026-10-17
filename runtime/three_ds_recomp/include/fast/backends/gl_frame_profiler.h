#pragma once

// Perfil por etapa da render thread do backend OpenGL (Switch: Mesa/nouveau).
//
// Ligado só pelo arquivo sdmc:/switch/oot3dre/config/gpu_timing (desktop:
// TRIAEVUM_GL_GPU_TIMING). O conteúdo opcional é N, a amostragem da GPU
// (1 quadro a cada N, padrão 30). Sem o arquivo, cada ponto de marcação é uma
// carga atômica e um desvio; os contadores de chamadas GL nem existem
// (são ganchos instalados nos ponteiros do glad só quando ligado).
//
// Mede, por quadro de host (passo, interpolado ou repetido):
//  - GPU: glQueryCounter(GL_TIMESTAMP) a cada troca de etapa num quadro
//    amostrado; resultado lido quadros depois com GL_QUERY_RESULT_AVAILABLE
//    (nunca bloqueia; sem resultado a tempo, a amostra é descartada);
//  - CPU da render thread por etapa (relógio monotônico a cada troca);
//  - contadores de chamadas GL (draws, clears, FBOs, uploads, sincronizações).
// O relatório (TakeReport) vira uma linha "glperf" no logs/perf.txt.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Fast::GlPerf {

enum class Stage : uint8_t {
    Begin,          // início do quadro até a primeira etapa
    Clear,          // limpezas (janela, alvos PICA, fills)
    PicaTop,        // draws PICA no alvo da tela de cima (amostra exibida)
    PicaBottom,     // draws PICA no alvo da tela de baixo
    PicaOffscreen,  // draws PICA em outros alvos (sombras, render-to-texture)
    PicaCanonical,  // draws da execução canônica (não exibida) de um quadro novo
    Transfer,       // display transfers (blit alvo -> imagem de display)
    PostAa,         // FXAA/SMAA numa imagem intermediária
    PostEasu,       // FSR EASU numa imagem intermediária
    Scanout,        // último passe na janela (RCAS, EASU/FXAA direto ou cópia)
    Ui,             // UI integrada (HUD/menus TopScreen)
    Present,        // EndFrame até depois do swap
    // Só CPU (não mudam a etapa da GPU):
    Other,          // comandos da render thread fora das etapas acima
    Pacing,         // espera do pacer antes do swap
    Idle,           // render thread sem comando
    Count
};
inline constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);
inline constexpr size_t kGpuStageCount = static_cast<size_t>(Stage::Other);

namespace detail {
// Header-only switch: code outside the renderer library (render command
// stream, UI renderer, their standalone tests) only reads these, so it needs
// no link dependency; Init() fills the table when the profiler is on.
struct Hooks {
    void (*Mark)(Stage) = nullptr;
    void (*MarkPicaDraw)(uint32_t, uint32_t) = nullptr;
    void (*FrameBegin)() = nullptr;
    void (*FrameEnd)() = nullptr;
    void (*SampleBegin)(bool, bool) = nullptr;
    void (*SampleEnd)() = nullptr;
    Stage (*Current)() = nullptr;
};
inline Hooks gHooks{};
inline std::atomic<bool> gEnabled{false};
} // namespace detail

inline bool Enabled() {
    return detail::gEnabled.load(std::memory_order_acquire);
}

// Lê a configuração uma vez (contexto GL corrente). Sem o arquivo: nada.
void Init();

inline void Mark(Stage stage) {
    if (Enabled()) detail::gHooks.Mark(stage);
}
inline void MarkPicaDraw(uint32_t framebufferWidth, uint32_t framebufferHeight) {
    if (Enabled()) detail::gHooks.MarkPicaDraw(framebufferWidth, framebufferHeight);
}
inline void FrameBegin() {
    if (Enabled()) detail::gHooks.FrameBegin();
}
inline void FrameEnd() {
    if (Enabled()) detail::gHooks.FrameEnd();
}

// Etapa aninhada: volta à etapa anterior no fim do escopo.
class Scope {
public:
    explicit Scope(Stage stage) : mArmed(Enabled()) {
        if (mArmed) {
            mPrevious = detail::gHooks.Current();
            detail::gHooks.Mark(stage);
        }
    }
    ~Scope() {
        if (mArmed) detail::gHooks.Mark(mPrevious);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    bool mArmed;
    Stage mPrevious = Stage::Other;
};

// Um comando da render thread: Other durante, Idle depois.
class CommandScope {
public:
    CommandScope() : mArmed(Enabled()) {
        if (mArmed) detail::gHooks.Mark(Stage::Other);
    }
    ~CommandScope() {
        if (mArmed) detail::gHooks.Mark(Stage::Idle);
    }
    CommandScope(const CommandScope&) = delete;
    CommandScope& operator=(const CommandScope&) = delete;

private:
    bool mArmed;
};

// Execução de uma amostra visual. authoritative = quadro novo do jogo
// (execução nativa/atual); present = amostra exibida. Os draws de uma
// execução não exibida contam como PicaCanonical; o quadro de host que
// executa um quadro novo é "step", o que só exibe uma amostra repetida
// (interpolada entre dois quadros já vistos) é "interp", o resto é "hold".
class SampleScope {
public:
    SampleScope(bool authoritative, bool present) : mArmed(Enabled()) {
        if (mArmed) detail::gHooks.SampleBegin(authoritative, present);
    }
    ~SampleScope() {
        if (mArmed) detail::gHooks.SampleEnd();
    }
    SampleScope(const SampleScope&) = delete;
    SampleScope& operator=(const SampleScope&) = delete;

private:
    bool mArmed;
};

// Linha "glperf ..." (sem '\n') com médias por quadro desde o último
// relatório; vazia quando desligado. Chamável de outra thread.
std::string TakeReport(uint64_t hostFrames);

} // namespace Fast::GlPerf
