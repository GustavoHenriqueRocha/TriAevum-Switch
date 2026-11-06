// Per-spike measurement (oot3d_frame_spike_log.h), vsync clock
// (oot3d_vsync_clock.h) and vsync-anchored pacer (config/pacer_vsync).
// --report prints the drift simulation table.
#include "oot3d_frame_spike_log.h"
#include "oot3d_native_cadence_sim.h"
#include "oot3d_native_presentation_pacer.h"
#include "oot3d_vsync_clock.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

namespace {

using namespace Oot3dNativeGame;

int gFailures = 0;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr double kNominalNs = 1.0e9 / 60.0;
// Display ~0.2% slower than the CPU clock (console).
constexpr double kDisplayNs = kNominalNs * 1.002;

void TestVsyncModelPeriodAndPhase() {
  std::mt19937 rng(7U);
  std::uniform_real_distribution<double> wake(0.0, 300000.0); // up to 300 us
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  VsyncClockModel model;
  const double phase0 = 3.0e9;
  for (int index = 0; index < 3000; ++index) {
    if (unit(rng) < 0.03) continue; // the thread missed this vsync
    const double t = phase0 + index * kDisplayNs + wake(rng);
    model.Observe(static_cast<int64_t>(t));
  }
  const auto snapshot = model.Snapshot();
  Require(snapshot.Valid, "valid model");
  Require(std::abs(snapshot.PeriodNs - kDisplayNs) < 20.0,
          "measured period within 20 ns of the real one");
  // Phase: a real vsync within 100 us (lower envelope).
  const double since = static_cast<double>(snapshot.LastVsyncNs) - phase0;
  const double error = since - std::round(since / kDisplayNs) * kDisplayNs;
  Require(error > -20000.0 && error < 100000.0, "phase near the real vsync");
  Require(snapshot.Resets == 0U, "no restart with normal jitter");
}

// tour19: without the display event (rc 0x1272), the fallback source is the return
// of an acquire that blocked: vsync + 50-500 us latency, on 1 of every 1-3
// vsyncs (the render thread doesn't always wait), mixed with acquires that
// didn't wait (return at any time) and that the filter discards.
void TestAcquireFallbackFindsVsync() {
  std::mt19937 rng(11U);
  std::uniform_real_distribution<double> latency(50000.0, 500000.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  AcquireVsyncFilter filter;
  const double phase0 = 5.0e9;
  int index = 0;
  for (int frame = 0; frame < 4000; ++frame) {
    index += unit(rng) < 0.6 ? 1 : (unit(rng) < 0.7 ? 2 : 3);
    if (unit(rng) < 0.35) {
      // Didn't block: returns mid-frame, short wait.
      const double t = phase0 + index * kDisplayNs + unit(rng) * kDisplayNs;
      Require(!filter.Observe(static_cast<int64_t>(t), unit(rng) * 1.4),
              "acquire without wait is left out");
      continue;
    }
    const double t = phase0 + index * kDisplayNs + latency(rng);
    (void)filter.Observe(static_cast<int64_t>(t), 2.0 + unit(rng) * 10.0);
  }
  const auto snapshot = filter.Snapshot();
  Require(snapshot.Valid, "fallback source: valid model");
  Require(std::abs(snapshot.PeriodNs - kDisplayNs) < 50.0,
          "fallback source: period within 50 ns of the real one");
  const double since = static_cast<double>(snapshot.LastVsyncNs) - phase0;
  const double error = since - std::round(since / kDisplayNs) * kDisplayNs;
  Require(error > -20000.0 && error < 150000.0,
          "fallback source: phase within 150 us of the real vsync");
  Require(snapshot.Resets == 0U, "fallback source: no restart");
  Require(filter.Used() > 2000U && filter.Ignored() > 1000U,
          "fallback source: used and ignored counts");
}

// The Switch clock on the host opens nothing; the fallback turns on and publishes.
void TestSwitchClockFallbackOnHost() {
  SwitchVsyncClock clock;
  Require(!clock.Start(), "host: no vsync event");
  Require(clock.Source() == VsyncClockSource::None, "host: no source");
  clock.ObserveAcquire(1000, 5.0); // before turning on: ignored
  Require(clock.AcquireSamples() == 0U, "host: disabled fallback ignores");
  Require(clock.StartAcquireFallback(), "host: fallback turns on");
  Require(clock.Source() == VsyncClockSource::Acquire, "host: acquire source");
  for (int index = 0; index < 100; ++index)
    clock.ObserveAcquire(static_cast<int64_t>(1.0e9 + index * kDisplayNs + 1.0e5),
                         4.0);
  const auto snapshot = clock.Read();
  Require(snapshot.Valid && std::abs(snapshot.PeriodNs - kDisplayNs) < 5.0,
          "host: fallback publishes the period");
  gAcquireVsyncClock.store(&clock);
}

void TestVsyncModelResetsOnModeChange() {
  VsyncClockModel model;
  for (int index = 0; index < 200; ++index)
    model.Observe(static_cast<int64_t>(1.0e9 + index * kDisplayNs));
  // Half-period phase jump (mode change, suspend).
  const double t0 = 1.0e9 + 200.5 * kDisplayNs;
  for (int index = 0; index < 200; ++index)
    model.Observe(static_cast<int64_t>(t0 + index * kDisplayNs));
  const auto snapshot = model.Snapshot();
  Require(snapshot.Resets == 1U, "one restart");
  Require(snapshot.Valid && std::abs(snapshot.PeriodNs - kDisplayNs) < 5.0,
          "period again after the restart");
}

void TestSnap() {
  const double p = kDisplayNs;
  // Deadline 3 ms after vsync 10: goes to vsync 10 + 1 ms.
  const int64_t last = 1000000000;
  const int64_t deadline = last + static_cast<int64_t>(10 * p + 3.0e6);
  const int64_t snapped = SnapDeadlineToVsync(deadline, last, p, 1.0e6);
  Require(std::llabs(snapped - (last + static_cast<int64_t>(10 * p + 1.0e6))) <= 1,
          "snap to nearest vsync + offset");
  // Deadline before lastVsync (new vsync published afterwards): still on the grid.
  const int64_t before = SnapDeadlineToVsync(last - 2000000, last, p, 0.0);
  Require(before == last, "snap backwards");
}

void TestRefreshCounterVsync() {
  SpikeRefreshCounter counter;
  const int64_t v0 = 1000000000;
  const double p = kDisplayNs;
  const auto at = [&](double periods, double extraNs) {
    return static_cast<int64_t>(v0 + periods * p + extraNs);
  };
  Require(counter.Observe(at(0, 1.0e6), true, v0, p, kNominalNs) == 1U, "first");
  Require(counter.Observe(at(1, 1.0e6), true, v0, p, kNominalNs) == 1U, "on time");
  Require(counter.Observe(at(2, 1.5e6), true, v0, p, kNominalNs) == 1U, "jitter within");
  // Swap 2 ms after vsync 4 (skipped 3): 2 refreshes.
  Require(counter.Observe(at(4, 2.0e6), true, v0, p, kNominalNs) == 2U, "missed one");
  // Two swaps in the same interval: the queue shows the second at the next vsync.
  Require(counter.Observe(at(4, 9.0e6), true, v0, p, kNominalNs) == 1U, "queue 1");
  Require(counter.Observe(at(5, 0.5e6), true, v0, p, kNominalNs) == 1U, "queue 2");
  // 3-refresh hold.
  Require(counter.Observe(at(9, 1.0e6), true, v0, p, kNominalNs) == 3U, "three");
}

void TestRefreshCounterFallback() {
  SpikeRefreshCounter counter;
  const double p = kNominalNs;
  int64_t t = 0;
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "first");
  t += static_cast<int64_t>(p * 1.1);
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "1.1 periods = 1");
  t += static_cast<int64_t>(p * 2.0);
  Require(counter.Observe(t, false, 0, 0.0, p) == 2U, "2 periods");
  t += static_cast<int64_t>(p * 0.3);
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "short counts 1");
}

