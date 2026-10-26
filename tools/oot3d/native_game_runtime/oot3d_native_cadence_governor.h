#pragma once

// Presentation cadence of config/fps60 (render thread): how regular the
// pictures on screen are, and the governor that locks the cadence to a
// steady 30 when 60 does not hold.
//
// Console, 2026-10-17 (switch-logs/integ2b, CPU 1785, 4.5 min at 60): 52 FPS
// on average, but what the player felt was the irregularity. In the heavy
// stretches the game thread spent 27-31 ms of every 33.3 ms step pair and
// the GPU ~16.5 ms per frame (pair 33.2 of 33.3). The per-frame governor
// (oot3d_native_interpolation_pacing.h) dropped most in-between frames, but
// pictures were still held 1, 2 or 3 refreshes in no fixed pattern: a step
// that came late (the host frame then carries two guest refreshes and no
// in-between frame) holds the previous picture 2 refreshes amid 1s at 60, or
// 3 amid 2s at 30, and every switch between drawing and dropping changes the
// hold again. The eye reads the changes of hold as judder, more than the
// average rate.
//
// config/fps60_mode:
//   adaptive (default) - the in-between frame is dropped as a whole, never
//     one here and one there: at 60 the per-frame governor may only choose
//     full or reduced; when 60 misses refreshes repeatedly (LockMisses
//     presents of two or more refreshes among the last MissWindow) the
//     cadence locks at 30 for at least the hold time (2 s, doubling up to
//     16 s when 60 failed again soon after coming back) and only returns
//     to 60 with proven headroom: game-thread step pair (p90) within
//     GameMargin, GPU/render pair estimate within GpuMargin and no late
//     step for MinHoldSeconds.
//   strict60 - every in-between frame drawn (full or reduced), never
//     dropped (as config/interp_pacing=0).
//   legacy - the per-frame governor alone (draw, reduce or drop each
//     in-between frame), the behaviour before this file.
// Options after the mode: "adaptive misses=3 window=60 hold=2 max_hold=16
// game=0.9 gpu=0.9".
//
// Counters (perf.txt, "cadence:"): presents 1/2/3/4+ refreshes after the
// previous one, how many refreshes each new picture stayed on screen, the
// repeats (the same picture again), the changes of hold between consecutive
// pictures, late steps and the time locked at 30.
//
// Pure logic (no clock, no GL): oot3d_native_cadence_governor_tests.cpp,
// which also simulates the console pipeline (oot3d_native_cadence_sim.h).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Oot3dNativeGame {

enum class Fps60Mode : uint8_t { Adaptive = 0, Strict60 = 1, Legacy = 2 };

inline constexpr std::string_view Fps60ModeName(Fps60Mode mode) noexcept {
  switch (mode) {
  case Fps60Mode::Adaptive:
    return "adaptive";
  case Fps60Mode::Strict60:
    return "strict60";
  case Fps60Mode::Legacy:
    return "legacy";
  }
  return "unknown";
}

struct Fps60ModeConfig {
  // Default legacy until a console session proves adaptive better
  // (integ4, 2026-10-17: the first adaptive locked 30 83% of the time).
  Fps60Mode Mode = Fps60Mode::Legacy;
  // Lock at 30 after LockMisses presents of 2+ refreshes (a refresh with no
  // new picture) among the last MissWindow new pictures at 60.
  uint32_t LockMisses = 3U;
  uint32_t MissWindow = 60U;
  // Shortest stay at 30; doubles (up to MaxHoldSeconds) when 60 fails again
  // within RelapseSeconds of coming back.
  double MinHoldSeconds = 2.0;
  double MaxHoldSeconds = 16.0;
  double RelapseSeconds = 10.0;
  // Headroom to return to 60, fractions of the pair budget (2 refreshes).
  double GameMargin = 0.92;
  double GpuMargin = 0.90;
  // Lock only when 60 cannot hold anyway: the mean game-thread pair or the
  // GPU pair estimate above LockFraction of the pair budget. 1.15 (38 ms):
  // with the 60 Hz host loop a locked 30 was no more regular than legacy in
  // the simulation of integ4, so it is kept for real overload only.
  double LockFraction = 1.15;
};

