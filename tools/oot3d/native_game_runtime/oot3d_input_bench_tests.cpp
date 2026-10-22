// The input bench (oot3d_input_bench.h): a recorded walk must replay frame
// for frame from the same point after the boot warp, whatever the frame rate.
#include "oot3d_input_bench.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_input_bench_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// A walk: hold up and turn, roll with A, stop; values the timeline keeps exactly.
NativeA32InputFrame Walk(uint32_t frame) {
    NativeA32InputFrame input;
    if (frame < 40U) input.Hid.CirclePadY = 154;
    if (frame >= 20U && frame < 40U) input.Hid.CirclePadX = -77;
    if (frame >= 30U && frame < 33U) input.Hid.Buttons |= NativeA32HidButtonMask(NativeA32HidButton::A);
    if (frame >= 45U && frame < 60U) input.CStick.X = 100;
    if (frame >= 50U && frame < 52U) input.Hid.Buttons |= NativeA32HidButtonMask(NativeA32HidButton::Zl);
    return input;
}

bool SameInput(const NativeA32InputFrame& a, const NativeA32InputFrame& b) {
    return a.Hid.Buttons == b.Hid.Buttons && a.Hid.CirclePadX == b.Hid.CirclePadX &&
           a.Hid.CirclePadY == b.Hid.CirclePadY && a.CStick.X == b.CStick.X && a.CStick.Y == b.CStick.Y;
}

constexpr uint32_t kWalkFrames = 64U;

// Recording starts kSettleFrames after the warp lands and stops when asked.
std::filesystem::path RecordWalk(uint64_t arrival) {
    InputBench bench(InputBench::Mode::Record);
    bool started = false;
    for (uint64_t frame = 0; frame < arrival + InputBench::kSettleFrames + kWalkFrames + 1U; ++frame) {
        const bool arrived = frame >= arrival;
        const uint64_t start = arrival + InputBench::kSettleFrames;
        // Before the walk the player's own input is whatever it is: not recorded.
        auto input = frame >= start ? Walk(static_cast<uint32_t>(frame - start)) : NativeA32InputFrame{};
        if (frame < start) input.Hid.Buttons = NativeA32HidButtonMask(NativeA32HidButton::B);
        const bool stop = frame == start + kWalkFrames;
        const auto event = bench.Step(frame, arrived, arrived, stop, input);
        if (event == InputBench::Event::Started) {
            Require(frame == start, "recording did not start kSettleFrames after the arrival");
            started = true;
        }
        if (event == InputBench::Event::Finished) Require(stop, "recording finished without a stop");
    }
    Require(started, "recording never started");
    Require(bench.Recording().EndFrame() == kWalkFrames, "recording does not hold the whole walk");
    Require(bench.Recording().SegmentCount() < kWalkFrames / 4U,
            "identical frames were not merged into segments");
    const auto path = std::filesystem::temp_directory_path() / "oot3d_input_bench_tests.json";
    std::ofstream(path) << bench.Recording().ToJson();
    return path;
}

// The replay starts at another arrival frame (another build, another title
// screen) and must hand the game the same walk; the player's input is ignored.
void ReplayWalk(const std::filesystem::path& path, uint64_t arrival) {
    InputBench bench(InputBench::Mode::Replay, NativeA32InputTimeline::LoadFile(path));
    const uint64_t start = arrival + InputBench::kSettleFrames;
    bool finished = false;
    for (uint64_t frame = 0; frame < start + kWalkFrames + 5U; ++frame) {
        NativeA32InputFrame input;
        input.Hid.Buttons = NativeA32HidButtonMask(NativeA32HidButton::Start);  // the player's
        // Title and file menu until `gameplay`; the warp lands a bit later.
        const bool gameplay = frame + 50U >= arrival;
        const auto event = bench.Step(frame, gameplay, frame >= arrival, false, input);
        if (frame < arrival) {
            const uint32_t pressed = input.Hid.Buttons & ~NativeA32HidButtonMask(NativeA32HidButton::Start);
            Require(input.Hid.Buttons & NativeA32HidButtonMask(NativeA32HidButton::Start),
                    "replay dropped the player's input before the warp landed");
            if (gameplay) {
                Require(pressed == 0U, "replay pressed through the title during gameplay");
            } else if (frame % InputBench::kTitlePressPeriod < InputBench::kTitlePressFrames &&
                       (frame / InputBench::kTitlePressPeriod) % 4U != 0U) {
                Require(pressed == NativeA32HidButtonMask(NativeA32HidButton::A),
                        "replay did not press A through the title screen");
            }
        } else if (frame < start) {
            Require(input.Hid.Buttons == 0U, "replay let the player act while the scene settled");
        } else if (frame < start + kWalkFrames) {
            Require(SameInput(input, Walk(static_cast<uint32_t>(frame - start))),
                    "replayed frame differs from the recorded walk");
        } else if (event == InputBench::Event::Finished) {
            Require(frame == start + kWalkFrames, "replay finished at the wrong frame");
            finished = true;
        }
    }
    Require(finished, "replay never finished");
    Require(!bench.Running(), "replay still running after its end");
}

} // namespace

int main() {
    const auto path = RecordWalk(100U);
    ReplayWalk(path, 100U);
    ReplayWalk(path, 437U);
    std::filesystem::remove(path);
    std::cout << "oot3d_input_bench_tests: ok\n";
    return 0;
}
