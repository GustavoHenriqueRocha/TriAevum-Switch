#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace Oot3dNativeGame {

// On-screen FPS counter (config/fps_counter). Counts presented frames,
// including the fps60 interpolated ones, and closes an average every `windowSeconds`;
// the window's worst interval shows hitches the average hides.
class FpsCounter {
public:
  explicit FpsCounter(double windowSeconds = 0.5) : mWindowSeconds(windowSeconds) {}

  // A frame presented at `nowSeconds` (monotonic clock).
  // Returns true when a window closed and the text changed.
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
    std::snprintf(mText, sizeof(mText), "%.1f FPS  %.1f ms", mFps, mShownWorstMs);
    return true;
  }

  double Fps() const { return mFps; }
  double WorstFrameMs() const { return mShownWorstMs; }
  const char *Text() const { return mText; }

private:
  double mWindowSeconds;
  double mLastSeconds = -1.0;
  double mWindowStartSeconds = 0.0;
  double mWorstMs = 0.0;
  double mShownWorstMs = 0.0;
  double mFps = 0.0;
  uint32_t mFrames = 0U;
  char mText[48] = "-- FPS";
};

} // namespace Oot3dNativeGame
