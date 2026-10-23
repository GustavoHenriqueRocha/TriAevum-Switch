// GPU time of a frame without the window acquire's idle gap
// (oot3d_native_gpu_frame_timer.h). Regression: the console counted the
// gap as GPU work (18.5 ms per step frame against 12.3 ms of real work) and
// the interpolation governor dropped almost every in-between frame.

#include "oot3d_native_gpu_frame_timer.h"
#include "oot3d_native_interpolation_pacing.h"

#include <algorithm>
#include <cstdio>

namespace {

int gFailures = 0;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__,          \
                   #condition);                                               \
      ++gFailures;                                                            \
    }                                                                         \
  } while (false)

using Oot3dNativeGame::NativeGpuFrameBusyTicks;

void TestWithoutAcquire() {
  CHECK(NativeGpuFrameBusyTicks(100, 300, 0, false, 0, 0) == 200U);
  // Queued behind the previous frame: starts at its end.
  CHECK(NativeGpuFrameBusyTicks(100, 300, 150, false, 0, 0) == 150U);
  // Previous end outside the frame: ignored.
  CHECK(NativeGpuFrameBusyTicks(100, 300, 400, false, 0, 0) == 200U);
  CHECK(NativeGpuFrameBusyTicks(300, 100, 0, false, 0, 0) == 0U);
}

void TestAcquireGapIsNotGpuWork() {
  // Scene 0-120, GPU idle while the render thread waits for the display
  // (120-180), scanout and UI 180-200.
  CHECK(NativeGpuFrameBusyTicks(0, 200, 0, true, 120, 180) == 140U);
  // GPU still busy when the acquire returned: no gap, nothing taken out.
  CHECK(NativeGpuFrameBusyTicks(0, 200, 0, true, 150, 150) == 150U + 50U);
  // Gap partly before a queued start or past the end: left in (inconsistent
  // timestamps are never subtracted).
  CHECK(NativeGpuFrameBusyTicks(0, 200, 130, true, 120, 180) == 70U);
  CHECK(NativeGpuFrameBusyTicks(0, 200, 0, true, 120, 260) == 200U);
  CHECK(NativeGpuFrameBusyTicks(0, 200, 0, true, 180, 120) == 200U);
}

// The governor's decision with the console's tour2 numbers: GPU span with
// the gap 18.5 ms (step) + 17.5 ms (in-between) drops; without it
// (12.3 + 10.0) the pair fits in 33.3 ms and the in-between frame is drawn.
void TestGovernorKeepsInBetweenWithoutTheGap() {
  using namespace Oot3dNativeGame;
  const auto decide = [](double stepMs, double fullMs) {
    InterpolationPacingGovernor governor;
    InterpolationPacingConfig config;
    config.MarginMs = 0.0; // the measure alone (the margin: pacing tests)
    governor.Configure(config, 2000.0 / 60.0, 2.0F,
                       false);
    for (int frame = 0; frame < 240; ++frame) {
      governor.ObserveStep(stepMs);
      const auto level = governor.Decide();
      governor.ObserveInterpolated(level, level ==
                                                  InterpolatedSampleLevel::Full
                                              ? fullMs
                                              : 0.5);
    }
    return governor.Decide();
  };
  CHECK(decide(18.5, 17.5) == InterpolatedSampleLevel::Dropped);
  CHECK(decide(12.3, 10.0) == InterpolatedSampleLevel::Full);
}

// tour14 (2026-10-23): the render-thread side counted the blocking window
// acquire (glperf clear 7-10 ms in the windows that dropped). Per frame the
// governor takes max(GPU, render thread).
void TestRenderThreadBusyWithoutTheWait() {
  using Oot3dNativeGame::NativeRenderThreadBusyMs;
  CHECK(NativeRenderThreadBusyMs(17.6, 9.8) > 7.79);
  CHECK(NativeRenderThreadBusyMs(17.6, 9.8) < 7.81);
  CHECK(NativeRenderThreadBusyMs(10.0, 0.0) == 10.0);
  CHECK(NativeRenderThreadBusyMs(10.0, -1.0) == 10.0);
  CHECK(NativeRenderThreadBusyMs(10.0, 12.0) == 0.0);
  CHECK(NativeRenderThreadBusyMs(0.0, 1.0) == 0.0);

  Oot3dNativeGame::NativeRenderWaitClock clock;
  clock.Begin(100U);
  clock.End(160U);
  clock.Begin(200U);
  clock.End(230U);
  CHECK(clock.Take() == 90U);
  CHECK(clock.Take() == 0U);
  // End without begin, begin without end, clock going back: nothing.
  clock.End(500U);
  clock.Begin(600U);
  CHECK(clock.Take() == 0U);
  clock.End(700U);
  CHECK(clock.Take() == 0U);
  clock.Begin(800U);
  clock.End(790U);
  CHECK(clock.Take() == 0U);
}

void TestGovernorKeepsInBetweenWithoutTheRenderWait() {
  using namespace Oot3dNativeGame;
  struct Frame {
    double Gpu, Cpu, Wait;
  };
  const auto decide = [](Frame step, Frame full, bool countWait) {
    const auto cost = [&](const Frame &frame) {
      return std::max(frame.Gpu, countWait ? frame.Cpu
                                           : NativeRenderThreadBusyMs(
                                                 frame.Cpu, frame.Wait));
    };
    InterpolationPacingGovernor governor;
    InterpolationPacingConfig config;
    config.MarginMs = 0.0; // the measure alone (the margin: pacing tests)
    governor.Configure(config, 2000.0 / 60.0, 2.0F,
                       false);
    for (int frame = 0; frame < 240; ++frame) {
      governor.ObserveStep(cost(step));
      const auto level = governor.Decide();
      governor.ObserveInterpolated(
          level, level == InterpolatedSampleLevel::Full ? cost(full) : 0.5);
    }
    return governor.Decide();
  };
  // A heavy window of tour14: GPU 14.5 + 14.0, render thread 19.0 and 15.5
  // ms of wall time of which ~10 and 7 are the acquire.
  const Frame step{14.5, 19.0, 10.0};
  const Frame full{14.0, 15.5, 7.0};
  CHECK(decide(step, full, true) == InterpolatedSampleLevel::Dropped);
  CHECK(decide(step, full, false) == InterpolatedSampleLevel::Full);
  // GPU over the pair: dropped either way (the fix only removes waits).
  const Frame gpuStep{17.5, 19.0, 10.0};
  const Frame gpuFull{17.0, 15.5, 7.0};
  CHECK(decide(gpuStep, gpuFull, false) == InterpolatedSampleLevel::Dropped);
}

} // namespace

int main() {
  TestRenderThreadBusyWithoutTheWait();
  TestGovernorKeepsInBetweenWithoutTheRenderWait();
  TestWithoutAcquire();
  TestAcquireGapIsNotGpuWork();
  TestGovernorKeepsInBetweenWithoutTheGap();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_native_gpu_frame_timer_tests: ok\n");
  return 0;
}
