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

#include <array>
#include <chrono>
#include <cstdint>

#if defined(__SWITCH__)
#include <glad/glad.h>
#endif

namespace Oot3dNativeGame {

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

  // First GPU work of the frame (render thread). Idempotent within a frame.
  void BeginFrame() noexcept {
#if defined(__SWITCH__)
    if (!Ensure() || mBegun)
      return;
    glQueryCounter(mSlots[mNext].Begin, GL_TIMESTAMP);
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
      slot.Pending = false;
      mOldest = (mOldest + 1U) % mSlots.size();
      Observe(begin, end, slot.Kind, slot.CpuMilliseconds, slot.Submitted,
              onResult);
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
#endif
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
    std::array<GLuint, kSlots * 2U> names{};
    glGenQueries(static_cast<GLsizei>(names.size()), names.data());
    for (size_t index = 0U; index < kSlots; ++index) {
      mSlots[index].Begin = names[index * 2U];
      mSlots[index].End = names[index * 2U + 1U];
      if (mSlots[index].Begin == 0U || mSlots[index].End == 0U) {
        mState = State::Failed;
        return false;
      }
    }
    mState = State::Calibrating;
    return true;
  }
#endif

  template <typename OnResult>
  void Observe(uint64_t begin, uint64_t end, uint8_t kind, double cpuMs,
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
    const uint64_t start = previousEnd > begin && previousEnd < end
                               ? previousEnd
                               : begin;
    onResult(NativeGpuFrameTime{
        kind, static_cast<double>(end - start) * mFactor / 1.0e6, cpuMs});
  }

  static constexpr size_t kSlots = 8U;
  std::array<Slot, kSlots> mSlots{};
  size_t mNext = 0U;
  size_t mOldest = 0U;
  bool mBegun = false;
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
