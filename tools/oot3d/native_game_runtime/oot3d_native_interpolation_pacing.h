#pragma once

// Paced 60 Hz interpolation (config/fps60): what each in-between frame costs
// and whether it is drawn at full resolution, at a reduced one, or dropped.
//
// With visual interpolation every guest step (30 Hz) is presented twice: the
// step frame (the delayed sample at alpha 0) and one in-between frame (alpha
// 1/2), each a full replay of the scene. When the pair does not fit into two
// refresh periods the presentation falls behind; the in-between frame is the
// one that can go, since the step frames alone carry the whole motion (the
// game's speed never depends on the in-between frames: the scheduler batches
// guest refreshes, NativePresentationScheduler).
//
// The governor runs on the render thread. It learns the recent cost of each
// kind of frame (max of GPU and render-thread time, ms) and picks, for the
// next in-between frame, the best level whose pair still fits:
//   Full     - the in-between sample at the configured resolution;
//   Reduced  - the same sample drawn at a lower scale (config/interp_scale);
//   Dropped  - the step image is presented again (no scene replay).
// Hysteresis: a better level than the current one must fit with margin
// (RecoverFraction), the current one only within BudgetFraction. The cost of
// a level not drawn lately is estimated from the step frame's cost (both are
// one replay of the same scene) times the ratio last measured, so it follows
// the scene without probing; a probe still runs after a long stretch at a
// worse level, in case the ratio went stale.
//
// Pure logic (no GL, no clock): tested by
// oot3d_native_interpolation_pacing_tests.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace Oot3dNativeGame {

enum class InterpolatedSampleLevel : uint8_t { Full = 0, Reduced = 1, Dropped = 2 };

inline constexpr std::string_view
InterpolatedSampleLevelName(InterpolatedSampleLevel level) noexcept {
  switch (level) {
  case InterpolatedSampleLevel::Full:
    return "full";
  case InterpolatedSampleLevel::Reduced:
    return "reduced";
  case InterpolatedSampleLevel::Dropped:
    return "dropped";
  }
  return "unknown";
}

// What a presented frame drew (perf.txt and the cost the governor learns).
enum class RenderFrameKind : uint8_t {
  Other = 0,   // no new sample (a reused snapshot, a menu without guest frame)
  Step = 1,    // a new guest frame (its delayed sample, or the direct one)
  Full = 2,    // an in-between sample at full resolution
  Reduced = 3, // an in-between sample at the reduced scale
  Dropped = 4, // an in-between frame dropped: the step image again
};
inline constexpr size_t kRenderFrameKindCount = 5U;
inline constexpr std::array<std::string_view, kRenderFrameKindCount>
    kRenderFrameKindNames{"other", "step", "full", "reduced", "dropped"};

[[nodiscard]] inline constexpr RenderFrameKind
RenderFrameKindOf(InterpolatedSampleLevel level) noexcept {
  return level == InterpolatedSampleLevel::Full      ? RenderFrameKind::Full
         : level == InterpolatedSampleLevel::Reduced ? RenderFrameKind::Reduced
                                                     : RenderFrameKind::Dropped;
}

enum class InterpolationReducedScaleMode : uint8_t {
  // Reduced only when the full in-between frame does not fit.
  Auto,
  // Every in-between frame reduced (still dropped when even that misses).
  Always,
};

struct InterpolationPacingConfig {
  // config/interp_pacing: drop in-between frames that do not fit ("keep the
  // game's speed"). Off: every in-between frame is drawn (the former
  // behaviour; a GPU-bound pair then delays the presentation).
  bool KeepGameSpeed = true;
  // config/interp_scale: absolute resolution scale of a reduced in-between
  // frame (e.g. 1.5 when resolution_scale is 2). 0: no reduced level.
  float ReducedScale = 0.0F;
  InterpolationReducedScaleMode ReducedMode =
      InterpolationReducedScaleMode::Auto;
  // Fractions of the pair budget (two refresh periods).
  double BudgetFraction = 0.95;
  double RecoverFraction = 0.90;
  // After this many consecutive in-between frames below the full level, one
  // is drawn a level higher to refresh its estimate (if not hopeless).
  uint32_t ProbeInterval = 150U;
};

