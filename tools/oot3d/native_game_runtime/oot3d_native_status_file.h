#pragma once

// A small status file that is replaced, not appended (boot-status.txt), with
// its latest text written by a service thread (config/async_status_writes;
// SWITCH_PORT.md "boot-status.txt heartbeat off the game thread").
//
// The game thread rewrote boot-status.txt every 600 host frames: one SD
// open/truncate/write/close inside the frame, 30-80 ms of game-thread time
// with no guest time (logs19-logs20f hitches.txt: every present 600k-3).
// Post hands the text over and returns; a text not yet written is replaced
// by the newer one (only the last state matters). WriteNow writes on the
// calling thread after the write in flight and drops a posted text still
// waiting: the boot stages and the final completed/failed/terminated state
// can never be overwritten by an older heartbeat.
//
// Every write of the process's boot-status.txt goes through
// SwitchBootStatusFile() (oot3d_native_a32_window.cpp, oot3d_native_game.cpp).
// Tests: oot3d_native_status_file_tests.

#include "oot3d_native_offload.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Oot3dNativeGame {

struct StatusFileWriterStats {
    uint64_t Posted = 0U;            // texts handed to the service thread
    uint64_t Written = 0U;           // of those, written (the others superseded)
    uint64_t Superseded = 0U;        // replaced by a newer text before their write
    uint64_t SyncWrites = 0U;        // WriteNow calls
    uint64_t WriteNanoseconds = 0U;  // service thread: open, write, close
    uint64_t SyncNanoseconds = 0U;   // WriteNow: wait for the write in flight + its own write
};

class StatusFileWriter final {
  public:
    explicit StatusFileWriter(std::string path) : mPath(std::move(path)) {}
    ~StatusFileWriter() { Stop(); }
    StatusFileWriter(const StatusFileWriter&) = delete;
    StatusFileWriter& operator=(const StatusFileWriter&) = delete;

    // Hands the text to the service thread; never waits for the file. After
    // Stop it writes here instead (WriteNow).
    void Post(std::string text) {
        bool taken = false, notify = false;
        {
            std::lock_guard lock(mMutex);
            if (!mStopped) {
                if (!mThread.joinable()) mThread = std::thread([this] { Loop(); });
                if (mPending.has_value()) ++mSuperseded;
                mPending = std::move(text);
                ++mPosted;
                taken = true;
                notify = mWaiting;
            }
        }
        if (notify) mReady.notify_one();
        if (!taken) (void)WriteNow(text);
    }

    // Writes on this thread once the write in flight (if any) is done; a
    // posted text still waiting is dropped (this one is newer). False when
    // the file could not be written.
    bool WriteNow(std::string_view text) {
        const auto start = std::chrono::steady_clock::now();
        std::lock_guard file(mFileMutex);
        {
            std::lock_guard lock(mMutex);
            if (mPending.has_value()) {
                mPending.reset();
                ++mSuperseded;
                mIdle.notify_all();
            }
        }
        const bool written = WriteFile(mPath, text);
        mSyncWrites.fetch_add(1U, std::memory_order_relaxed);
        mSyncNanoseconds.fetch_add(NanosecondsSince(start), std::memory_order_relaxed);
        return written;
    }

    // Waits until no posted text is waiting or being written.
    void Flush() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return !mPending.has_value() && !mWriting; });
    }

    // Writes the posted text still waiting and joins the thread (idempotent).
    void Stop() {
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
        }
        mReady.notify_all();
        if (mThread.joinable()) mThread.join();
    }

    [[nodiscard]] StatusFileWriterStats Stats() const {
        StatusFileWriterStats stats;
        {
            std::lock_guard lock(mMutex);
            stats.Posted = mPosted;
            stats.Superseded = mSuperseded;
        }
        stats.Written = mWritten.load(std::memory_order_relaxed);
        stats.SyncWrites = mSyncWrites.load(std::memory_order_relaxed);
        stats.WriteNanoseconds = mWriteNanoseconds.load(std::memory_order_relaxed);
        stats.SyncNanoseconds = mSyncNanoseconds.load(std::memory_order_relaxed);
        return stats;
    }

    [[nodiscard]] const std::string& Path() const noexcept { return mPath; }

  private:
    static uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - start)
                                         .count());
    }

    static bool WriteFile(const std::string& path, std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        return !output.fail();
    }

    // Lock order: mFileMutex, then mMutex (here and in WriteNow). The text is
    // taken from mPending only under mFileMutex, so a WriteNow that follows
    // can never be overwritten by a text taken before it.
    void Loop() {
        std::unique_lock lock(mMutex);
        for (;;) {
            if (!mPending.has_value()) {
                if (mStopped) return;
                mWaiting = true;
                mReady.wait(lock, [&] { return mStopped || mPending.has_value(); });
                mWaiting = false;
                continue;
            }
            lock.unlock();
            {
                std::lock_guard file(mFileMutex);
                std::optional<std::string> text;
                {
                    std::lock_guard again(mMutex);
                    text.swap(mPending);  // a WriteNow may have dropped it meanwhile
                    mWriting = text.has_value();
                }
                if (text.has_value()) {
                    const auto start = std::chrono::steady_clock::now();
                    (void)WriteFile(mPath, *text);
                    mWriteNanoseconds.fetch_add(NanosecondsSince(start), std::memory_order_relaxed);
                    mWritten.fetch_add(1U, std::memory_order_relaxed);
                }
            }
            lock.lock();
            mWriting = false;
            mIdle.notify_all();
        }
    }

    std::string mPath;
    std::mutex mFileMutex;
    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::condition_variable mIdle;
    std::optional<std::string> mPending;
    std::thread mThread;
    bool mWaiting = false;
    bool mWriting = false;
    bool mStopped = false;
    uint64_t mPosted = 0U;
    uint64_t mSuperseded = 0U;
    std::atomic<uint64_t> mWritten{0};
    std::atomic<uint64_t> mSyncWrites{0};
    std::atomic<uint64_t> mWriteNanoseconds{0};
    std::atomic<uint64_t> mSyncNanoseconds{0};
};

inline constexpr const char* kSwitchBootStatusPath = "sdmc:/switch/oot3dre/boot-status.txt";

// The process's boot-status.txt (Switch): the boot stages, the heartbeat and
// the final state all go through this one writer.
inline StatusFileWriter& SwitchBootStatusFile() {
    static StatusFileWriter writer(kSwitchBootStatusPath);
    return writer;
}

// config/async_status_writes: boot-status.txt's heartbeat (every 600 host
// frames) written by a service thread. Switch: the file absent = on, "0" =
// written on the game thread as before. Desktop:
// TRIAEVUM_ASYNC_STATUS_WRITES=1 (off otherwise; nothing posts there).
inline bool ReadNativeAsyncStatusWrites() {
#if defined(__SWITCH__)
    std::ifstream input("sdmc:/switch/oot3dre/config/async_status_writes");
    if (!input) return true;
    std::string text;
    std::getline(input, text);
    return ParseOffloadSwitch(text);
#else
    const char* value = std::getenv("TRIAEVUM_ASYNC_STATUS_WRITES");
    return value != nullptr && std::string_view(value) != "" && ParseOffloadSwitch(value);
#endif
}

} // namespace Oot3dNativeGame
