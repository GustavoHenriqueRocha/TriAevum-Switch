#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace Oot3dNativeGame {

// Ordered renderer work submitted by the guest loop.
//
// Inline mode runs every command at submission: the program order of the
// serial loop, unchanged. Worker mode runs the same commands, in the same
// order, on one worker thread, so the guest can simulate the next frame while
// the previous one is presented. Commands must own copies of their inputs;
// state they touch must be touched only by commands (or after Drain).
//
// Contract in worker mode:
// - Submit queues a command; at most MaxFramesInFlight frames (commands
//   submitted with EndsFrame) are pending, beyond that Submit waits.
// - Run executes a command synchronously after everything before it, on the
//   calling thread once the worker is idle. Thread-affine work (window
//   operations, which Windows delivers to the window's thread) therefore
//   stays on the submitter, and the renderer is never used by two threads at
//   once.
// - An exception thrown by a command stops the worker from running further
//   commands and is rethrown to the submitter by the next Submit, Run or
//   Drain. Pending commands are discarded.
// - Drain waits until the stream is empty.
// - A command may touch guest-owned state (guest memory, guest interrupts)
//   only while the submitter is known to be stopped: inline, or inside Run.
//   InSynchronousCommand tells the two cases apart; commands that may reach
//   guest state check it and fail instead of racing the guest.
//
// Shutdown (leaving the game): once BeginShutdown is called (from any
// thread, also from inside a command) no command that has not started yet
// ever runs: queued commands are discarded, and Submit and Run drop theirs.
// Shutdown waits for the one command that may be running, discards the
// queue (and a pending failure), runs a last command on the worker (to hand
// thread-affine resources such as the GL context back) and joins it. Frames
// queued behind an exit request are therefore never executed against state
// the exit is about to release.
class NativeRenderCommandStream final {
  public:
    enum class Mode : uint8_t { Inline, Worker };
    using Command = std::function<void()>;

    explicit NativeRenderCommandStream(Mode mode = Mode::Inline,
                                       size_t maxFramesInFlight = 1U);
    ~NativeRenderCommandStream();

    NativeRenderCommandStream(const NativeRenderCommandStream&) = delete;
    NativeRenderCommandStream& operator=(const NativeRenderCommandStream&) =
        delete;

    [[nodiscard]] Mode CurrentMode() const noexcept { return mMode; }
    // Drains, then switches. Inline -> Worker starts the thread lazily.
    void SetMode(Mode mode);

    void Submit(Command command, bool endsFrame = false);
    // False when the command was dropped because shutdown has begun.
    bool Run(Command command);
    void Drain();

    // From now on no command that has not started runs (see above).
    void BeginShutdown() noexcept;
    [[nodiscard]] bool ShutdownRequested() const noexcept {
        return mShutdown.load(std::memory_order_acquire);
    }
    // BeginShutdown, wait for the running command, discard the rest, run
    // `last` on the worker thread (on the caller without one), join the
    // worker and stay inline and closed. Never throws; idempotent. Returns
    // the number of discarded commands.
    size_t Shutdown(Command last = {}) noexcept;

    // True on the worker thread while it executes a command, and on the
    // submitting thread while an inline command runs.
    [[nodiscard]] bool InCommand() const noexcept;
    // True while a command runs and its submitter waits for it: every inline
    // command, and a Run command in worker mode.
    [[nodiscard]] bool InSynchronousCommand() const noexcept;
    [[nodiscard]] uint64_t ExecutedCommands() const noexcept;

    // Time spent blocked on the stream, cumulative (logs/perf.txt keeps the
    // previous copy and prints the difference). Clocks are read only when a
    // wait actually blocks.
    struct WaitStats {
        // Submitter blocked by MaxFramesInFlight (the renderer is behind).
        uint64_t SubmitWaits = 0U;
        uint64_t SubmitWaitNanoseconds = 0U;
        // Submitter blocked in Drain / Run / Shutdown (synchronous work).
        uint64_t DrainWaits = 0U;
        uint64_t DrainWaitNanoseconds = 0U;
        // Worker without a command (the guest is behind).
        uint64_t WorkerIdleWaits = 0U;
        uint64_t WorkerIdleNanoseconds = 0U;
        // Condition-variable signals skipped because nobody waited.
        uint64_t SkippedNotifies = 0U;
    };
    [[nodiscard]] WaitStats GetWaitStats() const noexcept;

  private:
    struct Entry {
        Command Work;
        bool EndsFrame = false;
        bool Synchronous = false;
        // The last command of Shutdown: runs although shutdown has begun.
        bool Final = false;
    };

    void Enqueue(Command command, bool endsFrame, bool synchronous);
    void StartWorker();
    void StopWorker();
    void WorkerLoop();
    void RethrowPendingFailureLocked(std::unique_lock<std::mutex>& lock);

    Mode mMode;
    size_t mMaxFramesInFlight;
    std::thread mWorker;
    mutable std::mutex mMutex;
    std::condition_variable mWork;
    std::condition_variable mProgress;
    std::deque<Entry> mQueue;
    size_t mFramesQueued = 0U;
    bool mBusy = false;
    bool mStop = false;
    std::exception_ptr mFailure;
    uint64_t mExecuted = 0U;
    uint64_t mSubmitted = 0U;
    std::atomic<bool> mShutdown{false};
    uint64_t mDiscarded = 0U;
    // Waiters, under mMutex: signals go out only when someone waits (each
    // one is a kernel call on Horizon).
    bool mWorkerWaiting = false;
    uint32_t mProgressWaiters = 0U;
    WaitStats mWaitStats;
};

} // namespace Oot3dNativeGame
