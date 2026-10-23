#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include "native_presentation_thread_scope.h"
#include "oot3d_vsync_clock.h"

namespace Oot3dNativeGame {

struct NativeRealtimeRefreshPacerStats {
    uint64_t Waits = 0;
    uint64_t DeadlineMisses = 0;
    uint64_t CarriedDeadlineDebt = 0;
    uint64_t DeadlineResyncs = 0;
    uint64_t PresentationIntervals = 0;
    double SleepSeconds = 0.0;
    double SpinSeconds = 0.0;
    double TotalIntervalSeconds = 0.0;
    double MinimumIntervalSeconds = 0.0;
    double MaximumIntervalSeconds = 0.0;
    double MaximumLatenessSeconds = 0.0;
    double MaximumIntervalErrorSeconds = 0.0;
    double SquaredIntervalErrorSeconds = 0.0;
};

// Cumulative, readable from any thread (logs/perf.txt): the owner writes
// with a load and a store (no read-modify-write).
struct NativePacerTailCounters {
    uint64_t Waits = 0;
    uint64_t SpinNanoseconds = 0;       // busy-wait before the deadline
    uint64_t OversleepNanoseconds = 0;  // woke after the sleep's target
    uint64_t MaximumOversleepNanoseconds = 0;
    // config/pacer_vsync: waits whose deadline was snapped to the display's
    // vsync grid, and the largest snap (phase correction) of any of them.
    uint64_t VsyncAnchoredWaits = 0;
    uint64_t MaximumVsyncSnapNanoseconds = 0;
};

// Reads the display's vsync clock (steady_clock nanoseconds); false or an
// invalid snapshot leaves the CPU clock in charge.
using NativePacerVsyncReader = bool (*)(const void* user,
                                        VsyncClockSnapshot* snapshot);

enum class NativePacerDeadlineAction : uint8_t {
    Wait,
    CarryDebt,
    Resync,
};

// A short overrun is retained so the next inexpensive interpolation sample
// can absorb it. Only a sustained stall rebases the absolute clock.
NativePacerDeadlineAction ResolveNativePacerDeadlineAction(
    std::chrono::nanoseconds lateness,
    std::chrono::nanoseconds presentationPeriod,
    uint32_t maximumRecoverablePeriods = 2U) noexcept;

// One absolute presentation clock owns software pacing. Coarse sleep performs
// most of the wait and a short high-precision tail avoids Windows scheduler
// quantization. Short deadline debt is preserved across the fixed x2/x3
// cadence; long stalls rebase instead of issuing unbounded catch-up frames.
class NativeRealtimeRefreshPacer final {
  public:
    NativeRealtimeRefreshPacer(bool pacingAllowed, uint32_t targetRateHz);
    ~NativeRealtimeRefreshPacer();

    NativeRealtimeRefreshPacer(const NativeRealtimeRefreshPacer&) = delete;
    NativeRealtimeRefreshPacer& operator=(
        const NativeRealtimeRefreshPacer&) = delete;

    void Configure(bool enabled, uint32_t targetRateHz) noexcept;
    void WaitForNextRefresh();
    // `periods` presentation periods after the previous deadline (2: a step
    // of the locked 30 of config/fps60_mode). resyncWhenLate: a late frame
    // rebases the clock instead of carrying the debt, so the next picture
    // keeps the full interval (no short hold after a long one).
    void WaitForNextRefresh(uint32_t periods, bool resyncWhenLate);
    void ObservePresentation(
        std::chrono::steady_clock::time_point presentationTime) noexcept;
    void ResetDeadline() noexcept;
    // Busy-wait tail before each deadline (at most period / 8). Default
    // 500 us (Windows timer quantization); the Switch passes its layout's
    // value (0: svcSleepThread wakes on time and the spin burns the render
    // thread's core).
    void SetMaximumSpin(std::chrono::nanoseconds spin) noexcept;
    // config/pacer_vsync: anchor the deadlines to the real vsync (measured
    // phase and period) instead of 1/TargetRateHz of the CPU clock. Each
    // deadline snaps to the nearest lastVsync + k*period + offset; the
    // pacer's period becomes the display's (no drift). nullptr: CPU clock.
    void SetVsyncAnchor(NativePacerVsyncReader reader, const void* user,
                        std::chrono::nanoseconds offset) noexcept;
    [[nodiscard]] bool VsyncAnchorConfigured() const noexcept {
        return mVsyncReader != nullptr;
    }
    [[nodiscard]] NativePacerTailCounters TailCounters() const noexcept;
    // Largest oversleep since the previous call (perf windows).
    uint64_t TakeMaximumOversleepNanoseconds() noexcept;

    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] bool HighResolutionWaitAvailable() const noexcept;
    [[nodiscard]] bool MultimediaSchedulingActive() const noexcept { return mThreadScheduling.Active(); }
    [[nodiscard]] uint32_t TargetRateHz() const noexcept;
    [[nodiscard]] double ElapsedSeconds() const noexcept;
    [[nodiscard]] double MeanIntervalSeconds() const noexcept;
    [[nodiscard]] double RmsIntervalErrorSeconds() const noexcept;
    [[nodiscard]] const NativeRealtimeRefreshPacerStats& Stats() const noexcept;

  private:
    std::chrono::nanoseconds NextPeriod() noexcept;
    void SleepUntil(
        std::chrono::steady_clock::time_point deadline) noexcept;

    bool mPacingAllowed = false;
    NativePresentationThreadScope mThreadScheduling;
    bool mEnabled = false;
    uint32_t mTargetRateHz = 0;
    bool mStarted = false;
    bool mHasPresentationTime = false;
    uint64_t mNanosecondRemainder = 0;
    std::chrono::steady_clock::time_point mStart;
    std::chrono::steady_clock::time_point mNextDeadline;
    std::chrono::steady_clock::time_point mLastPresentationTime;
    NativeRealtimeRefreshPacerStats mStats;
    std::chrono::nanoseconds mMaximumSpin{std::chrono::microseconds(500)};
    std::atomic<uint64_t> mTailWaits{0};
    std::atomic<uint64_t> mTailSpinNanoseconds{0};
    std::atomic<uint64_t> mTailOversleepNanoseconds{0};
    std::atomic<uint64_t> mTailMaximumOversleepNanoseconds{0};
    NativePacerVsyncReader mVsyncReader = nullptr;
    const void* mVsyncUser = nullptr;
    std::chrono::nanoseconds mVsyncOffset{0};
    std::atomic<uint64_t> mVsyncAnchoredWaits{0};
    std::atomic<uint64_t> mVsyncMaximumSnapNanoseconds{0};
#ifdef _WIN32
    void* mHighResolutionTimer = nullptr;
#endif
};

} // namespace Oot3dNativeGame
