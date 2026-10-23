// Paced 60 Hz interpolation: the governor that draws, reduces or drops the
// in-between frame (oot3d_native_interpolation_pacing.h), the reduced-replay
// eligibility of a sample and the config parser.
#include "oot3d_native_interpolation_pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Oot3dNativeGame::InterpolatedSampleLevel;
using Oot3dNativeGame::InterpolationPacingConfig;
using Oot3dNativeGame::InterpolationPacingGovernor;
using Oot3dNativeGame::InterpolationReducedScaleMode;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr double kPair = 2000.0 / 60.0;

// The mechanics are tested on the bare pair (margin 0); the margin has its
// own tests.
InterpolationPacingGovernor Make(bool keepSpeed = true, float reduced = 0.0F,
                                 bool always = false, double marginMs = 0.0) {
  InterpolationPacingConfig config;
  config.MarginMs = marginMs;
  config.KeepGameSpeed = keepSpeed;
  config.ReducedScale = reduced;
  config.ReducedMode = always ? InterpolationReducedScaleMode::Always
                              : InterpolationReducedScaleMode::Auto;
  InterpolationPacingGovernor governor;
  governor.Configure(config, kPair, 2.0F, reduced > 0.0F);
  return governor;
}

// One pair: the step frame, then the in-between decision and its cost.
// interpolated(level) gives the cost of the in-between frame at that level.
template <typename Cost>
InterpolatedSampleLevel Pair(InterpolationPacingGovernor &governor,
                             double stepMs, Cost interpolated) {
  governor.ObserveStep(stepMs);
  const auto level = governor.Decide();
  governor.ObserveInterpolated(level, interpolated(level));
  return level;
}

double Costs(InterpolatedSampleLevel level, double full, double reduced,
             double dropped = 1.0) {
  switch (level) {
  case InterpolatedSampleLevel::Full:
    return full;
  case InterpolatedSampleLevel::Reduced:
    return reduced;
  case InterpolatedSampleLevel::Dropped:
    return dropped;
  }
  return full;
}

void TestNoMeasurementsKeepsPreferredLevel() {
  auto governor = Make();
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  auto always = Make(true, 1.5F, true);
  CHECK(always.Decide() == InterpolatedSampleLevel::Reduced);
}

void TestLightLoadDrawsEveryInBetweenFrame() {
  auto governor = Make();
  for (int pair = 0; pair < 600; ++pair) {
    CHECK(Pair(governor, 12.0, [](auto level) {
            return Costs(level, 12.0, 8.0);
          }) == InterpolatedSampleLevel::Full);
  }
  CHECK(governor.Stats().DroppedFrames == 0U);
  CHECK(governor.Stats().LevelChanges == 0U);
}

void TestHeavyLoadDropsWithoutReducedLevel() {
  auto governor = Make();
  int dropped = 0;
  for (int pair = 0; pair < 600; ++pair) {
    dropped += Pair(governor, 19.0, [](auto level) {
                 return Costs(level, 19.0, 12.0);
               }) == InterpolatedSampleLevel::Dropped
                   ? 1
                   : 0;
  }
  // Only the first DropAfter decisions and the probes (one in
  // ProbeInterval) draw a full in-between frame.
  CHECK(dropped >= 580);
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
}

void TestHeavyLoadReducesWhenTheReducedFrameFits() {
  auto governor = Make(true, 1.5F);
  int reduced = 0;
  for (int pair = 0; pair < 600; ++pair) {
    reduced += Pair(governor, 19.0, [](auto level) {
                 return Costs(level, 19.0, 10.5);
               }) == InterpolatedSampleLevel::Reduced
                   ? 1
                   : 0;
  }
  CHECK(reduced >= 580);
  CHECK(governor.Level() == InterpolatedSampleLevel::Reduced);
}

void TestEvenReducedTooSlowDrops() {
  auto governor = Make(true, 1.5F);
  for (int pair = 0; pair < 300; ++pair) {
    Pair(governor, 24.0, [](auto level) { return Costs(level, 24.0, 16.0); });
  }
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
}