void TestClassificationRingAndFormat() {
  FrameSpikeTotals totals;
  FrameSpikeRing<8> ring;
  const double period = 1000.0 / 60.0;
  FrameSpikeSample sample;
  sample.Kind = SpikeFrameKind::Step;
  sample.Refreshes = 1U;
  Require(!ObserveFrameSpike(sample, period, totals, ring), "on time doesn't record");
  sample.Refreshes = 2U;
  sample.GameBusyMs = 22.0F;
  sample.HostFrame = 42U;
  sample.GuestFrame = 21U;
  sample.Context = kSpikeLoad | kSpikeShader;
  sample.TopStage = 2U;
  sample.TopStageMs = 9.5F;
  Require(ObserveFrameSpike(sample, period, totals, ring), "missed: records");
  sample.GameBusyMs = 5.0F;
  sample.RenderBusyMs = 18.0F;
  sample.Refreshes = 3U;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  sample.RenderBusyMs = 5.0F;
  sample.GpuMs = 17.5F;
  sample.Refreshes = 2U;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  sample.GpuMs = 10.0F;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  // Locked-30 step: 2 expected refreshes, 2 measured = on time.
  sample.Expected = 2U;
  Require(!ObserveFrameSpike(sample, period, totals, ring), "locked 30 on time");
  // ... and at 30 the budget is two periods: 25 ms of game isn't its fault.
  sample.Refreshes = 3U;
  sample.GameBusyMs = 25.0F;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  const auto snapshot = totals.Take();
  Require(snapshot.Presents == 7U, "presents");
  Require(snapshot.MissedFrames == 5U, "frames that missed");
  Require(snapshot.MissedRefreshes == 6U, "missed refreshes");
  Require(snapshot.MissedByStage[0] == 1U && snapshot.MissedByStage[1] == 2U &&
              snapshot.MissedByStage[2] == 1U && snapshot.MissedByStage[3] == 2U,
          "per stage (guest 1, render 2, gpu 1, pacer 2)");
  std::string text;
  const size_t drained = ring.Drain([&](const FrameSpikeRecord &record) {
    char line[512];
    const int size = FormatFrameSpike(record, line, sizeof(line));
    Require(size > 0 && static_cast<size_t>(size) < sizeof(line), "line fits");
    text += line;
  });
  Require(drained == 5U, "ring with 5");
  Require(text.find("host=42 guest=21 kind=step refreshes=2 expected=1 stage=guest") !=
              std::string::npos,
          "main fields");
  Require(text.find("top=pica_top top_ms=9.50 ctx=load,shader") != std::string::npos,
          "stage and context");
  Require(text.find("stage=render") != std::string::npos &&
              text.find("stage=gpu") != std::string::npos &&
              text.find("stage=pacer") != std::string::npos,
          "all stages");
  // Full ring: drops and counts, without blocking.
  for (int index = 0; index < 10; ++index) (void)ring.Push({});
  Require(ring.Dropped() == 2U, "full ring drops");
}

