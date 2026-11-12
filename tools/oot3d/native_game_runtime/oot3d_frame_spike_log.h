#pragma once

// Per-spike measurement (logs/spikes.txt; config/frame_spikes or config/gpu_timing).
//
// Window averages hide what matters for smoothness: the Switch has no
// VRR, and a frame that misses vsync leaves the previous image on screen for one
// more 16.7 ms refresh. Here every presented frame becomes a sample (POD, no
// allocation); only the ones that missed vsync go into a fixed-size SPSC ring
// (producer: render thread; consumer: the perf.txt window on the game
// thread, which formats in batch and sends to the AsyncLineWriter).
//
// Refreshes of a frame = refreshes between the previous image and this one (1 = on
// time; 2 = the previous one stayed one extra refresh, this frame missed vsync).
// With the vsync clock (oot3d_vsync_clock.h) uses the FIFO latch model:
// the buffer delivered at t appears at the first vsync after t and never before
// the vsync following the previous buffer's (swapchain queue). Without it,
// rounds the interval between swaps by the nominal period.
//
// Blamed stage (missed_by_stage_*): guest = the busy game thread exceeded
// the budget (expected refreshes x period); render = the busy render thread
// (excluding waits) exceeded it; gpu = the last GPU result exceeded it;
// pacer = nothing exceeded it (pacer phase/drift, swapchain wait, late
// wakeup). The order is this: the first stage over budget takes the blame.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Oot3dNativeGame {

enum class SpikeFrameKind : uint8_t { Step, Interp, Hold };

inline const char *SpikeFrameKindName(SpikeFrameKind kind) {
  switch (kind) {
  case SpikeFrameKind::Step: return "step";
  case SpikeFrameKind::Interp: return "interp";
  default: return "hold";
  }
}

enum class SpikeStage : uint8_t { Guest, Render, Gpu, Pacer, Count };
inline constexpr size_t kSpikeStageCount = static_cast<size_t>(SpikeStage::Count);

inline const char *SpikeStageName(SpikeStage stage) {
  switch (stage) {
  case SpikeStage::Guest: return "guest";
  case SpikeStage::Render: return "render";
  case SpikeStage::Gpu: return "gpu";
  default: return "pacer";
  }
}

// Frame context (bits): what happened since the previous frame.
enum SpikeContext : uint16_t {
  kSpikeLoad = 1U << 0,      // scene change in the last ~2 s (warp/loading)
  kSpikeTexUpload = 1U << 1, // textures created
  kSpikeShader = 1U << 2,    // GLSL compilation / nvc0 translation (Mesa)
  kSpikeProgram = 1U << 3,   // new GL program (ProgramsLive went up)
  kSpikeGlOom = 1U << 4,     // GL allocation failure
  kSpikeEvict = 1U << 5,     // residency eviction (textures)
  kSpikeDrs = 1U << 6,       // dynamic resolution changed scale
};

inline constexpr std::array<const char *, 7> kSpikeContextNames{
    "load", "tex", "shader", "program", "gl_oom", "evict", "drs"};

struct FrameSpikeSample {
  uint64_t HostFrame = 0U;  // presented host frame (render thread series)
  uint64_t GuestFrame = 0U; // game frames completed up to this one
  double TimeSeconds = 0.0; // end of swap, monotonic clock
  SpikeFrameKind Kind = SpikeFrameKind::Hold;
  uint8_t Expected = 1U;  // expected refreshes (2: locked-30 step)
  uint8_t Refreshes = 1U; // measured
  uint8_t TopStage = 0xFFU; // most expensive GlPerf::Stage on CPU (0xFF: no glperf)
  uint16_t Context = 0U;
  uint16_t TexturesNew = 0U;
  uint16_t ProgramsNew = 0U;
  uint16_t ShadersNew = 0U;
  float GuestMs = 0.0F;       // game logic in the last host frame
  float GameBusyMs = 0.0F;    // game thread busy (not waiting for the renderer)
  float RenderBusyMs = 0.0F;  // render thread working on the frame
  float RenderWaitMs = 0.0F;  // render thread waiting on the swapchain (acquire)
  float PacerWaitMs = 0.0F;   // pacer wait before the swap
  float GpuMs = 0.0F;         // last GPU result (lags 1-3 frames; 0 = none)
  float TopStageMs = 0.0F;
  float SwapMs = 0.0F;        // swap (eglSwapBuffers)
  // deko3d (p6o): parts of the swap on the render thread (Fast::Deko3dLastSwapTimes).
  float DkFinishMs = 0.0F;
  float DkFenceMs = 0.0F;
  float DkSubmitMs = 0.0F;
  float DkPresentMs = 0.0F;
  // p6v: wait for the present thread, acquire and the last nwindowQueueBuffer.
  float DkWaitPresentMs = 0.0F;
  float DkAcquireMs = 0.0F;
  float DkQueueBufferMs = 0.0F;
};

