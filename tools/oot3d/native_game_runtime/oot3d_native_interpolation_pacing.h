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
// It starts at the preferred level (full) and only goes down on real
// measurements: the current level must have been measured and its pair must
// stay above the budget (BudgetFraction, 100%) for DropAfter decisions in a
// row. A better level comes back as soon as its pair fits with a small
// margin (RecoverFraction, 97%). The cost of a level not drawn lately is
// estimated from the step frame's cost (both are one replay of the same
// scene) times the ratio last measured, so it follows the scene; a probe
// re-measures the level above every ProbeInterval decisions below the
// preferred one, in case the ratio went stale.
//
// Console bug of the first version (2026-10-17): budget 95% / recover 90%,
// probes every 150 decisions and, before the GPU clock was calibrated, a
// cost of render-thread time plus the swap (which includes the vsync wait).
// The first seconds dropped on those inflated costs, and a pair of ~16 + 16
// ms (31.9 of 33.3, it fits) never came back under 90%: stuck at dropped,
// 30 real frames a second presented irregularly.
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
  // Fractions of the pair budget (two refresh periods): a level is left when
  // its measured pair exceeds BudgetFraction for DropAfter decisions in a
  // row, and a better one is taken when its pair fits RecoverFraction.
  double BudgetFraction = 1.0;
  double RecoverFraction = 0.97;
  uint32_t DropAfter = 3U;
  // After this many consecutive in-between frames below the full level, one
  // is drawn a level higher to refresh its estimate (if not hopeless).
  uint32_t ProbeInterval = 60U;
  // config/interp_margin_ms: the pair must fit the budget minus this much
  // (ms). tour15 (2026-10-23, pgo5j, costs without the render thread's
  // acquire wait): areas whose measured GPU pair was 29-33 ms "fit" 33.3
  // but presented 54.7-57.1 FPS instead of 59.4-60 (the GPU cannot be kept
  // fed back to back: what it measures as busy is not all a frame occupies);
  // pairs up to ~27 kept 59+. 5 ms keeps in-between frames where the pair
  // is <= 28.3 ms (per-area replay of tour14/tour15: 59.32 FPS, 51.6
  // distinct pictures/s, 22 areas >= 59, against pgo5i's 59.35 / 50.5 / 21).
  // Default 0 since 2026-10-23 (pgo5l): the defaults are pgo5i's (cost
  // counts the acquire wait, no margin); kExcludedWaitMarginMs goes with
  // config/interp_cpu_exclude_wait.
  double MarginMs = 0.0;
};

// Margin measured for the cost without the acquire wait (tour15, pgo5k).
inline constexpr double kExcludedWaitMarginMs = 5.0;

