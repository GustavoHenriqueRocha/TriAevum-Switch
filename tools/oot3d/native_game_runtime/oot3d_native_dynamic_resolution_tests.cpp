// Dynamic resolution controller (oot3d_native_dynamic_resolution.h):
// limits, hysteresis, settling after a change, bounce backoff and the
// config parser.
#include "oot3d_native_dynamic_resolution.h"

#include <cmath>
#include <cstdio>

namespace {

using Oot3dNativeGame::DynamicResolutionConfig;
using Oot3dNativeGame::DynamicResolutionController;
using Oot3dNativeGame::ParseDynamicResolutionMinScale;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr double kPair = 2000.0 / 60.0;

bool Near(double a, double b) { return std::fabs(a - b) < 1.0e-4; }

DynamicResolutionController Make(float minScale = 2.25F) {
  DynamicResolutionConfig config;
  config.MaxScale = 3.0F;
  config.MinScale = minScale;
  DynamicResolutionController controller;
  controller.Configure(config, kPair);
  return controller;
}

// A scene whose pair costs `pairAt3` ms at scale 3 and follows the
// controller's own pixel model at other scales.
double PairAt(const DynamicResolutionController &c, double pairAt3) {
  return pairAt3 * c.CostRatio(3.0F, c.Scale());
}

void Run(DynamicResolutionController &c, double pairAt3, int steps) {
  for (int i = 0; i < steps; ++i)
    (void)c.ObservePair(PairAt(c, pairAt3));
}

void TestParser() {
  CHECK(Near(ParseDynamicResolutionMinScale("", 3.0F), 2.25));
  CHECK(Near(ParseDynamicResolutionMinScale("\n", 3.0F), 2.25));
  CHECK(Near(ParseDynamicResolutionMinScale("2.25", 3.0F), 2.25));
  CHECK(Near(ParseDynamicResolutionMinScale("2,5\r\n", 3.0F), 2.5));
  CHECK(Near(ParseDynamicResolutionMinScale(" 2.6", 3.0F), 2.5));
  CHECK(Near(ParseDynamicResolutionMinScale("2", 3.0F), 2.0));
  // Above the configured scale: nothing to vary.
  CHECK(Near(ParseDynamicResolutionMinScale("4", 3.0F), 3.0));
  // Below 1 or unreadable: the default.
  CHECK(Near(ParseDynamicResolutionMinScale("0.5", 3.0F), 2.25));
  CHECK(Near(ParseDynamicResolutionMinScale("abc", 3.0F), 2.25));
  // Low configured scales never go below 1.
  CHECK(Near(ParseDynamicResolutionMinScale("", 1.5F), 1.0));
  CHECK(Near(ParseDynamicResolutionMinScale("", 1.0F), 1.0));
}

void TestLevels() {
  auto c = Make();
  CHECK(c.LowestLevel() == 3U);
  CHECK(Near(c.ScaleOf(0U), 3.0));
  CHECK(Near(c.ScaleOf(1U), 2.75));
  CHECK(Near(c.ScaleOf(3U), 2.25));
  CHECK(Near(c.ScaleOf(9U), 2.25));
  auto none = Make(3.0F);
  CHECK(none.LowestLevel() == 0U);
  // Nothing to vary: never changes, whatever the cost.
  for (int i = 0; i < 100; ++i)
    CHECK(!none.ObservePair(60.0));
  CHECK(Near(none.Scale(), 3.0));
}

// A pair that fits stays at the configured scale.
void TestFitsStaysAtMax() {
  auto c = Make();
  Run(c, 20.0, 600);
  CHECK(Near(c.Scale(), 3.0));
  CHECK(c.Stats().Changes == 0U);
}

// tour17 Water Temple: 2 x 14.8 ms at 3.0 goes down, and settles at the
// first scale whose pair is under 85% (28.3 ms): 2.75 (~26.3 ms).
void TestWaterTempleGoesDownOneStep() {
  auto c = Make();
  const double water = 2.0 * 14.8;
  // Over budget, but only after DownAfter results in a row.
  CHECK(!c.ObservePair(water));
  CHECK(!c.ObservePair(water));
  CHECK(c.ObservePair(water));
  CHECK(Near(c.Scale(), 2.75));
  Run(c, water, 1200);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.Stats().Downs == 1U);
  CHECK(c.Stats().Ups == 0U);
}

// One slow result (a hitch) does not move the scale.
void TestSingleSpikeIgnored() {
  auto c = Make();
  Run(c, 20.0, 30);
  (void)c.ObservePair(60.0);
  Run(c, 20.0, 30);
  CHECK(Near(c.Scale(), 3.0));
}

// Never below the minimum, whatever the cost.
void TestMinimumLimit() {
  auto c = Make();
  Run(c, 80.0, 2000);
  CHECK(Near(c.Scale(), 2.25));
  CHECK(c.Level() == c.LowestLevel());
  auto high = Make(2.5F);
  Run(high, 80.0, 2000);
  CHECK(Near(high.Scale(), 2.5));
}

