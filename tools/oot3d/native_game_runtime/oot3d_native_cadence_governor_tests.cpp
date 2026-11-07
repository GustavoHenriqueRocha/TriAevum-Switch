// Presentation cadence of config/fps60 (oot3d_native_cadence_governor.h):
// the mode parser, the refresh plans of the game thread, the governor that
// locks 30, and a simulation of the console pipeline
// (oot3d_native_cadence_sim.h) on synthetic traces built from the console's
// perf.txt (switch-logs/integ2b), comparing legacy, strict60 and adaptive.
//
// Run with --report for the table (before/after) of every scenario.
#include "oot3d_native_cadence_governor.h"
#include "oot3d_native_cadence_sim.h"
#include "oot3d_native_interpolation_pacing.h"
#include "oot3d_native_rate_pacing.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
using CadenceSim::Params;
using CadenceSim::Result;
using CadenceSim::Segment;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr double kRefreshMs = 1000.0 / 60.0;

Fps60ModeConfig AdaptiveConfig() {
  Fps60ModeConfig config;
  config.Mode = Fps60Mode::Adaptive;
  return config;
}

void TestParseMode() {
  CHECK(ParseFps60Mode("").Mode == Fps60Mode::Legacy);
  CHECK(ParseFps60Mode("adaptive\n").Mode == Fps60Mode::Adaptive);
  CHECK(ParseFps60Mode("STRICT60").Mode == Fps60Mode::Strict60);
  CHECK(ParseFps60Mode("legacy\r\n").Mode == Fps60Mode::Legacy);
  CHECK(ParseFps60Mode("bogus").Mode == Fps60Mode::Legacy);
  const auto options =
      ParseFps60Mode("adaptive misses=5 window=90 hold=3 max_hold=20 game=0,8 "
                     "gpu=0.95 relapse=12");
  CHECK(options.LockMisses == 5U);
  CHECK(options.MissWindow == 90U);
  CHECK(std::abs(options.MinHoldSeconds - 3.0) < 1.0e-9);
  CHECK(std::abs(options.MaxHoldSeconds - 20.0) < 1.0e-9);
  CHECK(std::abs(options.GameMargin - 0.8) < 1.0e-9);
  CHECK(std::abs(options.GpuMargin - 0.95) < 1.0e-9);
  CHECK(std::abs(options.RelapseSeconds - 12.0) < 1.0e-9);
  // Out of range values keep the defaults; misses never above the window.
  const auto clamped = ParseFps60Mode("adaptive misses=0 game=7 window=4");
  CHECK(clamped.GameMargin == Fps60ModeConfig{}.GameMargin);
  CHECK(clamped.MissWindow == 4U);
  CHECK(clamped.LockMisses <= clamped.MissWindow);
  // Options without a mode: adaptive.
  CHECK(ParseFps60Mode("misses=4").LockMisses == 4U);
  CHECK(ParseFps60Mode("misses=4").Mode == Fps60Mode::Legacy);
}

void TestRefreshPlans() {
  // Legacy: every owed refresh up to two, the rest dropped.
  auto legacy = PlanLegacyRefreshes({1.2});
  CHECK(legacy.Due == 1U && legacy.Dropped == 0U &&
        std::abs(legacy.Phase - 0.2) < 1.0e-9);
  legacy = PlanLegacyRefreshes({2.1});
  CHECK(legacy.Due == 2U && legacy.Dropped == 0U);
  legacy = PlanLegacyRefreshes({3.5});
  CHECK(legacy.Due == 2U && legacy.Dropped == 1U);

  // Sixty: one per host frame; one refresh of debt kept, a second dropped.
  auto sixty = PlanSixtyRefreshes({1.0});
  CHECK(sixty.Due == 1U && sixty.Dropped == 0U && sixty.Phase < 1.0e-9);
  sixty = PlanSixtyRefreshes({0.4});
  CHECK(sixty.Due == 0U && std::abs(sixty.Phase - 0.4) < 1.0e-9);
  sixty = PlanSixtyRefreshes({2.3, 2.0}); // late: debt instead of a batch
  CHECK(sixty.Due == 1U && sixty.Dropped == 0U &&
        std::abs(sixty.Phase - 1.3) < 1.0e-9);
  sixty = PlanSixtyRefreshes({3.3, 2.0}); // with debt already: one dropped
  CHECK(sixty.Due == 1U && sixty.Dropped == 1U &&
        std::abs(sixty.Phase - 1.3) < 1.0e-9);
  // A stall (loading): catches up as legacy.
  sixty = PlanSixtyRefreshes({6.4, 6.4});
  CHECK(sixty.Due == 2U && sixty.Dropped == 4U);

  // Thirty: two per host frame, borrowing up to one refresh.
  auto thirty = PlanThirtyRefreshes({1.98});
  CHECK(thirty.Due == 2U && std::abs(thirty.Phase + 0.02) < 1.0e-9);
  thirty = PlanThirtyRefreshes({1.0});
  CHECK(thirty.Due == 2U && std::abs(thirty.Phase + 1.0) < 1.0e-9);
  thirty = PlanThirtyRefreshes({0.6});
  CHECK(thirty.Due == 0U);
  thirty = PlanThirtyRefreshes({4.2});
  CHECK(thirty.Due == 2U && thirty.Dropped == 1U &&
        std::abs(thirty.Phase - 1.2) < 1.0e-9);
  // Steady 33.3 ms host frames with jitter: always two, phase bounded.
  double phase = 0.0;
  uint32_t total = 0U;
  for (int frame = 0; frame < 3000; ++frame) {
    const double jitter = ((frame * 7919) % 11 - 5) * 0.05; // +-0.25 refresh
    const auto decision = PlanThirtyRefreshes({phase + 2.0 + jitter});
    CHECK(decision.Due == 2U);
    CHECK(decision.Dropped == 0U);
    total += decision.Due;
    phase = decision.Phase;
    CHECK(phase >= -1.0 && phase < 1.0);
  }
  CHECK(total == 6000U);
}

// Presents at given refresh offsets (picture kinds) into a governor.
struct Presenter {
  NativeCadenceGovernor &Governor;
  double Now = 0.0;
  void Present(PresentedPicture picture, uint32_t refreshes,
               double gpuPairMs = 20.0) {
    Now += refreshes * kRefreshMs / 1000.0;
    Governor.ObservePresent(picture, Now, gpuPairMs);
  }
};

void FeedGamePairs(NativeCadenceGovernor &governor, double pairMs, int pairs) {
  for (int pair = 0; pair < pairs; ++pair) {
    governor.ObserveGameFrame(pairMs / 2.0, pairMs / 2.0 - 2.5, 1U, false);
    governor.ObserveGameFrame(pairMs / 2.0, pairMs / 2.0 - 2.5, 1U, true);
  }
}