// Parses config/interp_scale ("1.5", "1,5"; multiples of 0.25 from 1 up to
// the base scale exclusive). 0 when absent, invalid or not below base.
[[nodiscard]] inline float ParseInterpolationReducedScale(
    std::string_view text, float baseScale) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
    text.remove_prefix(1U);
  double value = 0.0;
  double fraction = 0.0;
  double divisor = 1.0;
  bool digits = false;
  bool point = false;
  for (const char c : text) {
    if (c >= '0' && c <= '9') {
      digits = true;
      if (point) {
        divisor *= 10.0;
        fraction += (c - '0') / divisor;
      } else {
        value = value * 10.0 + (c - '0');
      }
    } else if ((c == '.' || c == ',') && !point) {
      point = true;
    } else if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
      break;
    } else {
      return 0.0F;
    }
  }
  if (!digits)
    return 0.0F;
  const double scale = std::round((value + fraction) * 4.0) / 4.0;
  if (scale < 1.0 || scale >= static_cast<double>(baseScale) - 1.0e-6)
    return 0.0F;
  return static_cast<float>(scale);
}

struct InterpolationPacingStats {
  uint64_t StepFrames = 0;
  uint64_t FullFrames = 0;
  uint64_t ReducedFrames = 0;
  uint64_t DroppedFrames = 0;
  uint64_t Probes = 0;
  uint64_t LevelChanges = 0;
};

class InterpolationPacingGovernor {
public:
  // pairBudgetMs: two presentation periods (33.3 ms at 60 Hz).
  // reducedAvailable: the backend accepted the reduced scale.
  void Configure(const InterpolationPacingConfig &config, double pairBudgetMs,
                 float baseScale, bool reducedAvailable) noexcept {
    mConfig = config;
    mPairBudgetMs = pairBudgetMs > 0.0 ? pairBudgetMs : 1000.0 / 30.0;
    mReducedAvailable = reducedAvailable && config.ReducedScale > 0.0F;
    // Initial guess of a reduced replay against a full one: a fixed part
    // (vertex work, state, present) plus the pixel part scaled by the area.
    double pixelRatio = 1.0;
    if (mReducedAvailable && baseScale > 0.0F) {
      const double ratio = config.ReducedScale / baseScale;
      pixelRatio = ratio * ratio;
    }
    mRatio[static_cast<size_t>(InterpolatedSampleLevel::Full)] = 1.0;
    mRatio[static_cast<size_t>(InterpolatedSampleLevel::Reduced)] =
        0.35 + 0.65 * pixelRatio;
    mRatio[static_cast<size_t>(InterpolatedSampleLevel::Dropped)] = 0.1;
    mMeasured = {};
    mProbing.reset();
    mBelowPreferred = 0U;
    mLevel = PreferredLevel();
  }

  [[nodiscard]] InterpolatedSampleLevel PreferredLevel() const noexcept {
    return mReducedAvailable &&
                   mConfig.ReducedMode == InterpolationReducedScaleMode::Always
               ? InterpolatedSampleLevel::Reduced
               : InterpolatedSampleLevel::Full;
  }