// Parses config/interp_margin_ms ("5", "1,5"): 0 to 15 ms; nullopt when
// invalid (the default stays).
[[nodiscard]] inline std::optional<double>
ParseInterpolationMarginMs(std::string_view text) noexcept {
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
      return std::nullopt;
    }
  }
  if (!digits || value > 15.0)
    return std::nullopt;
  return value;
}

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
    // The margin never takes more than half the pair.
    mDecisionBudgetMs =
        std::max(0.5 * mPairBudgetMs,
                 mPairBudgetMs - std::max(0.0, config.MarginMs));
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
    mOverBudget = 0U;
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
    const double budget = mDecisionBudgetMs;
    const auto index = [](InterpolatedSampleLevel level) {
      return static_cast<uint8_t>(level);
    };
    // A better level must fit with margin; the current one is kept while
    // it fits the budget, and always while it has not been measured (no
    // drop on a guess).
    const auto fits = [&](InterpolatedSampleLevel level) {
      if (level == mLevel && !mMeasured[index(level)])
        return true;
      const double fraction = index(level) < index(mLevel)
                                  ? mConfig.RecoverFraction
                                  : mConfig.BudgetFraction;
      return mStepCostMs + Estimate(level) <= fraction * budget;
    };
    std::optional<InterpolatedSampleLevel> best;
    for (auto level : {InterpolatedSampleLevel::Full,
                       InterpolatedSampleLevel::Reduced}) {
      if (index(level) < index(preferred))
        continue;
      if (level == InterpolatedSampleLevel::Reduced && !mReducedAvailable)
        continue;
      if (fits(level)) {
        best = level;
        break;
      }
    }
    if (!best.has_value()) {
      // Never dropped without KeepGameSpeed (or while the cadence governor
      // holds 60, SetDropAllowed): the cheapest drawn level.
      best = mConfig.KeepGameSpeed && mDropAllowed
                 ? InterpolatedSampleLevel::Dropped
             : mReducedAvailable   ? InterpolatedSampleLevel::Reduced
                                   : InterpolatedSampleLevel::Full;
    }
    InterpolatedSampleLevel chosen = *best;
    if (index(chosen) > index(mLevel)) {
      // Down only when the overrun lasts.
      if (++mOverBudget < std::max<uint32_t>(1U, mConfig.DropAfter))
        chosen = mLevel;
      else
        mOverBudget = 0U;
    } else {
      mOverBudget = 0U;
    }
    // Probe: a stretch below the preferred level re-measures the next level
    // up once (at once if it was never measured), unless it clearly cannot
    // fit. A probe's cost arrives some frames later (GPU timestamps): one at
    // a time, and one whose cost never came expires.
    if (mProbing.has_value() && ++mProbeAge > 8U)
      mProbing.reset();
    if (chosen != preferred && !mProbing.has_value()) {
      auto probe = static_cast<InterpolatedSampleLevel>(index(chosen) - 1U);
      if (probe == InterpolatedSampleLevel::Reduced && !mReducedAvailable)
        probe = InterpolatedSampleLevel::Full;
      if (++mBelowPreferred >= mConfig.ProbeInterval ||
          !mMeasured[index(probe)]) {
        mBelowPreferred = 0U;
        if (index(probe) >= index(preferred) &&
            mStepCostMs + Estimate(probe) <= 1.25 * budget) {
          ++mStats.Probes;
          mProbing = probe;
          mProbeAge = 0U;
          return Commit(probe, true);
        }
      }
    } else if (chosen == preferred) {
      mBelowPreferred = 0U;
    }
    return Commit(chosen, false);
  }

  // config/fps60_mode (oot3d_native_cadence_governor.h): with "adaptive" the
  // cadence governor drops in-between frames as a whole (a locked 30) and
  // this governor only chooses full or reduced; "strict60" never drops.
  // Leaving the dropped level when dropping is withdrawn.
  void SetDropAllowed(bool allowed) noexcept {
    if (mDropAllowed == allowed)
      return;
    mDropAllowed = allowed;
    mOverBudget = 0U;
    if (!allowed && mLevel == InterpolatedSampleLevel::Dropped)
      Commit(PreferredLevel(), false);
  }
  [[nodiscard]] bool DropAllowed() const noexcept { return mDropAllowed; }

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
  // The budget the decisions use: the pair minus config/interp_margin_ms.
  [[nodiscard]] double DecisionBudgetMs() const noexcept {
    return mDecisionBudgetMs;
  }
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
  double mDecisionBudgetMs = 1000.0 / 30.0;
  bool mReducedAvailable = false;
  bool mDropAllowed = true;
  double mStepCostMs = 0.0;
  std::array<double, 3> mRatio{1.0, 1.0, 0.1};
  std::array<bool, 3> mMeasured{};
  InterpolatedSampleLevel mLevel = InterpolatedSampleLevel::Full;
  std::optional<InterpolatedSampleLevel> mProbing;
  uint32_t mProbeAge = 0U;
  uint32_t mBelowPreferred = 0U;
  uint32_t mOverBudget = 0U;
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