// Drift: display 0.2% slower than the pacer, everything within budget
// (game 14 ms per step, render thread 9-11 ms, GPU 10 ms).
CadenceSim::Result RunDrift(bool pacerVsync, uint32_t framesInFlight) {
  CadenceSim::Segment segment;
  segment.Seconds = 60.0;
  segment.GameStepMs = 14.0;
  segment.GpuMs = 10.0;
  segment.RtStepMinMs = 9.0;
  segment.RtStepMaxMs = 11.0;
  segment.SpikeProbability = 0.0;
  CadenceSim::Params params;
  params.DisplayRefreshMs = params.RefreshMs * 1.002;
  params.SwapchainBuffers = 3U;
  params.FramesInFlight = framesInFlight;
  params.PacerVsync = pacerVsync;
  return CadenceSim::Run({segment}, params);
}

void TestDriftDisappearsWithVsyncPacer(bool report) {
  if (report)
    std::printf("drift 60 s (display 0.2%% slower, swapchain 3, within budget)\n");
  for (const uint32_t inFlight : {1U, 2U}) {
    const auto cpu = RunDrift(false, inFlight);
    const auto vsync = RunDrift(true, inFlight);
    if (report) {
      for (const auto *run : {&cpu, &vsync}) {
        std::printf("  frames_in_flight=%u pacer %-5s: held_refresh=%llu missed_refresh=%llu "
                    "pictures=%llu acquire_wait_ms=%.0f judder_ms=%.2f\n",
                    inFlight, run == &cpu ? "CPU" : "vsync",
                    static_cast<unsigned long long>(run->HeldPictureRefreshes),
                    static_cast<unsigned long long>(run->MissedRefreshes),
                    static_cast<unsigned long long>(run->Pictures), run->AcquireWaitMs,
                    run->JudderRmsMs);
      }
    }
    // CPU pacer: the swapchain fills up (acquire holds the render thread
    // half the time) and, at each phase wrap, one refresh with no new image.
    Require(cpu.HeldPictureRefreshes >= 3U, "CPU pacer: periodic refresh with no new image");
    Require(cpu.AcquireWaitMs > 10000.0, "CPU pacer: render thread stuck in acquire");
    // On vsync: no refresh without a new image, swapchain never full.
    Require(vsync.HeldPictureRefreshes == 0U, "vsync pacer: no refresh without a new image");
    Require(vsync.MissedRefreshes == 0U, "vsync pacer: no missed vsync");
    Require(vsync.AcquireWaitMs < 1.0, "vsync pacer: acquire doesn't wait");
    // One image per display refresh (59.88 Hz x 60 s).
    Require(vsync.Pictures + 2U >= static_cast<uint64_t>(60000.0 / (1000.0 / 60.0 * 1.002)),
            "vsync pacer: one new image per refresh");
  }
  // Without drift, the vsync pacer matches the CPU one.
  CadenceSim::Segment segment;
  segment.Seconds = 20.0;
  segment.SpikeProbability = 0.0;
  segment.GameStepMs = 14.0;
  segment.GpuMs = 10.0;
  CadenceSim::Params params;
  params.SwapchainBuffers = 3U;
  const auto cpu = CadenceSim::Run({segment}, params);
  params.PacerVsync = true;
  const auto vsync = CadenceSim::Run({segment}, params);
  Require(vsync.HeldPictureRefreshes <= cpu.HeldPictureRefreshes &&
              vsync.Pictures + 1U >= cpu.Pictures,
          "no drift: vsync doesn't make it worse");
}

