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

void TestParseMode() {
  CHECK(ParseFps60Mode("").Mode == Fps60Mode::Adaptive);
  CHECK(ParseFps60Mode("adaptive\n").Mode == Fps60Mode::Adaptive);
  CHECK(ParseFps60Mode("STRICT60").Mode == Fps60Mode::Strict60);
  CHECK(ParseFps60Mode("legacy\r\n").Mode == Fps60Mode::Legacy);
  CHECK(ParseFps60Mode("bogus").Mode == Fps60Mode::Adaptive);
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
  CHECK(ParseFps60Mode("misses=4").Mode == Fps60Mode::Adaptive);
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
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
  CHECK(!governor.PerFrameDropAllowed());
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  // Clean 60: never locks.
  for (int pair = 0; pair < 300; ++pair) {
    presenter.Present(PresentedPicture::InBetween, 1);
    presenter.Present(PresentedPicture::Step, 1);
  }
  CHECK(governor.Cadence() == PresentCadence::Sixty);
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

void TestMissesSpreadOutDoNotLock() {
  NativeCadenceGovernor governor;
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
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

void LockNow(Presenter &presenter) {
  for (int miss = 0; miss < 3; ++miss)
    presenter.Present(PresentedPicture::Step, 2);
}

void TestReturnsOnlyWithProvenHeadroom() {
  NativeCadenceGovernor governor;
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  LockNow(presenter);
  CHECK(governor.Cadence() == PresentCadence::Thirty);
  // Heavy game thread (31 ms per pair): stays at 30 however long.
  FeedGamePairs(governor, 31.0, 64);
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
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  FeedGamePairs(governor, 20.0, 64);
  LockNow(presenter);
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
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
  Presenter presenter{governor};
  presenter.Present(PresentedPicture::Step, 1);
  FeedGamePairs(governor, 20.0, 64);
  double expected = 2.0;
  for (int relapse = 0; relapse < 6; ++relapse) {
    LockNow(presenter);
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
  LockNow(presenter);
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
  governor.Configure(Fps60ModeConfig{}, kRefreshMs);
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
                uint32_t swapQueue = 0U) {
  Params params;
  params.Mode.Mode = mode;
  params.SwapQueue = swapQueue;
  return CadenceSim::Run(segments, params);
}

void Print(const char *name, Fps60Mode mode, const Result &result) {
  const auto &c = result.Cadence;
  const double seconds = result.Seconds;
  std::printf(
      "%-10s %-8s fps=%5.1f pics/s=%5.1f jogo=%5.1f refr/s  "
      "intervalos(1/2/3/4+)=%llu/%llu/%llu/%llu  "
      "hold(1/2/3/4+)=%llu/%llu/%llu/%llu  trocas/s=%5.2f  "
      "desvio=%4.1f ms  judder=%4.1f ms  em30=%3.0f%%  travas=%llu\n",
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

void TestSimulationHeavyLocksThirtyCleanly() {
  // Swap returns at the vblank (the console: "swap includes the vsync
  // wait"): legacy alternates holds of 1 and 2 (and 3).
  const auto before = Simulate({Heavy(60)}, Fps60Mode::Legacy);
  const auto after = Simulate({Heavy(60)}, Fps60Mode::Adaptive);
  CHECK(before.JudderRmsMs > 5.0);
  CHECK(after.JudderRmsMs < 1.5);
  CHECK(after.JudderRmsMs < before.JudderRmsMs / 4.0);
  CHECK(after.PictureIntervalStdMs < 1.5);
  CHECK(static_cast<double>(after.Cadence.CadenceSwitches) / after.Seconds <
        0.2);
  CHECK(after.Cadence.Holds[1] > 20U * (after.Cadence.Holds[0] +
                                        after.Cadence.Holds[2] +
                                        after.Cadence.Holds[3]));
  // Game speed kept (60 guest refreshes a second).
  CHECK(after.GuestRefreshesPerSecond > 59.5);
  CHECK(after.DroppedRefreshes < 10U);
}

void TestSimulationSessionIsMoreRegular() {
  for (uint32_t queue : {0U, 1U}) {
    const auto before = Simulate(Session(), Fps60Mode::Legacy, queue);
    const auto after = Simulate(Session(), Fps60Mode::Adaptive, queue);
    // Blocking swap: legacy judders (holds of 1, 2 and 3 mixed); with a
    // queued swap it mostly alternates long runs of 60 and of drops.
    if (queue == 0U)
      CHECK(after.JudderRmsMs < before.JudderRmsMs / 2.0);
    CHECK(after.JudderRmsMs < 1.0);
    CHECK(after.Cadence.CadenceSwitches < before.Cadence.CadenceSwitches);
    CHECK(after.GuestRefreshesPerSecond > 59.5);
  }
  // Strict60 never drops a frame on purpose.
  const auto strict = Simulate(Session(), Fps60Mode::Strict60);
  CHECK(strict.InBetweenDropped == 0U);
}

void Report() {
  struct Scenario {
    const char *Name;
    std::vector<Segment> Segments;
  };
  const std::vector<Scenario> scenarios{
      {"leve", {Light(60)}},          {"normal", {Normal(60)}},
      {"pesado", {Heavy(60)}},        {"m.pesado", {VeryHeavy(60)}},
      {"sessao", Session()},
  };
  for (uint32_t queue : {0U, 1U}) {
    std::printf("\n== swap %s ==\n",
                queue == 0U ? "bloqueante (volta no vblank que mostra)"
                            : "com fila de 1 quadro");
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
  TestReturnsOnlyWithProvenHeadroom();
  TestHoldAndLateSteps();
  TestRelapseDoublesTheHold();
  TestStrictAndLegacyNeverLock();
  TestGamePairEstimate();
  TestPerFrameGovernorWithoutDrop();
  TestSimulationLightStaysAtSixty();
  TestSimulationHeavyLocksThirtyCleanly();
  TestSimulationSessionIsMoreRegular();
  if (argc > 1 && std::strcmp(argv[1], "--report") == 0)
    Report();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_native_cadence_governor_tests: ok\n");
  return 0;
}