void TestHoldCounters() {
  NativeCadenceGovernor governor;
  Fps60ModeConfig config;
  config.Mode = Fps60Mode::Legacy;
  governor.Configure(config, kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  // 60: step, in-between, step, in-between: holds of 1.
  for (int pair = 0; pair < 10; ++pair) {
    presenter.Present(PresentedPicture::InBetween, 1);
    presenter.Present(PresentedPicture::Step, 1);
  }
  // Dropped in-between frames: holds of 2 (the repeat counts as a present).
  for (int pair = 0; pair < 10; ++pair) {
    presenter.Present(PresentedPicture::Repeat, 1);
    presenter.Present(PresentedPicture::Step, 1);
  }
  // A late step: the previous picture held 3.
  presenter.Present(PresentedPicture::Repeat, 1);
  presenter.Present(PresentedPicture::Step, 2);
  presenter.Present(PresentedPicture::InBetween, 1);
  const auto &stats = governor.Stats();
  // 20 at 60, then the step before the late one: held 1 again.
  CHECK(stats.Holds[0] == 21U);
  CHECK(stats.Holds[1] == 10U);
  CHECK(stats.Holds[2] == 1U);
  CHECK(stats.Repeats == 11U);
  CHECK(stats.Intervals[0] == 42U);
  CHECK(stats.Intervals[1] == 1U);
  // 1 -> 2, 2 -> 3 and 3 -> 1.
  CHECK(stats.CadenceSwitches == 3U);
  // Legacy never locks.
  CHECK(!governor.DropsInBetween());
  CHECK(governor.PerFrameDropAllowed());
}

void TestLocksThirtyOnRepeatedMisses() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  CHECK(!governor.PerFrameDropAllowed());
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  // Clean 60: never locks.
  for (int pair = 0; pair < 300; ++pair) {
    presenter.Present(PresentedPicture::InBetween, 1);
    presenter.Present(PresentedPicture::Step, 1);
  }
  CHECK(governor.Cadence() == PresentCadence::Sixty);
  FeedGamePairs(governor, 40.0, 64);
  // Two misses in a second: stays.
  presenter.Present(PresentedPicture::Step, 2);
  for (int pair = 0; pair < 10; ++pair) {
    presenter.Present(PresentedPicture::InBetween, 1);
    presenter.Present(PresentedPicture::Step, 1);
  }
  presenter.Present(PresentedPicture::Step, 2);
  CHECK(governor.Cadence() == PresentCadence::Sixty);
  // The third within the window: locked.
  presenter.Present(PresentedPicture::InBetween, 1);
  presenter.Present(PresentedPicture::Step, 2);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  CHECK(governor.DropsInBetween());
  CHECK(governor.Stats().LockChanges == 1U);
}

void TestMissesWithoutOverloadDoNotLock() {
  // integ4: a game pair of 30.6 ms of 33.3 misses often, but legacy kept
  // ~54 FPS; locking gave 38.
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  FeedGamePairs(governor, 30.6, 64);
  for (int miss = 0; miss < 20; ++miss)
    presenter.Present(PresentedPicture::Step, 2);
  CHECK(governor.Cadence() == PresentCadence::Sixty);
}

void TestMissesSpreadOutDoNotLock() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  // One miss every 40 pictures: never three among 60.
  for (int round = 0; round < 50; ++round) {
    for (int picture = 0; picture < 39; ++picture)
      presenter.Present(picture % 2 == 0 ? PresentedPicture::InBetween
                                         : PresentedPicture::Step,
                        1);
    presenter.Present(PresentedPicture::Step, 2);
  }
  CHECK(governor.Cadence() == PresentCadence::Sixty);
}

// Locks: misses while the game thread cannot hold 60 (pair of 35 ms), then
// a light game thread (20 ms) for the way back.
void LockNow(NativeCadenceGovernor &governor, Presenter &presenter) {
  FeedGamePairs(governor, 40.0, 64);
  for (int miss = 0; miss < 3; ++miss)
    presenter.Present(PresentedPicture::Step, 2);
  FeedGamePairs(governor, 20.0, 64);
}

void TestReturnsOnlyWithProvenHeadroom() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  LockNow(governor, presenter);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // Heavy game thread (32 ms per pair): stays at 30 however long.
  FeedGamePairs(governor, 32.0, 64);
  for (int step = 0; step < 600; ++step)
    presenter.Present(PresentedPicture::Step, 2, 20.0);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // Light game thread but a GPU pair over 90%: stays.
  FeedGamePairs(governor, 22.0, 64);
  for (int step = 0; step < 300; ++step)
    presenter.Present(PresentedPicture::Step, 2, 31.5);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // Both fit: back to 60 at once (the hold has passed).
  presenter.Present(PresentedPicture::Step, 2, 25.0);
  CHECK(governor.Cadence() == PresentCadence::Sixty);
  CHECK(governor.Stats().LockChanges == 2U);
}

void TestHoldAndLateSteps() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  FeedGamePairs(governor, 20.0, 64);
  LockNow(governor, presenter);
  // Within the 2 s hold: stays although everything fits.
  for (int step = 0; step < 50; ++step) // 1.67 s
    presenter.Present(PresentedPicture::Step, 2, 20.0);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // A late step (held 3) restarts the calm period.
  presenter.Present(PresentedPicture::Step, 3, 20.0);
  CHECK(governor.Stats().LateSteps >= 1U);
  for (int step = 0; step < 50; ++step)
    presenter.Present(PresentedPicture::Step, 2, 20.0);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // Back to 60 two seconds after the late step (65 steps = 2.17 s).
  bool returned = false;
  for (int step = 0; step < 15 && !returned; ++step) {
    presenter.Present(PresentedPicture::Step, 2, 20.0);
    returned = governor.Cadence() == PresentCadence::Sixty;
  }
  CHECK(returned);
}

void TestRelapseDoublesTheHold() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  FeedGamePairs(governor, 20.0, 64);
  double expected = 2.0;
  for (int relapse = 0; relapse < 6; ++relapse) {
    LockNow(governor, presenter);
    CHECK(governor.Cadence() == PresentCadence::Thirty);
    CHECK(std::abs(governor.HoldSeconds() - expected) < 1.0e-9);
    while (governor.Cadence() == PresentCadence::Thirty)
      presenter.Present(PresentedPicture::Step, 2, 20.0);
    expected = std::min(2.0 * expected, 16.0);
  }
  // A long stay at 60 forgives: the next lock holds 2 s again.
  for (int picture = 0; picture < 1200; ++picture) // 20 s
    presenter.Present(picture % 2 ? PresentedPicture::Step
                                  : PresentedPicture::InBetween,
                      1);
  LockNow(governor, presenter);
  CHECK(std::abs(governor.HoldSeconds() - 2.0) < 1.0e-9);
}

void TestStrictAndLegacyNeverLock() {
  for (auto mode : {Fps60Mode::Strict60, Fps60Mode::Legacy}) {
    NativeCadenceGovernor governor;
    Fps60ModeConfig config;
    config.Mode = mode;
    governor.Configure(config, kRefreshMs);
    Presenter presenter{governor};
    presenter.Present(PresentedPicture::Step, 1);
    for (int miss = 0; miss < 100; ++miss)
      presenter.Present(PresentedPicture::Step, 2);
    CHECK(governor.Cadence() == PresentCadence::Sixty);
    CHECK(governor.PerFrameDropAllowed() == (mode == Fps60Mode::Legacy));
  }
}