// The real pacer with a fake vsync reader: deadlines land on the grid
// (half-period phase relative to the start).
struct FakeVsync {
  int64_t LastNs = 0;
  double PeriodNs = kDisplayNs;
};
bool ReadFakeVsync(const void *user, VsyncClockSnapshot *snapshot) {
  const auto *fake = static_cast<const FakeVsync *>(user);
  snapshot->Valid = true;
  snapshot->LastVsyncNs = fake->LastNs;
  snapshot->PeriodNs = fake->PeriodNs;
  return true;
}

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void TestPacerAnchorsToVsyncGrid() {
  NativeRealtimeRefreshPacer pacer(true, 60U);
  FakeVsync fake;
  fake.LastNs = SteadyNs() + static_cast<int64_t>(kDisplayNs / 2.0);
  pacer.SetVsyncAnchor(&ReadFakeVsync, &fake, std::chrono::nanoseconds(0));
  pacer.WaitForNextRefresh(); // starts the clock (now, off the grid)
  int onGrid = 0;
  constexpr int kWaits = 24;
  for (int index = 0; index < kWaits; ++index) {
    pacer.WaitForNextRefresh();
    const double since = static_cast<double>(SteadyNs() - fake.LastNs);
    const double phase = since - std::floor(since / kDisplayNs) * kDisplayNs;
    // Woke up to 3 ms after a grid vsync (host: scheduler).
    if (phase < 3.0e6) ++onGrid;
  }
  Require(onGrid >= kWaits - 3, "anchored pacer wakes on the vsync grid");
  const auto tail = pacer.TailCounters();
  Require(tail.VsyncAnchoredWaits == static_cast<uint64_t>(kWaits),
          "anchored waits counted");
  // No reader: back to the CPU clock.
  pacer.SetVsyncAnchor(nullptr, nullptr, std::chrono::nanoseconds(0));
  pacer.WaitForNextRefresh();
  Require(pacer.TailCounters().VsyncAnchoredWaits == static_cast<uint64_t>(kWaits),
          "no reader doesn't anchor");
}

// config/pacer_vsync (shipped on) with the FPS of L3+R3 > Video: a 30 FPS
// target waits two display periods per present, 60 one, and a live switch
// (Configure) changes the stride at once. The anchored pacer used to wait one
// period whatever the target, so 30 FPS presented every vsync.
void TestAnchoredPacerHonoursTargetRate() {
  Require(VsyncPeriodsPerPresentation(30U, kDisplayNs) == 2U, "30 FPS: two display periods");
  Require(VsyncPeriodsPerPresentation(60U, kDisplayNs) == 1U, "60 FPS: one display period");
  Require(VsyncPeriodsPerPresentation(90U, kDisplayNs) == 1U, "above the display: every vsync");
  Require(VsyncPeriodsPerPresentation(0U, kDisplayNs) == 1U, "no target: one");
  NativeRealtimeRefreshPacer pacer(true, 30U);
  pacer.SetMaximumSpin(std::chrono::nanoseconds(0));
  FakeVsync fake;
  fake.LastNs = SteadyNs();
  pacer.SetVsyncAnchor(&ReadFakeVsync, &fake, std::chrono::nanoseconds(0));
  const auto strides = [&](int expected) {
    pacer.WaitForNextRefresh(); // (re)starts the clock: at once
    constexpr int kWaits = 10;
    int64_t previous = SteadyNs();
    int matching = 0;
    for (int index = 0; index < kWaits; ++index) {
      pacer.WaitForNextRefresh();
      const int64_t now = SteadyNs();
      const double periods = static_cast<double>(now - previous) / kDisplayNs;
      previous = now;
      if (std::abs(periods - expected) < 0.5) ++matching;
    }
    return matching >= kWaits - 2;
  };
  Require(strides(2), "anchored 30 FPS: presents two display periods apart");
  pacer.Configure(true, 60U); // L3+R3 > Video > 60 FPS
  Require(strides(1), "anchored 60 FPS after the switch: one display period");
  pacer.Configure(true, 30U);
  Require(strides(2), "anchored 30 FPS after switching back: two display periods");
}

