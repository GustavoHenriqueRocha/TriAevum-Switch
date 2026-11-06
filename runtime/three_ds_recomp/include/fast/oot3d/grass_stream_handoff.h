#pragma once

// Hand-off of the grass cull results from the cull worker to the frames that
// draw them (deko3d, gfx_deko3d_grass.cpp). Pure bookkeeping, unit-tested on
// the host (oot3d_grass_switch_tests).
//
// The worker writes a cull into one of a few GPU-visible slots. A finished
// cull becomes the result every later frame draws until a newer one
// finishes: a late worker means an older camera's selection, never a frame
// without grass. A slot is handed to the worker only when no frame still in
// flight on the GPU reads it, it is not the drawn result and it is not being
// written.

#include <array>
#include <cstdint>
#include <optional>

namespace Fast::Oot3d {

class GrassStreamSlots {
  public:
    static constexpr uint32_t kSlots = 4U;

    explicit GrassStreamSlots(uint32_t framesInFlight) noexcept : mFramesInFlight(framesInFlight) {}

    // A slot for the next cull, or nullopt (the cull waits a frame).
    [[nodiscard]] std::optional<uint32_t> BeginWrite(uint64_t frame) noexcept {
        if (mWriting) return std::nullopt;
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            if (mCurrent && *mCurrent == slot) continue;
            if (mRead[slot] && mReadFrame[slot] + mFramesInFlight > frame) continue;
            mWriting = slot;
            return slot;
        }
        return std::nullopt;
    }
    // The cull written into `slot` for the camera of `cullFrame` finished.
    void FinishWrite(uint32_t slot, uint64_t cullFrame) noexcept {
        if (!mWriting || *mWriting != slot) return;
        mWriting.reset();
        mCurrent = slot;
        mCurrentFrame = cullFrame;
        mFresh = true;
        mRead[slot] = false;
    }
    // The cull `frame` draws: the newest finished one (nullopt only before
    // the first). Marks it read by `frame`.
    [[nodiscard]] std::optional<uint32_t> Draw(uint64_t frame) noexcept {
        if (!mCurrent) return std::nullopt;
        mRead[*mCurrent] = true;
        mReadFrame[*mCurrent] = frame;
        if (mFresh) ++FreshFrames;
        else ++StaleFrames;
        mFresh = false;
        return mCurrent;
    }
    [[nodiscard]] std::optional<uint32_t> Current() const noexcept { return mCurrent; }
    [[nodiscard]] std::optional<uint32_t> Writing() const noexcept { return mWriting; }
    // The frame whose camera the drawn cull used.
    [[nodiscard]] uint64_t CurrentFrame() const noexcept { return mCurrentFrame; }

    // Frames that drew a cull no frame had drawn before / one already drawn
    // (no new cull finished since: a late or skipped worker).
    uint64_t FreshFrames = 0;
    uint64_t StaleFrames = 0;

  private:
    uint32_t mFramesInFlight = 2U;
    std::optional<uint32_t> mCurrent;
    std::optional<uint32_t> mWriting;
    uint64_t mCurrentFrame = 0;
    bool mFresh = false;
    std::array<bool, kSlots> mRead{};
    std::array<uint64_t, kSlots> mReadFrame{};
};

// One placement as drawn into one render target this frame: the same
// placement can be drawn into several targets (a native replay, a pause
// page), each with its own camera.
[[nodiscard]] constexpr uint64_t GrassFramePlacementKey(uint64_t identity, uint64_t renderTargetNamespace,
                                                        uint32_t colorAddress) noexcept {
    uint64_t key = identity;
    key ^= renderTargetNamespace + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
    key ^= uint64_t{colorAddress} + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
    return key;
}

} // namespace Fast::Oot3d
