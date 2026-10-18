#include "oot3d_native_render_command_stream.h"
#include "oot3d_switch_threads.h"
#include "oot3d_hang_watchdog.h"
#include "fast/backends/gl_frame_profiler.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace Oot3dNativeGame {
namespace {

thread_local bool tInCommand = false;

uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

// cv.wait(lock, ready), counting the waiter and the blocked time only when
// it really blocks (and marking the wait for the hang watchdog).
template <typename Ready>
void CountedWait(std::condition_variable& cv,
                 std::unique_lock<std::mutex>& lock, uint32_t& waiters,
                 uint64_t& waits, uint64_t& nanoseconds,
                 HangWatch::Stage stage, Ready ready) {
    if (ready()) return;
    HangWatch::StageScope hangStage(stage);
    const auto start = std::chrono::steady_clock::now();
    ++waiters;
    cv.wait(lock, ready);
    --waiters;
    ++waits;
    nanoseconds += NanosecondsSince(start);
}
thread_local bool tSynchronous = false;

struct CommandScope {
    explicit CommandScope(bool synchronous) {
        tInCommand = true;
        tSynchronous = synchronous;
    }
    ~CommandScope() {
        tInCommand = false;
        tSynchronous = false;
    }
};

} // namespace

NativeRenderCommandStream::NativeRenderCommandStream(Mode mode,
                                                     size_t maxFramesInFlight)
    : mMode(mode),
      mMaxFramesInFlight(maxFramesInFlight == 0U ? 1U : maxFramesInFlight) {}

NativeRenderCommandStream::~NativeRenderCommandStream() {
    StopWorker();
}

void NativeRenderCommandStream::SetMode(Mode mode) {
    Drain();
    if (mode == mMode) return;
    if (mode == Mode::Inline) StopWorker();
    mMode = mode;
}

bool NativeRenderCommandStream::InCommand() const noexcept {
    return tInCommand;
}

bool NativeRenderCommandStream::InSynchronousCommand() const noexcept {
    return tInCommand && tSynchronous;
}

uint64_t NativeRenderCommandStream::ExecutedCommands() const noexcept {
    std::lock_guard lock(mMutex);
    return mExecuted;
}

NativeRenderCommandStream::WaitStats
NativeRenderCommandStream::GetWaitStats() const noexcept {
    std::lock_guard lock(mMutex);
    return mWaitStats;
}