// "adaptive", "strict60", "legacy" (case-insensitive), then optional
// key=value options. Unknown or empty text: adaptive with the defaults.
[[nodiscard]] inline Fps60ModeConfig
ParseFps60Mode(std::string_view text) noexcept {
  Fps60ModeConfig config;
  const auto lower = [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  };
  const auto equals = [&](std::string_view a, std::string_view b) {
    if (a.size() != b.size())
      return false;
    for (size_t index = 0U; index < a.size(); ++index)
      if (lower(a[index]) != lower(b[index]))
        return false;
    return true;
  };
  const auto number = [](std::string_view value, double &out) {
    double whole = 0.0;
    double fraction = 0.0;
    double divisor = 1.0;
    bool digits = false;
    bool point = false;
    for (const char c : value) {
      if (c >= '0' && c <= '9') {
        digits = true;
        if (point) {
          divisor *= 10.0;
          fraction += (c - '0') / divisor;
        } else {
          whole = whole * 10.0 + (c - '0');
        }
      } else if ((c == '.' || c == ',') && !point) {
        point = true;
      } else {
        return false;
      }
    }
    if (!digits)
      return false;
    out = whole + fraction;
    return true;
  };
  bool first = true;
  while (!text.empty()) {
    size_t start = 0U;
    while (start < text.size() &&
           (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' ||
            text[start] == '\n'))
      ++start;
    text.remove_prefix(start);
    if (text.empty())
      break;
    size_t end = 0U;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' &&
           text[end] != '\r' && text[end] != '\n')
      ++end;
    const auto token = text.substr(0U, end);
    text.remove_prefix(end);
    const auto equal = token.find('=');
    if (first && equal == std::string_view::npos) {
      first = false;
      if (equals(token, "strict60") || equals(token, "strict"))
        config.Mode = Fps60Mode::Strict60;
      else if (equals(token, "adaptive"))
        config.Mode = Fps60Mode::Adaptive;
      else if (equals(token, "legacy"))
        config.Mode = Fps60Mode::Legacy;
      continue;
    }
    first = false;
    if (equal == std::string_view::npos)
      continue;
    const auto key = token.substr(0U, equal);
    double value = 0.0;
    if (!number(token.substr(equal + 1U), value))
      continue;
    if (equals(key, "misses") && value >= 1.0 && value <= 1000.0)
      config.LockMisses = static_cast<uint32_t>(value);
    else if (equals(key, "window") && value >= 2.0 && value <= 240.0)
      config.MissWindow = static_cast<uint32_t>(value);
    else if (equals(key, "hold") && value >= 0.0 && value <= 600.0)
      config.MinHoldSeconds = value;
    else if (equals(key, "max_hold") && value >= 0.0 && value <= 3600.0)
      config.MaxHoldSeconds = value;
    else if (equals(key, "relapse") && value >= 0.0 && value <= 3600.0)
      config.RelapseSeconds = value;
    else if (equals(key, "game") && value > 0.0 && value <= 2.0)
      config.GameMargin = value;
    else if (equals(key, "gpu") && value > 0.0 && value <= 2.0)
      config.GpuMargin = value;
    else if (equals(key, "lock") && value > 0.0 && value <= 2.0)
      config.LockFraction = value;
  }
  config.LockMisses = std::min(config.LockMisses, config.MissWindow);
  config.MaxHoldSeconds = std::max(config.MaxHoldSeconds, config.MinHoldSeconds);
  return config;
}

enum class PresentCadence : uint8_t { Sixty = 0, Thirty = 1 };