void TestGamePairEstimate() {
  NativeCadenceGovernor governor;
  governor.Configure(AdaptiveConfig(), kRefreshMs);
  CHECK(governor.GamePairP90Ms() == 0.0);
  // At 60: in-between host frame 12 ms (2.5 overhead), step 14 ms: pair 26.
  for (int pair = 0; pair < 64; ++pair) {
    governor.ObserveGameFrame(12.0, 9.5, 1U, false);
    governor.ObserveGameFrame(14.0, 10.0, 1U, true);
  }
  CHECK(std::abs(governor.GamePairP90Ms() - 26.0) < 1.0e-6);
  // Locked 30: one host frame of 23.5 ms with both refreshes; at 60 it
  // would cost one host-frame overhead more (2.5 ms): 26.
  for (int pair = 0; pair < 64; ++pair)
    governor.ObserveGameFrame(23.5, 19.5, 2U, true);
  CHECK(std::abs(governor.GamePairP90Ms() - 26.0) < 1.0e-6);
  // p90: a tenth of the pairs heavier does not move it, more does.
  for (int pair = 0; pair < 64; ++pair)
    governor.ObserveGameFrame(pair % 16 == 0 ? 33.5 : 23.5, 20.0, 2U, true);
  CHECK(governor.GamePairP90Ms() < 26.5);
  for (int pair = 0; pair < 64; ++pair)
    governor.ObserveGameFrame(pair % 4 == 0 ? 33.5 : 23.5, 20.0, 2U, true);
  CHECK(governor.GamePairP90Ms() > 35.0);
}

void TestPerFrameGovernorWithoutDrop() {
  InterpolationPacingGovernor governor;
  governor.Configure(InterpolationPacingConfig{}, 2.0 * kRefreshMs, 2.0F,
                     false);
  // Over budget: legacy drops...
  for (int pair = 0; pair < 50; ++pair) {
    governor.ObserveStep(19.0);
    governor.ObserveInterpolated(governor.Decide(), 19.0);
  }
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  // ...and without dropping it draws (and leaves the dropped level at once).
  governor.SetDropAllowed(false);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  for (int pair = 0; pair < 200; ++pair) {
    governor.ObserveStep(19.0);
    const auto level = governor.Decide();
    CHECK(level != InterpolatedSampleLevel::Dropped);
    governor.ObserveInterpolated(level, 19.0);
  }
}

// --- simulation ---------------------------------------------------------

Segment Light(double seconds) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = 13.0;
  segment.GameStepSdMs = 1.5;
  segment.GpuMs = 12.0;
  segment.RtStepMaxMs = 12.0;
  return segment;
}
Segment Normal(double seconds) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = 18.0;
  segment.GameStepSdMs = 2.5;
  segment.GpuMs = 14.0;
  return segment;
}
// host_frames 10350-13050 of integ2b: game thread ~27-28 ms per pair, GPU
// ~16.5 ms per replay, core 0 at 80-93%.
Segment Heavy(double seconds) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = 21.0;
  segment.GameStepSdMs = 3.0;
  segment.GpuMs = 16.5;
  segment.GpuSdMs = 0.6;
  segment.RtStepMaxMs = 17.0;
  return segment;
}
// host_frames 10950-11100 / 13950: ~31 ms per pair, core 0 at 90-95%.
Segment VeryHeavy(double seconds) {
  Segment segment = Heavy(seconds);
  segment.GameStepMs = 24.0;
  segment.GpuMs = 17.0;
  return segment;
}

// A session like integ2b: light start, normal play, the heavy stretch, a
// very heavy moment, back to normal.
std::vector<Segment> Session() {
  return {Light(30), Normal(60), Heavy(40), VeryHeavy(20), Normal(40),
          Light(30)};
}

Result Simulate(const std::vector<Segment> &segments, Fps60Mode mode,
                uint32_t swapQueue = 0U, uint32_t framesInFlight = 1U) {
  Params params;
  params.Mode.Mode = mode;
  params.SwapQueue = swapQueue;
  params.FramesInFlight = framesInFlight;
  // These scenarios were tuned on the bare pair (before
  // config/interp_margin_ms).
  params.Pacing.MarginMs = 0.0;
  return CadenceSim::Run(segments, params);
}

void Print(const char *name, Fps60Mode mode, const Result &result) {
  const auto &c = result.Cadence;
  const double seconds = result.Seconds;
  std::printf(
      "%-10s %-8s fps=%5.1f pics/s=%5.1f game=%5.1f refr/s  "
      "intervals(1/2/3/4+)=%llu/%llu/%llu/%llu  "
      "hold(1/2/3/4+)=%llu/%llu/%llu/%llu  switches/s=%5.2f  "
      "deviation=%4.1f ms  judder=%4.1f ms  at30=%3.0f%%  stalls=%llu\n",
      name, Fps60ModeName(mode).data(),
      static_cast<double>(result.Presents) / seconds,
      static_cast<double>(result.Pictures) / seconds,
      result.GuestRefreshesPerSecond,
      static_cast<unsigned long long>(c.Intervals[0]),
      static_cast<unsigned long long>(c.Intervals[1]),
      static_cast<unsigned long long>(c.Intervals[2]),
      static_cast<unsigned long long>(c.Intervals[3]),
      static_cast<unsigned long long>(c.Holds[0]),
      static_cast<unsigned long long>(c.Holds[1]),
      static_cast<unsigned long long>(c.Holds[2]),
      static_cast<unsigned long long>(c.Holds[3]),
      static_cast<double>(c.CadenceSwitches) / seconds,
      result.PictureIntervalStdMs, result.JudderRmsMs,
      100.0 * c.ThirtySeconds / seconds,
      static_cast<unsigned long long>(c.LockChanges));
}

void TestSimulationLightStaysAtSixty() {
  for (uint32_t queue : {0U, 1U}) {
    const auto result = Simulate({Light(30)}, Fps60Mode::Adaptive, queue);
    CHECK(result.Pictures >= 59U * 30U);
    CHECK(result.Cadence.LockChanges == 0U);
    CHECK(result.JudderRmsMs < 0.5);
  }
}

// integ4 (CPU 1785): game-thread pair ~30.6 ms of 33.3, GPU ~15 ms.
Segment Integ4(double seconds) {
  Segment segment = Heavy(seconds);
  segment.GameStepMs = 24.0;
  segment.GpuMs = 15.0;
  return segment;
}
// Over the pair on average: 60 is out of reach.
Segment Overloaded(double seconds) {
  Segment segment = Heavy(seconds);
  segment.GameStepMs = 33.0;
  return segment;
}

void TestSimulationNearTheLimitKeepsTheAverage() {
  for (uint32_t queue : {0U, 1U}) {
    const auto before = Simulate({Integ4(60)}, Fps60Mode::Legacy, queue);
    const auto after = Simulate({Integ4(60)}, Fps60Mode::Adaptive, queue);
    // No lock at 30 below the pair budget, and no worse than legacy.
    CHECK(after.Cadence.ThirtySeconds < 0.05 * after.Seconds);
    CHECK(static_cast<double>(after.Pictures) / after.Seconds >=
          static_cast<double>(before.Pictures) / before.Seconds - 1.0);
    CHECK(after.Cadence.CadenceSwitches <=
          before.Cadence.CadenceSwitches + before.Cadence.CadenceSwitches / 10U);
    CHECK(after.GuestRefreshesPerSecond > 59.5);
  }
}

void TestSimulationOverloadedLocksThirty() {
  const auto after = Simulate({Overloaded(60)}, Fps60Mode::Adaptive);
  CHECK(after.Cadence.ThirtySeconds > 0.8 * after.Seconds);
  CHECK(after.GuestRefreshesPerSecond > 53.0); // as legacy: the game itself is late
}