void TestStepFrameAloneOverBudgetDrops() {
  auto governor = Make(true, 1.5F);
  for (int pair = 0; pair < 100; ++pair)
    Pair(governor, 36.0, [](auto level) { return Costs(level, 36.0, 20.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
}

void TestRecoversWhenTheSceneGetsLighterWithoutProbing() {
  auto governor = Make();
  for (int pair = 0; pair < 120; ++pair)
    Pair(governor, 20.0, [](auto level) { return Costs(level, 20.0, 12.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  const auto probesBefore = governor.Stats().Probes;
  int pairsToRecover = -1;
  for (int pair = 0; pair < 60; ++pair) {
    const auto level = Pair(governor, 11.0, [](auto level) {
      return Costs(level, 11.0, 7.0);
    });
    if (level == InterpolatedSampleLevel::Full && pairsToRecover < 0)
      pairsToRecover = pair;
  }
  // The full in-between estimate follows the step frame's cost: a few
  // pairs for the step average to settle, no probe needed.
  CHECK(pairsToRecover >= 0 && pairsToRecover < 10);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  CHECK(governor.Stats().Probes == probesBefore);
}

void TestHysteresisDoesNotFlapNearTheBudget() {
  auto governor = Make();
  // Pairs between the recover (0.97) and budget (1.0) fractions, noisy.
  uint32_t seed = 12345U;
  for (int pair = 0; pair < 2000; ++pair) {
    seed = seed * 1664525U + 1013904223U;
    const double noise = ((seed >> 8) % 1000U) / 2500.0 - 0.2; // +-0.2 ms
    const double cost = 16.4 + noise;                           // pair ~32.8
    Pair(governor, cost, [&](auto level) { return Costs(level, cost, cost); });
  }
  CHECK(governor.Stats().LevelChanges <= 2U);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);

  // Coming back from dropped, the same pair must fit with margin: stays
  // dropped (up to probes) instead of alternating.
  auto dropped = Make();
  for (int pair = 0; pair < 100; ++pair)
    Pair(dropped, 18.0, [](auto level) { return Costs(level, 18.0, 18.0); });
  CHECK(dropped.Level() == InterpolatedSampleLevel::Dropped);
  const auto changes = dropped.Stats().LevelChanges;
  for (int pair = 0; pair < 600; ++pair)
    Pair(dropped, 16.4, [](auto level) { return Costs(level, 16.4, 16.4); });
  CHECK(dropped.Stats().LevelChanges == changes);
}

void TestHitchDoesNotKeepFramesOut() {
  auto governor = Make();
  for (int pair = 0; pair < 60; ++pair)
    Pair(governor, 12.0, [](auto level) { return Costs(level, 12.0, 8.0); });
  // A 120 ms step (a shader compiled): counted at most twice the average.
  governor.ObserveStep(120.0);
  int dropped = 0;
  for (int pair = 0; pair < 30; ++pair) {
    dropped += Pair(governor, 12.0, [](auto level) {
                 return Costs(level, 12.0, 8.0);
               }) == InterpolatedSampleLevel::Dropped
                   ? 1
                   : 0;
  }
  CHECK(dropped <= 4);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
}

void TestNeverDropsWithoutKeepGameSpeed() {
  auto governor = Make(false);
  for (int pair = 0; pair < 200; ++pair) {
    CHECK(Pair(governor, 25.0, [](auto level) {
            return Costs(level, 25.0, 15.0);
          }) == InterpolatedSampleLevel::Full);
  }
  auto reduced = Make(false, 1.5F);
  for (int pair = 0; pair < 200; ++pair)
    Pair(reduced, 25.0, [](auto level) { return Costs(level, 25.0, 15.0); });
  CHECK(reduced.Level() == InterpolatedSampleLevel::Reduced);
  CHECK(reduced.Stats().DroppedFrames == 0U);
}

void TestAlwaysReducedNeverDrawsFull() {
  auto governor = Make(true, 1.5F, true);
  for (int pair = 0; pair < 300; ++pair) {
    CHECK(Pair(governor, 10.0, [](auto level) {
            return Costs(level, 10.0, 6.0);
          }) == InterpolatedSampleLevel::Reduced);
  }
}

void TestProbeRefreshesAStaleEstimate() {
  auto governor = Make();
  // The full in-between frame once cost far more than the step (an effect
  // only the in-between frame had), then became as cheap as the step.
  for (int pair = 0; pair < 20; ++pair)
    Pair(governor, 10.0, [](auto level) { return Costs(level, 30.0, 30.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  int full = 0;
  for (int pair = 0; pair < 400; ++pair) {
    full += Pair(governor, 10.0, [](auto level) {
              return Costs(level, 10.0, 10.0);
            }) == InterpolatedSampleLevel::Full
                ? 1
                : 0;
  }
  CHECK(governor.Stats().Probes >= 1U);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  CHECK(full > 200);
}

// The point of dropping: a GPU-bound pair is late; the step frames stay on
// time when the in-between frames that do not fit go. Model: each frame
// occupies the GPU for its cost; a pair must end within two refreshes of
// its start (the step frame keeps the game's 30 Hz). Count late steps.
void TestDroppingKeepsStepFramesOnTime() {
  const auto simulate = [](bool keepSpeed) {
    auto governor = Make(keepSpeed);
    double gpuFree = 0.0;
    int lateSteps = 0;
    for (int pair = 0; pair < 900; ++pair) {
      const double start = pair * kPair;
      // A scene that alternates between light and heavy every 5 s.
      const double cost = (pair / 150) % 2 == 0 ? 11.0 : 19.0;
      const double stepStart = std::max(start, gpuFree);
      const double stepEnd = stepStart + cost;
      // Queued behind the previous pair by more than a refresh: the step
      // frame misses its slot and the backlog keeps growing.
      if (stepStart - start > 0.5 * kPair)
        ++lateSteps;
      governor.ObserveStep(cost);
      const auto level = governor.Decide();
      const double inBetween = Costs(level, cost, cost, 1.0);
      governor.ObserveInterpolated(level, inBetween);
      gpuFree = stepEnd + inBetween;
    }
    return lateSteps;
  };
  const int paced = simulate(true);
  const int unpaced = simulate(false);
  CHECK(paced <= 4);
  CHECK(unpaced > 100);
}

// Costs come back a few frames late (GPU timestamps read asynchronously):
// the governor must still settle, with one probe at a time.
void TestDelayedMeasurements() {
  auto governor = Make(true, 1.5F);
  struct Pending {
    bool Step;
    InterpolatedSampleLevel Level;
    double Cost;
  };
  std::vector<Pending> queue;
  int reduced = 0;
  for (int pair = 0; pair < 600; ++pair) {
    queue.push_back({true, InterpolatedSampleLevel::Full, 19.0});
    const auto level = governor.Decide();
    reduced += level == InterpolatedSampleLevel::Reduced ? 1 : 0;
    queue.push_back({false, level, Costs(level, 19.0, 10.5)});
    // Three frames of latency.
    while (queue.size() > 3U) {
      const auto result = queue.front();
      queue.erase(queue.begin());
      if (result.Step)
        governor.ObserveStep(result.Cost);
      else
        governor.ObserveInterpolated(result.Level, result.Cost);
    }
  }
  CHECK(reduced >= 580);
  CHECK(governor.Stats().Probes <= 10U);
}

// Console, 2026-10-17 (fps60, scale 2): perf.txt after 2.7 min showed
// level=dropped, interp_frames=0, probes=0, level_changes=0 with
// gpu_ms_step=15.93 rt_ms_step=11.68 est_step_ms=13.13 est_full_ms=13.22
// pair_budget_ms=33.33. The first seconds (no calibrated GPU clock) fed
// render-thread time plus the swap, vsync wait included (~27 ms a frame):
// dropped. Afterwards the pair fits and the governor must come back to full.
void TestConsoleStuckAtDroppedComesBackToFull() {
  auto governor = Make();
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  // Start-up: inflated costs (11.7 ms of render thread + ~15 ms of swap).
  for (int pair = 0; pair < 30; ++pair)
    Pair(governor, 27.0, [](auto level) { return Costs(level, 27.0, 27.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  // Calibrated GPU clock: the numbers of the console window.
  int full = 0;
  int pairsToFull = -1;
  for (int pair = 0; pair < 150; ++pair) {
    const auto level = Pair(governor, 13.13, [](auto level) {
      return Costs(level, 13.22, 13.22, 0.059);
    });
    if (level == InterpolatedSampleLevel::Full) {
      ++full;
      if (pairsToFull < 0) pairsToFull = pair;
    }
  }
  CHECK(pairsToFull >= 0 && pairsToFull <= 5);
  CHECK(full >= 140);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  CHECK(std::abs(governor.StepCostMs() - 13.13) < 0.01);
  CHECK(std::abs(governor.Estimate(InterpolatedSampleLevel::Full) - 13.22) <
        0.05);

  // The GPU side of that window: 15.93 ms step frames (render thread
  // 11.68). A 15.93 + 15.93 pair is 31.9 of 33.3: it fits, stays full.
  for (int pair = 0; pair < 600; ++pair) {
    CHECK(Pair(governor, std::max(15.93, 11.68), [](auto level) {
            return Costs(level, 15.93, 15.93, 0.059);
          }) == InterpolatedSampleLevel::Full);
  }
  // And from dropped it comes back to it as well (97% of 33.3 = 32.3).
  auto fromDropped = Make();
  for (int pair = 0; pair < 30; ++pair)
    Pair(fromDropped, 27.0, [](auto level) { return Costs(level, 27.0, 27.0); });
  CHECK(fromDropped.Level() == InterpolatedSampleLevel::Dropped);
  for (int pair = 0; pair < 60; ++pair)
    Pair(fromDropped, 15.93, [](auto level) {
      return Costs(level, 15.93, 15.93, 0.059);
    });
  CHECK(fromDropped.Level() == InterpolatedSampleLevel::Full);
}

// Full at the start, and no drop on a guess or on one slow pair.
void TestStartsFullAndDropsOnlyOnLastingMeasurements() {
  auto governor = Make();
  governor.ObserveStep(25.0);
  // The full in-between frame was never measured: kept.
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  governor.ObserveInterpolated(InterpolatedSampleLevel::Full, 25.0);
  // Measured over budget (50 of 33.3): dropped only on the third decision.
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  CHECK(governor.Decide() == InterpolatedSampleLevel::Full);
  CHECK(governor.Decide() == InterpolatedSampleLevel::Dropped);
  // One slow pair in a light scene does not drop.
  auto light = Make();
  for (int pair = 0; pair < 60; ++pair)
    Pair(light, 12.0, [](auto level) { return Costs(level, 12.0, 8.0); });
  light.ObserveStep(24.0);
  CHECK(light.Decide() == InterpolatedSampleLevel::Full);
  light.ObserveStep(12.0);
  for (int pair = 0; pair < 10; ++pair)
    CHECK(Pair(light, 12.0, [](auto level) { return Costs(level, 12.0, 8.0); }) ==
          InterpolatedSampleLevel::Full);
}

// Below the preferred level, probes keep re-measuring the level above.
void TestProbesHappenWhileDropped() {
  auto governor = Make();
  for (int pair = 0; pair < 600; ++pair)
    Pair(governor, 19.0, [](auto level) { return Costs(level, 19.0, 19.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  CHECK(governor.Stats().Probes >= 8U);
}

void TestSampleClearsEveryTarget() {
  using Oot3dNativeGame::ReducedReplayDraw;
  using Oot3dNativeGame::ReducedReplayFill;
  using Oot3dNativeGame::SampleClearsEveryTarget;
  const ReducedReplayDraw scene{10, 0x1000, 0x2000, 0x3000, 0x4000, true};
  const ReducedReplayDraw scene2{11, 0x1000, 0x2000, 0x3000, 0x4000, true};
  const ReducedReplayDraw ui{12, 0x1000, 0x2000, 0x3000, 0x4000, false};
  const ReducedReplayFill color{10, 0x1000, 0x2000};
  const ReducedReplayFill depth{10, 0x3000, 0x4000};
  const ReducedReplayFill lateColor{11, 0x1000, 0x2000};
  {
    const std::vector<ReducedReplayDraw> draws{scene, scene2, ui};
    const std::vector<ReducedReplayFill> fills{color, depth};
    CHECK(SampleClearsEveryTarget(draws, fills));
  }
  {
    // No depth clear while the draws test depth: last frame's depth.
    const std::vector<ReducedReplayDraw> draws{scene, scene2};
    const std::vector<ReducedReplayFill> fills{color};
    CHECK(!SampleClearsEveryTarget(draws, fills));
  }
  {
    // UI over a target whose depth is never used: colour clear suffices.
    const std::vector<ReducedReplayDraw> draws{ui};
    const std::vector<ReducedReplayFill> fills{
        ReducedReplayFill{12, 0x1000, 0x2000}};
    CHECK(SampleClearsEveryTarget(draws, fills));
  }
  {
    // The clear comes after the first draw into the target.
    const std::vector<ReducedReplayDraw> draws{scene, scene2};
    const std::vector<ReducedReplayFill> fills{lateColor, depth};
    CHECK(!SampleClearsEveryTarget(draws, fills));
  }
  {
    // A partial fill does not clear the target.
    const std::vector<ReducedReplayDraw> draws{ui};
    const std::vector<ReducedReplayFill> fills{
        ReducedReplayFill{12, 0x1000, 0x1800}};
    CHECK(!SampleClearsEveryTarget(draws, fills));
  }
  {
    const std::vector<ReducedReplayDraw> draws{};
    const std::vector<ReducedReplayFill> fills{};
    CHECK(SampleClearsEveryTarget(draws, fills));
  }
}

void TestParseReducedScale() {
  using Oot3dNativeGame::ParseInterpolationReducedScale;
  CHECK(ParseInterpolationReducedScale("1.5", 2.0F) == 1.5F);
  CHECK(ParseInterpolationReducedScale("1,5\n", 2.0F) == 1.5F);
  CHECK(ParseInterpolationReducedScale(" 1.75 ", 2.0F) == 1.75F);
  CHECK(ParseInterpolationReducedScale("1.6", 2.0F) == 1.5F);
  CHECK(ParseInterpolationReducedScale("2", 2.0F) == 0.0F);
  CHECK(ParseInterpolationReducedScale("2.5", 2.0F) == 0.0F);
  CHECK(ParseInterpolationReducedScale("0.5", 2.0F) == 0.0F);
  CHECK(ParseInterpolationReducedScale("", 2.0F) == 0.0F);
  CHECK(ParseInterpolationReducedScale("abc", 2.0F) == 0.0F);
  CHECK(ParseInterpolationReducedScale("2", 3.0F) == 2.0F);
  CHECK(ParseInterpolationReducedScale("1.5", 1.0F) == 0.0F);
}

} // namespace

// tour15 (pgo5j, margin 0): a measured pair of 29-30 ms "fit" 33.3 and the
// area lost 3-5 FPS. With the default margin (5 ms) that pair drops, one of
// 26-27 is still drawn.
void TestMarginDropsPairsNearTheBudget() {
  using Oot3dNativeGame::ParseInterpolationMarginMs;
  CHECK(InterpolationPacingConfig{}.MarginMs == 5.0);
  CHECK(ParseInterpolationMarginMs("0") == 0.0);
  CHECK(ParseInterpolationMarginMs("1,5\n") == 1.5);
  CHECK(ParseInterpolationMarginMs(" 2.25") == 2.25);
  CHECK(!ParseInterpolationMarginMs("x").has_value());
  CHECK(!ParseInterpolationMarginMs("").has_value());
  CHECK(!ParseInterpolationMarginMs("16").has_value());
  const auto steady = [](double marginMs, double stepMs, double fullMs) {
    auto governor = Make(true, 0.0F, false, marginMs);
    InterpolatedSampleLevel level = InterpolatedSampleLevel::Full;
    for (int pair = 0; pair < 300; ++pair) {
      governor.ObserveStep(stepMs);
      level = governor.Decide();
      governor.ObserveInterpolated(
          level, level == InterpolatedSampleLevel::Full ? fullMs : 0.5);
    }
    return governor.Level();
  };
  CHECK(steady(0.0, 15.0, 14.9) == InterpolatedSampleLevel::Full);
  CHECK(steady(5.0, 15.0, 14.9) == InterpolatedSampleLevel::Dropped);
  CHECK(steady(5.0, 13.5, 13.4) == InterpolatedSampleLevel::Full);
  // Never more than half the pair.
  CHECK(Make(true, 0.0F, false, 15.0).DecisionBudgetMs() == kPair - 15.0);
  InterpolationPacingConfig config;
  config.MarginMs = 15.0;
  InterpolationPacingGovernor governor;
  governor.Configure(config, 20.0, 2.0F, false); // a 100 Hz pair
  CHECK(governor.DecisionBudgetMs() == 10.0);
  CHECK(Make().DecisionBudgetMs() == kPair);
}

int main() {
  TestMarginDropsPairsNearTheBudget();
  TestNoMeasurementsKeepsPreferredLevel();
  TestLightLoadDrawsEveryInBetweenFrame();
  TestHeavyLoadDropsWithoutReducedLevel();
  TestHeavyLoadReducesWhenTheReducedFrameFits();
  TestEvenReducedTooSlowDrops();
  TestStepFrameAloneOverBudgetDrops();
  TestRecoversWhenTheSceneGetsLighterWithoutProbing();
  TestHysteresisDoesNotFlapNearTheBudget();
  TestHitchDoesNotKeepFramesOut();
  TestNeverDropsWithoutKeepGameSpeed();
  TestAlwaysReducedNeverDrawsFull();
  TestProbeRefreshesAStaleEstimate();
  TestDroppingKeepsStepFramesOnTime();
  TestDelayedMeasurements();
  TestConsoleStuckAtDroppedComesBackToFull();
  TestStartsFullAndDropsOnlyOnLastingMeasurements();
  TestProbesHappenWhileDropped();
  TestSampleClearsEveryTarget();
  TestParseReducedScale();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_native_interpolation_pacing_tests: ok");
  return EXIT_SUCCESS;
}