// Guest refreshes a host frame runs (game thread). Phase: refreshes owed by
// the wall clock, the previous remainder plus this frame's elapsed time in
// refresh periods (NativePresentationScheduler).
struct NativeCadenceRefreshPlan {
  double Phase = 0.0;
  // This host frame's elapsed time alone, in refresh periods.
  double Elapsed = 1.0;
};
struct NativeCadenceRefreshDecision {
  uint32_t Due = 0U;
  uint32_t Dropped = 0U; // refreshes given up (the game falls behind)
  double Phase = 0.0;    // remainder for the next host frame
};

// Before config/fps60_mode (and "legacy"): every owed refresh, at most two
// per host frame, the rest dropped (NativeSimulationCatchUpPolicy
// DropExcess).
[[nodiscard]] inline NativeCadenceRefreshDecision
PlanLegacyRefreshes(const NativeCadenceRefreshPlan &plan) noexcept {
  NativeCadenceRefreshDecision decision;
  const double phase = std::max(0.0, plan.Phase);
  const auto owed = static_cast<uint32_t>(std::floor(phase + 1.0e-9));
  decision.Due = std::min<uint32_t>(owed, 2U);
  decision.Dropped = owed - decision.Due;
  decision.Phase = std::max(0.0, phase - static_cast<double>(owed));
  return decision;
}

// At 60: one refresh per host frame. A late host frame used to run two
// refreshes (a whole step, ~23 ms of game thread in the heavy stretches),
// which made the next one late too: the loop fell into an irregular 30 Hz
// rhythm of 1- and 2-refresh frames and stayed there. Now one refresh of
// debt is carried instead (the picture lags one refresh) and only a second
// one is dropped. A real stall (a host frame of 2.5+ refreshes: loading,
// shader compiles) is not that rhythm: it catches up as before.
[[nodiscard]] inline NativeCadenceRefreshDecision
PlanSixtyRefreshes(const NativeCadenceRefreshPlan &plan) noexcept {
  if (plan.Elapsed >= 2.5)
    return PlanLegacyRefreshes(plan);
  NativeCadenceRefreshDecision decision;
  const double phase = std::max(0.0, plan.Phase);
  const auto owed = static_cast<uint32_t>(std::floor(phase + 1.0e-9));
  decision.Due = std::min<uint32_t>(owed, 1U);
  double rest = phase - static_cast<double>(decision.Due);
  if (rest >= 2.0 - 1.0e-9) {
    decision.Dropped =
        static_cast<uint32_t>(std::floor(rest + 1.0e-9)) - 1U;
    rest -= static_cast<double>(decision.Dropped);
  }
  decision.Phase = rest;
  return decision;
}

// Locked 30: two refreshes per host frame, so every host frame completes
// one step (the game steps once every two refreshes) and the in-between
// host frame disappears with its overhead; presented every two refreshes.
// Up to one refresh is borrowed ahead so wall-clock jitter around 33.3 ms
// never splits a pair (a host frame of 32.9 ms still runs two).
[[nodiscard]] inline NativeCadenceRefreshDecision
PlanThirtyRefreshes(const NativeCadenceRefreshPlan &plan) noexcept {
  NativeCadenceRefreshDecision decision;
  const double phase = plan.Phase;
  decision.Due = phase >= 1.0 ? 2U : 0U;
  double rest = phase - static_cast<double>(decision.Due);
  if (rest >= 2.0 - 1.0e-9) {
    decision.Dropped =
        static_cast<uint32_t>(std::floor(rest + 1.0e-9)) - 1U;
    rest -= static_cast<double>(decision.Dropped);
  }
  decision.Phase = std::max(-1.0, rest);
  return decision;
}

// What a present showed (the render thread's frame kind, see
// RenderFrameKind): a new guest frame, an in-between picture, or the
// picture already on screen again (a dropped in-between frame, a reused
// snapshot).
enum class PresentedPicture : uint8_t { Step, InBetween, Repeat };

