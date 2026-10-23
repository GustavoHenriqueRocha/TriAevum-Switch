#pragma once

// Dynamic resolution of the top-screen scene (config/dynamic_resolution).
//
// tour17 (pgo5l, 2026-10-23): the Water Temple is GPU-bound at scale 3: a
// step frame costs ~14.8 ms of GPU, so the step + in-between pair (~29.6 ms
// measured, plus ~2.5-3 ms the timer does not see) misses the 33.3 ms of two
// refreshes and the interpolation governor drops almost every in-between
// frame (55 FPS, 10 interpolated against 65 held per 150 frames). Drawing
// the scene a little smaller there (2.75, 2.5 ...) makes the pair fit; the
// areas that already fit keep the configured scale.
//
// The controller watches the GPU time of the step frames (the same
// stall-free GL_TIMESTAMP measure the governor learns,
// oot3d_native_gpu_frame_timer.h; no render-thread time: a smaller image
// only saves GPU work) and moves the scale in steps of 0.25 between
// MaxScale (config/resolution_scale) and MinScale:
//   down - the pair (twice the step frame while interpolating: both are one
//          replay of the same scene) stays above DownFraction of the budget
//          for DownAfter step frames in a row;
//   up   - the pair the next scale up would cost (the pixel model below)
//          stays at or under UpFraction for UpAfter step frames in a row.
// After every change SettleSteps results are discarded (the GPU timestamps
// arrive up to 8 frames late and still describe the old scale) and the
// average starts again. An up step that has to come back down within
// BounceWindow step frames doubles the wait before trying that level again
// (up to MaxBackoff times UpAfter); a level that held for BounceWindow
// returns to UpAfter.
//
// Cost model of one replay at scale s against the current scale c: a fixed
// part (vertex work, state, scanout) plus the pixel part scaled by the area,
// cost(s) = cost(c) * (F + (1 - F) * (s / c)^2), F = FixedFraction (the
// governor's initial guess for the reduced in-between frame, 0.35).
//
// Pure logic (no GL, no clock): oot3d_native_dynamic_resolution_tests.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Oot3dNativeGame {

struct DynamicResolutionConfig {
  // config/resolution_scale (the allocated images) and the lowest scale.
  float MaxScale = 3.0F;
  float MinScale = 2.25F;
  float StepScale = 0.25F;
  // Fractions of the pair budget (two refresh periods while interpolating).
  // tour15: GPU pairs up to ~27-28 ms of 33.3 kept 59+ FPS, 29-33 did not.
  double DownFraction = 0.85;
  double UpFraction = 0.80;
  uint32_t DownAfter = 3U;
  uint32_t UpAfter = 60U;
  uint32_t SettleSteps = 5U;
  uint32_t BounceWindow = 180U;
  uint32_t MaxBackoff = 8U;
  double FixedFraction = 0.35;
};

// Default lowest scale: three steps below the configured one (3 -> 2.25).
inline constexpr float kDynamicResolutionDefaultSteps = 3.0F;

// config/dynamic_resolution: present turns it on; its text, if any, is the
// lowest scale ("2.25", "2,5"; multiples of 0.25, from 1 up to maxScale).
// Empty or unreadable: three steps below maxScale (at least 1). A value at
// or above maxScale leaves nothing to vary (MinScale == MaxScale).
[[nodiscard]] inline float ParseDynamicResolutionMinScale(
    std::string_view text, float maxScale) noexcept {
  const float fallback =
      std::max(1.0F, maxScale - kDynamicResolutionDefaultSteps * 0.25F);
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1U);
  double value = 0.0, divisor = 1.0;
  bool digits = false, point = false;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      digits = true;
      if (point) {
        divisor *= 10.0;
        value += (c - '0') / divisor;
      } else {
        value = value * 10.0 + (c - '0');
      }
    } else if ((c == '.' || c == ',') && !point) {
      point = true;
    } else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
      break;
    } else {
      return fallback;
    }
  }
  if (!digits)
    return fallback;
  const float scale = static_cast<float>(std::round(value * 4.0) / 4.0);
  if (scale < 1.0F)
    return fallback;
  return std::min(scale, maxScale);
}

struct DynamicResolutionStats {
  uint64_t Steps = 0;    // step-frame GPU results observed
  uint64_t Changes = 0;  // scale changes (down + up)
  uint64_t Downs = 0;
  uint64_t Ups = 0;
  uint64_t Bounces = 0;  // up steps undone within BounceWindow
};

class DynamicResolutionController {
public:
  // pairBudgetMs: two presentation periods (33.3 ms at 60 Hz).
  void Configure(const DynamicResolutionConfig &config,
                 double pairBudgetMs) noexcept {
    mConfig = config;
    if (!(mConfig.StepScale > 0.0F))
      mConfig.StepScale = 0.25F;
    mConfig.MaxScale = std::max(1.0F, mConfig.MaxScale);
    mConfig.MinScale = std::clamp(mConfig.MinScale, 1.0F, mConfig.MaxScale);
    mLevels = static_cast<uint32_t>(std::floor(
        (mConfig.MaxScale - mConfig.MinScale) / mConfig.StepScale + 1.0e-3));
    mLevels = std::min<uint32_t>(mLevels, kMaxLevels - 1U);
    mBudgetMs = pairBudgetMs > 0.0 ? pairBudgetMs : 1000.0 / 30.0;
    mLevel = 0U;
    mAverageMs = 0.0;
    mSettle = 0U;
    mOver = 0U;
    mUnder = 0U;
    mSinceUp = 0U;
    mLastWasUp = false;
    mBackoff.fill(1U);
  }

