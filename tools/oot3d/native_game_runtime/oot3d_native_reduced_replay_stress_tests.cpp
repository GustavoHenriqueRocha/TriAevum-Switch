// Stress of the reduced in-between path (config/interp_scale) on the CPU
// side, with the session's threads: a plan worker builds whole guest frames
// (real Oot3dPicaVisualFrame / Oot3dPicaVulkanDrawPlan) and hands them over
// through a bounded queue; the render thread keeps the previous/latest frame
// as the session does, views the in-between sample (plan pointers and the
// fill span of the latest frame), lets the governor pick full / reduced /
// dropped, runs SampleSuitsReducedReplay on reduced decisions and publishes
// perf counters under a mutex that a guest thread reads and copies, like the
// perf.txt block. Costs alternate between regimes so the level changes
// thousands of times. Run under ASan and under TSan.
#include "oot3d_native_reduced_replay.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

struct PerfCounters {
  uint64_t Decisions = 0;
  uint64_t Reduced = 0;
  uint64_t Refused = 0;
  uint64_t Dropped = 0;
  uint64_t Full = 0;
  uint64_t LevelChanges = 0;
  double StepCostMs = 0.0;
};

// One guest frame: a scene target (colour + depth) and a few UI/offscreen
// targets; some frames clear everything with fills, some do not (frozen
// background), some clear after the first draw.
Oot3dPicaVisualFrame BuildFrame(std::mt19937 &random, uint64_t sequence,
                                uint64_t &nextSubmission) {
  Oot3dPicaVisualFrame frame;
  frame.Sequence = sequence;
  const uint32_t targets = 1U + random() % 40U; // past the 32-entry sets too
  const uint32_t draws = 1U + random() % 400U;
  const int mode = static_cast<int>(random() % 3U);
  const uint64_t first = nextSubmission;
  for (uint32_t target = 0; target < targets; ++target) {
    const uint32_t color = 0x18000000U + target * 0x80000U;
    const uint32_t depth = color + 0x40000U;
    if (mode == 0 || (mode == 2 && target % 2U == 0U)) {
      Oot3dPicaMemoryFillSubmission fill;
      fill.BeforeDrawSubmissionId = mode == 2 ? first + 5U : first;
      fill.StartPhysicalAddress = color;
      fill.EndPhysicalAddress = color + 240U * 400U * 4U;
      frame.MemoryFills.push_back(fill);
      fill.StartPhysicalAddress = depth;
      fill.EndPhysicalAddress = depth + 240U * 400U * 4U;
      frame.MemoryFills.push_back(fill);
    }
  }
  frame.Draws.reserve(draws);
  for (uint32_t index = 0; index < draws; ++index) {
    Oot3dPicaVulkanDrawPlan plan;
    plan.SubmissionId = nextSubmission++;
    const uint32_t target = random() % targets;
    auto &framebuffer = plan.State.Framebuffer;
    framebuffer.ColorPhysicalAddress = 0x18000000U + target * 0x80000U;
    framebuffer.DepthPhysicalAddress =
        random() % 4U == 0U ? 0U : framebuffer.ColorPhysicalAddress + 0x40000U;
    framebuffer.Width = 240U;
    framebuffer.Height = 400U;
    framebuffer.ColorFormat = static_cast<uint8_t>(random() % 3U);
    framebuffer.DepthFormat = 3U;
    framebuffer.DepthStencilWriteEnabled = random() % 2U == 0U;
    plan.State.OutputMerger.Depth.TestEnabled = random() % 2U == 0U;
    plan.State.OutputMerger.Stencil.Enabled = random() % 8U == 0U;
    plan.IndexBytes.resize(random() % 256U); // heap traffic like real plans
    frame.Draws.push_back(std::move(plan));
  }
  return frame;
}

} // namespace

