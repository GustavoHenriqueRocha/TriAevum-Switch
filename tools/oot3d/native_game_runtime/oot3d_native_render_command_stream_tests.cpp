#include "oot3d_native_render_command_stream.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Oot3dNativeGame::NativeRenderCommandStream;
using Mode = NativeRenderCommandStream::Mode;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_native_render_command_stream_tests: " << message << '\n';
        std::exit(1);
    }
}

void TestInlineRunsAtSubmission() {
    NativeRenderCommandStream stream(Mode::Inline);
    int value = 0;
    bool inside = false;
    stream.Submit([&] { value = 1; inside = stream.InCommand(); });
    Require(value == 1 && inside && !stream.InCommand(),
            "inline command did not run at submission inside a command scope");
    bool threw = false;
    try {
        stream.Submit([] { throw std::runtime_error("inline"); });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw, "inline command exception did not reach the submitter");
}

void TestWorkerKeepsProgramOrder() {
    NativeRenderCommandStream stream(Mode::Worker, 2U);
    std::vector<int> order;
    const auto mainThread = std::this_thread::get_id();
    std::atomic<bool> offThread{true};
    for (int i = 0; i < 1000; ++i) {
        stream.Submit([&, i] {
            order.push_back(i);
            if (std::this_thread::get_id() == mainThread) offThread = false;
        }, i % 10 == 9);
    }
    int observed = -1;
    stream.Run([&] { observed = static_cast<int>(order.size()); });
    Require(observed == 1000, "Run did not execute after every earlier command");
    for (int i = 0; i < 1000; ++i) {
        Require(order[static_cast<size_t>(i)] == i, "worker changed command order");
    }
    Require(offThread.load(), "worker commands ran on the submitting thread");
    stream.Drain();
}

