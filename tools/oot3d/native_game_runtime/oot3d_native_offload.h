#pragma once

// Work taken off the game thread (Switch core 0), each part switchable for
// A/B on the console. Every part leaves the image, the audio and the order
// the guest observes unchanged (tests: oot3d_native_offload_tests).
//
// Switch: sdmc:/switch/oot3dre/config/offload_<part>; absent = on, "0" = the
// previous code path. Desktop: TRIAEVUM_OFFLOAD=1 turns every part on,
// TRIAEVUM_OFFLOAD_<PART>=0/1 one part (all off by default off the Switch).
//
//  capture: PICA submission capture (inside the guest's GX commands): the
//           draw packet is built where the queue keeps it (one ~11 KiB copy
//           less per draw), texture hashes in one pass over the bytes,
//           geometry hashes only when a savestate asks for them, index
//           ranges remembered with the cached index bytes.
//  plan:    the deferred placeholder is written in place in the accumulator
//           (no ~16 KiB plan moved three times per draw) and closing a guest
//           frame hands the pending vector over instead of moving each plan.
//  perf:    logs/perf.txt lines are written by a service thread (the SD
//           write and file open leave the game thread; the line is the same).
//  actors:  the actor walk for the grass collider bridge (read only by the
//           Vulkan/NRI grass) is skipped on the GL backend.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Oot3dNativeGame {

struct NativeOffloadOptions {
    bool Capture = false;
    bool Plan = false;
    bool Perf = false;
    bool Actors = false;
};

// "0" (first word) disables, anything else (also empty) enables.
[[nodiscard]] inline bool ParseOffloadSwitch(std::string_view text) {
    size_t begin = 0U;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
            text[begin] == '\n'))
        ++begin;
    size_t end = begin;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' &&
           text[end] != '\r' && text[end] != '\n')
        ++end;
    const std::string_view word = text.substr(begin, end - begin);
    return !(word == "0" || word == "off" || word == "false");
}

inline NativeOffloadOptions ReadNativeOffloadOptions() {
    NativeOffloadOptions options;
#if defined(__SWITCH__)
    const auto part = [](const char* name) {
        std::ifstream input(std::string("sdmc:/switch/oot3dre/config/offload_") + name);
        if (!input) return true;
        std::string text;
        std::getline(input, text);
        return ParseOffloadSwitch(text);
    };
#else
    const char* all = std::getenv("TRIAEVUM_OFFLOAD");
    const bool allOn = all != nullptr && ParseOffloadSwitch(all) && std::string_view(all) != "";
    const auto part = [allOn](const char* name) {
        std::string variable = "TRIAEVUM_OFFLOAD_";
        for (const char* c = name; *c != '\0'; ++c)
            variable.push_back(static_cast<char>(*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c));
        const char* value = std::getenv(variable.c_str());
        return value != nullptr ? ParseOffloadSwitch(value) : allOn;
    };
#endif
    options.Capture = part("capture");
    options.Plan = part("plan");
    options.Perf = part("perf");
    options.Actors = part("actors");
    return options;
}

// Appends lines to one file from a service thread, in submission order.
// Push never waits for the file: it takes a mutex the writer holds only to
// pop (never during the write) and drops the line when the queue is full.
class AsyncLineWriter final {
  public:
    explicit AsyncLineWriter(std::string path, size_t maxQueued = 32U)
        : mPath(std::move(path)), mMaxQueued(maxQueued == 0U ? 1U : maxQueued) {}
    ~AsyncLineWriter() { Stop(); }
    AsyncLineWriter(const AsyncLineWriter&) = delete;
    AsyncLineWriter& operator=(const AsyncLineWriter&) = delete;

    // False when the line was dropped (queue full).
    bool Push(std::string text) {
        bool notify = false;
        {
            std::lock_guard lock(mMutex);
            if (!mThread.joinable() && !mStopped) {
                mThread = std::thread([this] { Loop(); });
            }
            if (mQueue.size() >= mMaxQueued) {
                ++mDropped;
                return false;
            }
            mQueue.push_back(std::move(text));
            notify = mWaiting;
        }
        if (notify) mReady.notify_one();
        return true;
    }
    // Writes everything queued and joins the thread (idempotent).
    void Stop() {
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
        }
        mReady.notify_all();
        if (mThread.joinable()) mThread.join();
    }
    [[nodiscard]] uint64_t Dropped() const {
        std::lock_guard lock(mMutex);
        return mDropped;
    }
    [[nodiscard]] uint64_t Written() const noexcept {
        return mWritten.load(std::memory_order_relaxed);
    }
    // Nanoseconds the writer thread spent opening/writing the file.
    [[nodiscard]] uint64_t WriteNanoseconds() const noexcept {
        return mWriteNanoseconds.load(std::memory_order_relaxed);
    }

  private:
    void Loop() {
        std::unique_lock lock(mMutex);
        for (;;) {
            if (mQueue.empty()) {
                if (mStopped) return;
                mWaiting = true;
                mReady.wait(lock, [&] { return mStopped || !mQueue.empty(); });
                mWaiting = false;
                continue;
            }
            std::string text = std::move(mQueue.front());
            mQueue.pop_front();
            lock.unlock();
            const auto started = std::chrono::steady_clock::now();
            {
                std::ofstream output(mPath, std::ios::app | std::ios::binary);
                output.write(text.data(), static_cast<std::streamsize>(text.size()));
            }
            mWriteNanoseconds.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count()),
                std::memory_order_relaxed);
            mWritten.fetch_add(1U, std::memory_order_relaxed);
            lock.lock();
        }
    }

    std::string mPath;
    size_t mMaxQueued;
    mutable std::mutex mMutex;
    std::condition_variable mReady;
    std::deque<std::string> mQueue;
    std::thread mThread;
    bool mWaiting = false;
    bool mStopped = false;
    uint64_t mDropped = 0U;
    std::atomic<uint64_t> mWritten{0};
    std::atomic<uint64_t> mWriteNanoseconds{0};
};

} // namespace Oot3dNativeGame
