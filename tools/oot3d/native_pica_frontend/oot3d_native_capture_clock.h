#pragma once

// Cheap timestamps for the per-draw capture sub-stages (logs/perf.txt
// "capture:" group). Several are taken per draw on the game thread, so the
// Switch reads the system counter directly (cntpct_el0, 19.2 MHz, what libnx's
// armGetSystemTick reads) instead of going through clock_gettime; sums over
// thousands of draws average the 52 ns granularity out. Elsewhere: the
// steady clock in nanoseconds.

#include <chrono>
#include <cstdint>

namespace Oot3dNativeGame {

[[nodiscard]] inline uint64_t CaptureTicks() noexcept {
#if defined(__SWITCH__)
    uint64_t ticks;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

[[nodiscard]] inline double CaptureTicksToNanoseconds(uint64_t ticks) noexcept {
#if defined(__SWITCH__)
    return static_cast<double>(ticks) * (625.0 / 12.0);  // 1e9 / 19.2e6
#else
    return static_cast<double>(ticks);
#endif
}

} // namespace Oot3dNativeGame
