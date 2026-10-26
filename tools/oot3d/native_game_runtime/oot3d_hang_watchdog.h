#pragma once

// Watchdog de travamento (diagnóstico; oot3d_hang_watchdog.cpp).
//
// Cada thread que importa (jogo, render, planos PICA, áudio) tem um "slot"
// com a etapa em que está e um contador de progresso. Marcar uma etapa é um
// store relaxed no slot da própria thread (sem read-modify-write, sem
// barreira); o progresso é um load + store da dona (quadro do jogo, comando
// da render thread). Nada disso fica por draw.
//
// No Switch uma thread de serviço (0x3B) acorda a cada ~1 s; se o jogo ou a
// render thread ficar mais de 3 s sem progresso (com o app em foco), grava
// UMA vez sdmc:/switch/oot3dre/logs/hang.txt pela API do FS da libnx (sem
// malloc: o travamento pode ser justamente o lock do malloc), com flush: a
// etapa e o progresso de cada thread, PC/LR/FP e uma cadeia de retorno de
// cada thread registrada (pausada por um instante com svcSetThreadActivity,
// se o NPDM permitir), a última chamada de serviço e os contadores de sync.
// Quando as threads voltam a andar, acrescenta "recuperou após N ms" e o
// watchdog se rearma. config/no_hang_watchdog desliga.
//
// Fora do Switch só os marcadores existem (iguais, baratos) e a política de
// detecção, testável no host (oot3d_hang_watchdog_tests).

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame::HangWatch {

enum class Thread : uint8_t { Game, Render, PicaPlan, HostAudio, SdlAudio, Count };

enum class Stage : uint8_t {
    Unknown,
    // thread do jogo
    FrameBegin,
    GuestRefresh,
    PresentVisual,
    UiSubmit,
    PresentSubmit,
    FrameEnd,
    PerfWindow,      // montando/gravando a linha do perf.txt
    PerfClocks,      // IPC do sys-clk
    PerfHeapWalk,    // varredura do heap (config/heap_check_windows)
    PerfFileClose,   // fechando o perf.txt (escrita no SD)
    // esperas no NativeRenderCommandStream (thread que submete)
    WaitFramesInFlight,
    WaitDrain,
    SyncCommand,     // comando síncrono (Run) na thread que submete
    // render thread
    RenderIdle,
    RenderCommand,
    RenderPacer,
    RenderSwap,
    RenderResolvePlans,
    Count
};

inline constexpr const char* kStageNames[] = {
    "unknown",          "frame_begin",       "guest_refresh",  "present_visual",
    "ui_submit",        "present_submit",    "frame_end",      "perf_window",
    "perf_clocks",      "perf_heap_walk",    "perf_file_close", "wait_frames_in_flight",
    "wait_drain",       "sync_command",      "render_idle",    "render_command",
    "render_pacer",     "render_swap",       "render_resolve_plans"};
static_assert(sizeof(kStageNames) / sizeof(kStageNames[0]) ==
              static_cast<size_t>(Stage::Count));

inline constexpr const char* kThreadNames[] = {"game", "render", "pica_plan",
                                               "host_audio", "sdl_audio"};
static_assert(sizeof(kThreadNames) / sizeof(kThreadNames[0]) ==
              static_cast<size_t>(Thread::Count));

constexpr const char* StageName(uint8_t stage) noexcept {
    return stage < static_cast<uint8_t>(Stage::Count) ? kStageNames[stage] : "?";
}

struct Slot {
    std::atomic<uint8_t> CurrentStage{0};
    std::atomic<uint8_t> Bound{0};
    std::atomic<uint32_t> NativeHandle{0};  // handle da thread no Switch
    std::atomic<uint64_t> Progress{0};
};

inline Slot gSlots[static_cast<size_t>(Thread::Count)];
inline thread_local Slot* tSlot = nullptr;

// A thread que chama passa a ser `thread` (oot3d_switch_threads.cpp chama no
// PlaceSwitchThread de cada papel).
inline void Bind(Thread thread, uint32_t nativeHandle) noexcept {
    Slot& slot = gSlots[static_cast<size_t>(thread)];
    slot.NativeHandle.store(nativeHandle, std::memory_order_relaxed);
    slot.Bound.store(1U, std::memory_order_release);
    tSlot = &slot;
}

inline Stage CurrentStage() noexcept {
    const Slot* slot = tSlot;
    return slot != nullptr ? static_cast<Stage>(slot->CurrentStage.load(std::memory_order_relaxed))
                           : Stage::Unknown;
}

// Um store relaxed; nada numa thread sem slot.
inline void Mark(Stage stage) noexcept {
    if (Slot* slot = tSlot) {
        slot->CurrentStage.store(static_cast<uint8_t>(stage), std::memory_order_relaxed);
    }
}

// Progresso da thread dona (load + store, sem RMW).
inline void Advance() noexcept {
    if (Slot* slot = tSlot) {
        slot->Progress.store(slot->Progress.load(std::memory_order_relaxed) + 1U,
                             std::memory_order_relaxed);
    }
}

// Etapa durante um escopo; devolve a anterior na saída.
class StageScope {
  public:
    explicit StageScope(Stage stage) noexcept : mSlot(tSlot) {
        if (mSlot != nullptr) {
            mPrevious = mSlot->CurrentStage.load(std::memory_order_relaxed);
            mSlot->CurrentStage.store(static_cast<uint8_t>(stage), std::memory_order_relaxed);
        }
    }
    ~StageScope() {
        if (mSlot != nullptr) {
            mSlot->CurrentStage.store(mPrevious, std::memory_order_relaxed);
        }
    }
    StageScope(const StageScope&) = delete;
    StageScope& operator=(const StageScope&) = delete;

  private:
    Slot* mSlot;
    uint8_t mPrevious = 0;
};

// Relógio do watchdog: ticks do contador do sistema no Switch (19,2 MHz),
// nanossegundos no host.
uint64_t NowTicks() noexcept;
uint64_t TicksToMicroseconds(uint64_t ticks) noexcept;

// Última chamada de serviço (IPC) feita pelo app: nome (literal estático),
// início e fim (0 = em andamento).
struct ServiceCallRecord {
    std::atomic<const char*> Name{nullptr};
    std::atomic<uint64_t> StartTicks{0};
    std::atomic<uint64_t> EndTicks{0};
    std::atomic<uint64_t> Calls{0};
};
inline ServiceCallRecord gLastServiceCall;

class ServiceCallScope {
  public:
    explicit ServiceCallScope(const char* name) noexcept {
        auto& record = gLastServiceCall;
        record.EndTicks.store(0U, std::memory_order_relaxed);
        record.Name.store(name, std::memory_order_relaxed);
        record.StartTicks.store(NowTicks(), std::memory_order_relaxed);
        record.Calls.store(record.Calls.load(std::memory_order_relaxed) + 1U,
                           std::memory_order_relaxed);
    }
    ~ServiceCallScope() {
        gLastServiceCall.EndTicks.store(NowTicks(), std::memory_order_relaxed);
    }
    ServiceCallScope(const ServiceCallScope&) = delete;
    ServiceCallScope& operator=(const ServiceCallScope&) = delete;
};

// ---- texto sem malloc ---------------------------------------------------------

// Acumula texto num buffer fixo; nunca aloca (o relatório é montado com o
// heap possivelmente travado). Trunca em silêncio quando enche.
class Text {
  public:
    Text(char* buffer, size_t capacity) noexcept : mBuffer(buffer), mCapacity(capacity) {
        if (mCapacity != 0U) mBuffer[0] = '\0';
    }
    Text& Str(const char* text) noexcept;
    Text& Dec(uint64_t value) noexcept;
    Text& SignedDec(int64_t value) noexcept;
    Text& Hex(uint64_t value) noexcept;  // 0x...
    Text& Char(char value) noexcept;
    [[nodiscard]] const char* Data() const noexcept { return mBuffer; }
    [[nodiscard]] size_t Size() const noexcept { return mSize; }
    void Clear() noexcept {
        mSize = 0U;
        if (mCapacity != 0U) mBuffer[0] = '\0';
    }

  private:
    char* mBuffer;
    size_t mCapacity;
    size_t mSize = 0U;
};

// ---- política de detecção (pura, testada no host) ---------------------------

struct StallPolicy {
    uint64_t StallMilliseconds = 3000U;
    // Um intervalo do watchdog bem maior que o seu período = o processo ficou
    // suspenso (sleep do console): recomeça a contagem sem acusar.
    uint64_t SuspendGapMilliseconds = 2500U;
};

class StallTracker {
  public:
    struct Observation {
        uint64_t NowMilliseconds = 0;
        // Fora de foco (HOME, overlay) ou desligado: nada conta.
        bool Active = true;
        // Por thread: vigiada (registrada e relevante) e progresso atual.
        bool Watched[static_cast<size_t>(Thread::Count)] = {};
        uint64_t Progress[static_cast<size_t>(Thread::Count)] = {};
    };
    enum class Verdict : uint8_t { None, Hang, Recovered };
    struct Result {
        Verdict Kind = Verdict::None;
        // Hang: maior tempo parado entre as threads vigiadas; Recovered:
        // quanto durou o travamento relatado.
        uint64_t Milliseconds = 0;
        // Hang: bit por thread parada há >= StallMilliseconds.
        uint32_t StalledMask = 0;
        // Tempo parado de cada thread no momento da observação.
        uint64_t StalledFor[static_cast<size_t>(Thread::Count)] = {};
    };

    explicit StallTracker(StallPolicy policy = {}) noexcept : mPolicy(policy) {}
    Result Observe(const Observation& observation) noexcept;
    [[nodiscard]] bool Reporting() const noexcept { return mReported; }

  private:
    StallPolicy mPolicy;
    bool mStarted = false;
    bool mReported = false;
    uint32_t mReportedMask = 0;
    uint64_t mLastObservation = 0;
    uint64_t mStallBegan = 0;
    uint64_t mLastProgress[static_cast<size_t>(Thread::Count)] = {};
    uint64_t mLastChange[static_cast<size_t>(Thread::Count)] = {};
    uint64_t mProgressAtReport[static_cast<size_t>(Thread::Count)] = {};
};

// ---- ciclo de vida (Switch; no host não faz nada) ---------------------------

// Informação extra no relatório (contadores de sync da sessão). Chamada pela
// thread do watchdog: não pode bloquear (só try_lock) nem alocar.
using ExtraReporter = void (*)(void* user, Text& text);

// Liga o watchdog (uma thread) se config/no_hang_watchdog não existir.
void Start(ExtraReporter extra, void* user) noexcept;
// Para e junta a thread (antes de a sessão destruir o que `extra` lê).
void Stop() noexcept;
[[nodiscard]] bool Running() noexcept;
// Relatórios gravados nesta execução (travamentos + eventos).
[[nodiscard]] uint32_t ReportsWritten() noexcept;
// Grava já um relatório com `title`, `details` e o estado de todas as
// threads (heap corrompido achado pela varredura, por exemplo). Pode ser
// chamado de qualquer thread; a que chama não é pausada.
void ReportEvent(const char* title, const char* details) noexcept;

// Registradores da thread que falhou (handler de exceção da libnx).
struct FaultFrame {
    uint64_t Pc = 0;
    uint64_t Lr = 0;
    uint64_t Sp = 0;
    uint64_t Fp = 0;
    uint64_t X0 = 0;
    uint64_t X1 = 0;
    uint64_t X2 = 0;
    uint64_t X19 = 0;
};
// Relatório de exceção: `details`, PC/LR e a cadeia de frames da thread que
// falhou (chamada nela mesma, na pilha de exceção) e o estado das outras.
// Não aloca; funciona com o watchdog desligado (config/no_hang_watchdog).
void ReportFault(const char* title, const char* details, const FaultFrame& frame) noexcept;
// Endereço de código como nos relatórios: 0x... e o offset no NRO (+0x...).
void AppendCodeAddress(Text& text, uint64_t address) noexcept;

} // namespace Oot3dNativeGame::HangWatch