// config/early_frame_slot: the pacer announces the deadline once per wait, before
// sleeping (the frame slot is returned there); when off or late, the
// announcement is "now" and is never skipped (otherwise the slot would only return at the end).
void TestPacerAnnouncesDeadlineBeforeWait() {
  struct Seen {
    int Calls = 0;
    std::chrono::steady_clock::time_point Deadline{};
  } seen;
  const auto record = [](void *user, std::chrono::steady_clock::time_point deadline) {
    auto &s = *static_cast<Seen *>(user);
    ++s.Calls;
    s.Deadline = deadline;
  };
  NativeRealtimeRefreshPacer pacer(true, 60U);
  pacer.WaitForNextRefresh(1U, false, record, &seen); // first: starts the clock
  Require(seen.Calls == 1, "first wait announces");
  pacer.WaitForNextRefresh(1U, false, record, &seen);
  const auto woke = std::chrono::steady_clock::now();
  Require(seen.Calls == 2, "normal wait announces once");
  Require(seen.Deadline <= woke && woke - seen.Deadline < std::chrono::milliseconds(5),
          "announcement is the deadline at which the pacer wakes");
  std::this_thread::sleep_for(std::chrono::milliseconds(60)); // late
  const auto beforeLate = std::chrono::steady_clock::now();
  pacer.WaitForNextRefresh(1U, true, record, &seen);
  Require(seen.Calls == 3 && seen.Deadline >= beforeLate, "late announces now");
  NativeRealtimeRefreshPacer off(false, 60U);
  off.WaitForNextRefresh(1U, false, record, &seen);
  Require(seen.Calls == 4, "disabled pacer also announces");
  pacer.WaitForNextRefresh(1U, false, nullptr, nullptr); // no callback doesn't break
}

} // namespace

// p6v: wait for the present thread, acquire and nwindowQueueBuffer at the end of the
// line (the old columns stay in the same place for spike_report.py).
void TestDeko3dPresentFieldsAtTheEnd() {
  FrameSpikeRecord record;
  record.Sample.DkPresentMs = 0.01F;
  record.Sample.DkWaitPresentMs = 23.5F;
  record.Sample.DkAcquireMs = 0.25F;
  record.Sample.DkQueueBufferMs = 24.0F;
  char line[1024];
  const int size = FormatFrameSpike(record, line, sizeof(line));
  Require(size > 0 && static_cast<size_t>(size) < sizeof(line), "line fits");
  const std::string text(line);
  Require(text.find(" dk_present_ms=0.01 dk_wait_present_ms=23.50 dk_acquire_ms=0.25 dk_qb_ms=24.00\n") !=
              std::string::npos,
          "present fields at the end, in order");
}

int main(int argc, char **argv) {
  const bool report = argc > 1 && std::strcmp(argv[1], "--report") == 0;
  TestVsyncModelPeriodAndPhase();
  TestVsyncModelResetsOnModeChange();
  TestAcquireFallbackFindsVsync();
  TestSwitchClockFallbackOnHost();
  Require(gAcquireVsyncClock.load() == nullptr,
          "destructor releases the fallback pointer");
  TestSnap();
  TestRefreshCounterVsync();
  TestRefreshCounterFallback();
  TestClassificationRingAndFormat();
  TestDriftDisappearsWithVsyncPacer(report);
  TestPacerAnchorsToVsyncGrid();
  TestAnchoredPacerHonoursTargetRate();
  TestPacerAnnouncesDeadlineBeforeWait();
  TestDeko3dPresentFieldsAtTheEnd();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_frame_spike_tests: ok\n");
  return 0;
}
