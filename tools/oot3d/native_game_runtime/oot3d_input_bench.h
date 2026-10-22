#pragma once

#include "oot3d_native_a32_input.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace Oot3dNativeGame {

// A walk through a scene, the same on every build: the input of a play
// recorded frame by frame from the boot warp's arrival, then replayed from
// the same guest frame after the same warp. Guest frames, not time, so a
// faster build walks the same path (config/bench_record, config/bench_replay).
class InputBench {
  public:
    enum class Mode : uint8_t { Off, Record, Replay };
    enum class Event : uint8_t { None, Started, Finished };

    // Frames between the warp's arrival and frame 0 of the walk: the scene
    // settles (camera, actors) the same way before either run.
    static constexpr uint64_t kSettleFrames = 30U;

    InputBench() = default;
    explicit InputBench(Mode mode, NativeA32InputTimeline replay = {})
        : mMode(mode), mReplay(std::move(replay)) {}

    Mode GetMode() const { return mMode; }
    bool Running() const { return mStart.has_value() && !mFinished; }
    // Frame of the walk now running (0 at its start).
    uint32_t WalkFrame() const { return mWalkFrame; }
    const NativeA32InputRecorder& Recording() const { return mRecorder; }

    // A replay gets itself from the title screen into the game: A or Start
    // held kTitlePressFrames every kTitlePressPeriod frames (title, file 1,
    // confirm), so nothing outside the game has to press them.
    static constexpr uint64_t kTitlePressPeriod = 40U;
    static constexpr uint64_t kTitlePressFrames = 3U;

    // Once per guest frame, before the game reads `input`. `gameplay`: the
    // player controls the game (not title or file menu); `arrived`: the boot
    // warp has landed; `stop`: end a recording (the debug menu opened).
    Event Step(uint64_t guestFrame, bool gameplay, bool arrived, bool stop, NativeA32InputFrame& input) {
        if (mMode == Mode::Off || mFinished) return Event::None;
        if (!mArrival.has_value()) {
            if (mMode == Mode::Replay && !gameplay) PressThroughTitle(guestFrame, input);
            if (!arrived) return Event::None;
            mArrival = guestFrame;
        }
        // Settling: a replay keeps the player's hands off the scene too.
        if (guestFrame < *mArrival + kSettleFrames) {
            if (mMode == Mode::Replay) Neutral(input);
            return Event::None;
        }
        Event event = Event::None;
        if (!mStart.has_value()) {
            mStart = guestFrame;
            event = Event::Started;
        }
        mWalkFrame = static_cast<uint32_t>(guestFrame - *mStart);
        if (mMode == Mode::Record) {
            if (stop && mRecorder.SegmentCount() != 0U) {
                mFinished = true;
                return Event::Finished;
            }
            mRecorder.Observe(mWalkFrame, input);
            return event;
        }
        if (mWalkFrame >= mReplay.EndFrame()) {
            mFinished = true;
            return Event::Finished;
        }
        const bool exit = input.Exit;
        input = mReplay.Sample(mWalkFrame, mWalkFrame);
        input.Exit = exit;
        return event;
    }

  private:
    static void PressThroughTitle(uint64_t guestFrame, NativeA32InputFrame& input) {
        if (guestFrame % kTitlePressPeriod >= kTitlePressFrames) return;
        const bool start = (guestFrame / kTitlePressPeriod) % 4U == 0U;
        input.Hid.Buttons |= NativeA32HidButtonMask(start ? NativeA32HidButton::Start : NativeA32HidButton::A);
    }

    static void Neutral(NativeA32InputFrame& input) {
        const bool exit = input.Exit;
        input = {};
        input.Exit = exit;
    }

    Mode mMode = Mode::Off;
    NativeA32InputTimeline mReplay;
    NativeA32InputRecorder mRecorder;
    std::optional<uint64_t> mArrival;
    std::optional<uint64_t> mStart;
    uint32_t mWalkFrame = 0;
    bool mFinished = false;
};

} // namespace Oot3dNativeGame
