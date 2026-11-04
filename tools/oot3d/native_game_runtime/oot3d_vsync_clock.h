#pragma once

// Clock of the display's real vsync (config/pacer_vsync, config/frame_spikes).
//
// The pacer followed the CPU clock (60.000 Hz) and the Switch display runs a
// little slower (~0.2%): the phase between swap and vsync drifts and, on each
// wrap, a frame lands on the wrong side of the vsync (periodic missed refresh, and
// the swapchain fills until acquire holds the render thread).
//
// VsyncClockModel (pure, tested on the host) receives the instant of each vsync
// seen (ns, monotonic clock) and estimates:
//  - period: least squares of t against the vsync index over a long
//    window (the wake-up delay is noise with a ~constant mean: it goes into the
//    phase, not the slope);
//  - phase: lower envelope (waking is never before the vsync): a
//    negative residual pulls the phase immediately, a positive one only 1/16.
// Vsyncs missed by the thread (woke too late) just skip indices.
// A residual larger than 1/4 of a period (video mode change, suspend)
// restarts the estimate.
//
// SwitchVsyncClock (oot3d_switch_vsync_clock.cpp): thread that waits for the
// display's vsync event (viGetDisplayVsyncEvent) and publishes the model.
//
// tour19 (pgo5n): the console returned rc 0x1272 (module 114 = vi, desc 9,
// not documented on switchbrew nor in the emulators) and the clock never
// existed (vsync_clock=0; pacer_vsync had no effect; spikes.txt counted the
// refreshes by the 60.000 Hz clock). libnx's default window
// (nwindowGetDefault, used by SDL/Mesa) already opened the "Default" display in the
// same IApplicationDisplayService session and its handle is static in
// libnx (g_viDisplay, inaccessible). Now:
//  1. tries the event (vi -> display -> event) and stores the stage that failed
//     (StartStage: 1 viInitialize, 2 viOpenDefaultDisplay, 3
//     viGetDisplayVsyncEvent);
//  2. if it fails, fallback source: the return of the window's acquire (swapchain
//     dequeue) that blocked >= kAcquireVsyncMinWaitMs. With the queue full the
//     buffer only comes back on vsync: the return is vsync + short latency, and the
//     model's lower envelope finds the phase (AcquireVsyncFilter).
// Clock: armGetSystemTick (cntpct_el0); never cntvct_el0 (Data Abort on
// Horizon).

#include <atomic>
#include <cmath>
#include <cstdint>

namespace Oot3dNativeGame {

struct VsyncClockSnapshot {
  bool Valid = false;
  int64_t LastVsyncNs = 0; // phase: one vsync (caller's clock)
  double PeriodNs = 0.0;
  uint64_t Vsyncs = 0U;    // seen since the start
  uint64_t Resets = 0U;
};

class VsyncClockModel {
public:
  static constexpr uint32_t kMinimumSamples = 30U;
  static constexpr uint32_t kWindow = 1200U; // ~20 s: restarts the regression

  explicit VsyncClockModel(double nominalPeriodNs = 1.0e9 / 60.0)
      : mNominalNs(nominalPeriodNs), mPeriodNs(nominalPeriodNs) {}

  void Observe(int64_t vsyncNs) {
    ++mVsyncs;
    if (!mStarted) {
      Restart(vsyncNs);
      return;
    }
    const double since = static_cast<double>(vsyncNs - mPhaseNs);
    const double steps = std::round(since / mPeriodNs);
    if (steps < 1.0) {
      // Two wake-ups on the same vsync (or the clock went back): envelope only.
      const double residual = since - steps * mPeriodNs;
      if (steps == 0.0 && residual < 0.0) mPhaseNs = vsyncNs;
      return;
    }
    const double residual = since - steps * mPeriodNs;
    if (std::abs(residual) > 0.25 * mPeriodNs || steps > 600.0) {
      ++mResets;
      Restart(vsyncNs);
      return;
    }
    mIndex += static_cast<int64_t>(steps);
    const double base = static_cast<double>(mPhaseNs) + steps * mPeriodNs;
    mPhaseNs = static_cast<int64_t>(std::llround(
        residual < 0.0 ? base + residual : base + residual / 16.0));
    // Regression: x = index, y = t - anchor.
    const double x = static_cast<double>(mIndex - mFitIndex0);
    const double y = static_cast<double>(vsyncNs - mFitT0);
    mSx += x; mSy += y; mSxx += x * x; mSxy += x * y; ++mN;
    if (mN >= kMinimumSamples) {
      const double n = static_cast<double>(mN);
      const double den = n * mSxx - mSx * mSx;
      if (den > 0.0) {
        const double slope = (n * mSxy - mSx * mSy) / den;
        if (slope > 0.9 * mNominalNs && slope < 1.1 * mNominalNs) {
          mPeriodNs = slope;
          mValid = true;
        }
      }
    }
    if (mN >= kWindow) {
      // New window from here on, keeping the current period.
      mFitIndex0 = mIndex;
      mFitT0 = vsyncNs;
      mSx = mSy = mSxx = mSxy = 0.0;
      mN = 0U;
    }
  }

