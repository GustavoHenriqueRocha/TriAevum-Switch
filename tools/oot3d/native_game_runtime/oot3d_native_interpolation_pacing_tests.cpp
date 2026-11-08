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

// config/pacer_entry_recover on (the default) or off ("0").
InterpolationPacingGovernor MakeEntryRecover(bool on) {
  InterpolationPacingConfig config;
  config.EntryRecover = on;
  InterpolationPacingGovernor governor;
  governor.Configure(config, kPair, 2.0F, false);
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

// Below the preferred level, probes keep re-measuring the level above:
// every ProbeInterval decisions without config/pacer_entry_recover, and
// with it at 60, 120, 240, 240... decisions (the back-off).
void TestProbesHappenWhileDropped() {
  auto governor = Make();
  for (int pair = 0; pair < 600; ++pair)
    Pair(governor, 19.0, [](auto level) { return Costs(level, 19.0, 19.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  CHECK(governor.Stats().Probes >= 3U);
  CHECK(governor.Stats().FailedProbes == governor.Stats().Probes);
  auto former = MakeEntryRecover(false);
  for (int pair = 0; pair < 600; ++pair)
    Pair(former, 19.0, [](auto level) { return Costs(level, 19.0, 19.0); });
  CHECK(former.Level() == InterpolatedSampleLevel::Dropped);
  CHECK(former.Stats().Probes >= 8U);
}

// config/pacer_entry_recover, logs20a Kokiri Forest (20a/perf.txt:625,632;
// hitches.txt:79-97): 60 FPS before (pair ~26 ms); entering, about a second
// of one-off work (texture uploads, slow fences: step ~16.5, in-between
// ~19.3 ms) drops the in-between frames; afterwards a 16 + 16 pair fits
// (32.0 of the 32.3 recovery threshold) but the ratio learnt in the work
// stays: before, only the probe 60 decisions after the drop brought 60 back.
// With `signal`, the entry work is announced (SetEntryWork) while it runs,
// as the HD pack's preload and the grass placements do on deko3d.
// Returns the pairs at dropped after the work ended (-1: never recovered).
int EntrySpikeStay(bool entryRecover, bool signal, int workPairs,
                   uint64_t *failed) {
  auto governor = MakeEntryRecover(entryRecover);
  for (int pair = 0; pair < 120; ++pair)
    Pair(governor, 13.0, [](auto level) { return Costs(level, 13.0, 13.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  governor.NoteEntry();
  for (int pair = 0; pair < workPairs; ++pair) {
    governor.SetEntryWork(signal);
    Pair(governor, 16.5, [](auto level) { return Costs(level, 19.3, 19.3); });
  }
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  int stay = -1;
  for (int pair = 0; pair < 120; ++pair) {
    governor.SetEntryWork(false);
    const auto level = Pair(governor, 16.0, [](auto level) {
      return Costs(level, 16.0, 16.0, 0.6);
    });
    if (level == InterpolatedSampleLevel::Full && stay < 0)
      stay = pair;
  }
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  CHECK(governor.Stats().Entries == 1U);
  if (failed != nullptr)
    *failed = governor.Stats().FailedProbes;
  return stay;
}

void TestEntrySpikeThenRecovery() {
  // Work of 15, 30 and 45 pairs (0.5, 1 and 1.5 s). Before, the stay
  // lasted until the probe 60 decisions after the drop. With the work
  // announced, the probe EntryWorkSettle decisions after its end brings 60
  // back; without the signal, the entry probes (10, 20, 40, 60 decisions
  // apart) do within 30 pairs (1 s).
  for (const int work : {15, 30, 45}) {
    uint64_t failedSignal = 0, failedBlind = 0, failedOff = 0;
    const int signal = EntrySpikeStay(true, true, work, &failedSignal);
    const int blind = EntrySpikeStay(true, false, work, &failedBlind);
    const int off = EntrySpikeStay(false, true, work, &failedOff);
    std::printf("entry spike of %d pairs: %d pairs at dropped after it with "
                "the work signal (failed probes %llu), %d without (%llu), "
                "formerly %d (%llu)\n",
                work, signal, static_cast<unsigned long long>(failedSignal),
                blind, static_cast<unsigned long long>(failedBlind), off,
                static_cast<unsigned long long>(failedOff));
    CHECK(signal >= 0 &&
          signal <= static_cast<int>(InterpolationPacingGovernor::kEntryWorkSettle));
    CHECK(blind >= 0 && blind <= 30);
    CHECK(off > signal);
    CHECK(failedSignal <= 1U);
    CHECK(failedBlind <= 3U);
  }
  // The entry window ends: the shorter interval is for entries only.
  auto governor = MakeEntryRecover(true);
  governor.NoteEntry();
  CHECK(governor.InEntry());
  for (int pair = 0; pair < 300; ++pair)
    Pair(governor, 13.0, [](auto level) { return Costs(level, 13.0, 13.0); });
  CHECK(!governor.InEntry());
  CHECK(governor.ProbeIntervalNow(false) == 60U);
}

// A view that really is too heavy (20c 6900-7350, the Hyrule Field grove at
// Full: 18 + 18 ms, 1.08 of the budget; 20a 6450, the field entrance with
// its grass at the cap: 19.5 + 21 ms, 1.22) entered (its entry work
// announced for 1.2 s, as the pack's preload of 1.1-1.35 s) and held for
// 20 s: one drop and no oscillation; at most 2 more failed probes (each a
// late in-between frame) than the former every 60 decisions in the 10 s
// entry window, and fewer over the 20 s.
void TestSustainedHeavyViewDoesNotOscillate() {
  struct View {
    double Step, Full;
  };
  for (const View view : {View{18.0, 18.0}, View{19.5, 21.0}}) {
    uint64_t probes[2]{}, failed[2]{}, failedEntry[2]{}, entryProbes[2]{},
        changes[2]{};
    for (const bool on : {false, true}) {
      auto governor = MakeEntryRecover(on);
      for (int pair = 0; pair < 120; ++pair)
        Pair(governor, 13.0, [](auto level) { return Costs(level, 13.0, 13.0); });
      const auto before = governor.Stats();
      governor.NoteEntry();
      for (int pair = 0; pair < 600; ++pair) {
        governor.SetEntryWork(pair < 36);
        Pair(governor, view.Step, [&](auto level) {
          return Costs(level, view.Full, view.Full, 0.6);
        });
        if (pair == 299)
          failedEntry[on] = governor.Stats().FailedProbes - before.FailedProbes;
      }
      CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
      const auto &stats = governor.Stats();
      probes[on] = stats.Probes - before.Probes;
      failed[on] = stats.FailedProbes - before.FailedProbes;
      entryProbes[on] = stats.EntryProbes;
      changes[on] = stats.LevelChanges - before.LevelChanges;
      CHECK(failed[on] == probes[on]);
      // One drop, never back: the probes do not change the level.
      CHECK(changes[on] == 1U);
      CHECK(stats.EntryBelowDecisions >= 290U);
    }
    std::printf("heavy view %.1f + %.1f ms: failed probes %llu in the entry "
                "window, %llu in 20 s (%llu entry probes); formerly %llu, "
                "%llu\n",
                view.Step, view.Full,
                static_cast<unsigned long long>(failedEntry[1]),
                static_cast<unsigned long long>(failed[1]),
                static_cast<unsigned long long>(entryProbes[1]),
                static_cast<unsigned long long>(failedEntry[0]),
                static_cast<unsigned long long>(failed[0]));
    CHECK(failedEntry[1] <= failedEntry[0] + 2U);
    CHECK(failed[1] <= failed[0]);
    CHECK(entryProbes[0] == 0U);
  }
  // Over the entry fraction (1.22): no entry probe at all.
  auto governor = MakeEntryRecover(true);
  governor.NoteEntry();
  for (int pair = 0; pair < 300; ++pair)
    Pair(governor, 19.5, [](auto level) { return Costs(level, 21.0, 21.0); });
  CHECK(governor.Stats().EntryProbes == 0U);
}

// Outside an entry: each failed probe doubles the interval (60, 120, 240,
// capped), a probe that fits or a return to full resets it.
void TestFailedProbeBacksOff() {
  auto governor = MakeEntryRecover(true);
  std::vector<int> probeAt;
  for (int pair = 0; pair < 900; ++pair) {
    const auto probes = governor.Stats().Probes;
    Pair(governor, 18.0, [](auto level) { return Costs(level, 18.0, 18.0); });
    if (governor.Stats().Probes != probes)
      probeAt.push_back(pair);
  }
  CHECK(probeAt.size() >= 4U);
  std::vector<int> gaps;
  for (size_t index = 1U; index < probeAt.size(); ++index)
    gaps.push_back(probeAt[index] - probeAt[index - 1U]);
  CHECK(gaps.size() >= 3U && gaps[0] == 120 && gaps[1] == 240 &&
        gaps[2] == 240);
  CHECK(governor.FailedProbesInRow() == probeAt.size());
  CHECK(governor.ProbeIntervalNow(false) == 240U);
  // Lighter: the estimate brings full back (no probe needed) and the
  // back-off starts over.
  for (int pair = 0; pair < 30; ++pair)
    Pair(governor, 11.0, [](auto level) { return Costs(level, 11.0, 11.0); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  CHECK(governor.FailedProbesInRow() == 0U);
  CHECK(governor.ProbeIntervalNow(false) == 60U);
  // A probe that fits resets it too (the full frame got cheaper than its
  // stale estimate; TestProbeRefreshesAStaleEstimate).
  auto stale = MakeEntryRecover(true);
  for (int pair = 0; pair < 20; ++pair)
    Pair(stale, 10.0, [](auto level) { return Costs(level, 30.0, 30.0); });
  CHECK(stale.Level() == InterpolatedSampleLevel::Dropped);
  for (int pair = 0; pair < 100; ++pair)
    Pair(stale, 10.0, [](auto level) { return Costs(level, 10.0, 10.0); });
  CHECK(stale.Level() == InterpolatedSampleLevel::Full);
  CHECK(stale.Stats().FailedProbes == 0U);
  // Without config/pacer_entry_recover: always ProbeInterval.
  auto former = MakeEntryRecover(false);
  for (int pair = 0; pair < 600; ++pair)
    Pair(former, 18.0, [](auto level) { return Costs(level, 18.0, 18.0); });
  CHECK(former.FailedProbesInRow() >= 8U);
  CHECK(former.ProbeIntervalNow(false) == 60U);
  CHECK(former.ProbeIntervalNow(true) == 60U);
}

// Learns a full-frame ratio of about 1.24 (22a 6150-6300, the Hyrule Field
// entry: est_full 26.2 against est_step 21.1) and drops.
void LearnHighRatio(InterpolationPacingGovernor &governor) {
  for (int pair = 0; pair < 60; ++pair)
    Pair(governor, 11.0, [](auto level) { return Costs(level, 12.3, 12.3); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  // The full frames measured while the step average still climbs: the
  // ratio settles at about 1.24.
  for (int pair = 0; pair < 10; ++pair)
    Pair(governor, 21.0, [](auto level) { return Costs(level, 20.5, 20.5); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  const double ratio =
      governor.Estimate(InterpolatedSampleLevel::Full) / governor.StepCostMs();
  CHECK(ratio > 1.2 && ratio < 1.3);
}

struct StaleRun {
  int RecoveredAt = -1; // pair of the fall at which full came back
  uint64_t Probes = 0, StaleProbes = 0, LevelChanges = 0;
};

// logs22a/perf.txt:359-373: after the drop a long heavy stretch with
// nothing drawn (pair 42-47 ms: no probe, `probes=0` in 6450 and 6600),
// then the step falls to 11 ms while the full frame really costs 1.12 x
// the step (as 6750 measured). heavyPairs sets where the fall lands in the
// 60-decision probe schedule.
StaleRun StaleRatioFall(bool on, int heavyPairs) {
  auto governor = MakeEntryRecover(on);
  LearnHighRatio(governor);
  for (int pair = 0; pair < heavyPairs; ++pair)
    Pair(governor, 19.5, [](auto level) { return Costs(level, 19.5, 19.5); });
  CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
  CHECK(governor.Estimate(InterpolatedSampleLevel::Full) >
        1.2 * governor.StepCostMs());
  CHECK(governor.RatioAge(InterpolatedSampleLevel::Full) >=
        static_cast<uint32_t>(heavyPairs));
  const auto before = governor.Stats();
  StaleRun run;
  for (int pair = 0; pair < 120; ++pair) {
    const double step = std::max(11.0, 19.5 - 0.15 * pair);
    const auto level = Pair(governor, step, [&](auto level) {
      return Costs(level, 1.12 * step, 1.12 * step, 0.6);
    });
    if (level == InterpolatedSampleLevel::Full && run.RecoveredAt < 0 &&
        governor.Level() == InterpolatedSampleLevel::Full)
      run.RecoveredAt = pair;
  }
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
  run.Probes = governor.Stats().Probes - before.Probes;
  run.StaleProbes = governor.Stats().StaleProbes - before.StaleProbes;
  run.LevelChanges = governor.Stats().LevelChanges - before.LevelChanges;
  return run;
}

void TestStaleRatioRecoversWithinOneProbe() {
  // The fall at every phase of the 60-decision schedule: with the stale
  // probe, full comes back after one probe, at the first decision whose
  // step alone fits with a fresh ratio; formerly at the next scheduled
  // probe or when the step fits the stale 1.24.
  int worstOn = 0, worstOff = 0;
  uint64_t staleProbes = 0;
  for (int heavy = 150; heavy < 210; heavy += 6) {
    const auto on = StaleRatioFall(true, heavy);
    const auto off = StaleRatioFall(false, heavy);
    std::printf("stale ratio 1.24, %d heavy pairs, step falling 19.5 -> 11 "
                "ms: back at full after %d pairs (probes %llu, stale %llu), "
                "formerly %d (%llu)\n",
                heavy, on.RecoveredAt, static_cast<unsigned long long>(on.Probes),
                static_cast<unsigned long long>(on.StaleProbes), off.RecoveredAt,
                static_cast<unsigned long long>(off.Probes));
    CHECK(on.RecoveredAt >= 0 && on.RecoveredAt <= off.RecoveredAt);
    // One probe and one level change: back to full.
    CHECK(on.Probes == 1U);
    CHECK(on.LevelChanges == 1U);
    CHECK(off.StaleProbes == 0U);
    worstOn = std::max(worstOn, on.RecoveredAt);
    worstOff = std::max(worstOff, off.RecoveredAt);
    staleProbes += on.StaleProbes;
  }
  CHECK(worstOn < worstOff);
  CHECK(staleProbes >= 5U);
  // The step settles between the two thresholds (15.0 ms: 32.33 / 2.24 =
  // 14.43 < 15.0 < 32.33 / 2.10 = 15.40) after regular probes failed in a
  // heavier view whose in-between frame really cost 1.24 x (the back-off
  // at 240 decisions): at every phase of that schedule the stale probe
  // still comes within about a second.
  int plateauOn = 0, plateauOff = 0;
  for (int heavy = 600; heavy < 840; heavy += 12) {
    for (const bool on : {false, true}) {
      auto governor = MakeEntryRecover(on);
      for (int pair = 0; pair < 60; ++pair)
        Pair(governor, 11.0, [](auto level) { return Costs(level, 12.3, 12.3); });
      for (int pair = 0; pair < heavy; ++pair)
        Pair(governor, 17.0, [](auto level) { return Costs(level, 21.1, 21.1); });
      CHECK(governor.Level() == InterpolatedSampleLevel::Dropped);
      if (on)
        CHECK(governor.ProbeIntervalNow(false) == 240U);
      int recovered = -1;
      for (int pair = 0; pair < 300; ++pair) {
        const auto level = Pair(governor, 15.0, [](auto level) {
          return Costs(level, 15.0, 15.0, 0.6);
        });
        if (level == InterpolatedSampleLevel::Full && recovered < 0 &&
            governor.Level() == InterpolatedSampleLevel::Full)
          recovered = pair;
      }
      CHECK(governor.Level() == InterpolatedSampleLevel::Full);
      CHECK(recovered >= 0);
      int &worst = on ? plateauOn : plateauOff;
      worst = std::max(worst, recovered);
    }
  }
  std::printf("stale ratio 1.24 after failed probes, step settling at 15.0 "
              "ms: back at full within %d pairs at any phase, formerly %d\n",
              plateauOn, plateauOff);
  CHECK(plateauOn <= 40 && plateauOn < plateauOff);
}

// The heavy views of Perf21 and Perf22 (steps of 15.9-22 ms; the step alone
// keeps the pair out even with a fresh ratio) with a stale ratio of 1.24
// first: no stale probe and no extra level change. A view whose ratio
// really is high (15 ms, 1.2: pair 33.0, over 32.33) gets stale probes with
// the back-off and never changes level.
void TestStaleRatioHeavyViewsDoNotFlap() {
  struct View {
    double Step, Ratio, Noise;
  };
  for (const View view : {View{16.8, 1.02, 0.0}, View{16.0, 1.0, 0.45},
                          View{17.4, 1.01, 0.0}, View{20.4, 1.08, 0.0},
                          View{15.0, 1.2, 0.0}}) {
    uint64_t changes[2]{}, stale[2]{}, probes[2]{};
    for (const bool on : {false, true}) {
      auto governor = MakeEntryRecover(on);
      LearnHighRatio(governor);
      const auto before = governor.Stats();
      uint32_t seed = 4242U;
      for (int pair = 0; pair < 600; ++pair) {
        seed = seed * 1664525U + 1013904223U;
        const double noise =
            view.Noise * (((seed >> 8) % 1000U) / 500.0 - 1.0);
        const double step = view.Step + noise;
        Pair(governor, step, [&](auto level) {
          return Costs(level, view.Ratio * step, view.Ratio * step, 0.6);
        });
      }
      changes[on] = governor.Stats().LevelChanges - before.LevelChanges;
      stale[on] = governor.Stats().StaleProbes - before.StaleProbes;
      probes[on] = governor.Stats().Probes - before.Probes;
    }
    std::printf("heavy view %.1f ms x %.2f (noise %.2f): %llu level changes, "
                "%llu stale probes of %llu; formerly %llu changes, %llu "
                "probes\n",
                view.Step, view.Ratio, view.Noise,
                static_cast<unsigned long long>(changes[1]),
                static_cast<unsigned long long>(stale[1]),
                static_cast<unsigned long long>(probes[1]),
                static_cast<unsigned long long>(changes[0]),
                static_cast<unsigned long long>(probes[0]));
    CHECK(changes[1] <= changes[0]);
    if (view.Step * (1.0 + 1.10) > 0.97 * kPair)
      CHECK(stale[1] == 0U);
    else
      CHECK(stale[1] <= 5U && changes[1] == 0U);
  }
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
  CHECK(InterpolationPacingConfig{}.MarginMs == 0.0); // pgo5i
  CHECK(Oot3dNativeGame::kExcludedWaitMarginMs == 5.0);
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
  TestEntrySpikeThenRecovery();
  TestSustainedHeavyViewDoesNotOscillate();
  TestFailedProbeBacksOff();
  TestStaleRatioRecoversWithinOneProbe();
  TestStaleRatioHeavyViewsDoNotFlap();
  TestSampleClearsEveryTarget();
  TestParseReducedScale();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_native_interpolation_pacing_tests: ok");
  return EXIT_SUCCESS;
}