bool NativeRenderCommandStream::TryGetSyncSnapshot(
    SyncSnapshot& out) const noexcept {
    std::unique_lock lock(mMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out.Queued = mQueue.size();
    out.FramesQueued = mFramesQueued;
    out.Busy = mBusy;
    out.WorkerWaiting = mWorkerWaiting;
    out.Stop = mStop;
    out.Failed = mFailure != nullptr;
    out.ProgressWaiters = mProgressWaiters;
    out.Submitted = mSubmitted;
    out.Executed = mExecuted;
    out.Waits = mWaitStats;
    return true;
}

void NativeRenderCommandStream::Submit(Command command, bool endsFrame) {
    Enqueue(std::move(command), endsFrame, false);
}

void NativeRenderCommandStream::Enqueue(Command command, bool endsFrame,
                                        bool synchronous) {
    // Leaving the game: nothing submitted from now on is executed.
    if (ShutdownRequested()) return;
    if (mMode == Mode::Inline) {
        {
            CommandScope scope(true);
            Fast::GlPerf::CommandScope perfCommand;
            command();
        }
        std::lock_guard lock(mMutex);
        ++mSubmitted;
        ++mExecuted;
        return;
    }
    StartWorker();
    std::unique_lock lock(mMutex);
    RethrowPendingFailureLocked(lock);
    if (endsFrame) {
        CountedWait(mProgress, lock, mProgressWaiters,
                    mWaitStats.SubmitWaits, mWaitStats.SubmitWaitNanoseconds,
                    HangWatch::Stage::WaitFramesInFlight, [&] {
                        return mFailure != nullptr || ShutdownRequested() ||
                               mFramesQueued < mMaxFramesInFlight;
                    });
        if (ShutdownRequested()) return;
        RethrowPendingFailureLocked(lock);
        ++mFramesQueued;
    }
    if (ShutdownRequested()) return;
    mQueue.push_back({std::move(command), endsFrame, synchronous});
    ++mSubmitted;
    if (mWorkerWaiting) {
        mWork.notify_one();
    } else {
        ++mWaitStats.SkippedNotifies;
    }
}

bool NativeRenderCommandStream::Run(Command command) {
    Drain();
    if (ShutdownRequested()) return false;
    {
        HangWatch::StageScope hangStage(HangWatch::Stage::SyncCommand);
        CommandScope scope(true);
        Fast::GlPerf::CommandScope perfCommand;
        command();
    }
    std::lock_guard lock(mMutex);
    ++mSubmitted;
    ++mExecuted;
    return true;
}

void NativeRenderCommandStream::Drain() {
    if (mMode == Mode::Inline || !mWorker.joinable()) return;
    std::unique_lock lock(mMutex);
    CountedWait(mProgress, lock, mProgressWaiters, mWaitStats.DrainWaits,
                mWaitStats.DrainWaitNanoseconds, HangWatch::Stage::WaitDrain, [&] {
                    return mFailure != nullptr || (mQueue.empty() && !mBusy);
                });
    // A failure behind an exit request is not reported: Shutdown drops it.
    if (ShutdownRequested()) return;
    RethrowPendingFailureLocked(lock);
}

void NativeRenderCommandStream::BeginShutdown() noexcept {
    std::deque<Entry> discarded;
    {
        std::lock_guard lock(mMutex);
        mShutdown.store(true, std::memory_order_release);
        // Final entries (only Shutdown queues one) stay.
        for (auto it = mQueue.begin(); it != mQueue.end();) {
            if (it->Final) {
                ++it;
                continue;
            }
            discarded.push_back(std::move(*it));
            it = mQueue.erase(it);
        }
        mDiscarded += discarded.size();
        mFramesQueued = 0U;
    }
    mProgress.notify_all();
    mWork.notify_all();
    // The discarded commands (and what they captured) die here, outside the
    // lock: nothing of theirs ran.
}

size_t NativeRenderCommandStream::Shutdown(Command last) noexcept {
    BeginShutdown();
    if (mWorker.joinable() && mWorker.get_id() == std::this_thread::get_id()) {
        // From inside a command: the submitter finishes the shutdown.
        return 0U;
    }
    if (mWorker.joinable()) {
        {
            std::unique_lock lock(mMutex);
            // The command already running finishes; nothing after it starts.
            CountedWait(mProgress, lock, mProgressWaiters,
                        mWaitStats.DrainWaits,
                        mWaitStats.DrainWaitNanoseconds,
                        HangWatch::Stage::WaitDrain, [&] { return !mBusy; });
            if (last) {
                Entry entry;
                entry.Work = std::move(last);
                entry.Final = true;
                mQueue.push_back(std::move(entry));
            }
            mStop = true;
        }
        mWork.notify_all();
        mWorker.join();
    } else if (last) {
        try {
            CommandScope scope(false);
            last();
        } catch (...) {
        }
    }
    std::lock_guard lock(mMutex);
    mFailure = nullptr;
    mQueue.clear();
    mFramesQueued = 0U;
    mMode = Mode::Inline;
    return static_cast<size_t>(mDiscarded);
}

void NativeRenderCommandStream::RethrowPendingFailureLocked(
    std::unique_lock<std::mutex>& lock) {
    if (mFailure == nullptr) return;
    // Report the failure once; the stream is empty and usable afterwards.
    auto failure = std::exchange(mFailure, nullptr);
    mQueue.clear();
    mFramesQueued = 0U;
    mExecuted = mSubmitted;
    lock.unlock();
    std::rethrow_exception(failure);
}

void NativeRenderCommandStream::StartWorker() {
    if (mWorker.joinable()) return;
    {
        // Under the lock: TryGetSyncSnapshot (hang watchdog) reads it.
        std::lock_guard lock(mMutex);
        mStop = false;
    }
    mWorker = std::thread([this] { WorkerLoop(); });
}

void NativeRenderCommandStream::StopWorker() {
    if (!mWorker.joinable()) return;
    {
        std::lock_guard lock(mMutex);
        mStop = true;
    }
    mWork.notify_all();
    mWorker.join();
}

void NativeRenderCommandStream::WorkerLoop() {
    // Switch: 0x2D, below the game (0x2C) and audio (0x2B) threads: Horizon
    // does not time-slice equal priorities, so a busy render worker sharing a
    // core with the audio thread starved it and the sound crackled. Core 2
    // (layout fixed) or cores 1|2 (float): oot3d_switch_thread_layout.h.
    PlaceSwitchThread(SwitchThreadRole::Render);
    std::unique_lock lock(mMutex);
    for (;;) {
        if (!mStop && mQueue.empty()) {
            const auto idleStart = std::chrono::steady_clock::now();
            HangWatch::Mark(HangWatch::Stage::RenderIdle);
            mWorkerWaiting = true;
            mWork.wait(lock, [&] { return mStop || !mQueue.empty(); });
            mWorkerWaiting = false;
            ++mWaitStats.WorkerIdleWaits;
            mWaitStats.WorkerIdleNanoseconds += NanosecondsSince(idleStart);
        }
        if (mQueue.empty()) return;  // stopping with nothing left to run
        Entry entry = std::move(mQueue.front());
        mQueue.pop_front();
        if (ShutdownRequested() && !entry.Final) {
            // Queued behind an exit request: never run.
            ++mDiscarded;
            if (entry.EndsFrame && mFramesQueued != 0U) --mFramesQueued;
            lock.unlock();
            entry = {};
            lock.lock();
            if (mProgressWaiters != 0U) mProgress.notify_all();
            continue;
        }
        mBusy = true;
        lock.unlock();
        HangWatch::Mark(HangWatch::Stage::RenderCommand);
        // Where the render thread runs, once per frame (perf.txt threads:).
        if (entry.EndsFrame) NoteSwitchThreadCore();
        std::exception_ptr failure;
        try {
            CommandScope scope(entry.Synchronous);
            Fast::GlPerf::CommandScope perfCommand;
            entry.Work();
        } catch (...) {
            failure = std::current_exception();
        }
        // Progress for the hang watchdog: one command done.
        HangWatch::Advance();
        lock.lock();
        mBusy = false;
        if (entry.EndsFrame && mFramesQueued != 0U) --mFramesQueued;
        if (failure != nullptr) {
            mFailure = failure;
            mQueue.clear();
            mFramesQueued = 0U;
        } else {
            ++mExecuted;
        }
        if (mProgressWaiters != 0U) {
            mProgress.notify_all();
        } else {
            ++mWaitStats.SkippedNotifies;
        }
    }
}

} // namespace Oot3dNativeGame