struct CadenceStats {
  // Presents 1, 2, 3 and 4+ refreshes after the previous present.
  std::array<uint64_t, 4> Intervals{};
  // New pictures (step or in-between) that stayed 1, 2, 3, 4+ refreshes.
  std::array<uint64_t, 4> Holds{};
  uint64_t Repeats = 0;
  // Consecutive pictures held for a different number of refreshes.
  uint64_t CadenceSwitches = 0;
  // At 60: a new picture 2+ refreshes after the previous present. At 30: a
  // step 3+ refreshes after the previous step.
  uint64_t LateSteps = 0;
  uint64_t LockChanges = 0;
  double ThirtySeconds = 0.0;
  // Part of ThirtySeconds forced by low clocks (SetClockThirty).
  double ClockThirtySeconds = 0.0;
  // Game-thread busy ms of each step pair (step host frame + in-between).
  double GamePairMsSum = 0.0;
  uint64_t GamePairs = 0;
};

class NativeCadenceGovernor {
public:
  static constexpr size_t kMaxMissWindow = 240U;
  static constexpr size_t kGamePairRing = 64U;

  void Configure(const Fps60ModeConfig &config, double refreshMs) noexcept {
    mConfig = config;
    mConfig.MissWindow = std::clamp<uint32_t>(
        mConfig.MissWindow, 2U, static_cast<uint32_t>(kMaxMissWindow));
    mConfig.LockMisses = std::clamp<uint32_t>(mConfig.LockMisses, 1U,
                                              mConfig.MissWindow);
    mRefreshMs = refreshMs > 0.0 ? refreshMs : 1000.0 / 60.0;
    mCadence = PresentCadence::Sixty;
    mMissRing = {};
    mMissIndex = 0U;
    mMissCount = 0U;
    mHoldSeconds = mConfig.MinHoldSeconds;
    mHasThirtyBefore = false;
    mSixtySince = -1.0;
    mThirtySince = 0.0;
    mLastLate = -1.0e9;
    mGameRing = {};
    mGameCount = 0U;
    mGameIndex = 0U;
  }

  [[nodiscard]] const Fps60ModeConfig &Config() const noexcept {
    return mConfig;
  }
  [[nodiscard]] PresentCadence Cadence() const noexcept { return mCadence; }
  // Every in-between frame dropped (the cadence is locked at 30, by the
  // adaptive governor or by low clocks).
  [[nodiscard]] bool DropsInBetween() const noexcept {
    return mClockThirty || mCadence == PresentCadence::Thirty;
  }
  // config/fps60_clock (oot3d_switch_clocks.h): the console runs below the
  // clocks 60 needs, so the cadence stays at 30 in any fps60_mode until the
  // clocks come back. Kept across Configure (it is the console's state).
  void SetClockThirty(bool thirty) noexcept { mClockThirty = thirty; }
  [[nodiscard]] bool ClockThirty() const noexcept { return mClockThirty; }
  // The per-frame governor may drop single in-between frames (legacy only).
  [[nodiscard]] bool PerFrameDropAllowed() const noexcept {
    return mConfig.Mode == Fps60Mode::Legacy;
  }
  [[nodiscard]] double HoldSeconds() const noexcept { return mHoldSeconds; }
  [[nodiscard]] const CadenceStats &Stats() const noexcept { return mStats; }

  // One host frame of the game thread: its busy ms (frame time minus the
  // waits for the renderer), the guest refreshes it ran and whether it
  // completed a guest frame (a step).
  // guestMs: the part of busyMs spent in the guest (its refreshes).
  void ObserveGameFrame(double busyMs, double guestMs, uint32_t refreshes,
                        bool step) noexcept {
    if (!(busyMs >= 0.0) || !std::isfinite(busyMs))
      return;
    if (!(guestMs >= 0.0) || !std::isfinite(guestMs))
      guestMs = 0.0;
    // Host-frame overhead (input, UI, submission): what an extra host frame
    // per step costs at 60 beyond the guest work.
    const double overhead = std::max(0.0, busyMs - guestMs);
    if (refreshes == 1U && !step)
      mOverheadEmaMs = mOverheadEmaMs <= 0.0
                           ? overhead
                           : (7.0 * mOverheadEmaMs + overhead) / 8.0;
    if (!step) {
      if (refreshes == 1U) {
        mLastInBetweenMs = busyMs;
        mHasLastInBetween = true;
      }
      return;
    }
    // Both refreshes in one host frame (a late frame, or the locked 30):
    // at 60 they take two host frames, one more overhead.
    double pair = busyMs;
    if (refreshes >= 2U)
      pair += mOverheadEmaMs > 0.0 ? mOverheadEmaMs : overhead;
    else if (mHasLastInBetween)
      pair += mLastInBetweenMs;
    else
      pair += busyMs;
    mHasLastInBetween = false;
    mGameRing[mGameIndex] = pair;
    mGameIndex = (mGameIndex + 1U) % kGamePairRing;
    mGameCount = std::min<size_t>(mGameCount + 1U, kGamePairRing);
    mStats.GamePairMsSum += pair;
    ++mStats.GamePairs;
  }