  // The level of the next in-between frame.
  [[nodiscard]] InterpolatedSampleLevel Decide() noexcept {
    const auto preferred = PreferredLevel();
    if (mStepCostMs <= 0.0) {
      return Commit(preferred, false);
    }
    const double budget = mPairBudgetMs;
    const auto fits = [&](InterpolatedSampleLevel level) {
      const double fraction =
          static_cast<uint8_t>(level) < static_cast<uint8_t>(mLevel)
              ? mConfig.RecoverFraction
              : mConfig.BudgetFraction;
      return mStepCostMs + Estimate(level) <= fraction * budget;
    };
    std::optional<InterpolatedSampleLevel> chosen;
    for (auto level : {InterpolatedSampleLevel::Full,
                       InterpolatedSampleLevel::Reduced}) {
      if (static_cast<uint8_t>(level) < static_cast<uint8_t>(preferred))
        continue;
      if (level == InterpolatedSampleLevel::Reduced && !mReducedAvailable)
        continue;
      if (fits(level)) {
        chosen = level;
        break;
      }
    }
    if (!chosen.has_value()) {
      if (mConfig.KeepGameSpeed) {
        chosen = InterpolatedSampleLevel::Dropped;
      } else {
        // Never dropped: the cheapest drawn level.
        chosen = mReducedAvailable ? InterpolatedSampleLevel::Reduced
                                   : InterpolatedSampleLevel::Full;
      }
    }
    // Probe: a long stretch below the preferred level re-measures the next
    // level up once (at once if it was never measured: the first estimate is
    // a guess), unless it clearly cannot fit.
    // A probe's cost arrives some frames later (GPU timestamps): one at a
    // time, and a probe whose cost never came expires.
    if (mProbing.has_value() && ++mProbeAge > 8U)
      mProbing.reset();
    if (*chosen != preferred && !mProbing.has_value()) {
      const auto up = static_cast<InterpolatedSampleLevel>(
          static_cast<uint8_t>(*chosen) - 1U);
      const auto probe =
          up == InterpolatedSampleLevel::Reduced && !mReducedAvailable
              ? InterpolatedSampleLevel::Full
              : up;
      if (++mBelowPreferred >= mConfig.ProbeInterval ||
          !mMeasured[static_cast<size_t>(probe)]) {
        if (static_cast<uint8_t>(probe) >= static_cast<uint8_t>(preferred) &&
            mStepCostMs + Estimate(probe) <= 1.25 * budget) {
          mBelowPreferred = 0U;
          ++mStats.Probes;
          mProbing = probe;
          mProbeAge = 0U;
          return Commit(probe, true);
        }
        mBelowPreferred = 0U;
      }
    } else if (*chosen == preferred) {
      mBelowPreferred = 0U;
    }
    return Commit(*chosen, false);
  }

  void ObserveStep(double costMs) noexcept {
    if (!(costMs > 0.0) || !std::isfinite(costMs))
      return;
    mStepCostMs = Average(mStepCostMs, costMs);
    ++mStats.StepFrames;
  }

  void ObserveInterpolated(InterpolatedSampleLevel level,
                           double costMs) noexcept {
    switch (level) {
    case InterpolatedSampleLevel::Full:
      ++mStats.FullFrames;
      break;
    case InterpolatedSampleLevel::Reduced:
      ++mStats.ReducedFrames;
      break;
    case InterpolatedSampleLevel::Dropped:
      ++mStats.DroppedFrames;
      break;
    }
    if (!(costMs > 0.0) || !std::isfinite(costMs) || mStepCostMs <= 0.0)
      return;
    const size_t index = static_cast<size_t>(level);
    double ratio = costMs / mStepCostMs;
    if (mProbing == level) {
      // A probe replaces the estimate; one that met a hitch counts twice the
      // estimate at most (SwitchWakerHD interp_pacing.h, probe_estimate).
      mProbing.reset();
      mRatio[index] = std::min(ratio, 2.0 * mRatio[index]);
      mMeasured[index] = true;
      return;
    }
    // One sample counts at most twice the average (a hitch must not keep the
    // level out for long; a lasting change gets through within a few).
    if (mMeasured[index])
      ratio = std::min(ratio, 2.0 * mRatio[index]);
    mRatio[index] =
        mMeasured[index] ? (3.0 * mRatio[index] + ratio) / 4.0 : ratio;
    mMeasured[index] = true;
  }