struct FrameSpikeRecord {
  FrameSpikeSample Sample;
  SpikeStage Stage = SpikeStage::Pacer;
};

[[nodiscard]] inline SpikeStage ClassifyFrameSpike(const FrameSpikeSample &sample,
                                                   double periodMs) {
  const double budget = std::max<double>(1.0, sample.Expected) * periodMs;
  if (sample.GameBusyMs > budget) return SpikeStage::Guest;
  if (sample.RenderBusyMs > budget) return SpikeStage::Render;
  if (sample.GpuMs > budget) return SpikeStage::Gpu;
  return SpikeStage::Pacer;
}

// Refreshes of each presented frame (see the top). Times in ns on the same
// clock as vsync.
class SpikeRefreshCounter {
public:
  // vsyncValid: lastVsyncNs/periodNs come from the vsync clock.
  uint32_t Observe(int64_t presentNs, bool vsyncValid, int64_t lastVsyncNs,
                   double periodNs, double nominalPeriodNs) {
    double refreshes = 1.0;
    if (vsyncValid && periodNs > 0.0) {
      // First vsync after the swap; with queueing, the one after the previous one's.
      const double since = static_cast<double>(presentNs - lastVsyncNs);
      double latch = static_cast<double>(lastVsyncNs) +
                     (std::floor(since / periodNs) + 1.0) * periodNs;
      if (mHasPrevious && mLatchValid) {
        if (latch < mPreviousLatchNs + 0.5 * periodNs)
          latch = mPreviousLatchNs + periodNs;
        refreshes = (latch - mPreviousLatchNs) / periodNs;
      } else if (mHasPrevious && nominalPeriodNs > 0.0) {
        refreshes = static_cast<double>(presentNs - mPreviousPresentNs) / nominalPeriodNs;
      }
      mPreviousLatchNs = latch;
      mLatchValid = true;
    } else {
      if (mHasPrevious && nominalPeriodNs > 0.0)
        refreshes = static_cast<double>(presentNs - mPreviousPresentNs) / nominalPeriodNs;
      mLatchValid = false;
    }
    mHasPrevious = true;
    mPreviousPresentNs = presentNs;
    return static_cast<uint32_t>(std::clamp<int64_t>(std::llround(refreshes), 1, 255));
  }
  void Reset() {
    mHasPrevious = false;
    mLatchValid = false;
  }

private:
  bool mHasPrevious = false;
  bool mLatchValid = false;
  int64_t mPreviousPresentNs = 0;
  double mPreviousLatchNs = 0.0;
};