  [[nodiscard]] VsyncClockSnapshot Snapshot() const {
    VsyncClockSnapshot snapshot;
    snapshot.Valid = mValid;
    snapshot.LastVsyncNs = mPhaseNs;
    snapshot.PeriodNs = mPeriodNs;
    snapshot.Vsyncs = mVsyncs;
    snapshot.Resets = mResets;
    return snapshot;
  }

private:
  void Restart(int64_t vsyncNs) {
    mStarted = true;
    mValid = false;
    mPhaseNs = vsyncNs;
    mIndex = 0;
    mFitIndex0 = 0;
    mFitT0 = vsyncNs;
    mSx = mSy = mSxx = mSxy = 0.0;
    mN = 0U;
    mPeriodNs = mNominalNs;
  }

  double mNominalNs;
  double mPeriodNs;
  bool mStarted = false;
  bool mValid = false;
  int64_t mPhaseNs = 0;
  int64_t mIndex = 0;
  int64_t mFitIndex0 = 0;
  int64_t mFitT0 = 0;
  double mSx = 0.0, mSy = 0.0, mSxx = 0.0, mSxy = 0.0;
  uint32_t mN = 0U;
  uint64_t mVsyncs = 0U;
  uint64_t mResets = 0U;
};

// Pacer deadline anchored to the vsync: the instant closest to `deadlineNs`
// on the grid lastVsync + k*period + offset.
[[nodiscard]] inline int64_t SnapDeadlineToVsync(int64_t deadlineNs, int64_t lastVsyncNs,
                                                 double periodNs, double offsetNs) {
  if (!(periodNs > 0.0)) return deadlineNs;
  const double grid = static_cast<double>(lastVsyncNs) + offsetNs;
  const double k = std::round((static_cast<double>(deadlineNs) - grid) / periodNs);
  return static_cast<int64_t>(std::llround(grid + k * periodNs));
}

// Lock-free (seqlock) publication of the snapshot: written by one thread,
// read by any.
class VsyncClockPublisher {
public:
  void Publish(const VsyncClockSnapshot &snapshot) {
    const uint32_t sequence = mSequence.load(std::memory_order_relaxed);
    mSequence.store(sequence + 1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    mValid.store(snapshot.Valid, std::memory_order_relaxed);
    mLast.store(snapshot.LastVsyncNs, std::memory_order_relaxed);
    mPeriod.store(snapshot.PeriodNs, std::memory_order_relaxed);
    mVsyncs.store(snapshot.Vsyncs, std::memory_order_relaxed);
    mResets.store(snapshot.Resets, std::memory_order_relaxed);
    mSequence.store(sequence + 2U, std::memory_order_release);
  }
  [[nodiscard]] VsyncClockSnapshot Read() const {
    VsyncClockSnapshot snapshot;
    for (int attempt = 0; attempt < 8; ++attempt) {
      const uint32_t before = mSequence.load(std::memory_order_acquire);
      if ((before & 1U) != 0U) continue;
      snapshot.Valid = mValid.load(std::memory_order_relaxed);
      snapshot.LastVsyncNs = mLast.load(std::memory_order_relaxed);
      snapshot.PeriodNs = mPeriod.load(std::memory_order_relaxed);
      snapshot.Vsyncs = mVsyncs.load(std::memory_order_relaxed);
      snapshot.Resets = mResets.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (mSequence.load(std::memory_order_relaxed) == before) return snapshot;
    }
    return {};
  }

private:
  std::atomic<uint32_t> mSequence{0U};
  std::atomic<bool> mValid{false};
  std::atomic<int64_t> mLast{0};
  std::atomic<double> mPeriod{0.0};
  std::atomic<uint64_t> mVsyncs{0U};
  std::atomic<uint64_t> mResets{0U};
};

// An acquire that blocked at least this long (ms) returned on a vsync.
inline constexpr double kAcquireVsyncMinWaitMs = 1.5;

// Fallback source: returns of the window's acquire -> vsync model. Only the
// ones that blocked (an acquire that didn't wait returns at any time).
class AcquireVsyncFilter {
public:
  explicit AcquireVsyncFilter(double nominalPeriodNs = 1.0e9 / 60.0)
      : mModel(nominalPeriodNs) {}
  // true if the sample went into the model.
  bool Observe(int64_t returnNs, double waitMs) {
    if (!(waitMs >= kAcquireVsyncMinWaitMs)) {
      ++mIgnored;
      return false;
    }
    mModel.Observe(returnNs);
    ++mUsed;
    return true;
  }
  [[nodiscard]] VsyncClockSnapshot Snapshot() const { return mModel.Snapshot(); }
  [[nodiscard]] uint64_t Used() const { return mUsed; }
  [[nodiscard]] uint64_t Ignored() const { return mIgnored; }

private:
  VsyncClockModel mModel;
  uint64_t mUsed = 0U;
  uint64_t mIgnored = 0U;
};

enum class VsyncClockSource : uint8_t { None = 0, DisplayEvent = 1, Acquire = 2 };

// Vsync thread on the Switch (oot3d_switch_vsync_clock.cpp; on the host, Start()
// returns false). Times on the std::chrono::steady_clock clock (ns).
class SwitchVsyncClock {
public:
  SwitchVsyncClock() = default;
  ~SwitchVsyncClock();
  SwitchVsyncClock(const SwitchVsyncClock &) = delete;
  SwitchVsyncClock &operator=(const SwitchVsyncClock &) = delete;

