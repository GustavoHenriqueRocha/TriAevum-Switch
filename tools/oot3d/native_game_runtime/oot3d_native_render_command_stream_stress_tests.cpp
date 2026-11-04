// Stress test of NativeRenderCommandStream against lost wakeup and deadlock
// (optimization "only signal the condvar when someone is waiting", commit 9b45d8e6).
// Run with TSan: millions of commands, frames in flight 1 and 2, Run/Drain
// mixed, commands of random duration (empty, yield, short sleep),
// a third thread reading the stats and the state with try_lock (like the
// hang watchdog) and stops requested from inside a command. A
// vigil aborts if nothing moves for 20 s: a lost signal shows up as an eternal
// wait, not as a race.
//
// Usage: oot3d_native_render_command_stream_stress_tests [commands_per_config]
#include "oot3d_hang_watchdog.h"
#include "oot3d_native_render_command_stream.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

namespace {

using Oot3dNativeGame::NativeRenderCommandStream;
using Mode = NativeRenderCommandStream::Mode;
namespace HangWatch = Oot3dNativeGame::HangWatch;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "oot3d_native_render_command_stream_stress_tests: %s\n", message);
        std::exit(1);
    }
}

std::atomic<uint64_t> gProgress{0};

// Aborts if gProgress stops advancing (deadlock / lost signal).
class Vigil {
  public:
    explicit Vigil(const char* phase) : mPhase(phase) {
        mThread = std::thread([this] {
            uint64_t last = gProgress.load();
            auto lastChange = std::chrono::steady_clock::now();
            while (!mStop.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const uint64_t now = gProgress.load();
                if (now != last) {
                    last = now;
                    lastChange = std::chrono::steady_clock::now();
                } else if (std::chrono::steady_clock::now() - lastChange >
                           std::chrono::seconds(20)) {
                    std::fprintf(stderr,
                                 "DEADLOCK: no progress for 20 s in %s (game stage %s)\n",
                                 mPhase,
                                 HangWatch::StageName(HangWatch::gSlots[0].CurrentStage.load()));
                    std::_Exit(2);
                }
            }
        });
    }
    ~Vigil() {
        mStop = true;
        mThread.join();
    }

  private:
    const char* mPhase;
    std::atomic<bool> mStop{false};
    std::thread mThread;
};

// Random duration: mostly empty (the case that most exercises the race
// between the push and the worker's wait), some yields, rare sleeps.
void Jitter(uint32_t roll) {
    if (roll % 64U == 0U) {
        std::this_thread::sleep_for(std::chrono::microseconds(roll % 50U));
    } else if (roll % 8U == 0U) {
        std::this_thread::yield();
    }
}

