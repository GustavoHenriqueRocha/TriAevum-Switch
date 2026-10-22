// GPU time of a frame without the window acquire's idle gap
// (oot3d_native_gpu_frame_timer.h). Regression: the console counted the
// gap as GPU work (18.5 ms per step frame against 12.3 ms of real work) and
// the interpolation governor dropped almost every in-between frame.

#include "oot3d_native_gpu_frame_timer.h"
#include "oot3d_native_interpolation_pacing.h"

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
    governor.Configure(InterpolationPacingConfig{}, 2000.0 / 60.0, 2.0F,
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

} // namespace

int main() {
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