  // Opens the display and the event and starts the thread; false if it failed (the
  // reason is in StartResult()).
  bool Start();
  void Stop();
  [[nodiscard]] bool Running() const { return mRunning.load(std::memory_order_acquire); }
  [[nodiscard]] uint32_t StartResult() const { return mStartResult; }
  // Stage that failed (0 none; 1 vi, 2 display, 3 event).
  [[nodiscard]] uint32_t StartStage() const { return mStartStage; }
  [[nodiscard]] VsyncClockSnapshot Read() const { return mPublisher.Read(); }

  // Fallback source (tour19): without the event, the return of the window's acquire.
  // StartAcquireFallback() enables it; ObserveAcquire() comes from the render thread
  // (a single writer of the publisher, since the event thread doesn't exist).
  bool StartAcquireFallback() {
    if (Running()) return false;
    mSource.store(VsyncClockSource::Acquire, std::memory_order_release);
    return true;
  }
  void ObserveAcquire(int64_t returnNs, double waitMs) {
    if (mSource.load(std::memory_order_acquire) != VsyncClockSource::Acquire)
      return;
    if (mAcquire.Observe(returnNs, waitMs))
      mPublisher.Publish(mAcquire.Snapshot());
  }
  [[nodiscard]] VsyncClockSource Source() const {
    return Running() ? VsyncClockSource::DisplayEvent
                     : mSource.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint64_t AcquireSamples() const { return mAcquire.Used(); }

private:
  void Loop();
  VsyncClockPublisher mPublisher;
  AcquireVsyncFilter mAcquire;
  std::atomic<VsyncClockSource> mSource{VsyncClockSource::None};
  uint32_t mStartStage = 0U;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mStop{false};
  uint32_t mStartResult = 0U;
  void *mImpl = nullptr;
};

// The clock whose fallback source receives the acquires (window hook on the
// render thread); null without it.
inline std::atomic<SwitchVsyncClock *> gAcquireVsyncClock{nullptr};

} // namespace Oot3dNativeGame
