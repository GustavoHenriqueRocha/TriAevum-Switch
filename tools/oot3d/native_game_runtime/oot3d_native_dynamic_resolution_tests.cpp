// Dynamic resolution controller (oot3d_native_dynamic_resolution.h):
// limits, hysteresis, settling after a change, bounce backoff, the config
// parser and the deko3d calibration replayed on log16's perf.txt windows.
#include "oot3d_native_dynamic_resolution.h"
#include "fast/backends/gl_pica_dynamic_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Oot3dNativeGame::DynamicResolutionConfig;
using Oot3dNativeGame::UseDeko3dDynamicResolutionTiming;
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

// The threshold tests below use the bare timer pair (no hidden GPU); the
// tour19 tests use the defaults.
DynamicResolutionController Make(float minScale = 2.25F,
                                 double hiddenMs = 0.0) {
  DynamicResolutionConfig config;
  config.MaxScale = 3.0F;
  config.MinScale = minScale;
  config.HiddenGpuMs = hiddenMs;
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

// Resolution at 60 FPS (config/drs_60): Full holds 3.00 at 60 in a heavy
// scene (no down, shedding or not), a docked 4.5 likewise; Dynamic and 30
// FPS keep their levels.
void TestFullAtSixty() {
  using Oot3dNativeGame::ApplyDynamicResolutionSixtyMode;
  using Oot3dNativeGame::DynamicResolutionSixtyMode;
  using Oot3dNativeGame::ParseDynamicResolutionSixtyMode;
  CHECK(ParseDynamicResolutionSixtyMode("full") == DynamicResolutionSixtyMode::Full);
  CHECK(ParseDynamicResolutionSixtyMode(" Full\r\n") == DynamicResolutionSixtyMode::Full);
  CHECK(ParseDynamicResolutionSixtyMode("") == DynamicResolutionSixtyMode::Dynamic);
  CHECK(ParseDynamicResolutionSixtyMode("dynamic") == DynamicResolutionSixtyMode::Dynamic);
  CHECK(ParseDynamicResolutionSixtyMode("fullx") == DynamicResolutionSixtyMode::Dynamic);
  for (const float maxScale : {3.0F, 4.5F}) {
    DynamicResolutionConfig config;
    config.MaxScale = maxScale;
    config.MinScale = maxScale - 0.75F;
    UseDeko3dDynamicResolutionTiming(config);
    auto full = config;
    ApplyDynamicResolutionSixtyMode(60U, DynamicResolutionSixtyMode::Full, full);
    DynamicResolutionController c;
    c.Configure(full, kPair, config.MinScale);
    CHECK(Near(c.Scale(), maxScale));
    for (int i = 0; i < 2000; ++i)
      (void)c.ObservePair(1.5 * kPair, i % 2 == 0);
    CHECK(Near(c.Scale(), maxScale));
    CHECK(c.Stats().Downs == 0U && c.Stats().Changes == 0U);
    // Dynamic: the same scene goes down.
    auto dynamic = config;
    ApplyDynamicResolutionSixtyMode(60U, DynamicResolutionSixtyMode::Dynamic, dynamic);
    DynamicResolutionController d;
    d.Configure(dynamic, kPair);
    for (int i = 0; i < 2000; ++i)
      (void)d.ObservePair(1.5 * kPair);
    CHECK(d.Stats().Downs > 0U && Near(d.Scale(), config.MinScale));
    // 30 FPS: Full changes nothing.
    auto thirty = config;
    ApplyDynamicResolutionSixtyMode(30U, DynamicResolutionSixtyMode::Full, thirty);
    CHECK(Near(thirty.MinScale, config.MinScale));
  }
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

// tour19 Water Temple: the timer saw a pair of ~26 ms (under 85%) while the
// governor dropped in-between frames. With the hidden GPU (3 ms) it is over
// (29 > 28.3): down to 2.75 (~26.3 with the hidden part), and it stays.
void TestTour19WaterTempleHiddenGpu() {
  DynamicResolutionConfig defaults;
  CHECK(Near(defaults.HiddenGpuMs, 3.0));
  auto c = Make(2.25F, 3.0);
  Run(c, 26.0, 3);
  CHECK(Near(c.Scale(), 2.75));
  Run(c, 26.0, 2000);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.Stats().Downs == 1U);
  CHECK(c.Stats().Ups == 0U);
  // Without the hidden part the same scene never moved (the tour19 bug).
  auto old = Make(2.25F, 0.0);
  Run(old, 26.0, 2000);
  CHECK(Near(old.Scale(), 3.0));
}

// Calibration (tour17/tour19 perf.txt): pairs under ~24 ms kept almost
// every in-between frame at 3x: they stay at the configured scale.
void TestTour19FittingAreasStay() {
  auto c = Make(2.25F, 3.0);
  Run(c, 23.5, 3000);
  CHECK(Near(c.Scale(), 3.0));
  CHECK(c.Stats().Changes == 0U);
}

// The governor shedding in-between frames counts as over budget when the
// GPU pair is a real part of the budget (>= 65%: 21.7 ms).
void TestGovernorSheddingGoesDown() {
  auto c = Make(2.25F, 3.0);
  for (int i = 0; i < 3; ++i)
    (void)c.ObservePair(22.0, true);
  CHECK(Near(c.Scale(), 2.75));
  CHECK(c.Stats().Shedding == 3U);
  // A light GPU pair (CPU-bound shedding) never moves the scale.
  auto light = Make(2.25F, 3.0);
  for (int i = 0; i < 500; ++i)
    (void)light.ObservePair(15.0, true);
  CHECK(Near(light.Scale(), 3.0));
  CHECK(light.Stats().Shedding == 0U);
}

// No up step while the governor sheds; up again once it stops.
void TestNoUpWhileShedding() {
  auto c = Make(2.25F, 3.0);
  Run(c, 40.0, 200);
  CHECK(Near(c.Scale(), 2.25));
  for (int i = 0; i < 1000; ++i)
    (void)c.ObservePair(PairAt(c, 12.0), true);
  CHECK(Near(c.Scale(), 2.25));
  Run(c, 12.0, 400);
  CHECK(Near(c.Scale(), 3.0));
}

// Backend bookkeeping (gl_pica_dynamic_scale.h): the scale moves at a
// frame's colour clear or, without one, at its first draw (tour19: no
// colour clear ever reached the top-screen target, drs_applied=0); never
// twice within one frame.
void TestBackendAppliesAtFirstDraw() {
  using Fast::PicaGlDynamicScale::ScaleAtColourClear;
  using Fast::PicaGlDynamicScale::ScaleAtDraw;
  using Fast::PicaGlDynamicScale::TargetPhase;
  TargetPhase target{3.0F, 0U};
  uint32_t serial = 0U;
  // Frame 1 asks 2.75; the target is never cleared: first draw applies.
  ++serial;
  CHECK(ScaleAtDraw(target, serial, 2.75F));
  CHECK(Near(target.Scale, 2.75));
  // Later draws of the same frame do nothing, even if Requested moved.
  CHECK(!ScaleAtDraw(target, serial, 2.5F));
  CHECK(Near(target.Scale, 2.75));
  // The in-between frame (same serial) keeps it.
  CHECK(!ScaleAtDraw(target, serial, 2.75F));
  // Frame 2 asks the same scale: nothing to do.
  ++serial;
  CHECK(!ScaleAtDraw(target, serial, 2.75F));
  // Frame 3 clears first: the clear applies, its draws do not move again.
  ++serial;
  CHECK(Near(ScaleAtColourClear(target, serial, 2.5F), 2.5));
  CHECK(!ScaleAtDraw(target, serial, 2.5F));
  CHECK(Near(target.Scale, 2.5));
  // Every frame of a long run applies exactly once per change.
  int applied = 0;
  float want = 2.5F;
  for (int frame = 0; frame < 300; ++frame) {
    ++serial;
    if (frame % 50 == 0)
      want = want == 2.5F ? 3.0F : 2.5F;
    for (int draw = 0; draw < 40; ++draw)
      applied += ScaleAtDraw(target, serial, want) ? 1 : 0;
    CHECK(Near(target.Scale, want));
  }
  CHECK(applied == 6);
}

// ---- log16 (0.1.2-test.8, 07/11): the deko3d calibration ----

struct Log16Window {
  int Stretch;
  int HostFrames;
  const char *Scene;
  int Room;
  int Steps;
  float Scale;
  double StepMs;
  double MaxMs;
};

constexpr Log16Window kLog16[] = {
#include "oot3d_native_dynamic_resolution_log16.inc"
};

// Deterministic jitter: the same sequence in every run.
struct Lcg {
  uint64_t State = 12345U;
  explicit Lcg(uint64_t seed = 12345U) : State(seed) {}
  double Next() {
    State = (State * 1103515245U + 12345U) & 0x7FFFFFFFU;
    return static_cast<double>(State) / 0x7FFFFFFF;
  }
};

// The real cost of a step frame at `scale` against 2.25. realFixed: its
// fixed share, as measured in log16 between the same spot at 30 FPS (3.00)
// and 60 FPS (2.25): 0 in the Temple of Time (scene x1.81 for area x1.78),
// 0.3 in Forest Temple room 8 (x1.55). The controller keeps its own 0.35.
double RealRatio(double realFixed, double scale) {
  const double area = scale / 2.25;
  return realFixed + (1.0 - realFixed) * area * area;
}

DynamicResolutionConfig Deko3dConfig() {
  DynamicResolutionConfig config;
  config.MaxScale = 3.0F;
  config.MinScale = 2.25F;
  UseDeko3dDynamicResolutionTiming(config);
  return config;
}

DynamicResolutionConfig GlConfig() {
  DynamicResolutionConfig config;
  config.MaxScale = 3.0F;
  config.MinScale = 2.25F;
  return config;
}

struct Log16Replay {
  std::vector<std::vector<float>> Scales; // per window, each step frame's
  int OverBudget = 0; // step frames whose real pair passed 33.3 ms
  int Steps = 0;
  Oot3dNativeGame::DynamicResolutionStats Stats{};
};

// Every 60 FPS window of log16 in order, at its step_frames: the window's
// gpu_ms_step (taken back to 2.25 when the window ran at 2.5) with +-4%
// jitter and one step frame at the window's busy_max; the controller starts
// over at each stretch (the user's switches to 30 and back), and the
// governor sheds whenever the real pair passes the budget. config nullptr:
// held at 2.25.
Log16Replay ReplayLog16(const DynamicResolutionConfig *config,
                        double realFixed) {
  Log16Replay replay;
  Lcg random;
  DynamicResolutionController controller;
  int stretch = -1;
  for (const auto &window : kLog16) {
    if (window.Stretch != stretch) {
      stretch = window.Stretch;
      if (config != nullptr)
        controller.Configure(*config, kPair);
    }
    const double measured = RealRatio(realFixed, window.Scale);
    const double stepMs = window.StepMs / measured;
    const double maxMs = window.MaxMs / measured;
    auto &scales = replay.Scales.emplace_back();
    for (int k = 0; k < window.Steps; ++k) {
      double cost = stepMs * (0.96 + 0.08 * random.Next());
      if (k == window.Steps / 2)
        cost = std::max(cost, maxMs);
      const double scale = config != nullptr ? controller.Scale() : 2.25;
      const double pair = 2.0 * cost * RealRatio(realFixed, scale);
      ++replay.Steps;
      if (pair > kPair)
        ++replay.OverBudget;
      if (config != nullptr)
        (void)controller.ObservePair(pair, pair > kPair);
      scales.push_back(static_cast<float>(scale));
    }
  }
  replay.Stats = controller.Stats();
  return replay;
}

// Mean scale of the step frames in the windows firstFrames..lastFrames.
double MeanScale(const Log16Replay &replay, int firstFrames, int lastFrames) {
  double sum = 0.0;
  int count = 0;
  for (size_t i = 0; i < std::size(kLog16); ++i) {
    if (kLog16[i].HostFrames < firstFrames || kLog16[i].HostFrames > lastFrames)
      continue;
    for (const float scale : replay.Scales[i]) {
      sum += scale;
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

// The test.8 bug: with the GL calibration (3 ms hidden, down at 85%, up at
// 80%) the deko3d pairs of the title screen (11.1-11.4 ms a step frame at
// 2.25) and the Temple of Time (10.5-11.9) never climb.
void TestLog16GlCalibrationStaysAtFloor() {
  for (const double realFixed : {0.0, 0.3}) {
    const auto config = GlConfig();
    const auto replay = ReplayLog16(&config, realFixed);
    CHECK(MeanScale(replay, 450, 900) < 2.26);
    CHECK(MeanScale(replay, 4950, 6150) < 2.26);
    CHECK(MeanScale(replay, 22800, 23250) < 2.26);
  }
}

// The deko3d calibration on the same windows: the light areas climb to 2.5
// (600 lines; 3.00 costs 16-20 ms a frame there, more than a refresh), room
// 8 stays at 2.25 once in it, and the real pairs over budget (in-between
// frames the governor drops) barely move against the floor log16 ran at.
void TestLog16Deko3dReplay() {
  for (const double realFixed : {0.0, 0.3}) {
    const auto config = Deko3dConfig();
    const auto replay = ReplayLog16(&config, realFixed);
    const auto floor = ReplayLog16(nullptr, realFixed);
    std::printf("log16 replay (real fixed %.1f): title %.3f, Temple of Time "
                "%.3f / %.3f, all %.3f; over budget %d (floor %d) of %d; "
                "changes %llu ups %llu bounces %llu\n",
                realFixed, MeanScale(replay, 450, 900),
                MeanScale(replay, 4950, 6150), MeanScale(replay, 22800, 23250),
                MeanScale(replay, 0, 1 << 30), replay.OverBudget,
                floor.OverBudget, replay.Steps,
                static_cast<unsigned long long>(replay.Stats.Changes),
                static_cast<unsigned long long>(replay.Stats.Ups),
                static_cast<unsigned long long>(replay.Stats.Bounces));
    CHECK(MeanScale(replay, 450, 900) >= 2.45);
    CHECK(MeanScale(replay, 4950, 6150) >= 2.45);
    CHECK(MeanScale(replay, 22800, 23250) >= 2.45);
    CHECK(MeanScale(replay, 0, 1 << 30) > 2.29);
    // Forest Temple room 8 (busy 14-21 ms at 2.25): the floor, from the
    // second window in the room on.
    for (size_t i = 1; i < std::size(kLog16); ++i) {
      const auto &window = kLog16[i];
      const auto &before = kLog16[i - 1];
      if (std::strcmp(window.Scene, "bmori1") != 0 || window.Room != 8 ||
          std::strcmp(before.Scene, "bmori1") != 0 || before.Room != 8 ||
          before.Stretch != window.Stretch)
        continue;
      for (const float scale : replay.Scales[i])
        CHECK(Near(scale, 2.25));
    }
    CHECK(replay.OverBudget - floor.OverBudget <= 50);
    CHECK(replay.OverBudget - floor.OverBudget <= replay.Steps / 300);
    CHECK(replay.Stats.Bounces <= 6U);
  }
}

// No oscillation: a steady area (step frame 8-17 ms at 2.25, real fixed
// share 0-0.5, +-4-8% jitter and a 15% spike every 75 step frames) settles
// in 20 s and then changes the scale at most once in 80 s, never bouncing;
// the level it holds fits the budget.
void TestDeko3dSteadyAreasHoldOneLevel() {
  const auto config = Deko3dConfig();
  struct Noise {
    uint64_t Seed;
    double Jitter;
  };
  for (const double realFixed : {0.0, 0.15, 0.3, 0.5}) {
    for (int i = 0; i <= 36; ++i) {
      const double stepMs = 8.0 + 0.25 * i;
      for (const Noise noise : {Noise{1U, 0.04}, Noise{99U, 0.06},
                                Noise{5U, 0.08}}) {
        DynamicResolutionController c;
        c.Configure(config, kPair);
        Lcg random(noise.Seed);
        float last = c.Scale();
        int changes = 0;
        uint64_t bounces = 0;
        for (int k = 0; k < 3000; ++k) {
          double cost =
              stepMs * (1.0 - noise.Jitter + 2.0 * noise.Jitter * random.Next());
          if (k % 75 == 37)
            cost *= 1.15;
          const double pair = 2.0 * cost * RealRatio(realFixed, c.Scale());
          (void)c.ObservePair(pair, pair > kPair);
          if (k == 600)
            bounces = c.Stats().Bounces;
          if (k > 600 && !Near(c.Scale(), last))
            ++changes;
          last = c.Scale();
        }
        const double settled = 2.0 * stepMs * RealRatio(realFixed, c.Scale());
        const bool ok = changes <= 1 && c.Stats().Bounces == bounces &&
                        (Near(c.Scale(), 2.25) || settled <= 0.92 * kPair);
        if (!ok)
          std::fprintf(stderr,
                       "  steady %.2f ms fixed %.2f jitter %.2f: scale %.2f "
                       "changes %d pair %.1f\n",
                       stepMs, realFixed, noise.Jitter, c.Scale(), changes,
                       settled);
        CHECK(ok);
      }
    }
  }
  // The areas of log16 at their typical step frame: title 11.2 ms -> 2.5 or
  // 2.75, room 8 14.2 / 16.6 ms -> 2.25; a light 8 ms area reaches 3.00.
  const auto settle = [&](double stepMs, double realFixed) {
    DynamicResolutionController c;
    c.Configure(config, kPair);
    for (int k = 0; k < 2000; ++k)
      (void)c.ObservePair(2.0 * stepMs * RealRatio(realFixed, c.Scale()));
    return c.Scale();
  };
  // From 3.00 (a start) the descent stops at the first level under 92%:
  // with a fixed share of 0.3 the title's 2.75 (pair 30.1) holds.
  CHECK(Near(settle(11.2, 0.0), 2.5));
  CHECK(Near(settle(11.2, 0.3), 2.75));
  CHECK(Near(settle(14.2, 0.3), 2.25));
  CHECK(Near(settle(16.6, 0.3), 2.25));
  CHECK(Near(settle(8.0, 0.0), 3.0));
}

} // namespace

int main() {
  TestFullAtSixty();
  TestLog16GlCalibrationStaysAtFloor();
  TestLog16Deko3dReplay();
  TestDeko3dSteadyAreasHoldOneLevel();
  TestTour19WaterTempleHiddenGpu();
  TestTour19FittingAreasStay();
  TestGovernorSheddingGoesDown();
  TestNoUpWhileShedding();
  TestBackendAppliesAtFirstDraw();
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