void TestSimulationSessionIsMoreRegular() {
  for (uint32_t queue : {0U, 1U}) {
    const auto before = Simulate(Session(), Fps60Mode::Legacy, queue);
    const auto after = Simulate(Session(), Fps60Mode::Adaptive, queue);
    // Blocking swap: legacy judders (holds of 1, 2 and 3 mixed); with a
    // queued swap it mostly alternates long runs of 60 and of drops.
    if (queue == 0U)
      CHECK(after.JudderRmsMs <= before.JudderRmsMs + 0.2);
    CHECK(after.GuestRefreshesPerSecond > 59.5);
  }
  // Strict60 never drops a frame on purpose.
  const auto strict = Simulate(Session(), Fps60Mode::Strict60);
  CHECK(strict.InBetweenDropped == 0U);
}

// Tour tour2 (2026-10-22, CPU 1785, gpu_timing): slow areas with the game
// pair at 21-24 ms of 33.3 and the GPU (glperf, without the window-acquire
// bubble) at 12.3 ms per step and 10 ms per in-between; render thread
// ~10 ms of work per step and ~5 ms per in-between.
Segment SlowArea(double seconds, double gameStepMs) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = gameStepMs;
  segment.GameStepSdMs = 2.5;
  segment.GpuMs = 12.3;
  segment.GpuSdMs = 1.5;
  segment.RtStepMinMs = 8.0;
  segment.RtStepMaxMs = 12.0;
  segment.RtFullMinMs = 3.0;
  segment.RtFullMaxMs = 6.0;
  return segment;
}

// config/frames_in_flight=2: with the game thread's step host frame longer
// than a refresh and the in-between one short, a second pending frame lets
// the pair average out instead of missing the step's refresh.
void TestSecondFrameInFlightKeepsSlowAreasAtSixty() {
  for (uint32_t queue : {0U, 1U}) {
    for (double step : {15.0, 18.0}) {
      const auto one =
          Simulate({SlowArea(30, step)}, Fps60Mode::Legacy, queue, 1U);
      const auto two =
          Simulate({SlowArea(30, step)}, Fps60Mode::Legacy, queue, 2U);
      CHECK(two.Pictures >= one.Pictures);
      CHECK(two.GuestRefreshesPerSecond >= one.GuestRefreshesPerSecond - 0.5);
    }
  }
}

// tour14 (2026-10-23, pgo5i, GPU 614): a heavy area whose work fits the
// pair. GPU ~14.5 ms per replay, render thread 6-10 ms of work per step and
// 5-8 per in-between, game thread ~10 ms per step. The EGL window has 3
// buffers and the render thread blocks at the window acquire.
Segment Tour14Heavy(double seconds, double gpuMs) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = 10.0;
  segment.GameStepSdMs = 1.5;
  segment.GpuMs = gpuMs;
  segment.GpuSdMs = 1.2;
  segment.RtStepMinMs = 6.0;
  segment.RtStepMaxMs = 10.0;
  segment.RtFullMinMs = 5.0;
  segment.RtFullMaxMs = 8.0;
  segment.SpikeProbability = 0.0;
  return segment;
}

Result SimulateSwapchain(const std::vector<Segment> &segments,
                         bool countWaits, double marginMs = 0.0,
                         bool split = false) {
  Params params;
  params.SplitWaitByGpu = split;
  params.Pacing.MarginMs = marginMs;
  params.Mode.Mode = Fps60Mode::Legacy;
  params.SwapchainBuffers = 3U;
  params.CountRenderWaits = countWaits;
  // Display 0.2% slower than the pacer: the swapchain fills within seconds.
  params.DisplayRefreshMs = 1000.0 / 59.88;
  return CadenceSim::Run(segments, params);
}

// Regression (tour14): the render thread's acquire wait counted as its cost
// dropped in-between frames whose GPU and render-thread work fit the pair.
void TestRenderWaitIsNotCost() {
  for (double gpu : {13.5, 14.5}) {
    const auto before = SimulateSwapchain({Tour14Heavy(60, gpu)}, true);
    const auto after = SimulateSwapchain({Tour14Heavy(60, gpu)}, false);
    const double total =
        static_cast<double>(after.InBetweenDrawn + after.InBetweenDropped);
    CHECK(after.InBetweenDropped <= before.InBetweenDropped);
    CHECK(static_cast<double>(after.InBetweenDropped) < 0.05 * total);
    CHECK(after.Pictures >= before.Pictures);
    CHECK(after.GuestRefreshesPerSecond > 59.5);
  }
  // Over the GPU pair the drops stay (only waits were taken out).
  const auto over = SimulateSwapchain({Tour14Heavy(30, 18.0)}, false);
  CHECK(over.InBetweenDropped > over.InBetweenDrawn);
}

// tour15 (2026-10-23, pgo5j): a heavy area by its measured GPU per replay;
// render thread ~8.5 ms of work per step, ~6.5 per in-between.
Segment Tour15(double seconds, double gpuMs, double hiddenMs) {
  Segment segment = Tour14Heavy(seconds, gpuMs);
  segment.RtStepMinMs = 7.0;
  segment.RtStepMaxMs = 10.0;
  segment.RtFullMinMs = 5.0;
  segment.RtFullMaxMs = 8.0;
  segment.GpuSdMs = 1.0;
  segment.GpuHiddenMs = hiddenMs;
  return segment;
}

void SweepMargin() {
  for (bool split : {false, true}) {
  std::printf("margin (split %d) | per GPU measured/replay: fps pics\n", split ? 1 : 0);
  for (double margin : {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0}) {
    std::printf("%4.1f ms:", margin);
    double fps = 0.0, pics = 0.0;
    int n = 0;
    for (double gpu : {11.0, 12.0, 12.5, 13.15, 13.6, 14.0, 14.5, 14.95, 15.25, 16.4}) {
      const auto r = SimulateSwapchain({Tour15(60, gpu, 2.75)}, false, margin, split);
      std::printf(" %.2f:%.1f/%.1f", gpu, r.Presents / r.Seconds, r.Pictures / r.Seconds);
      fps += r.Presents / r.Seconds;
      pics += r.Pictures / r.Seconds;
      ++n;
    }
    std::printf("  | average %.2f/%.1f\n", fps / n, pics / n);
  }
  }
  std::printf("counts wait, margin 0 (pgo5i):");
  for (double gpu : {11.0, 12.0, 12.5, 13.15, 13.6, 14.0, 14.5, 14.95, 15.25, 16.4}) {
    const auto r = SimulateSwapchain({Tour15(60, gpu, 2.75)}, true, 0.0);
    std::printf(" %.2f:%.1f/%.1f", gpu, r.Presents / r.Seconds, r.Pictures / r.Seconds);
  }
  std::printf("\n");
}

// GpuHiddenMs against tour15: 2.5-3 ms reproduces 55.8 FPS at a measured
// 14.95 ms per replay (Kakariko) and 59+ at 13.15-13.6.
void Calibrate() {
  for (double hidden : {0.0, 1.0, 1.5, 2.0, 2.5, 3.0}) {
    std::printf("hidden %.1f:", hidden);
    for (double gpu : {13.15, 13.6, 14.95, 15.25, 16.4}) {
      const auto r = SimulateSwapchain({Tour15(60, gpu, hidden)}, false, 0.0);
      const auto w = SimulateSwapchain({Tour15(60, gpu, hidden)}, true, 0.0);
      std::printf("  g%.2f fps %.1f pics %.1f | counts %.1f/%.1f", gpu,
                  r.Presents / r.Seconds, r.Pictures / r.Seconds,
                  w.Presents / w.Seconds, w.Pictures / w.Seconds);
    }
    std::printf("\n");
  }
}

