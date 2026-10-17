// Paced 60 Hz interpolation: the governor that draws, reduces or drops the
// in-between frame (oot3d_native_interpolation_pacing.h), the reduced-replay
// eligibility of a sample and the config parser.
#include "oot3d_native_interpolation_pacing.h"

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

InterpolationPacingGovernor Make(bool keepSpeed = true, float reduced = 0.0F,
                                 bool always = false) {
  InterpolationPacingConfig config;
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
  // Only the probes (one in ProbeInterval) draw a full in-between frame.
  CHECK(dropped >= 590);
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
  CHECK(reduced >= 590);
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
  // Pairs between the recover (0.90) and budget (0.95) fractions, noisy.
  uint32_t seed = 12345U;
  for (int pair = 0; pair < 2000; ++pair) {
    seed = seed * 1664525U + 1013904223U;
    const double noise = ((seed >> 8) % 1000U) / 1000.0 - 0.5; // +-0.5 ms
    const double cost = 15.25 + noise;                          // pair ~30.5
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
    Pair(dropped, 15.25, [](auto level) { return Costs(level, 15.25, 15.25); });
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

int main() {
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
  TestSampleClearsEveryTarget();
  TestParseReducedScale();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_native_interpolation_pacing_tests: ok");
  return EXIT_SUCCESS;
}