  // Mean of the recent game-thread step pairs (0: none yet).
  [[nodiscard]] double GamePairMeanMs() const noexcept {
    if (mGameCount == 0U)
      return 0.0;
    double sum = 0.0;
    for (size_t index = 0U; index < mGameCount; ++index)
      sum += mGameRing[index];
    return sum / static_cast<double>(mGameCount);
  }

  // 90th percentile of the recent game-thread step pairs (0: none yet).
  [[nodiscard]] double GamePairP90Ms() const noexcept {
    if (mGameCount == 0U)
      return 0.0;
    std::array<double, kGamePairRing> sorted{};
    std::copy_n(mGameRing.begin(), mGameCount, sorted.begin());
    const auto last = sorted.begin() + static_cast<std::ptrdiff_t>(mGameCount);
    const size_t rank = std::min(
        mGameCount - 1U,
        static_cast<size_t>(std::ceil(0.9 * static_cast<double>(mGameCount))) -
            1U);
    std::nth_element(sorted.begin(),
                     sorted.begin() + static_cast<std::ptrdiff_t>(rank), last);
    return sorted[rank];
  }

  // A present, right after the swap. nowSeconds: any monotonic clock.
  // gpuPairMs: the render cost of a step frame plus a full in-between frame
  // (InterpolationPacingGovernor estimates; 0 when unknown).
  void ObservePresent(PresentedPicture picture, double nowSeconds,
                      double gpuPairMs) noexcept {
    if (!mHasPresent) {
      mHasPresent = true;
      mLastPresent = nowSeconds;
      mLastStepPresent = nowSeconds;
      mHasPicture = picture != PresentedPicture::Repeat;
      mPictureRefreshes = 0U;
      if (mSixtySince < 0.0)
        mSixtySince = nowSeconds;
      return;
    }
    const double dt = std::max(0.0, nowSeconds - mLastPresent);
    mLastPresent = nowSeconds;
    const uint32_t refreshes = std::max<uint32_t>(
        1U, static_cast<uint32_t>(std::lround(dt * 1000.0 / mRefreshMs)));
    ++mStats.Intervals[std::min<size_t>(refreshes, 4U) - 1U];
    if (DropsInBetween())
      mStats.ThirtySeconds += dt;
    if (mClockThirty)
      mStats.ClockThirtySeconds += dt;
    mPictureRefreshes += refreshes;
    if (picture == PresentedPicture::Repeat) {
      ++mStats.Repeats;
      return;
    }
    // The previous picture stayed until now.
    if (mHasPicture) {
      const uint32_t hold = mPictureRefreshes;
      ++mStats.Holds[std::min<size_t>(hold, 4U) - 1U];
      if (mLastHold != 0U && hold != mLastHold)
        ++mStats.CadenceSwitches;
      mLastHold = hold;
    }
    mHasPicture = true;
    mPictureRefreshes = 0U;

    bool late = false;
    if (!DropsInBetween()) {
      late = refreshes >= 2U;
    } else if (picture == PresentedPicture::Step) {
      const double sinceStep = std::max(0.0, nowSeconds - mLastStepPresent);
      late = std::lround(sinceStep * 1000.0 / mRefreshMs) >= 3;
    }
    if (picture == PresentedPicture::Step)
      mLastStepPresent = nowSeconds;
    if (late) {
      ++mStats.LateSteps;
      mLastLate = nowSeconds;
    }
    // Forced by the clocks: the adaptive lock waits (its misses at 30 mean
    // nothing) and resumes at 60 when the clocks come back.
    if (mConfig.Mode != Fps60Mode::Adaptive || mClockThirty)
      return;

    if (mCadence == PresentCadence::Sixty) {
      // Sliding count of misses over the last MissWindow pictures.
      const bool old = mMissRing[mMissIndex];
      mMissRing[mMissIndex] = late;
      mMissIndex = (mMissIndex + 1U) % mConfig.MissWindow;
      mMissCount = mMissCount - (old ? 1U : 0U) + (late ? 1U : 0U);
      // Misses alone are not enough (integ4: a game pair of ~30.6 ms of
      // 33.3 missed often but kept ~54 FPS in legacy; locked it gave 38):
      // 60 must be out of reach on average too.
      const double budget = 2.0 * mRefreshMs;
      const bool overloaded =
          GamePairMeanMs() > mConfig.LockFraction * budget ||
          gpuPairMs > mConfig.LockFraction * budget;
      if (mMissCount >= mConfig.LockMisses && overloaded)
        EnterThirty(nowSeconds);
      return;
    }
    if (picture != PresentedPicture::Step)
      return;
    const double pairBudget = 2.0 * mRefreshMs;
    const bool held = nowSeconds - mThirtySince >= mHoldSeconds;
    const bool calm = nowSeconds - mLastLate >= mConfig.MinHoldSeconds;
    const double game = GamePairMeanMs();
    const bool gameFits = game > 0.0 && game <= mConfig.GameMargin * pairBudget;
    const bool gpuFits = gpuPairMs <= mConfig.GpuMargin * pairBudget;
    if (held && calm && gameFits && gpuFits)
      EnterSixty(nowSeconds);
  }

private:
  void EnterThirty(double now) noexcept {
    // Back at 30 soon after leaving it: stay longer this time.
    if (mHasThirtyBefore && now - mSixtySince < mConfig.RelapseSeconds)
      mHoldSeconds = std::min(2.0 * std::max(mHoldSeconds, 0.25),
                              mConfig.MaxHoldSeconds);
    else
      mHoldSeconds = mConfig.MinHoldSeconds;
    mHasThirtyBefore = true;
    mCadence = PresentCadence::Thirty;
    mThirtySince = now;
    ++mStats.LockChanges;
  }