int main() {
  constexpr uint64_t kFrames = 6000U;
  std::mutex queueMutex;
  std::condition_variable queueChanged;
  std::deque<Oot3dPicaVisualFrame> queue;
  bool producerDone = false;
  std::mutex perfMutex;
  PerfCounters perf;
  std::atomic<bool> renderDone{false};

  std::thread worker([&] {
    std::mt19937 random(1234U);
    uint64_t submission = 1U;
    for (uint64_t sequence = 1U; sequence <= kFrames; ++sequence) {
      auto frame = BuildFrame(random, sequence, submission);
      std::unique_lock lock(queueMutex);
      queueChanged.wait(lock, [&] { return queue.size() < 2U; });
      queue.push_back(std::move(frame));
      queueChanged.notify_all();
    }
    std::lock_guard lock(queueMutex);
    producerDone = true;
    queueChanged.notify_all();
  });

  std::thread guest([&] {
    std::vector<PerfCounters> windows;
    while (!renderDone.load()) {
      PerfCounters copy;
      {
        std::lock_guard lock(perfMutex);
        copy = perf;
      }
      windows.push_back(copy);
      if (windows.size() > 64U) windows.erase(windows.begin());
      std::this_thread::yield();
    }
  });

  bool ok = true;
  std::thread render([&] {
    InterpolationPacingConfig config;
    config.ReducedScale = 1.5F;
    InterpolationPacingGovernor governor;
    governor.Configure(config, 2000.0 / 60.0, 2.0F, true);
    ReducedReplayScratch scratch;
    std::optional<Oot3dPicaVisualFrame> previous;
    std::optional<Oot3dPicaVisualFrame> latest;
    Oot3dPicaVisualFrameViewSample sample;
    std::mt19937 random(99U);
    uint64_t frames = 0U;
    for (;;) {
      Oot3dPicaVisualFrame frame;
      {
        std::unique_lock lock(queueMutex);
        queueChanged.wait(lock, [&] { return !queue.empty() || producerDone; });
        if (queue.empty()) break;
        frame = std::move(queue.front());
        queue.pop_front();
        queueChanged.notify_all();
      }
      // Step frame: history moves as in PresentVisualFrame.
      if (latest.has_value()) previous = std::move(latest);
      latest = std::move(frame);
      ++frames;
      // Regimes of ~40 frames: light, full fits only reduced, nothing fits.
      const int regime = static_cast<int>((frames / 40U) % 3U);
      const double step = regime == 0 ? 12.0 : regime == 1 ? 19.0 : 26.0;
      governor.ObserveStep(step + (random() % 100U) / 100.0);
      if (!previous.has_value()) continue;
      // In-between sample: views of the latest frame's plans and fills.
      sample = {};
      sample.Draws.resize(latest->Draws.size());
      for (size_t index = 0; index < latest->Draws.size(); ++index)
        sample.Draws[index].BasePlan = &latest->Draws[index];
      sample.MemoryFills = latest->MemoryFills;
      auto level = governor.Decide();
      bool refused = false;
      if (level == InterpolatedSampleLevel::Reduced &&
          !SampleSuitsReducedReplay(sample, scratch)) {
        level = InterpolatedSampleLevel::Full;
        refused = true;
      }
      const double cost = level == InterpolatedSampleLevel::Full    ? step
                          : level == InterpolatedSampleLevel::Reduced ? step * 0.6
                                                                      : 0.1;
      governor.ObserveInterpolated(level, cost);
      // Every reduced eligibility answer is checked against a direct scan.
      if (level == InterpolatedSampleLevel::Reduced || refused) {
        const bool expected = SampleClearsEveryTarget(scratch.Draws, scratch.Fills);
        if (expected == refused) ok = false;
        if (scratch.Draws.size() != sample.Draws.size()) ok = false;
      }
      std::lock_guard lock(perfMutex);
      ++perf.Decisions;
      perf.Reduced += level == InterpolatedSampleLevel::Reduced ? 1U : 0U;
      perf.Refused += refused ? 1U : 0U;
      perf.Dropped += level == InterpolatedSampleLevel::Dropped ? 1U : 0U;
      perf.Full += level == InterpolatedSampleLevel::Full ? 1U : 0U;
      perf.LevelChanges = governor.Stats().LevelChanges;
      perf.StepCostMs = governor.StepCostMs();
    }
    renderDone.store(true);
  });

  worker.join();
  render.join();
  guest.join();
  std::printf("decisions=%llu full=%llu reduced=%llu refused=%llu dropped=%llu "
              "level_changes=%llu\n",
              static_cast<unsigned long long>(perf.Decisions),
              static_cast<unsigned long long>(perf.Full),
              static_cast<unsigned long long>(perf.Reduced),
              static_cast<unsigned long long>(perf.Refused),
              static_cast<unsigned long long>(perf.Dropped),
              static_cast<unsigned long long>(perf.LevelChanges));
  if (!ok || perf.Reduced == 0U || perf.Refused == 0U || perf.Dropped == 0U ||
      perf.LevelChanges < 100U) {
    std::fprintf(stderr, "oot3d_native_reduced_replay_stress_tests: FAIL\n");
    return EXIT_FAILURE;
  }
  std::puts("oot3d_native_reduced_replay_stress_tests: ok");
  return EXIT_SUCCESS;
}
