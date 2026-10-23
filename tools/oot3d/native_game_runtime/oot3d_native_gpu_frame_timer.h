#pragma once

// GPU time of each presented frame, from GL_TIMESTAMP queries read back
// asynchronously (never waits for the GPU): a frame's two timestamps are read
// several frames later, once the driver reports them available.
//
// Only on the Switch (GL is the only renderer there and its context is
// current on the render thread, the only caller). Elsewhere it reports
// nothing and the pacing governor uses render-thread time alone.
//
// switch-mesa's GPU clock is not in nanoseconds (SwitchWakerHD measured
// 1.63 real ns per GPU ns: a 19.2 MHz timer read as 31.25 MHz), so the
// factor is measured against the CPU clock over a long baseline (>= 3 s; a
// timestamp executes 0-2 frames after its submission, under 2% of error);
// no time is reported before that. An implausible factor (clock reset after
// sleep) restarts the baseline.
//
// GPU time of a frame = end - max(begin, previous end): a frame queued behind
// the previous one starts when that one ends (nouveau writes a timestamp
// when the work before it passes the geometry stages, so single passes are
// approximate but frame totals are right).
//
// The window acquire (Switch: nwindowDequeueBuffer, blocking until the
// display frees a buffer, on the first use of framebuffer 0) happens in the
// middle of the frame, after the scene. While the render thread waits there
// the GPU runs dry, and end - begin counted that idle gap as GPU work: the
// console's tour2 showed 18.5 ms per step frame against 12.3 ms of real GPU
// work (glperf), so the interpolation governor saw step + in-between at
// ~37 ms > 33.3 and dropped almost every in-between frame (interp_dropped
// 74 of 75). Two more timestamps around the acquire (MarkAcquire) take the
// gap out; config/interp_count_acquire_wait keeps the old measure.

#include <array>
#include <chrono>
#include <cstdint>

#if defined(__SWITCH__)
#include <glad/glad.h>
#endif

namespace Oot3dNativeGame {

// GPU ticks of one frame: from its begin (or the previous frame's end, when
// it was queued behind it) to its end, minus the idle gap around the window
// acquire when both of its timestamps fall inside the frame. Pure, for the
// host tests.
[[nodiscard]] inline uint64_t NativeGpuFrameBusyTicks(
    uint64_t begin, uint64_t end, uint64_t previousEnd, bool hasAcquire,
    uint64_t acquireBegin, uint64_t acquireEnd) noexcept {
  if (end < begin)
    return 0U;
  const uint64_t start =
      previousEnd > begin && previousEnd < end ? previousEnd : begin;
  uint64_t busy = end - start;
  if (hasAcquire && acquireBegin >= start && acquireEnd >= acquireBegin &&
      acquireEnd <= end) {
    busy -= acquireEnd - acquireBegin;
  }
  return busy;
}

// Render-thread side of the same bug (2026-10-23, tour14): the frame's
// render-thread time (renderFrameCpuMs, wall clock of its commands) also
// contains the blocking window acquire (glperf `clear`: 3.5 ms on average,
// 7-14 ms in Castle Town / the heavy windows). The governor takes, per
// frame, max(GPU, render thread), so a frame whose GPU fits was costed at
// 15-17 ms of "render thread" of which only 6-8 were work, and the pair
// went over 33.3 ms: interp=53 hold=41 per window of 150. The wait is
// measured around the acquire (cntpct_el0 ticks, never cntvct_el0) and
// taken out with config/interp_cpu_exclude_wait (the default keeps it in).
//
// Busy part of a render-thread frame: wall time minus the waits inside it,
// never below zero or above the wall time.
[[nodiscard]] inline double NativeRenderThreadBusyMs(double wallMs,
                                                     double waitMs) noexcept {
  if (!(wallMs > 0.0))
    return 0.0;
  if (!(waitMs > 0.0))
    return wallMs;
  return waitMs >= wallMs ? 0.0 : wallMs - waitMs;
}

// Blocking waits of the render thread inside one frame (pure: the caller
// passes its clock's ticks). Only a begin followed by its end counts; a
// begin without end (an exception, a frame cut short) is forgotten at the
// next Begin or Take.
class NativeRenderWaitClock {
public:
  void Begin(uint64_t ticks) noexcept {
    mOpen = true;
    mBegin = ticks;
  }
  void End(uint64_t ticks) noexcept {
    if (!mOpen)
      return;
    mOpen = false;
    if (ticks > mBegin)
      mTicks += ticks - mBegin;
  }
  // Ticks waited since the last Take; resets.
  [[nodiscard]] uint64_t Take() noexcept {
    const uint64_t ticks = mTicks;
    mTicks = 0U;
    mOpen = false;
    return ticks;
  }

private:
  bool mOpen = false;
  uint64_t mBegin = 0U;
  uint64_t mTicks = 0U;
};

struct NativeGpuFrameTime {
  uint8_t Kind = 0;
  double Milliseconds = 0.0;
  // Render-thread milliseconds of the same frame (passed to EndFrame).
  double CpuMilliseconds = 0.0;
};

class NativeGpuFrameTimer final {
public:
  [[nodiscard]] bool Available() const noexcept { return mState == State::Ready; }
  [[nodiscard]] double NanosecondsPerTick() const noexcept { return mFactor; }
  // config/interp_count_acquire_wait: false keeps the acquire gap in.
  void SetExcludeAcquireWait(bool exclude) noexcept { mExcludeAcquire = exclude; }