  void EnterSixty(double now) noexcept {
    mCadence = PresentCadence::Sixty;
    mSixtySince = now;
    mMissRing = {};
    mMissIndex = 0U;
    mMissCount = 0U;
    ++mStats.LockChanges;
  }

  Fps60ModeConfig mConfig{};
  double mRefreshMs = 1000.0 / 60.0;
  PresentCadence mCadence = PresentCadence::Sixty;
  bool mClockThirty = false;
  std::array<bool, kMaxMissWindow> mMissRing{};
  size_t mMissIndex = 0U;
  uint32_t mMissCount = 0U;
  double mHoldSeconds = 2.0;
  bool mHasThirtyBefore = false;
  double mSixtySince = -1.0;
  double mThirtySince = 0.0;
  double mLastLate = -1.0e9;
  bool mHasPresent = false;
  double mLastPresent = 0.0;
  double mLastStepPresent = 0.0;
  bool mHasPicture = false;
  uint32_t mPictureRefreshes = 0U;
  uint32_t mLastHold = 0U;
  std::array<double, kGamePairRing> mGameRing{};
  size_t mGameCount = 0U;
  size_t mGameIndex = 0U;
  double mLastInBetweenMs = 0.0;
  bool mHasLastInBetween = false;
  double mOverheadEmaMs = 0.0;
  CadenceStats mStats{};
};

} // namespace Oot3dNativeGame