  // Level 0 is MaxScale; level n is n steps below.
  [[nodiscard]] float Scale() const noexcept { return ScaleOf(mLevel); }
  [[nodiscard]] float ScaleOf(uint32_t level) const noexcept {
    return std::max(mConfig.MinScale,
                    mConfig.MaxScale -
                        static_cast<float>(level) * mConfig.StepScale);
  }
  [[nodiscard]] uint32_t Level() const noexcept { return mLevel; }
  [[nodiscard]] uint32_t LowestLevel() const noexcept { return mLevels; }
  [[nodiscard]] double AveragePairMs() const noexcept { return mAverageMs; }
  [[nodiscard]] double BudgetMs() const noexcept { return mBudgetMs; }
  [[nodiscard]] const DynamicResolutionStats &Stats() const noexcept {
    return mStats;
  }
  [[nodiscard]] const DynamicResolutionConfig &Config() const noexcept {
    return mConfig;
  }

  // Relative cost of one replay at scale `to` against scale `from`.
  [[nodiscard]] double CostRatio(float from, float to) const noexcept {
    if (!(from > 0.0F))
      return 1.0;
    const double area = static_cast<double>(to) / from;
    return mConfig.FixedFraction + (1.0 - mConfig.FixedFraction) * area * area;
  }

  // The pair the next scale up would cost (0 at the top).
  [[nodiscard]] double PredictedUpPairMs() const noexcept {
    if (mLevel == 0U || mAverageMs <= 0.0)
      return 0.0;
    return mAverageMs * CostRatio(Scale(), ScaleOf(mLevel - 1U));
  }

  // One step frame's pair cost at the current scale (ms of GPU). Returns
  // true when the scale changed.
  bool ObservePair(double pairMs) noexcept {
    if (!(pairMs > 0.0) || !std::isfinite(pairMs))
      return false;
    ++mStats.Steps;
    if (mLastWasUp && mSinceUp < mConfig.BounceWindow &&
        ++mSinceUp >= mConfig.BounceWindow) {
      // The level reached by the last up step held: no more backoff there.
      mBackoff[mLevel] = 1U;
      mLastWasUp = false;
    }
    if (mSettle > 0U) {
      --mSettle;
      return false;
    }
    if (mAverageMs <= 0.0) {
      mAverageMs = pairMs;
    } else {
      const double sample = std::min(pairMs, 2.0 * mAverageMs);
      mAverageMs = (3.0 * mAverageMs + sample) / 4.0;
    }
    if (mLevels == 0U)
      return false;
    if (mAverageMs > mConfig.DownFraction * mBudgetMs) {
      mUnder = 0U;
      if (mLevel < mLevels && ++mOver >= std::max<uint32_t>(1U, mConfig.DownAfter)) {
        if (mLastWasUp && mSinceUp < mConfig.BounceWindow) {
          // The up step did not hold: wait longer before the next try.
          ++mStats.Bounces;
          // (Up from level L + 1 waits on mBackoff[L]; this is level L.)
          mBackoff[mLevel] = std::min<uint32_t>(
              mConfig.MaxBackoff, std::max<uint32_t>(1U, mBackoff[mLevel]) * 2U);
        }
        mLastWasUp = false;
        ++mStats.Downs;
        Change(mLevel + 1U);
        return true;
      }
      return false;
    }
    mOver = 0U;
    if (mLevel == 0U) {
      mUnder = 0U;
      return false;
    }
    if (PredictedUpPairMs() <= mConfig.UpFraction * mBudgetMs) {
      const uint32_t wait = std::max<uint32_t>(1U, mConfig.UpAfter) *
                            std::max<uint32_t>(1U, mBackoff[mLevel - 1U]);
      if (++mUnder >= wait) {
        ++mStats.Ups;
        mLastWasUp = true;
        mSinceUp = 0U;
        Change(mLevel - 1U);
        return true;
      }
    } else {
      mUnder = 0U;
    }
    return false;
  }

  // Steps the next up from `level` currently waits (UpAfter times backoff).
  [[nodiscard]] uint32_t UpWait(uint32_t level) const noexcept {
    if (level == 0U || level > mLevels)
      return 0U;
    return std::max<uint32_t>(1U, mConfig.UpAfter) *
           std::max<uint32_t>(1U, mBackoff[level - 1U]);
  }

private:
  static constexpr uint32_t kMaxLevels = 16U;

  void Change(uint32_t level) noexcept {
    mLevel = level;
    ++mStats.Changes;
    mAverageMs = 0.0;
    mSettle = mConfig.SettleSteps;
    mOver = 0U;
    mUnder = 0U;
  }

  DynamicResolutionConfig mConfig{};
  double mBudgetMs = 1000.0 / 30.0;
  uint32_t mLevels = 0U;
  uint32_t mLevel = 0U;
  double mAverageMs = 0.0;
  uint32_t mSettle = 0U;
  uint32_t mOver = 0U;
  uint32_t mUnder = 0U;
  uint32_t mSinceUp = 0U;
  bool mLastWasUp = false;
  std::array<uint32_t, kMaxLevels> mBackoff{};
  DynamicResolutionStats mStats{};
};

} // namespace Oot3dNativeGame