  // Around the blocking window acquire of the frame (render thread; only
  // the first acquire of a begun frame is marked).
  void MarkAcquire(bool after) noexcept {
#if defined(__SWITCH__)
    if (!mExcludeAcquire || mState == State::Failed ||
        mState == State::Uninitialized || !mBegun)
      return;
    auto &slot = mSlots[mNext];
    if (!after) {
      if (mAcquireBegun || slot.HasAcquire)
        return;
      glQueryCounter(slot.AcquireBegin, GL_TIMESTAMP);
      mAcquireBegun = true;
    } else if (mAcquireBegun) {
      glQueryCounter(slot.AcquireEnd, GL_TIMESTAMP);
      mAcquireBegun = false;
      slot.HasAcquire = true;
    }
#else
    (void)after;
#endif
  }

  // First GPU work of the frame (render thread). Idempotent within a frame.
  void BeginFrame() noexcept {
#if defined(__SWITCH__)
    if (!Ensure() || mBegun)
      return;
    glQueryCounter(mSlots[mNext].Begin, GL_TIMESTAMP);
    mSlots[mNext].HasAcquire = false;
    mAcquireBegun = false;
    mBegun = true;
#endif
  }

  // Last GPU work of the frame before the swap. Frames without a begin are
  // not measured.
  void EndFrame(uint8_t kind, double cpuMilliseconds) noexcept {
#if defined(__SWITCH__)
    if (!Ensure() || !mBegun)
      return;
    auto &slot = mSlots[mNext];
    glQueryCounter(slot.End, GL_TIMESTAMP);
    slot.Kind = kind;
    slot.CpuMilliseconds = cpuMilliseconds;
    slot.Pending = true;
    slot.Submitted = std::chrono::steady_clock::now();
    if (mAcquireBegun) {
      // Acquire begun without its end: not measured.
      slot.HasAcquire = false;
      mAcquireBegun = false;
    }
    mBegun = false;
    mNext = (mNext + 1U) % mSlots.size();
    // A slot reused while still pending loses its frame (no wait).
    if (mSlots[mNext].Pending) {
      mSlots[mNext].Pending = false;
      ++mLost;
      if (mOldest == mNext)
        mOldest = (mOldest + 1U) % mSlots.size();
    }
#else
    (void)kind;
    (void)cpuMilliseconds;
#endif
  }

  // Calls onResult(NativeGpuFrameTime) for every frame whose timestamps are
  // available, oldest first.
  template <typename OnResult> void Poll(OnResult &&onResult) noexcept {
#if defined(__SWITCH__)
    if (mState == State::Failed || mState == State::Uninitialized)
      return;
    while (mOldest != mNext) {
      auto &slot = mSlots[mOldest];
      if (!slot.Pending) {
        mOldest = (mOldest + 1U) % mSlots.size();
        continue;
      }
      GLuint available = 0U;
      glGetQueryObjectuiv(slot.End, GL_QUERY_RESULT_AVAILABLE, &available);
      if (available == 0U)
        break;
      GLuint64 begin = 0U;
      GLuint64 end = 0U;
      glGetQueryObjectui64v(slot.Begin, GL_QUERY_RESULT, &begin);
      glGetQueryObjectui64v(slot.End, GL_QUERY_RESULT, &end);
      GLuint64 acquireBegin = 0U;
      GLuint64 acquireEnd = 0U;
      if (slot.HasAcquire) {
        glGetQueryObjectui64v(slot.AcquireBegin, GL_QUERY_RESULT, &acquireBegin);
        glGetQueryObjectui64v(slot.AcquireEnd, GL_QUERY_RESULT, &acquireEnd);
      }
      slot.Pending = false;
      mOldest = (mOldest + 1U) % mSlots.size();
      Observe(begin, end, slot.HasAcquire, acquireBegin, acquireEnd, slot.Kind,
              slot.CpuMilliseconds, slot.Submitted, onResult);
    }
#else
    (void)onResult;
#endif
  }