void StressOrderAndWakeups(size_t framesInFlight, uint64_t commands, uint32_t seed) {
    Vigil vigil("order/wakeups");
    NativeRenderCommandStream stream(Mode::Worker, framesInFlight);
    std::mt19937 random(seed);
    // Sequence: each command checks that it is the next one (program order).
    uint64_t executed = 0;  // only the commands touch it (or after Drain/Run)
    std::atomic<bool> stopMonitor{false};
    std::atomic<uint64_t> snapshots{0};
    std::atomic<uint64_t> busySnapshots{0};
    // Concurrent reader, like the watchdog: try_lock and counters.
    std::thread monitor([&] {
        while (!stopMonitor.load(std::memory_order_relaxed)) {
            NativeRenderCommandStream::SyncSnapshot sync;
            if (stream.TryGetSyncSnapshot(sync)) {
                Require(sync.Executed <= sync.Submitted, "executed > submitted");
                Require(sync.FramesQueued <= framesInFlight, "frames in flight over the limit");
                snapshots.fetch_add(1U, std::memory_order_relaxed);
            } else {
                busySnapshots.fetch_add(1U, std::memory_order_relaxed);
            }
            (void)stream.GetWaitStats();
            (void)HangWatch::gSlots[0].Progress.load(std::memory_order_relaxed);
            (void)HangWatch::gSlots[0].CurrentStage.load(std::memory_order_relaxed);
            // Like the watchdog, from time to time (under TSan, spinning on
            // try_lock just serializes everything).
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });
    uint64_t submitted = 0;
    uint64_t frames = 0;
    while (submitted < commands) {
        HangWatch::Advance();
        const uint32_t perFrame = random() % 6U;
        for (uint32_t index = 0; index <= perFrame; ++index) {
            const bool endsFrame = index == perFrame;
            const uint64_t expected = submitted++;
            const uint32_t roll = random();
            stream.Submit(
                [&executed, expected, roll] {
                    Require(executed == expected, "worker changed command order");
                    ++executed;
                    gProgress.fetch_add(1U, std::memory_order_relaxed);
                    Jitter(roll);
                },
                endsFrame);
        }
        ++frames;
        const uint32_t pick = random() % 997U;
        if (pick < 7U) {
            stream.Drain();
            Require(executed == submitted, "Drain returned before the queue emptied");
        } else if (pick < 17U) {
            const uint64_t expected = submitted++;
            stream.Run([&executed, expected] {
                Require(executed == expected, "Run did not see every earlier command");
                ++executed;
            });
        }
        if (random() % 4096U == 0U) {
            // Submitter slower than the render thread: the worker sleeps.
            std::this_thread::sleep_for(std::chrono::microseconds(random() % 200U));
        }
    }
    stream.Drain();
    Require(executed == submitted, "commands lost");
    const auto waits = stream.GetWaitStats();
    stopMonitor = true;
    monitor.join();
    std::printf("  frames_in_flight=%zu commands=%llu frames=%llu submit_waits=%llu "
                "worker_idle_waits=%llu skipped_notifies=%llu snapshots=%llu/%llu busy\n",
                framesInFlight, static_cast<unsigned long long>(submitted),
                static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(waits.SubmitWaits),
                static_cast<unsigned long long>(waits.WorkerIdleWaits),
                static_cast<unsigned long long>(waits.SkippedNotifies),
                static_cast<unsigned long long>(snapshots.load()),
                static_cast<unsigned long long>(busySnapshots.load()));
    Require(waits.WorkerIdleWaits > 0U && waits.SkippedNotifies > 0U,
            "stress did not exercise both the sleeping and the skipped-signal paths");
}

// Stop requested from inside a command (HOME > Close) with the submitter
// blocked on frames in flight, in Drain or in Run.
void StressShutdownFromCommand(uint32_t rounds, uint32_t seed) {
    Vigil vigil("shutdown");
    std::mt19937 random(seed);
    for (uint32_t round = 0; round < rounds; ++round) {
        NativeRenderCommandStream stream(Mode::Worker, 1U + random() % 2U);
        const uint32_t closeAt = 1U + random() % 200U;
        std::atomic<uint32_t> ran{0};
        uint32_t submitted = 0;
        while (!stream.ShutdownRequested() && submitted < 400U) {
            ++submitted;
            const uint32_t roll = random();
            stream.Submit(
                [&, roll] {
                    if (ran.fetch_add(1U) + 1U == closeAt) stream.BeginShutdown();
                    gProgress.fetch_add(1U, std::memory_order_relaxed);
                    Jitter(roll);
                },
                roll % 3U == 0U);
            const uint32_t pick = random() % 50U;
            if (pick == 0U) stream.Drain();
            if (pick == 1U) (void)stream.Run([] {});
        }
        stream.Shutdown([] {});
        Require(ran.load() <= submitted, "ran more commands than submitted");
        gProgress.fetch_add(1U, std::memory_order_relaxed);
    }
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t commands =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 2'000'000ULL;
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    HangWatch::Bind(HangWatch::Thread::Game, 1U);
    std::printf("oot3d_native_render_command_stream_stress_tests: %llu commands per config\n",
                static_cast<unsigned long long>(commands));
    StressOrderAndWakeups(1U, commands, 1U);
    StressOrderAndWakeups(2U, commands, 2U);
    StressShutdownFromCommand(static_cast<uint32_t>(commands / 2000U + 50U), 3U);
    std::puts("oot3d_native_render_command_stream_stress_tests: ok");
    return 0;
}