// Regression (tour15, pgo5j): without the wait in the cost, measured pairs
// of 29-33 ms were kept and the area lost 3-5 FPS; the margin of that mode
// (5 ms) keeps the refresh there and still draws every in-between frame at 12.5.
void TestMarginKeepsTheRefresh() {
  const double margin = Oot3dNativeGame::kExcludedWaitMarginMs;
  for (double gpu : {14.95, 15.25, 16.4}) {
    const auto bare = SimulateSwapchain({Tour15(60, gpu, 2.75)}, false, 0.0);
    const auto kept = SimulateSwapchain({Tour15(60, gpu, 2.75)}, false, margin);
    CHECK(kept.Presents / kept.Seconds > 59.7);
    CHECK(kept.Presents >= bare.Presents);
  }
  CHECK(SimulateSwapchain({Tour15(60, 14.95, 2.75)}, false, 0.0).Presents /
            60.0 < 58.0);
  const auto light = SimulateSwapchain({Tour15(60, 12.5, 2.75)}, false, margin);
  CHECK(light.Pictures / light.Seconds > 59.5);
}

void ReportRenderWaits() {
  std::printf("\n== tour14: acquire wait in the cost (swapchain of 3) ==\n");
  for (double gpu : {12.5, 13.5, 14.5, 15.5, 16.5}) {
    for (bool count : {true, false}) {
      const auto result = SimulateSwapchain({Tour14Heavy(60, gpu)}, count);
      char name[32];
      std::snprintf(name, sizeof(name), "gpu%.1f/%s", gpu,
                    count ? "counts" : "busy");
      Print(name, Fps60Mode::Legacy, result);
      std::printf("           in_between_drawn=%llu dropped=%llu "
                  "acquire_wait=%.2f ms/frame\n",
                  static_cast<unsigned long long>(result.InBetweenDrawn),
                  static_cast<unsigned long long>(result.InBetweenDropped),
                  result.AcquireWaitMs /
                      static_cast<double>(std::max<uint64_t>(1U, result.HostFrames)));
    }
  }
}

void ReportFramesInFlight() {
  for (uint32_t queue : {0U, 1U}) {
    std::printf("\n== slow areas (tour2), swap %s ==\n",
                queue == 0U ? "blocking" : "with a 1-frame queue");
    for (double step : {12.0, 15.0, 18.0, 21.0, 24.0}) {
      for (uint32_t inFlight : {1U, 2U}) {
        char name[32];
        std::snprintf(name, sizeof(name), "g%.0f/f%u", step, inFlight);
        Print(name, Fps60Mode::Legacy,
              Simulate({SlowArea(60, step)}, Fps60Mode::Legacy, queue,
                       inFlight));
      }
    }
  }
}

// tour20/tour22 (pgo5o, 24/10): outside loads the missed refreshes are
// isolated step frames whose game thread ran 20-29 ms (guest 17-24) while the
// following in-between frame cost ~3: the pair fits 33.3 ms, the step does
// not. Most of the guest step closes in one refresh.
Segment Tour20Area(double seconds, double stepMs) {
  Segment segment;
  segment.Seconds = seconds;
  segment.GameStepMs = stepMs;
  segment.GameStepSdMs = 2.5;
  segment.GpuMs = 12.5;
  segment.GpuSdMs = 1.5;
  segment.RtStepMinMs = 8.0;
  segment.RtStepMaxMs = 14.0;
  segment.RtFullMinMs = 5.0;
  segment.RtFullMaxMs = 8.0;
  segment.SpikeProbability = 0.03;
  segment.SpikeMs = 8.0;
  return segment;
}

enum class SlotMode { One, Early, EarlyClock, Two };

Result SimulateSlots(const std::vector<Segment> &segments, SlotMode slot) {
  Params params;
  params.Mode.Mode = Fps60Mode::Legacy;
  params.Pacing.MarginMs = 0.0;
  params.CloseFraction = 0.85;
  params.FramesInFlight = slot == SlotMode::Two ? 2U : 1U;
  params.ReleaseSlotAtPacer =
      slot == SlotMode::Early || slot == SlotMode::EarlyClock;
  params.RefreshClockFromDeadline = slot == SlotMode::EarlyClock;
  return CadenceSim::Run(segments, params);
}

// config/early_frame_slot: handing the frame slot back when the pacer starts
// waiting absorbs most isolated long steps (the game thread starts the next
// host frame a pacer wait earlier), without the second frame in flight
// (which corrupted the heap on the console, tour3b). With the refresh clock
// on the pacer deadline it keeps the game speed.
void TestEarlyFrameSlotAbsorbsLongSteps() {
  for (double step : {16.0, 18.0, 20.0}) {
    const auto one = SimulateSlots({Tour20Area(60, step)}, SlotMode::One);
    const auto early =
        SimulateSlots({Tour20Area(60, step)}, SlotMode::EarlyClock);
    CHECK(early.MissedFrames < one.MissedFrames);
    CHECK(early.Pictures >= one.Pictures);
    CHECK(early.GuestRefreshesPerSecond >= one.GuestRefreshesPerSecond - 0.5);
  }
  // Without long steps nothing changes for the worse.
  const auto oneLight = SimulateSlots({Light(30)}, SlotMode::One);
  const auto earlyLight = SimulateSlots({Light(30)}, SlotMode::EarlyClock);
  CHECK(earlyLight.MissedFrames <= oneLight.MissedFrames);
  CHECK(earlyLight.GuestRefreshesPerSecond >=
        oneLight.GuestRefreshesPerSecond - 0.5);
}

void ReportFrameSlots() {
  std::printf("\n== tour20: isolated long steps, frame slot ==\n");
  static constexpr const char *kNames[] = {"f1", "early", "early+clk", "f2"};
  for (double step : {10.0, 11.0, 12.0, 13.0, 14.0, 16.0, 18.0, 20.0}) {
    for (auto slot : {SlotMode::One, SlotMode::Early, SlotMode::EarlyClock,
                      SlotMode::Two}) {
      const auto result = SimulateSlots({Tour20Area(60, step)}, slot);
      std::printf("g%.0f %-9s missed/min=%6.1f refr_missed/min=%6.1f "
                  "pics/s=%5.1f game=%5.1f refr/s interm=%llu/%llu held=%llu\n",
                  step, kNames[static_cast<int>(slot)],
                  60.0 * static_cast<double>(result.MissedFrames) /
                      result.Seconds,
                  60.0 * static_cast<double>(result.MissedRefreshes) /
                      result.Seconds,
                  static_cast<double>(result.Pictures) / result.Seconds,
                  result.GuestRefreshesPerSecond,
                  static_cast<unsigned long long>(result.InBetweenDrawn),
                  static_cast<unsigned long long>(result.InBetweenDropped),
                  static_cast<unsigned long long>(result.HeldPictureRefreshes));
    }
  }
}

// L3+R3 > Video > FPS applied while the game runs (oot3d_native_rate_pacing.h):
// the in-between governor, dynamic resolution and the cadence governor of a
// session that ran at one rate, reconfigured for the other at a frame
// boundary, must behave exactly as a start at the new rate (same budgets,
// interpolation level, scale, lock, holds), whatever they learned before;
// the one exception is the scale a switch to 60 begins at
// (RatePacingResolutionMemory), given to both as their start.
struct RateRig {
  InterpolationPacingGovernor Pacing;
  DynamicResolutionController Drs;
  NativeCadenceGovernor Cadence;
  double Now = 0.0;
};

