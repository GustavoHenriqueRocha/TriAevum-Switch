#pragma once

// Writes the settings menus' config/ files off the game thread. The L3+R3
// tabs are drawn on the game thread (ImGui, before the frame's guest work):
// each of their saves opened, wrote and closed files on the SD card there,
// 10-60 ms each (test.15: an HD texture size change wrote config/texture_pack
// and config/texture_pack_scale and probed the pack folder in one frame, and
// the game thread missed 117-157 ms; logs23/hitches.txt:313-510). A setting
// still applies at once (each tab publishes it); only its file is written
// here, by one service thread, in order. A newer write of the same file
// replaces one not started yet. Header-only, checked on the host
// (oot3d_texture_pack_scale_tests).

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace Fast::Oot3d {

class ConfigWriter {
  public:
    // A write: true when it succeeded.
    using Write = std::function<bool()>;

    struct Stats {
        uint64_t Writes = 0;        // done
        uint64_t Failures = 0;
        uint64_t LongestMicros = 0; // the longest write since ResetLongest
    };

    ConfigWriter() = default;
    ConfigWriter(const ConfigWriter&) = delete;
    ConfigWriter& operator=(const ConfigWriter&) = delete;
    // Every queued write is done before the thread ends (the last change of a
    // menu is not lost when the game closes).
    ~ConfigWriter() {
        {
            std::scoped_lock lock(mMutex);
            mStopping = true;
        }
        mWork.notify_all();
        if (mThread.joinable()) mThread.join();
    }

    // Queues `write` for the file `key` (its config/ name). The game thread
    // only takes the lock and moves the job.
    void Submit(std::string key, Write write) {
        {
            std::scoped_lock lock(mMutex);
            const auto queued = std::find_if(mQueue.begin(), mQueue.end(),
                                             [&](const Job& job) { return job.Key == key; });
            if (queued != mQueue.end()) {
                queued->Run = std::move(write);
            } else {
                mQueue.push_back({std::move(key), std::move(write)});
            }
            if (!mThread.joinable()) mThread = std::thread([this] { Main(); });
        }
        mWork.notify_all();
    }
    // The last write of `key` failed (until one succeeds).
    [[nodiscard]] bool Failed(std::string_view key) const {
        std::scoped_lock lock(mMutex);
        return std::find(mFailed.begin(), mFailed.end(), key) != mFailed.end();
    }
    // Until nothing is queued or being written.
    void Flush() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return mQueue.empty() && !mWriting; });
    }
    [[nodiscard]] Stats GetStats() const {
        std::scoped_lock lock(mMutex);
        return mStats;
    }
    void ResetLongest() {
        std::scoped_lock lock(mMutex);
        mStats.LongestMicros = 0U;
    }

  private:
    struct Job {
        std::string Key;
        Write Run;
    };

    void Main() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mMutex);
                mWork.wait(lock, [&] { return mStopping || !mQueue.empty(); });
                if (mQueue.empty()) return; // stopping, nothing left
                job = std::move(mQueue.front());
                mQueue.pop_front();
                mWriting = true;
            }
            const auto start = std::chrono::steady_clock::now();
            const bool written = job.Run();
            const auto micros = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                    .count());
            {
                std::scoped_lock lock(mMutex);
                ++mStats.Writes;
                mStats.Failures += written ? 0U : 1U;
                mStats.LongestMicros = std::max(mStats.LongestMicros, micros);
                const auto failed = std::find(mFailed.begin(), mFailed.end(), job.Key);
                if (written && failed != mFailed.end()) mFailed.erase(failed);
                if (!written && failed == mFailed.end()) mFailed.push_back(std::move(job.Key));
                mWriting = false;
            }
            mIdle.notify_all();
        }
    }

    mutable std::mutex mMutex;
    std::condition_variable mWork, mIdle;
    std::deque<Job> mQueue;
    std::deque<std::string> mFailed;
    Stats mStats;
    bool mWriting = false;
    bool mStopping = false;
    std::thread mThread;
};

// The settings menus' writer (one for the process).
inline ConfigWriter& SettingsConfigWriter() {
    static ConfigWriter writer;
    return writer;
}

} // namespace Fast::Oot3d