// Fixed-size SPSC ring: Push only on the render thread, Drain only on the
// consumer. Full: drops and counts.
template <size_t Capacity> class FrameSpikeRing {
  static_assert((Capacity & (Capacity - 1U)) == 0U, "power of 2");

public:
  bool Push(const FrameSpikeRecord &record) {
    const uint64_t head = mHead.load(std::memory_order_relaxed);
    const uint64_t tail = mTail.load(std::memory_order_acquire);
    if (head - tail >= Capacity) {
      mDropped.store(mDropped.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
      return false;
    }
    mSlots[head & (Capacity - 1U)] = record;
    mHead.store(head + 1U, std::memory_order_release);
    return true;
  }
  template <typename Fn> size_t Drain(Fn &&fn) {
    const uint64_t head = mHead.load(std::memory_order_acquire);
    uint64_t tail = mTail.load(std::memory_order_relaxed);
    size_t count = 0U;
    for (; tail != head; ++tail, ++count) fn(mSlots[tail & (Capacity - 1U)]);
    mTail.store(tail, std::memory_order_release);
    return count;
  }
  uint64_t Dropped() const { return mDropped.load(std::memory_order_relaxed); }

private:
  std::array<FrameSpikeRecord, Capacity> mSlots{};
  std::atomic<uint64_t> mHead{0U};
  std::atomic<uint64_t> mTail{0U};
  std::atomic<uint64_t> mDropped{0U};
};

// Accumulated totals (written by the render thread, read by the perf.txt
// window): missed refreshes and frames that missed, per blamed stage.
struct FrameSpikeTotals {
  std::atomic<uint64_t> Presents{0U};
  std::atomic<uint64_t> MissedFrames{0U};
  std::atomic<uint64_t> MissedRefreshes{0U};
  std::array<std::atomic<uint64_t>, kSpikeStageCount> MissedByStage{};

  struct Snapshot {
    uint64_t Presents = 0U, MissedFrames = 0U, MissedRefreshes = 0U;
    std::array<uint64_t, kSpikeStageCount> MissedByStage{};
  };
  Snapshot Take() const {
    Snapshot snapshot;
    snapshot.Presents = Presents.load(std::memory_order_relaxed);
    snapshot.MissedFrames = MissedFrames.load(std::memory_order_relaxed);
    snapshot.MissedRefreshes = MissedRefreshes.load(std::memory_order_relaxed);
    for (size_t index = 0U; index < kSpikeStageCount; ++index)
      snapshot.MissedByStage[index] = MissedByStage[index].load(std::memory_order_relaxed);
    return snapshot;
  }
};

inline void AddOwner(std::atomic<uint64_t> &counter, uint64_t value) {
  counter.store(counter.load(std::memory_order_relaxed) + value,
                std::memory_order_relaxed);
}

// One sample per presented frame (render thread). Returns true when the
// frame missed vsync (and went into the ring).
template <size_t Capacity>
bool ObserveFrameSpike(const FrameSpikeSample &sample, double periodMs,
                       FrameSpikeTotals &totals, FrameSpikeRing<Capacity> &ring) {
  AddOwner(totals.Presents, 1U);
  const uint32_t expected = std::max<uint32_t>(1U, sample.Expected);
  if (sample.Refreshes <= expected) return false;
  FrameSpikeRecord record;
  record.Sample = sample;
  record.Stage = ClassifyFrameSpike(sample, periodMs);
  const uint64_t lost = sample.Refreshes - expected;
  AddOwner(totals.MissedFrames, 1U);
  AddOwner(totals.MissedRefreshes, lost);
  AddOwner(totals.MissedByStage[static_cast<size_t>(record.Stage)], lost);
  (void)ring.Push(record);
  return true;
}

// GlPerf stage names (gl_frame_profiler.h, same order).
inline const char *SpikeTopStageName(uint8_t stage) {
  static constexpr std::array<const char *, 15> kNames{
      "begin", "clear", "pica_top", "pica_bottom", "pica_off", "pica_canon",
      "transfer", "post_aa", "post_easu", "scanout", "ui", "present",
      "other", "pacing", "idle"};
  return stage < kNames.size() ? kNames[stage] : "-";
}

// One line of logs/spikes.txt (with '\n'); returns the size written.
inline int FormatFrameSpike(const FrameSpikeRecord &record, char *out, size_t size) {
  const auto &s = record.Sample;
  char context[64] = "-";
  size_t used = 0U;
  for (size_t bit = 0U; bit < kSpikeContextNames.size(); ++bit) {
    if ((s.Context & (1U << bit)) == 0U) continue;
    const int wrote = std::snprintf(context + used, sizeof(context) - used, "%s%s",
                                    used == 0U ? "" : ",", kSpikeContextNames[bit]);
    if (wrote <= 0 || static_cast<size_t>(wrote) >= sizeof(context) - used) break;
    used += static_cast<size_t>(wrote);
  }
  return std::snprintf(
      out, size,
      "spike t=%.3f host=%llu guest=%llu kind=%s refreshes=%u expected=%u stage=%s "
      "guest_ms=%.2f game_ms=%.2f render_ms=%.2f render_wait_ms=%.2f pacer_ms=%.2f "
      "swap_ms=%.2f gpu_ms=%.2f top=%s top_ms=%.2f ctx=%s tex_new=%u prog_new=%u "
      "shader_new=%u dk_finish_ms=%.2f dk_fence_ms=%.2f dk_submit_ms=%.2f dk_present_ms=%.2f "
      "dk_wait_present_ms=%.2f dk_acquire_ms=%.2f dk_qb_ms=%.2f\n",
      s.TimeSeconds, static_cast<unsigned long long>(s.HostFrame),
      static_cast<unsigned long long>(s.GuestFrame), SpikeFrameKindName(s.Kind),
      static_cast<unsigned>(s.Refreshes), static_cast<unsigned>(s.Expected),
      SpikeStageName(record.Stage), s.GuestMs, s.GameBusyMs, s.RenderBusyMs,
      s.RenderWaitMs, s.PacerWaitMs, s.SwapMs, s.GpuMs, SpikeTopStageName(s.TopStage),
      s.TopStageMs, context, static_cast<unsigned>(s.TexturesNew),
      static_cast<unsigned>(s.ProgramsNew), static_cast<unsigned>(s.ShadersNew), s.DkFinishMs, s.DkFenceMs,
      s.DkSubmitMs, s.DkPresentMs, s.DkWaitPresentMs, s.DkAcquireMs, s.DkQueueBufferMs);
}

} // namespace Oot3dNativeGame