RatePacingConfig LiveRateConfig() {
  RatePacingConfig config;
  config.Interpolation.KeepGameSpeed = true;
  config.Interpolation.ReducedScale = 2.0F;
  config.BaseScale = 3.0F;
  config.ReducedAvailable = true;
  config.Resolution.MaxScale = 3.0F;
  config.Resolution.MinScale = 2.25F;
  config.Cadence = AdaptiveConfig();
  return config;
}

// `steps` guest steps at `rateHz` under a load (GPU ms of a step frame, ms
// of the game thread's step pair), modelled like the console: at 60 a step
// and an in-between picture (or the step again when the in-between is
// dropped), at 30 one step picture per period. Returns every decision and
// estimate in order, and the counters this drive added.
std::vector<double> DriveRate(RateRig &rig, uint32_t rateHz, double stepMs,
                              double gamePairMs, int steps) {
  std::vector<double> trace;
  const CadenceStats before = rig.Cadence.Stats();
  const double period = 1.0 / static_cast<double>(rateHz);
  const bool interpolating = rateHz == 60U;
  const auto present = [&](PresentedPicture picture, uint32_t periods) {
    rig.Now += periods * period;
    const double gpuPair =
        rig.Pacing.StepCostMs() > 0.0
            ? rig.Pacing.StepCostMs() +
                  rig.Pacing.Estimate(InterpolatedSampleLevel::Full)
            : 0.0;
    rig.Cadence.ObservePresent(picture, rig.Now, gpuPair);
  };
  uint32_t stepPeriods = 1U;
  for (int step = 0; step < steps; ++step) {
    rig.Cadence.ObserveGameFrame(gamePairMs / 2.0, gamePairMs / 2.0 - 2.5, 1U,
                                 false);
    rig.Cadence.ObserveGameFrame(gamePairMs / 2.0, gamePairMs / 2.0 - 2.5, 1U,
                                 true);
    rig.Pacing.ObserveStep(stepMs);
    const bool shedding =
        interpolating &&
        (rig.Pacing.Level() != rig.Pacing.PreferredLevel() ||
         rig.Cadence.DropsInBetween());
    (void)rig.Drs.ObservePair(interpolating ? 2.0 * stepMs : stepMs, shedding);
    // The scene gets cheaper with the scale (the controller's own model).
    const double scaledStepMs =
        stepMs * rig.Drs.CostRatio(rig.Drs.Config().MaxScale, rig.Drs.Scale());
    present(PresentedPicture::Step, stepPeriods);
    if (interpolating) {
      const auto level = rig.Cadence.DropsInBetween()
                             ? InterpolatedSampleLevel::Dropped
                             : rig.Pacing.Decide();
      const double inBetweenMs =
          level == InterpolatedSampleLevel::Full      ? scaledStepMs
          : level == InterpolatedSampleLevel::Reduced ? 0.6 * scaledStepMs
                                                      : 0.5;
      rig.Pacing.ObserveInterpolated(level, inBetweenMs);
      present(level == InterpolatedSampleLevel::Dropped
                  ? PresentedPicture::Repeat
                  : PresentedPicture::InBetween,
              1U);
      // A pair over two periods pushes the next step a period late.
      stepPeriods = scaledStepMs + inBetweenMs > 2000.0 / rateHz ? 2U : 1U;
      trace.push_back(static_cast<double>(level));
    } else {
      stepPeriods = scaledStepMs > 1000.0 / rateHz ? 2U : 1U;
    }
    trace.push_back(rig.Pacing.StepCostMs());
    trace.push_back(rig.Pacing.Estimate(InterpolatedSampleLevel::Full));
    trace.push_back(rig.Pacing.Estimate(InterpolatedSampleLevel::Reduced));
    trace.push_back(rig.Pacing.DropAllowed() ? 1.0 : 0.0);
    trace.push_back(rig.Drs.Scale());
    trace.push_back(rig.Drs.AveragePairMs());
    trace.push_back(rig.Cadence.DropsInBetween() ? 1.0 : 0.0);
    trace.push_back(rig.Cadence.HoldSeconds());
    trace.push_back(rig.Cadence.GamePairMeanMs());
  }
  const CadenceStats &after = rig.Cadence.Stats();
  for (size_t bucket = 0U; bucket < after.Intervals.size(); ++bucket) {
    trace.push_back(static_cast<double>(after.Intervals[bucket] -
                                        before.Intervals[bucket]));
    trace.push_back(
        static_cast<double>(after.Holds[bucket] - before.Holds[bucket]));
  }
  trace.push_back(static_cast<double>(after.Repeats - before.Repeats));
  trace.push_back(
      static_cast<double>(after.CadenceSwitches - before.CadenceSwitches));
  trace.push_back(static_cast<double>(after.LateSteps - before.LateSteps));
  trace.push_back(static_cast<double>(after.LockChanges - before.LockChanges));
  return trace;
}

void CheckFreshRateState(const RateRig &rig, uint32_t rateHz,
                         uint32_t drsLevel = 0U) {
  const double pair = 2000.0 / rateHz;
  CHECK(std::abs(rig.Pacing.PairBudgetMs() - pair) < 1.0e-9);
  CHECK(std::abs(rig.Pacing.DecisionBudgetMs() - pair) < 1.0e-9);
  CHECK(std::abs(rig.Drs.BudgetMs() - pair) < 1.0e-9);
  CHECK(rig.Pacing.StepCostMs() == 0.0);
  CHECK(rig.Pacing.Level() == rig.Pacing.PreferredLevel());
  CHECK(rig.Drs.Level() == drsLevel && rig.Drs.AveragePairMs() == 0.0);
  CHECK(rig.Cadence.Cadence() == PresentCadence::Sixty);
  CHECK(rig.Cadence.GamePairMeanMs() == 0.0);
  CHECK(rig.Cadence.Config().Mode ==
        (rateHz == 60U ? Fps60Mode::Adaptive : Fps60Mode::Legacy));
  CHECK(rig.Pacing.DropAllowed() == (rateHz != 60U));
}

