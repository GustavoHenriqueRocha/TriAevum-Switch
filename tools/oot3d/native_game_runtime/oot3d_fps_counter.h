#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "oot3d_switch_clocks.h"

namespace Oot3dNativeGame {

// New-image rate. Called once per presented refresh with the number of new
// images (a game step's frame or an interpolated in-between frame) that
// reached the screen since the previous call; a repeat of the previous image
// (a dropped in-between frame, a reused snapshot) passes 0. Each image counts
// once however many refreshes it stays, so 60 with the in-betweens dropped
// reads ~30, not the refresh rate. Closes an average every `windowSeconds`;
// the window's worst is the longest time one image stayed on screen (a held
// frame at 60 Hz reads 33.3 ms), which shows hitches the average hides.
class FpsCounter {
public:
  explicit FpsCounter(double windowSeconds = 0.5) : mWindowSeconds(windowSeconds) {}

  // A refresh presented at `nowSeconds` (monotonic clock) with `newImages`
  // new images since the previous call.
  // Returns true when a window closed and the values changed.
  bool Observe(double nowSeconds, uint32_t newImages) {
    if (mWindowStartSeconds < 0.0) {
      mWindowStartSeconds = mLastImageSeconds = nowSeconds;
      return false;
    }
    if (newImages != 0U) {
      mWorstMs = std::max(mWorstMs, (nowSeconds - mLastImageSeconds) * 1000.0);
      mLastImageSeconds = nowSeconds;
      mImages += newImages;
    }
    const double elapsed = nowSeconds - mWindowStartSeconds;
    if (elapsed < mWindowSeconds) return false;
    mFps = static_cast<double>(mImages) / elapsed;
    // An image still on screen counts with the time it has stayed so far
    // (a frozen picture shows in `max`).
    mShownWorstMs = std::max(mWorstMs, (nowSeconds - mLastImageSeconds) * 1000.0);
    mImages = 0U;
    mWorstMs = 0.0;
    mWindowStartSeconds = nowSeconds;
    return true;
  }

  double Fps() const { return mFps; }
  double WorstFrameMs() const { return mShownWorstMs; }

private:
  double mWindowSeconds;
  double mWindowStartSeconds = -1.0;
  double mLastImageSeconds = 0.0;
  double mWorstMs = 0.0;
  double mShownWorstMs = 0.0;
  double mFps = 0.0;
  uint64_t mImages = 0U;
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
//   "CPU 1224  GPU 461  MEM 1600  clkrst" (CPU High on: the rates clkrst reports)
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
  case SwitchClockSource::Clkrst:
    std::snprintf(out, size, "CPU %s  GPU %s  MEM %s  clkrst", cpu, gpu, mem);
    return;
  case SwitchClockSource::None: break;
  }
  std::snprintf(out, size, "CPU --  GPU --  MEM --");
}

// On-screen performance overlay (L3+R3 > Video, config/fps_counter): the
// new images per second and the longest time one image stayed on screen in
// the last window (FpsCounter), the game speed
// (guest refreshes run / refreshes owed, from the presentation scheduler's
// per-frame counts; the drops are the time the game lost) and the clocks.
// Game thread only; the text is formatted only when a window closes.
class PerfOverlay {
public:
  explicit PerfOverlay(double windowSeconds = 0.5) : mFps(windowSeconds) {
    FormatPerfOverlayClockLine(mClockLine, sizeof(mClockLine), mClocks);
  }

  // A presented refresh and the new images shown since the previous one
  // (FpsCounter::Observe). True when a window closed: FrameLine() changed
  // and the caller should pass the latest clocks to SetClocks().
  bool ObserveFrame(double nowSeconds, uint32_t newImages) {
    if (!mFps.Observe(nowSeconds, newImages)) return false;
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
