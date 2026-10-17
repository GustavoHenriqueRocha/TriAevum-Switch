#include "oot3d_native_render_command_stream.h"
#include "oot3d_switch_threads.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <stdexcept>
#include <utility>

namespace Oot3dNativeGame {
namespace {

thread_local bool tInCommand = false;
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
        mProgress.wait(lock, [&] {
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
    mWork.notify_one();
}

bool NativeRenderCommandStream::Run(Command command) {
    Drain();
    if (ShutdownRequested()) return false;
    {
        CommandScope scope(true);
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
    mProgress.wait(lock, [&] {
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
            mProgress.wait(lock, [&] { return !mBusy; });
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
    mStop = false;
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
#if defined(__SWITCH__)
    // One step below the game and audio threads (0x2C): Horizon does not
    // time-slice equal priorities, so a busy render worker sharing a core
    // with the audio thread starved it and the sound crackled.
    svcSetThreadPriority(threadGetCurHandle(), 0x2D);
    // The heaviest thread after the game: core 2 to itself (the plan worker
    // and audio take core 1) instead of wherever creation order put it.
    (void)SetSwitchThreadCore(kSwitchRenderThreadCore);
#endif
    std::unique_lock lock(mMutex);
    for (;;) {
        mWork.wait(lock, [&] { return mStop || !mQueue.empty(); });
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
            mProgress.notify_all();
            continue;
        }
        mBusy = true;
        lock.unlock();
        std::exception_ptr failure;
        try {
            CommandScope scope(entry.Synchronous);
            entry.Work();
        } catch (...) {
            failure = std::current_exception();
        }
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
        mProgress.notify_all();
    }
}

} // namespace Oot3dNativeGame