void TestLiveRateSwitchMatchesFreshStart() {
  const auto baseConfig = LiveRateConfig();
  struct Load {
    double StepMs, GamePairMs;
  };
  // Kokiri Forest-like light load, and a heavy one that pushes every
  // governor away from its start (DRS down, in-betweens shed, 30 locked).
  const Load light{9.0, 20.0}, heavy60{21.0, 42.0}, heavy30{62.0, 40.0};
  for (const uint32_t from : {30U, 60U}) {
    const uint32_t to = from == 60U ? 30U : 60U;
    RateRig switched;
    RatePacingResolutionMemory memory;
    ConfigureRatePacing(from, baseConfig, switched.Pacing, switched.Drs,
                        switched.Cadence);
    (void)DriveRate(switched, from, from == 60U ? heavy60.StepMs : heavy30.StepMs,
                    from == 60U ? heavy60.GamePairMs : heavy30.GamePairMs, 600);
    // The old rate really left state behind.
    CHECK(switched.Drs.Level() > 0U);
    CHECK(switched.Pacing.StepCostMs() > 0.0);
    CHECK(switched.Cadence.GamePairMeanMs() > 0.0);
    if (from == 60U)
      CHECK(switched.Cadence.DropsInBetween());
    // The frame boundary: L3+R3 > Video > FPS applied. 60 never ran before
    // a switch from 30: the floor; a switch to 30 starts at the top.
    memory.Leave(from, switched.Drs);
    auto config = baseConfig;
    config.ResolutionStartScale = memory.StartScale(to, true, config.Resolution);
    const uint32_t drsLevel = to == 60U ? switched.Drs.LowestLevel() : 0U;
    ConfigureRatePacing(to, config, switched.Pacing, switched.Drs,
                        switched.Cadence);
    CheckFreshRateState(switched, to, drsLevel);

    RateRig fresh;
    fresh.Now = switched.Now;
    ConfigureRatePacing(to, config, fresh.Pacing, fresh.Drs, fresh.Cadence);
    CheckFreshRateState(fresh, to, drsLevel);
    for (const Load load : {light, to == 60U ? heavy60 : heavy30}) {
      const auto expected = DriveRate(fresh, to, load.StepMs, load.GamePairMs, 400);
      const auto actual = DriveRate(switched, to, load.StepMs, load.GamePairMs, 400);
      CHECK(expected == actual);
    }
  }
  // Back and forth: 30 -> 60 -> 30 ends as a start at 30.
  RateRig rig;
  ConfigureRatePacing(30U, baseConfig, rig.Pacing, rig.Drs, rig.Cadence);
  (void)DriveRate(rig, 30U, heavy30.StepMs, heavy30.GamePairMs, 300);
  ConfigureRatePacing(60U, baseConfig, rig.Pacing, rig.Drs, rig.Cadence);
  (void)DriveRate(rig, 60U, heavy60.StepMs, heavy60.GamePairMs, 300);
  ConfigureRatePacing(30U, baseConfig, rig.Pacing, rig.Drs, rig.Cadence);
  CheckFreshRateState(rig, 30U);
}

// log16 (07/11): a 30 -> 60 switch in Forest Temple room 8 restarted dynamic
// resolution at 3.00 and dropped 16 and 23 in-between frames on the way down
// (windows 32250, 34800). A switch to 60 begins at the scale 60 last ran at
// (or the floor), so a toggle never begins above what 60 sustained.
// Room 8 standing: 15 ms a step frame at 2.25 (pair 30 of 33.3), a real
// fixed share of 0.3 (log16: 16.2 -> 25.2 ms between 2.25 and 3.00).
void TestLiveRateSwitchKeepsSixtyScale() {
  auto base = LiveRateConfig();
  UseDeko3dDynamicResolutionTiming(base.Resolution);
  const auto realMs = [](double at225, double fixed, float scale) {
    const double area = scale / 2.25;
    return at225 * (fixed + (1.0 - fixed) * area * area);
  };
  // `steps` step frames at rateHz; returns those whose pair passed the
  // budget (an in-between frame the governor drops).
  const auto drive = [&](RateRig &rig, uint32_t rateHz, double at225,
                         int steps) {
    int over = 0;
    for (int step = 0; step < steps; ++step) {
      const double stepMs = realMs(at225, 0.3, rig.Drs.Scale());
      const double pair = rateHz == 60U ? 2.0 * stepMs : stepMs;
      const bool shedding = pair > rig.Drs.BudgetMs();
      over += shedding ? 1 : 0;
      (void)rig.Drs.ObservePair(pair, shedding);
    }
    return over;
  };
  const auto configureAt = [&](RateRig &rig, RatePacingResolutionMemory &memory,
                               uint32_t from, uint32_t to, bool remember) {
    memory.Leave(from, rig.Drs);
    auto config = base;
    if (remember)
      config.ResolutionStartScale = memory.StartScale(to, true, config.Resolution);
    ConfigureRatePacing(to, config, rig.Pacing, rig.Drs, rig.Cadence);
  };
  for (const bool remember : {true, false}) {
    RateRig rig;
    RatePacingResolutionMemory memory;
    // A session start at 60 still begins at the top and settles at 2.25.
    ConfigureRatePacing(60U, base, rig.Pacing, rig.Drs, rig.Cadence);
    CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
    (void)drive(rig, 60U, 15.0, 600);
    CHECK(std::abs(rig.Drs.Scale() - 2.25F) < 1.0e-4F);
    // 60 -> 30: the top (one picture per 66.7 ms fits at 3.00).
    configureAt(rig, memory, 60U, 30U, remember);
    CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
    CHECK(drive(rig, 30U, 15.0, 300) == 0);
    CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
    // 30 -> 60: back at 2.25 at once, no pair over budget.
    configureAt(rig, memory, 30U, 60U, remember);
    const float start = rig.Drs.Scale();
    const int over = drive(rig, 60U, 15.0, 300);
    if (remember) {
      CHECK(std::abs(start - 2.25F) < 1.0e-4F);
      CHECK(over == 0);
      CHECK(rig.Drs.Stats().Changes == 3U); // only the start's descent
    } else {
      // Before the fix: from 3.00, a burst of dropped in-between frames.
      CHECK(std::abs(start - 3.0F) < 1.0e-4F);
      CHECK(over >= 10);
    }
    CHECK(std::abs(rig.Drs.Scale() - 2.25F) < 1.0e-4F);
  }
  // An area left at 2.5 (12 ms a step frame at 2.25): the switch back begins
  // at 2.5 (not 3.00) and climbs on its own when there is room.
  {
    RateRig rig;
    RatePacingResolutionMemory memory;
    ConfigureRatePacing(60U, base, rig.Pacing, rig.Drs, rig.Cadence);
    (void)drive(rig, 60U, 12.0, 600);
    CHECK(std::abs(rig.Drs.Scale() - 2.5F) < 1.0e-4F);
    configureAt(rig, memory, 60U, 30U, true);
    configureAt(rig, memory, 30U, 60U, true);
    CHECK(std::abs(rig.Drs.Scale() - 2.5F) < 1.0e-4F);
    CHECK(drive(rig, 60U, 12.0, 300) == 0);
    CHECK(std::abs(rig.Drs.Scale() - 2.5F) < 1.0e-4F);
    CHECK(drive(rig, 60U, 8.0, 600) == 0);
    CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  }
  // A session that starts at 30: the first switch to 60 begins at the floor.
  {
    RateRig rig;
    RatePacingResolutionMemory memory;
    ConfigureRatePacing(30U, base, rig.Pacing, rig.Drs, rig.Cadence);
    (void)drive(rig, 30U, 15.0, 300);
    configureAt(rig, memory, 30U, 60U, true);
    CHECK(std::abs(rig.Drs.Scale() - 2.25F) < 1.0e-4F);
    CHECK(drive(rig, 60U, 15.0, 300) == 0);
  }
  // Start scales between levels round down; out of range clamps.
  DynamicResolutionController c;
  c.Configure(base.Resolution, 2000.0 / 60.0, 2.6F);
  CHECK(std::abs(c.Scale() - 2.5F) < 1.0e-4F);
  c.Configure(base.Resolution, 2000.0 / 60.0, 1.0F);
  CHECK(std::abs(c.Scale() - 2.25F) < 1.0e-4F);
  c.Configure(base.Resolution, 2000.0 / 60.0, 4.0F);
  CHECK(std::abs(c.Scale() - 3.0F) < 1.0e-4F);
  c.Configure(base.Resolution, 2000.0 / 60.0, 2.75F);
  CHECK(std::abs(c.Scale() - 2.75F) < 1.0e-4F);
}

