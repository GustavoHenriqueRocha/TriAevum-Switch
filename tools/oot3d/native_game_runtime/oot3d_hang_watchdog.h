#pragma once

// Hang watchdog (diagnostics; oot3d_hang_watchdog.cpp).
//
// Each thread that matters (game, render, PICA plans, audio) has a "slot"
// with the stage it is in and a progress counter. Marking a stage is a
// relaxed store into the thread's own slot (no read-modify-write, no
// barrier); progress is a load + store by the owner (game frame, render
// thread command). None of this is per draw.
//
// On Switch a service thread (0x3B) wakes every ~1 s; if the game or the
// render thread goes more than 3 s without progress (with the app in focus), it writes
// sdmc:/switch/oot3dre/logs/hang.txt ONCE through libnx's FS API (no
// malloc: the hang may be the malloc lock itself), with flush: the
// stage and progress of each thread, PC/LR/FP and a return chain for
// each registered thread (paused for an instant with svcSetThreadActivity,
// if the NPDM allows it), the last service call and the sync counters.
// When the threads move again, it appends "recuperou após N ms" and the
// watchdog rearms. config/no_hang_watchdog disables it.
//
// Outside Switch only the markers exist (identical, cheap) plus the detection
// policy, testable on the host (oot3d_hang_watchdog_tests).

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame::HangWatch {

enum class Thread : uint8_t { Game, Render, PicaPlan, HostAudio, SdlAudio, Count };

enum class Stage : uint8_t {
    Unknown,
    // game thread
    FrameBegin,
    GuestRefresh,
    PresentVisual,
    UiSubmit,
    PresentSubmit,
    FrameEnd,
    PerfWindow,      // building/writing the perf.txt line
    PerfClocks,      // sys-clk IPC
    PerfHeapWalk,    // heap walk (config/heap_check_windows)
    PerfFileClose,   // closing perf.txt (SD write)
    // waits in NativeRenderCommandStream (submitting thread)
    WaitFramesInFlight,
    WaitDrain,
    SyncCommand,     // synchronous command (Run) on the submitting thread
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
    std::atomic<uint32_t> NativeHandle{0};  // thread handle on Switch
    std::atomic<uint64_t> Progress{0};
};

inline Slot gSlots[static_cast<size_t>(Thread::Count)];
inline thread_local Slot* tSlot = nullptr;

// The calling thread becomes `thread` (oot3d_switch_threads.cpp calls this in
// PlaceSwitchThread for each role).
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

// One relaxed store; nothing on a thread without a slot.
inline void Mark(Stage stage) noexcept {
    if (Slot* slot = tSlot) {
        slot->CurrentStage.store(static_cast<uint8_t>(stage), std::memory_order_relaxed);
    }
}

// Progress of the owning thread (load + store, no RMW).
inline void Advance() noexcept {
    if (Slot* slot = tSlot) {
        slot->Progress.store(slot->Progress.load(std::memory_order_relaxed) + 1U,
                             std::memory_order_relaxed);
    }
}

// Stage during a scope; restores the previous one on exit.
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

// Watchdog clock: system counter ticks on Switch (19.2 MHz),
// nanoseconds on the host.
uint64_t NowTicks() noexcept;
uint64_t TicksToMicroseconds(uint64_t ticks) noexcept;

// Last service call (IPC) made by the app: name (static literal),
// start and end (0 = in progress).
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

// ---- malloc-free text ---------------------------------------------------------

// Accumulates text in a fixed buffer; never allocates (the report is built with
// the heap possibly locked). Silently truncates when full.
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

// ---- detection policy (pure, tested on the host) ----------------------------

struct StallPolicy {
    uint64_t StallMilliseconds = 3000U;
    // A watchdog interval much longer than its period = the process was
    // suspended (console sleep): restarts the count without flagging.
    uint64_t SuspendGapMilliseconds = 2500U;
};

class StallTracker {
  public:
    struct Observation {
        uint64_t NowMilliseconds = 0;
        // Out of focus (HOME, overlay) or disabled: nothing counts.
        bool Active = true;
        // Per thread: watched (registered and relevant) and current progress.
        bool Watched[static_cast<size_t>(Thread::Count)] = {};
        uint64_t Progress[static_cast<size_t>(Thread::Count)] = {};
    };
    enum class Verdict : uint8_t { None, Hang, Recovered };
    struct Result {
        Verdict Kind = Verdict::None;
        // Hang: longest stall among the watched threads; Recovered:
        // how long the reported hang lasted.
        uint64_t Milliseconds = 0;
        // Hang: bit per thread stalled for >= StallMilliseconds.
        uint32_t StalledMask = 0;
        // Stall time of each thread at observation time.
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

// ---- lifecycle (Switch; no-op on the host) ----------------------------------

// Extra report info (session sync counters). Called by the
// watchdog thread: must not block (try_lock only) or allocate.
using ExtraReporter = void (*)(void* user, Text& text);

// Starts the watchdog (one thread) if config/no_hang_watchdog does not exist.
void Start(ExtraReporter extra, void* user) noexcept;
// Stops and joins the thread (before the session destroys what `extra` reads).
void Stop() noexcept;
[[nodiscard]] bool Running() noexcept;
// Reports written in this run (hangs + events).
[[nodiscard]] uint32_t ReportsWritten() noexcept;
// Writes a report right now with `title`, `details` and the state of all
// threads (corrupted heap found by the walk, for example). May be
// called from any thread; the caller is not paused.
void ReportEvent(const char* title, const char* details) noexcept;

// Registers of the faulting thread (libnx exception handler).
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
// Exception report: `details`, PC/LR and the frame chain of the faulting
// thread (called on it, on the exception stack) and the state of the others.
// Does not allocate; works with the watchdog disabled (config/no_hang_watchdog).
void ReportFault(const char* title, const char* details, const FaultFrame& frame) noexcept;
// Code address as in the reports: 0x... and the offset in the NRO (+0x...).
void AppendCodeAddress(Text& text, uint64_t address) noexcept;

} // namespace Oot3dNativeGame::HangWatch