  [[nodiscard]] uint64_t LostFrames() const noexcept { return mLost; }

private:
  enum class State : uint8_t { Uninitialized, Calibrating, Ready, Failed };

  struct Slot {
#if defined(__SWITCH__)
    GLuint Begin = 0U;
    GLuint End = 0U;
    GLuint AcquireBegin = 0U;
    GLuint AcquireEnd = 0U;
#endif
    bool HasAcquire = false;
    uint8_t Kind = 0U;
    double CpuMilliseconds = 0.0;
    bool Pending = false;
    std::chrono::steady_clock::time_point Submitted{};
  };

#if defined(__SWITCH__)
  bool Ensure() noexcept {
    if (mState == State::Failed)
      return false;
    if (mState != State::Uninitialized)
      return true;
    if (glQueryCounter == nullptr || glGenQueries == nullptr ||
        glGetQueryObjectui64v == nullptr) {
      mState = State::Failed;
      return false;
    }
    std::array<GLuint, kSlots * 4U> names{};
    glGenQueries(static_cast<GLsizei>(names.size()), names.data());
    for (size_t index = 0U; index < kSlots; ++index) {
      mSlots[index].Begin = names[index * 4U];
      mSlots[index].End = names[index * 4U + 1U];
      mSlots[index].AcquireBegin = names[index * 4U + 2U];
      mSlots[index].AcquireEnd = names[index * 4U + 3U];
      if (mSlots[index].Begin == 0U || mSlots[index].End == 0U ||
          mSlots[index].AcquireBegin == 0U || mSlots[index].AcquireEnd == 0U) {
        mState = State::Failed;
        return false;
      }
    }
    mState = State::Calibrating;
    return true;
  }
#endif

  template <typename OnResult>
  void Observe(uint64_t begin, uint64_t end, bool hasAcquire,
               uint64_t acquireBegin, uint64_t acquireEnd, uint8_t kind,
               double cpuMs,
               std::chrono::steady_clock::time_point submitted,
               OnResult &&onResult) noexcept {
    const uint64_t previousEnd = mPreviousEnd;
    mPreviousEnd = end;
    if (end == 0U || end < begin) {
      // No GPU clock (reads zero) or a reset: start over.
      if (++mInvalid > 120U)
        mState = State::Failed;
      mBaselineSet = false;
      return;
    }
    mInvalid = 0U;
    // Calibration against the CPU clock.
    if (!mBaselineSet) {
      mBaselineGpu = end;
      mBaselineCpu = submitted;
      mBaselineSet = true;
    } else {
      const double cpuNs = std::chrono::duration<double, std::nano>(
                               submitted - mBaselineCpu)
                               .count();
      if (end <= mBaselineGpu) {
        mBaselineSet = false;
      } else if (cpuNs >= 3.0e9) {
        const double factor =
            cpuNs / static_cast<double>(end - mBaselineGpu);
        if (factor < 0.25 || factor > 4.0) {
          mBaselineSet = false;
          mState = State::Calibrating;
        } else {
          mFactor = factor;
          mState = State::Ready;
        }
      }
    }
    if (mState != State::Ready)
      return;
    const uint64_t busy = NativeGpuFrameBusyTicks(
        begin, end, previousEnd, hasAcquire, acquireBegin, acquireEnd);
    onResult(NativeGpuFrameTime{
        kind, static_cast<double>(busy) * mFactor / 1.0e6, cpuMs});
  }

  static constexpr size_t kSlots = 8U;
  std::array<Slot, kSlots> mSlots{};
  size_t mNext = 0U;
  size_t mOldest = 0U;
  bool mBegun = false;
  bool mAcquireBegun = false;
  bool mExcludeAcquire = true;
  State mState = State::Uninitialized;
  double mFactor = 1.0;
  bool mBaselineSet = false;
  uint64_t mBaselineGpu = 0U;
  std::chrono::steady_clock::time_point mBaselineCpu{};
  uint64_t mPreviousEnd = 0U;
  uint32_t mInvalid = 0U;
  uint64_t mLost = 0U;
};

} // namespace Oot3dNativeGame
