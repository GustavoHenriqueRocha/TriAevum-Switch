#pragma once

#include "oot3d_host_audio_resampler.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Oot3dNativeGame {

// Pitch-preserving tempo change and dropout concealment for the DSP output
// (interleaved stereo s16, before the host resampler).
//
// Whole pitch periods are inserted (slower tempo) or removed (faster tempo)
// with a one-period linear crossfade, PICOLA/Sonic style: the pitch does not
// move, only the duration. With no input at all the same insertion repeats the
// last period (a seamless loop by construction) under a fade, so a late guest
// frame is heard as a short held note instead of a hole in the output.
//
// With ratio 1 the input is copied unchanged, one period late.
class HostAudioStretcher {
  public:
    static constexpr size_t MinimumPeriod = 64;   // ~511 Hz at 32728 Hz
    static constexpr size_t MaximumPeriod = 400;  // ~82 Hz at 32728 Hz
    static constexpr size_t HistoryFrames = MaximumPeriod * 2;

    struct Settings {
        // Concealment keeps full level this long, then fades to silence.
        size_t ConcealFullFrames = 1300;  // ~40 ms at 32728 Hz
        size_t ConcealFadeFrames = 2600;  // ~80 ms
        // Gain recovery after a concealment that faded.
        size_t RecoverFrames = 256;
        // Frames always kept pending (~12 ms): the material a concealment
        // loops. 0 makes ratio 1 a zero-latency copy.
        size_t HoldbackFrames = MaximumPeriod;
    };

    HostAudioStretcher();
    explicit HostAudioStretcher(Settings settings);

    // Appends `input` and emits to `output`. `ratio` is output duration over
    // input duration (>1 inserts periods, <1 removes them; 1 is a plain copy).
    // `concealFrames` asks for that many extra frames when the input is late.
    void Process(std::span<const int16_t> input, double ratio,
                 size_t concealFrames, std::vector<int16_t>& output);

    void Reset();
    // Emits every pending frame unchanged (end of a stream; tests).
    void Drain(std::vector<int16_t>& output);

    size_t PendingFrames() const { return mPending.size() / 2U; }
    bool Concealing() const { return mConcealedRun != 0; }
    uint64_t InsertedFrames() const { return mInsertedFrames; }
    uint64_t RemovedFrames() const { return mRemovedFrames; }
    uint64_t ConcealedFrames() const { return mConcealedFrames; }
    uint64_t ConcealEvents() const { return mConcealEvents; }

    // Best period P in [MinimumPeriod, MaximumPeriod] such that the P frames
    // starting at `first` resemble the P frames that follow them, searched over
    // `frames` frames of interleaved stereo. `anchorEnd` aligns the pair of
    // periods with the end of the window instead of its start. Returns 0 when
    // the window holds fewer than 2 * MinimumPeriod frames.
    static size_t FindPeriod(const int16_t* stereo, size_t frames, bool anchorEnd);

  private:
    void Emit(const int16_t* stereo, size_t frames, std::vector<int16_t>& output);
    void EmitFrame(float left, float right, std::vector<int16_t>& output);
    // Inserts one period at the end of the pending frames (they must hold at
    // least the period); returns the period, or 0 when not possible.
    size_t InsertAtEnd(std::vector<int16_t>& output);
    size_t RemoveAtStart(std::vector<int16_t>& output);
    void FlushPending(size_t keepFrames, std::vector<int16_t>& output);
    void FadeOutTail(size_t frames, std::vector<int16_t>& output);
    float ConcealGainTarget() const;

    Settings mSettings;
    std::vector<int16_t> mPending;   // input not emitted yet
    std::vector<int16_t> mHistory;   // last emitted frames, before gain
    std::vector<int16_t> mScratch;
    double mCredit = 0.0;            // frames still to insert (+) or remove (-)
    int mCreditDirection = 0;        // +1 inserting, -1 removing, 0 neither
    float mGain = 1.0F;
    float mGainStep = 0.0F;
    float mGainTarget = 1.0F;
    size_t mConcealedRun = 0;
    uint64_t mInsertedFrames = 0;
    uint64_t mRemovedFrames = 0;
    uint64_t mConcealedFrames = 0;
    uint64_t mConcealEvents = 0;
};