// Resolution at 60 FPS = Full (config/drs_60): 60 holds 3.00 in a heavy room
// (room 8 standing, 15 ms a step frame at 2.25: ~26 at 3.00), the cadence
// governor sheds the in-between frames that do not fit, and a 30 -> 60
// switch begins at 3.00 (not the remembered scale or the floor). Back to
// Dynamic, the same switch resumes where Dynamic last ran.
void TestLiveRateSwitchFullStartsAtFull() {
  auto base = LiveRateConfig();
  UseDeko3dDynamicResolutionTiming(base.Resolution);
  auto full = base;
  full.ResolutionSixty = DynamicResolutionSixtyMode::Full;
  const double heavyStepAt3 = 26.0;
  // Session start at 60 under Full.
  RateRig rig;
  RatePacingResolutionMemory memory;
  ConfigureRatePacing(60U, full, rig.Pacing, rig.Drs, rig.Cadence);
  CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  CHECK(rig.Drs.LowestLevel() == 0U);
  (void)DriveRate(rig, 60U, heavyStepAt3, 42.0, 600);
  CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  CHECK(rig.Drs.Stats().Downs == 0U);
  // The cadence governor still sheds what does not fit.
  CHECK(rig.Cadence.DropsInBetween());
  // 60 -> 30 -> 60 under Full: a held full scale is not remembered.
  memory.Leave(60U, rig.Drs);
  CHECK(memory.LastScaleAtSixty == 0.0F);
  ConfigureRatePacing(30U, full, rig.Pacing, rig.Drs, rig.Cadence);
  CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  (void)DriveRate(rig, 30U, heavyStepAt3, 42.0, 300);
  memory.Leave(30U, rig.Drs);
  auto config = full;
  config.ResolutionStartScale = memory.StartScale(60U, true, config.Resolution);
  ConfigureRatePacing(60U, config, rig.Pacing, rig.Drs, rig.Cadence);
  CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  (void)DriveRate(rig, 60U, heavyStepAt3, 42.0, 300);
  CHECK(std::abs(rig.Drs.Scale() - 3.0F) < 1.0e-4F);
  CHECK(rig.Drs.Stats().Downs == 0U);
  // A remembered Dynamic scale does not apply under Full either.
  {
    RateRig dyn;
    RatePacingResolutionMemory remembered;
    ConfigureRatePacing(60U, base, dyn.Pacing, dyn.Drs, dyn.Cadence);
    (void)DriveRate(dyn, 60U, heavyStepAt3, 42.0, 600);
    CHECK(dyn.Drs.Scale() < 3.0F);
    remembered.Leave(60U, dyn.Drs);
    CHECK(remembered.LastScaleAtSixty > 0.0F && remembered.LastScaleAtSixty < 3.0F);
    ConfigureRatePacing(30U, base, dyn.Pacing, dyn.Drs, dyn.Cadence);
    auto toFull = full;
    toFull.ResolutionStartScale = remembered.StartScale(60U, true, toFull.Resolution);
    ConfigureRatePacing(60U, toFull, dyn.Pacing, dyn.Drs, dyn.Cadence);
    CHECK(std::abs(dyn.Drs.Scale() - 3.0F) < 1.0e-4F);
    // Dynamic again: resumes at the remembered scale.
    auto toDynamic = base;
    toDynamic.ResolutionStartScale =
        remembered.StartScale(60U, true, toDynamic.Resolution);
    ConfigureRatePacing(60U, toDynamic, dyn.Pacing, dyn.Drs, dyn.Cadence);
    CHECK(std::abs(dyn.Drs.Scale() - remembered.LastScaleAtSixty) < 1.0e-4F);
  }
  // Full changes nothing at 30.
  RateRig thirty;
  ConfigureRatePacing(30U, full, thirty.Pacing, thirty.Drs, thirty.Cadence);
  CHECK(thirty.Drs.LowestLevel() == 3U);
}

// The first presents after a switch are measured against the new period
// only: no late step, hold change or lock from an interval of the old rate.
void TestLiveRateSwitchFirstPresents() {
  const auto config = LiveRateConfig();
  RateRig rig;
  ConfigureRatePacing(30U, config, rig.Pacing, rig.Drs, rig.Cadence);
  (void)DriveRate(rig, 30U, 9.0, 20.0, 120);
  ConfigureRatePacing(60U, config, rig.Pacing, rig.Drs, rig.Cadence);
  const CadenceStats before = rig.Cadence.Stats();
  // The previous picture was presented 33 ms ago (one 30 Hz period).
  rig.Now += 1.0 / 30.0;
  rig.Cadence.ObservePresent(PresentedPicture::Step, rig.Now, 0.0);
  rig.Now += 1.0 / 60.0;
  rig.Cadence.ObservePresent(PresentedPicture::InBetween, rig.Now, 0.0);
  const CadenceStats &after = rig.Cadence.Stats();
  CHECK(after.LateSteps == before.LateSteps);
  CHECK(after.CadenceSwitches == before.CadenceSwitches);
  CHECK(after.Intervals[1] == before.Intervals[1]);
  CHECK(after.Intervals[0] == before.Intervals[0] + 1U);
}

void Report() {
  std::printf("\n== tour15: calibration of the hidden time (margin 0) ==\n");
  Calibrate();
  std::printf("\n== tour15: pair margin (hidden 2.75 ms) ==\n");
  SweepMargin();
  ReportRenderWaits();
  ReportFramesInFlight();
  ReportFrameSlots();
  struct Scenario {
    const char *Name;
    std::vector<Segment> Segments;
  };
  const std::vector<Scenario> scenarios{
      {"light", {Light(60)}},          {"normal", {Normal(60)}},
      {"heavy", {Heavy(60)}},        {"v.heavy", {VeryHeavy(60)}},
      {"integ4", {Integ4(60)}},       {"overload", {Overloaded(60)}},
      {"session", Session()},
  };
  for (uint32_t queue : {0U, 1U}) {
    std::printf("\n== swap %s ==\n",
                queue == 0U ? "blocking (returns on the vblank that shows it)"
                            : "with a 1-frame queue");
    for (const auto &scenario : scenarios) {
      for (auto mode :
           {Fps60Mode::Legacy, Fps60Mode::Strict60, Fps60Mode::Adaptive})
        Print(scenario.Name, mode, Simulate(scenario.Segments, mode, queue));
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  TestParseMode();
  TestRefreshPlans();
  TestHoldCounters();
  TestLocksThirtyOnRepeatedMisses();
  TestMissesSpreadOutDoNotLock();
  TestMissesWithoutOverloadDoNotLock();
  TestReturnsOnlyWithProvenHeadroom();
  TestHoldAndLateSteps();
  TestRelapseDoublesTheHold();
  TestStrictAndLegacyNeverLock();
  TestGamePairEstimate();
  TestPerFrameGovernorWithoutDrop();
  TestSimulationLightStaysAtSixty();
  TestSimulationNearTheLimitKeepsTheAverage();
  TestSimulationOverloadedLocksThirty();
  TestSimulationSessionIsMoreRegular();
  TestSecondFrameInFlightKeepsSlowAreasAtSixty();
  TestRenderWaitIsNotCost();
  TestMarginKeepsTheRefresh();
  TestEarlyFrameSlotAbsorbsLongSteps();
  TestLiveRateSwitchMatchesFreshStart();
  TestLiveRateSwitchFirstPresents();
  TestLiveRateSwitchKeepsSixtyScale();
  TestLiveRateSwitchFullStartsAtFull();
  if (argc > 1 && std::strcmp(argv[1], "--report") == 0)
    Report();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_native_cadence_governor_tests: ok\n");
  return 0;
}