// After a change the next SettleSteps results (old scale, late GPU
// timestamps) are ignored: no second step down on stale costs.
void TestSettleAfterChange() {
  auto c = Make();
  for (int i = 0; i < 3; ++i)
    (void)c.ObservePair(40.0);
  CHECK(Near(c.Scale(), 2.75));
  // Five more stale results at the old cost: still 2.75.
  for (int i = 0; i < 5; ++i)
    CHECK(!c.ObservePair(40.0));
  CHECK(Near(c.Scale(), 2.75));
  // Fresh results still over budget: the next step down after DownAfter.
  CHECK(!c.ObservePair(40.0));
  CHECK(!c.ObservePair(40.0));
  CHECK(c.ObservePair(40.0));
  CHECK(Near(c.Scale(), 2.5));
}

// Up only with sustained room: UpAfter results in a row whose predicted
// pair at the scale above fits UpFraction; one result over breaks the run.
void TestUpNeedsSustainedRoom() {
  auto c = Make();
  Run(c, 40.0, 200);
  CHECK(Near(c.Scale(), 2.25));
  // The scene gets light: 18 ms at 3.0.
  const double light = 18.0;
  Run(c, light, 30);
  CHECK(Near(c.Scale(), 2.25));
  // One slow result is averaged away and does not break the run...
  {
    auto spiked = c;
    (void)spiked.ObservePair(40.0);
    Run(spiked, light, 31);
    CHECK(Near(spiked.Scale(), 2.5));
  }
  // ...but three in a row lift the predicted pair over 80% and do.
  for (int i = 0; i < 3; ++i)
    (void)c.ObservePair(40.0);
  CHECK(Near(c.Scale(), 2.25));
  Run(c, light, 59);
  CHECK(Near(c.Scale(), 2.25));
  Run(c, light, 30);
  CHECK(Near(c.Scale(), 2.5));
  // Then the rest of the way up (each level settles and waits again).
  Run(c, light, 400);
  CHECK(Near(c.Scale(), 3.0));
}

// Hysteresis: between the up threshold (predicted <= 80%) and the down one
// (> 85%) nothing moves. 2.75 measuring 27 ms (81%): it would cost ~30.3
// at 3.0, so it stays; 3.0 measuring 27.5 (82.5%) stays as well.
void TestHysteresisBand() {
  auto c = Make();
  for (int i = 0; i < 3; ++i)
    (void)c.ObservePair(30.0);
  CHECK(Near(c.Scale(), 2.75));
  for (int i = 0; i < 1000; ++i)
    (void)c.ObservePair(27.0);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.PredictedUpPairMs() > 0.85 * kPair);
  auto top = Make();
  for (int i = 0; i < 1000; ++i)
    (void)top.ObservePair(27.5);
  CHECK(Near(top.Scale(), 3.0));
}

// An up step that has to come back down soon doubles the wait before the
// next try of that level; a level that holds returns to UpAfter.
void TestBounceBackoff() {
  auto c = Make();
  for (int i = 0; i < 3; ++i)
    (void)c.ObservePair(30.0);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.UpWait(1U) == 60U);
  // Model says 3.0 fits (25 at 2.75 -> ~28.1 > 26.7: not yet). Use 23:
  // predicted ~25.9 <= 26.7, so it goes up after 60 + settle.
  Run(c, 23.0 / c.CostRatio(3.0F, 2.75F), 70);
  CHECK(Near(c.Scale(), 3.0));
  // But at 3.0 the scene really costs 30 (the model underestimated).
  for (int i = 0; i < 5 + 3; ++i)
    (void)c.ObservePair(30.0);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.Stats().Bounces == 1U);
  CHECK(c.UpWait(1U) == 120U);
  // Twice more: capped at MaxBackoff (8x).
  for (int round = 0; round < 4; ++round) {
    for (uint32_t i = 0; i < c.UpWait(1U) + 10U; ++i)
      (void)c.ObservePair(23.0);
    for (int i = 0; i < 5 + 3; ++i)
      (void)c.ObservePair(30.0);
  }
  CHECK(c.UpWait(1U) == 480U);
  // Now 3.0 really fits: up, and after BounceWindow results it holds.
  for (uint32_t i = 0; i < c.UpWait(1U) + 10U; ++i)
    (void)c.ObservePair(23.0);
  CHECK(Near(c.Scale(), 3.0));
  for (int i = 0; i < 200; ++i)
    (void)c.ObservePair(24.0);
  CHECK(Near(c.Scale(), 3.0));
  CHECK(c.UpWait(1U) == 60U);
}

void TestInvalidSamples() {
  auto c = Make();
  CHECK(!c.ObservePair(0.0));
  CHECK(!c.ObservePair(-1.0));
  CHECK(!c.ObservePair(std::nan("")));
  CHECK(!c.ObservePair(INFINITY));
  CHECK(c.Stats().Steps == 0U);
}

} // namespace

int main() {
  TestParser();
  TestLevels();
  TestFitsStaysAtMax();
  TestWaterTempleGoesDownOneStep();
  TestSingleSpikeIgnored();
  TestMinimumLimit();
  TestSettleAfterChange();
  TestUpNeedsSustainedRoom();
  TestHysteresisBand();
  TestBounceBackoff();
  TestInvalidSamples();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("dynamic resolution: all tests passed\n");
  return 0;
}