// Host side of the game's audio: takes the DSP output (32728 Hz) from the game
// thread, and on a worker thread (or inline) stretches it when the device queue
// runs low, resamples it to the device rate with the queue-holding rate trim
// and queues it on the device. Nothing here is visible to the guest: the DSP
// HLE itself stays on the game thread.
class HostAudioStream {
  public:
    struct Sink {
        std::function<int32_t()> Buffered;  // device-queued frames
        std::function<void(const int16_t*, size_t)> Play;  // samples
        std::function<void()> Flush;
    };
    struct Options {
        uint32_t InputRate = 32728;
        uint32_t OutputRate = 48000;
        int32_t TargetFrames = 4096;  // device frames the queue is held at
        int32_t DeviceFrames = 512;   // frames per device buffer
        bool Thread = true;
        bool Stretch = true;
        // Worker wake-up period while no input arrives.
        uint32_t PollMicroseconds = 4000;
        // Thread priority on the Switch (0 keeps the inherited one).
        int SwitchPriority = 0x2B;
        // Wall clock in seconds (tests); empty = steady_clock.
        std::function<double()> Clock;
    };
    struct Stats {
        uint64_t Pumps = 0;
        uint64_t InsertedFrames = 0;   // input-rate frames
        uint64_t RemovedFrames = 0;
        uint64_t ConcealedFrames = 0;
        uint64_t ConcealEvents = 0;
        uint64_t DeviceStarvations = 0;  // queue seen empty after priming
        int32_t MinimumQueued = 0;       // since the last TakeWindowStats
        double MaximumPumpMilliseconds = 0.0;
        double PumpMilliseconds = 0.0;   // total
        double GameSpeed = 1.0;          // smoothed DSP delivery / nominal
    };

    HostAudioStream(Options options, Sink sink);
    ~HostAudioStream();

    HostAudioStream(const HostAudioStream&) = delete;
    HostAudioStream& operator=(const HostAudioStream&) = delete;

    // Game thread. Copies the samples; with Options::Thread the work happens
    // on the worker, otherwise right here.
    void Submit(std::span<const int16_t> interleavedStereo);
    // Game thread: drops queued audio and history (savestate load). Returns
    // once the worker is idle and the device queue is clear.
    void Flush();
    // One step of the worker (exposed for tests and the inline mode).
    void Pump();

    Stats GetStats() const;
    // "audio: thread=1 stretch=1 ins=..": for logs/perf.txt.
    std::string PerfSummary();
    const Options& GetOptions() const { return mOptions; }

  private:
    void WorkerMain();
    double StretchRatio(double level) const;
    int32_t CriticalFrames() const;
    double NowSeconds() const;
    void UpdateSpeed(double now, size_t inputFrames);

    // Queue levels, as fractions of Options::TargetFrames, outside which the
    // stretcher acts (in between only the inaudible rate trim does).
    static constexpr double kInsertBelowLevel = 0.3;
    static constexpr double kRemoveAboveLevel = 2.6;
    // Sustained slowdown: entered below this game speed, left above the
    // other; while in it the tempo follows the speed under this queue level.
    static constexpr double kSlowGameEnterSpeed = 0.93;
    static constexpr double kSlowGameLeaveSpeed = 0.97;
    static constexpr double kSlowGameHoldLevel = 0.75;

    Options mOptions;
    Sink mSink;
    std::optional<HostAudioResampler> mResampler;
    HostAudioStretcher mStretcher;
    double mQueueEstimate = -1.0;
    bool mPrimed = false;
    double mSpeedEstimate = 1.0;
    double mSpeedWindowStart = -1.0;
    uint64_t mSpeedWindowFrames = 0;
    bool mSlowGame = false;
    std::vector<int16_t> mWork;
    std::vector<int16_t> mStretched;
    std::vector<int16_t> mResampled;

    mutable std::mutex mStatsMutex;
    Stats mStats;
    int32_t mWindowMinimumQueued = -1;
    // Per perf window (reset by PerfSummary): device drain and input rates.
    int32_t mLastQueuedAfter = -1;
    uint64_t mWindowConsumedFrames = 0;
    uint64_t mWindowInputFrames = 0;
    double mWindowQueuedSum = 0.0;
    uint64_t mWindowQueuedCount = 0;
    bool mWindowStarted = false;
    double mWindowStart = 0.0;  // NowSeconds()
    double mRateAdjustment = 0.0;

    std::mutex mPumpMutex;   // held by Pump and Flush
    std::mutex mInputMutex;  // guards mInput and mStop
    std::condition_variable mWake;
    std::vector<int16_t> mInput;
    bool mStop = false;
    std::thread mWorker;
};

} // namespace Oot3dNativeGame