  [[nodiscard]] double Estimate(InterpolatedSampleLevel level) const noexcept {
    return mStepCostMs * mRatio[static_cast<size_t>(level)];
  }
  [[nodiscard]] double StepCostMs() const noexcept { return mStepCostMs; }
  [[nodiscard]] double PairBudgetMs() const noexcept { return mPairBudgetMs; }
  [[nodiscard]] InterpolatedSampleLevel Level() const noexcept {
    return mLevel;
  }
  [[nodiscard]] bool ReducedAvailable() const noexcept {
    return mReducedAvailable;
  }
  [[nodiscard]] const InterpolationPacingConfig &Config() const noexcept {
    return mConfig;
  }
  [[nodiscard]] const InterpolationPacingStats &Stats() const noexcept {
    return mStats;
  }

private:
  static double Average(double average, double sample) noexcept {
    if (average <= 0.0)
      return sample;
    sample = std::min(sample, 2.0 * average);
    return (3.0 * average + sample) / 4.0;
  }

  InterpolatedSampleLevel Commit(InterpolatedSampleLevel level,
                                 bool probe) noexcept {
    if (!probe && level != mLevel) {
      ++mStats.LevelChanges;
      mLevel = level;
    }
    return level;
  }

  InterpolationPacingConfig mConfig{};
  double mPairBudgetMs = 1000.0 / 30.0;
  bool mReducedAvailable = false;
  double mStepCostMs = 0.0;
  std::array<double, 3> mRatio{1.0, 1.0, 0.1};
  std::array<bool, 3> mMeasured{};
  InterpolatedSampleLevel mLevel = InterpolatedSampleLevel::Full;
  std::optional<InterpolatedSampleLevel> mProbing;
  uint32_t mProbeAge = 0U;
  uint32_t mBelowPreferred = 0U;
  InterpolationPacingStats mStats{};
};

// Whether a sample can be replayed into images that hold nothing from
// earlier frames (the reduced namespace sees only some frames): every image
// a draw writes must be cleared by a memory fill of the sample before that
// image's first draw (colour always; depth/stencil when the draw tests or
// writes it). A frame that builds on last frame's pixels (a frozen
// background, a canvas initialised by a draw) keeps the full namespace.
struct ReducedReplayDraw {
  uint64_t SubmissionId = 0;
  uint32_t ColorAddress = 0;
  uint32_t ColorEnd = 0;
  uint32_t DepthAddress = 0;
  uint32_t DepthEnd = 0;
  bool UsesDepthStencil = false;
};
struct ReducedReplayFill {
  uint64_t BeforeDrawSubmissionId = 0;
  uint32_t Start = 0;
  uint32_t End = 0;
};

[[nodiscard]] inline bool SampleClearsEveryTarget(
    std::span<const ReducedReplayDraw> draws,
    std::span<const ReducedReplayFill> fills) noexcept {
  // Small sets (a frame writes a handful of images): linear lists.
  std::array<uint32_t, 32> cleared{};
  size_t clearedCount = 0U;
  std::array<uint32_t, 32> seen{};
  size_t seenCount = 0U;
  size_t fillIndex = 0U;
  const auto contains = [](const auto &set, size_t count, uint32_t value) {
    for (size_t index = 0U; index < count; ++index)
      if (set[index] == value)
        return true;
    return false;
  };
  for (const auto &draw : draws) {
    // Fills before this draw (BeforeDrawSubmissionId <= draw id), as the
    // scheduler orders them.
    while (fillIndex < fills.size() &&
           fills[fillIndex].BeforeDrawSubmissionId <= draw.SubmissionId) {
      ++fillIndex;
    }
    const auto clearedBefore = [&](uint32_t address, uint32_t end) {
      for (size_t index = 0U; index < fillIndex; ++index) {
        if (fills[index].Start <= address && fills[index].End >= end)
          return true;
      }
      return false;
    };
    const auto check = [&](uint32_t address, uint32_t end) {
      if (address == 0U)
        return true;
      if (contains(seen, seenCount, address))
        return contains(cleared, clearedCount, address);
      if (seenCount == seen.size() || clearedCount == cleared.size())
        return false;
      seen[seenCount++] = address;
      if (!clearedBefore(address, end))
        return false;
      cleared[clearedCount++] = address;
      return true;
    };
    if (!check(draw.ColorAddress, draw.ColorEnd))
      return false;
    if (draw.UsesDepthStencil && !check(draw.DepthAddress, draw.DepthEnd))
      return false;
  }
  return true;
}

} // namespace Oot3dNativeGame
