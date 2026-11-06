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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
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
    // Forgets the drawn cull (another area, or none of its placements left):
    // nothing is drawn until a new cull finishes. Slots read by frames in
    // flight stay protected.
    void Reset() noexcept {
        mCurrent.reset();
        mFresh = false;
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

// Whether a finished cull may be drawn in this frame: it was culled in the
// same area (scene and room) and at least one of its placements is among
// this frame's. Each of its ranges is then drawn only if its own placement
// is this frame's (GrassFramePlacementKey); the rest is dropped.
enum class GrassCullFit : uint8_t { Draw, OtherArea, NoPlacement };
template <typename Identities, typename Contains>
[[nodiscard]] GrassCullFit GrassCullFitsFrame(uint32_t cullArea, uint32_t frameArea, const Identities& cullIdentities,
                                              Contains&& frameHas) {
    if (cullArea != frameArea) return GrassCullFit::OtherArea;
    for (const auto identity : cullIdentities)
        if (frameHas(identity)) return GrassCullFit::Draw;
    return GrassCullFit::NoPlacement;
}
// Scene (s16) and room (s8) as one value; 0xffffffff outside gameplay.
[[nodiscard]] constexpr uint32_t GrassArea(int scene, int room) noexcept {
    return scene < 0 ? 0xffffffffU : (static_cast<uint32_t>(scene) << 8U) | static_cast<uint8_t>(room);
}

// The model-to-clip (column-major) of a rigid OoT3D CMB draw from its packed
// vertex uniforms (projection f[0..3], view f[4..6], model f[20..22]), y
// negated for a flipped framebuffer. A grass source keeps the world
// transform its placement was made with and takes only this from each frame,
// in-between (interpolated) frames included: their light-binding camera is
// the step's, so a world transform derived from it drifts.
[[nodiscard]] inline std::optional<std::array<float, 16>> GrassRigidModelToClip(
    std::span<const uint8_t> vertexUniforms, bool framebufferFlipped) noexcept {
    constexpr size_t kFloats = 16U + 4U * 4U * sizeof(uint32_t);
    constexpr size_t kVec4 = 4U * sizeof(float);
    const auto at = [](size_t first) { return kFloats + first * kVec4; };
    if (vertexUniforms.size() < at(20U) + 3U * kVec4) return std::nullopt;
    std::array<float, 16> projection{}, view{}, model{};
    view[15] = model[15] = 1.0F;
    std::memcpy(projection.data(), vertexUniforms.data() + at(0U), 4U * kVec4);
    std::memcpy(view.data(), vertexUniforms.data() + at(4U), 3U * kVec4);
    std::memcpy(model.data(), vertexUniforms.data() + at(20U), 3U * kVec4);
    const auto multiply = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
        std::array<float, 16> r{};
        for (size_t i = 0; i < 4U; ++i)
            for (size_t j = 0; j < 4U; ++j)
                for (size_t k = 0; k < 4U; ++k) r[i * 4U + j] += a[i * 4U + k] * b[k * 4U + j];
        return r;
    };
    auto clip = multiply(projection, multiply(view, model));
    if (!std::all_of(clip.begin(), clip.end(), [](float v) { return std::isfinite(v); })) return std::nullopt;
    if (framebufferFlipped)
        for (size_t column = 0; column < 4U; ++column) clip[4U + column] *= -1.0F;
    std::array<float, 16> columnMajor{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column) columnMajor[column * 4U + row] = clip[row * 4U + column];
    return columnMajor;
}

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
