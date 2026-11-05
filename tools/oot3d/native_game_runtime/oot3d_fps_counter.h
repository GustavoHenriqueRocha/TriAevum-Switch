#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "oot3d_switch_clocks.h"

namespace Oot3dNativeGame {

// Presented-frame rate. Counts presented frames, including the fps60
// interpolated ones, and closes an average every `windowSeconds`; the
// window's worst interval shows hitches the average hides.
class FpsCounter {
public:
  explicit FpsCounter(double windowSeconds = 0.5) : mWindowSeconds(windowSeconds) {}

  // A frame presented at `nowSeconds` (monotonic clock).
  // Returns true when a window closed and the values changed.
  bool Observe(double nowSeconds) {
    if (mLastSeconds < 0.0) {
      mLastSeconds = mWindowStartSeconds = nowSeconds;
      return false;
    }
    mWorstMs = std::max(mWorstMs, (nowSeconds - mLastSeconds) * 1000.0);
    mLastSeconds = nowSeconds;
    ++mFrames;
    const double elapsed = nowSeconds - mWindowStartSeconds;
    if (elapsed < mWindowSeconds) return false;
    mFps = static_cast<double>(mFrames) / elapsed;
    mShownWorstMs = mWorstMs;
    mFrames = 0U;
    mWorstMs = 0.0;
    mWindowStartSeconds = nowSeconds;
    return true;
  }

  double Fps() const { return mFps; }
  double WorstFrameMs() const { return mShownWorstMs; }

private:
  double mWindowSeconds;
  double mLastSeconds = -1.0;
  double mWindowStartSeconds = 0.0;
  double mWorstMs = 0.0;
  double mShownWorstMs = 0.0;
  double mFps = 0.0;
  uint32_t mFrames = 0U;
};

// The overlay's two lines. A negative `speed` (no guest refresh was owed in
// the window) shows "--"; so does a clock of -1.
//   "59.9 FPS  max 18.2 ms  speed 100%"
inline void FormatPerfOverlayFrameLine(char *out, size_t size, double fps, double worstMs,
                                       double speed) {
  if (speed < 0.0) {
    std::snprintf(out, size, "%.1f FPS  max %.1f ms  speed --", fps, worstMs);
  } else {
    std::snprintf(out, size, "%.1f FPS  max %.1f ms  speed %.0f%%", fps, worstMs,
                  speed * 100.0);
  }
}

// MHz, without the unit to keep the line short:
//   "CPU 1785  GPU 768  MEM 1600  sys-clk"
//   "CPU 1020  GPU 307  MEM 1331  stock"
//   "CPU --  GPU --  MEM --  stock 0x92220010" (configuration not in the table)
//   "CPU --  GPU --  MEM --" (nothing read)
inline void FormatPerfOverlayClockLine(char *out, size_t size,
                                       const SwitchClockReading &reading) {
  char cpu[12] = "--";
  char gpu[12] = "--";
  char mem[12] = "--";
  if (reading.CpuMhz > 0) std::snprintf(cpu, sizeof(cpu), "%d", reading.CpuMhz);
  if (reading.GpuMhz > 0) std::snprintf(gpu, sizeof(gpu), "%d", reading.GpuMhz);
  if (reading.MemMhz > 0) std::snprintf(mem, sizeof(mem), "%d", reading.MemMhz);
  const bool known = reading.CpuMhz > 0 || reading.GpuMhz > 0 || reading.MemMhz > 0;
  switch (reading.Source) {
  case SwitchClockSource::SysClkReal:
  case SwitchClockSource::SysClkTarget:
    std::snprintf(out, size, "CPU %s  GPU %s  MEM %s  sys-clk", cpu, gpu, mem);
    return;
  case SwitchClockSource::Stock:
    if (known) {
      std::snprintf(out, size, "CPU %s  GPU %s  MEM %s  stock", cpu, gpu, mem);
    } else {
      std::snprintf(out, size, "CPU --  GPU --  MEM --  stock 0x%08X",
                    static_cast<unsigned>(reading.PerformanceConfiguration));
    }
    return;
  case SwitchClockSource::None: break;
  }
  std::snprintf(out, size, "CPU --  GPU --  MEM --");
}

// On-screen performance overlay (L3+R3 > Video, config/fps_counter): the
// presented FPS and worst frame interval of the last window, the game speed
// (guest refreshes run / refreshes owed, from the presentation scheduler's
// per-frame counts; the drops are the time the game lost) and the clocks.
// Game thread only; the text is formatted only when a window closes.
class PerfOverlay {
public:
  explicit PerfOverlay(double windowSeconds = 0.5) : mFps(windowSeconds) {
    FormatPerfOverlayClockLine(mClockLine, sizeof(mClockLine), mClocks);
  }

  // A presented frame. True when a window closed: FrameLine() changed and
  // the caller should pass the latest clocks to SetClocks().
  bool ObserveFrame(double nowSeconds) {
    if (!mFps.Observe(nowSeconds)) return false;
    const double speed =
        mOwed == 0U ? -1.0 : static_cast<double>(mRun) / static_cast<double>(mOwed);
    mRun = 0U;
    mOwed = 0U;
    FormatPerfOverlayFrameLine(mFrameLine, sizeof(mFrameLine), mFps.Fps(), mFps.WorstFrameMs(),
                               speed);
    return true;
  }

  // One host frame's refreshes: `run` were simulated out of `due` planned,
  // and the scheduler dropped `dropped` more (the game fell behind).
  void ObserveRefreshes(uint32_t run, uint32_t due, uint32_t dropped) {
    mRun += run;
    mOwed += due + dropped;
  }

  void SetClocks(const SwitchClockReading &clocks) {
    if (clocks == mClocks) return;
    mClocks = clocks;
    FormatPerfOverlayClockLine(mClockLine, sizeof(mClockLine), mClocks);
  }

  const char *FrameLine() const { return mFrameLine; }
  const char *ClockLine() const { return mClockLine; }

private:
  FpsCounter mFps;
  uint64_t mRun = 0U;
  uint64_t mOwed = 0U;
  SwitchClockReading mClocks;
  char mFrameLine[64] = "-- FPS";
  char mClockLine[64] = "";
};

// The L3+R3 Video tab's switch (oot3d_video_settings_panel.cpp loads it from
// config/fps_counter, present = on, and writes the file back); RunFrame reads
// it every frame, so a change shows in the same frame.
inline constexpr const char *kPerfOverlayConfigName = "fps_counter";
inline std::atomic<bool> &PerfOverlayEnabled() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

} // namespace Oot3dNativeGame