void TestFramesInFlightBound() {
    NativeRenderCommandStream stream(Mode::Worker, 1U);
    std::atomic<bool> release{false};
    std::atomic<bool> secondSubmitted{false};
    stream.Submit([&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, true);
    std::thread producer([&] {
        stream.Submit([] {}, true);
        secondSubmitted = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(!secondSubmitted.load(),
            "a second frame was accepted while the first was still in flight");
    release = true;
    producer.join();
    Require(secondSubmitted.load(), "second frame was never accepted");
    stream.Drain();
}

void TestWorkerFailureIsRethrownOnce() {
    NativeRenderCommandStream stream(Mode::Worker);
    bool laterRan = false;
    stream.Submit([] { throw std::runtime_error("render failed"); });
    stream.Submit([&] { laterRan = true; });
    bool rethrown = false;
    try {
        stream.Drain();
    } catch (const std::runtime_error& error) {
        rethrown = std::string(error.what()) == "render failed";
    }
    Require(rethrown, "worker exception was not rethrown by Drain");
    Require(!laterRan, "commands after a failure still ran");
    int value = 0;
    stream.Run([&] { value = 7; });
    Require(value == 7, "stream was not usable after reporting its failure");
}

void TestModeSwitchDrainsAndDestructorRunsPending() {
    std::vector<int> order;
    {
        NativeRenderCommandStream stream(Mode::Worker);
        for (int i = 0; i < 50; ++i) stream.Submit([&, i] { order.push_back(i); });
        stream.SetMode(Mode::Inline);
        Require(order.size() == 50U, "switching to inline did not drain the worker");
        stream.Submit([&] { order.push_back(50); });
        Require(order.size() == 51U, "inline mode did not run immediately after a switch");
        stream.SetMode(Mode::Worker);
        for (int i = 51; i < 60; ++i) stream.Submit([&, i] { order.push_back(i); });
    }
    Require(order.size() == 60U, "destruction discarded pending commands");
    for (int i = 0; i < 60; ++i) {
        Require(order[static_cast<size_t>(i)] == i, "order changed across mode switches");
    }
}

// Only commands whose submitter waits for them may reach guest state.
void TestSynchronousCommandsAreDistinguished() {
    for (const Mode mode : {Mode::Inline, Mode::Worker}) {
        NativeRenderCommandStream stream(mode);
        bool submitted = true;
        bool ran = false;
        stream.Submit([&] { submitted = stream.InSynchronousCommand(); });
        std::thread::id runThread;
        stream.Run([&] {
            ran = stream.InSynchronousCommand();
            runThread = std::this_thread::get_id();
        });
        Require(runThread == std::this_thread::get_id(),
                "Run did not execute on the calling thread");
        Require(submitted == (mode == Mode::Inline) && ran &&
                    !stream.InSynchronousCommand(),
                "stream misreported a synchronous command");
    }
}

} // namespace

// Wait counters (logs/perf.txt "sync:"): a blocked submitter, a blocked
// Drain and an idle worker are timed; signals with nobody waiting are
// skipped without losing a wake-up (many tiny commands, each could race the
// worker going to sleep).
void TestWaitStatsAndSkippedSignals() {
    NativeRenderCommandStream stream(Mode::Worker, 1U);
    std::atomic<bool> release{false};
    stream.Submit([&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, true);
    std::thread releaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        release = true;
    });
    stream.Submit([] {}, true);  // blocks: one frame in flight
    releaser.join();
    stream.Drain();
    auto stats = stream.GetWaitStats();
    Require(stats.SubmitWaits == 1U && stats.SubmitWaitNanoseconds >= 10'000'000U,
            "blocked submission was not timed");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::atomic<bool> releaseDrain{false};
    stream.Submit([&] {
        while (!releaseDrain.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    std::thread drainReleaser([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        releaseDrain = true;
    });
    const auto before = stream.GetWaitStats();
    stream.Drain();
    drainReleaser.join();
    stats = stream.GetWaitStats();
    Require(stats.DrainWaits == before.DrainWaits + 1U &&
                stats.DrainWaitNanoseconds - before.DrainWaitNanoseconds >= 5'000'000U,
            "blocked Drain was not timed");
    Require(stats.WorkerIdleWaits >= 1U && stats.WorkerIdleNanoseconds >= 10'000'000U,
            "idle worker time was not counted");
    for (int round = 0; round < 2000; ++round) {
        int value = -1;
        stream.Submit([&value, round] { value = round; }, true);
        stream.Drain();
        Require(value == round, "a command was lost after a skipped signal");
    }
    stats = stream.GetWaitStats();
    Require(stats.SkippedNotifies > 0U, "no signal was ever skipped");
}

// config/frames_in_flight=2: a second frame is accepted while the first
// runs, a third waits.
void TestTwoFramesInFlight() {
    NativeRenderCommandStream stream(Mode::Worker, 1U);
    stream.SetMaxFramesInFlight(2U);
    Require(stream.MaxFramesInFlight() == 2U, "frames in flight not set");
    std::atomic<bool> release{false};
    std::atomic<int> accepted{0};
    std::atomic<int> executed{0};
    stream.Submit([&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ++executed;
    }, true);
    std::thread producer([&] {
        stream.Submit([&] { ++executed; }, true);
        accepted = 1;
        stream.Submit([&] { ++executed; }, true);
        accepted = 2;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Require(accepted.load() == 1,
            "with two frames in flight the second must pass and the third wait");
    release = true;
    producer.join();
    stream.Drain();
    Require(accepted.load() == 2 && executed.load() == 3,
            "every frame must run once, in order");
    stream.SetMaxFramesInFlight(0U);
    Require(stream.MaxFramesInFlight() == 1U, "zero frames in flight means one");
}

// tour24: o áudio adiado só entra antes da apresentação quando o próximo
// fim de quadro esperaria o renderer (FrameSlotsFull).
void TestFrameSlotsFull() {
    NativeRenderCommandStream inlineStream(Mode::Inline, 1U);
    Require(!inlineStream.FrameSlotsFull(), "inline stream never waits for a slot");
    NativeRenderCommandStream stream(Mode::Worker, 1U);
    Require(!stream.FrameSlotsFull(), "empty stream reported full slots");
    std::atomic<bool> release{false};
    stream.Submit([&] {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }, true);
    Require(stream.FrameSlotsFull(), "frame in flight did not fill the only slot");
    release = true;
    stream.Drain();
    Require(!stream.FrameSlotsFull(), "slot still full after the frame finished");
}

int main() {
    TestInlineRunsAtSubmission();
    TestWorkerKeepsProgramOrder();
    TestFramesInFlightBound();
    TestTwoFramesInFlight();
    TestWorkerFailureIsRethrownOnce();
    TestModeSwitchDrainsAndDestructorRunsPending();
    TestSynchronousCommandsAreDistinguished();
    TestWaitStatsAndSkippedSignals();
    TestFrameSlotsFull();
    std::cout << "oot3d_native_render_command_stream_tests: ok\n";
    return 0;
}
