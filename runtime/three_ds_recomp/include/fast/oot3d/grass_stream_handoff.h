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
#include <unordered_map>
#include <utility>
#include <vector>

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
// same area (scene and room) and at least one of its placements (by
// placement identity, whatever target the frame draws into) is among this
// frame's. Each of its ranges is then drawn only if its own placement is
// this frame's (GrassFramePlacements::Find); the rest is dropped.
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

// This frame's placements: one per placement and render target it was drawn
// into (GrassFramePlacementKey), each with that draw's camera, also found by
// placement identity alone: a cull fits a frame that drew one of its
// placements into any target, and a range drawn into a target without its
// own copy of the placement uses another target's (a native replay).
template <typename Placement>
class GrassFramePlacements {
  public:
    void Clear() noexcept {
        mByKey.clear();
        mByIdentity.clear();
    }
    // The first entry per (identity, target) stays.
    void Add(uint64_t identity, uint64_t renderTargetNamespace, uint32_t colorAddress, Placement placement) {
        const uint64_t key = GrassFramePlacementKey(identity, renderTargetNamespace, colorAddress);
        if (mByKey.try_emplace(key, std::move(placement)).second) mByIdentity.try_emplace(identity, key);
    }
    [[nodiscard]] bool Has(uint64_t identity) const noexcept { return mByIdentity.contains(identity); }
    // The placement to draw `identity` with into this target: as drawn into
    // it this frame, else as drawn into another target this frame
    // (`otherTarget` set), else null.
    [[nodiscard]] Placement* Find(uint64_t identity, uint64_t renderTargetNamespace, uint32_t colorAddress,
                                  bool* otherTarget = nullptr) {
        if (otherTarget != nullptr) *otherTarget = false;
        if (const auto own = mByKey.find(GrassFramePlacementKey(identity, renderTargetNamespace, colorAddress));
            own != mByKey.end())
            return &own->second;
        const auto any = mByIdentity.find(identity);
        if (any == mByIdentity.end()) return nullptr;
        if (otherTarget != nullptr) *otherTarget = true;
        return &mByKey.at(any->second);
    }
    [[nodiscard]] bool Empty() const noexcept { return mByKey.empty(); }
    [[nodiscard]] auto begin() const noexcept { return mByKey.begin(); }
    [[nodiscard]] auto end() const noexcept { return mByKey.end(); }
    // These placements: `from`'s, each as `move` returns it (nullopt: left
    // out), under the same identities and targets.
    template <typename Move>
    void AssignMoved(const GrassFramePlacements& from, Move&& move) {
        Clear();
        for (const auto& [key, placement] : from.mByKey)
            if (auto moved = move(placement)) mByKey.emplace(key, std::move(*moved));
        for (const auto& [identity, key] : from.mByIdentity)
            if (mByKey.contains(key)) mByIdentity.emplace(identity, key);
    }

  private:
    std::unordered_map<uint64_t, Placement> mByKey;
    std::unordered_map<uint64_t, uint64_t> mByIdentity; // identity -> its first key
};

// Column-major 4x4 product a * b.
[[nodiscard]] inline std::array<float, 16> GrassMultiplyClip(const std::array<float, 16>& a,
                                                           const std::array<float, 16>& b) noexcept {
    std::array<float, 16> r{};
    for (size_t column = 0; column < 4U; ++column)
        for (size_t row = 0; row < 4U; ++row) {
            double sum = 0.0;
            for (size_t k = 0; k < 4U; ++k) sum += double{a[k * 4U + row]} * double{b[column * 4U + k]};
            r[column * 4U + row] = static_cast<float>(sum);
        }
    return r;
}

// The camera change between two model-to-clips of one rigid draw
// (GrassRigidModelToClip, column-major): now * inverse(before). Applied to a
// world-to-clip of `before`'s frame it gives the one of `now`'s frame,
// whatever the draw's model transform. Nullopt when `before` is singular.
[[nodiscard]] inline std::optional<std::array<float, 16>> GrassClipChange(const std::array<float, 16>& before,
                                                                          const std::array<float, 16>& now) noexcept {
    // Gauss-Jordan with partial pivoting, in double.
    std::array<std::array<double, 8>, 4> m{};
    for (size_t row = 0; row < 4U; ++row) {
        for (size_t column = 0; column < 4U; ++column) m[row][column] = before[column * 4U + row];
        m[row][4U + row] = 1.0;
    }
    for (size_t column = 0; column < 4U; ++column) {
        size_t pivot = column;
        for (size_t row = column + 1U; row < 4U; ++row)
            if (std::abs(m[row][column]) > std::abs(m[pivot][column])) pivot = row;
        if (!(std::abs(m[pivot][column]) > 1.0e-12)) return std::nullopt;
        std::swap(m[column], m[pivot]);
        const double scale = 1.0 / m[column][column];
        for (auto& value : m[column]) value *= scale;
        for (size_t row = 0; row < 4U; ++row) {
            if (row == column) continue;
            const double factor = m[row][column];
            for (size_t k = 0; k < 8U; ++k) m[row][k] -= factor * m[column][k];
        }
    }
    std::array<float, 16> inverse{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column) inverse[column * 4U + row] = static_cast<float>(m[row][4U + column]);
    auto change = GrassMultiplyClip(now, inverse);
    if (!std::all_of(change.begin(), change.end(), [](float v) { return std::isfinite(v); })) return std::nullopt;
    return change;
}

// fps60 in-between frames (the renderer's synthetic samples, strictly
// between two guest frames) draw the placements of the last real frame, each
// with the in-between frame's camera. The in-between frame's draw list and
// views are interpolated (carried and inserted unmatched draws,
// vertex-binding overrides, interpolated uniforms), so grass sources of its
// own came out with other placement identities than the real frame's: the
// cull of the real frame no longer fitted the in-between frame, nor the
// in-between's cull the next real frame (logs14g: 257/600 frames drawn).
// Instead, a real frame's source draws are remembered by draw key (the plan's
// geometry identity and target, which interpolation does not change) with
// their model-to-clip; the same draw in an in-between frame gives its
// target's camera change (GrassClipChange).
class GrassInBetweenCameras {
  public:
    // A grass source draw of the real frame being drawn.
    void RealSource(uint64_t drawKey, const std::array<float, 16>& modelToClip) {
        mPending.try_emplace(drawKey, modelToClip);
    }
    // The end of real frame `frame`: its sources become the reference of
    // the in-between frames that follow it.
    void EndRealFrame(uint64_t frame) {
        mReference.swap(mPending);
        mPending.clear();
        mReferenceFrame = frame;
        mChanges.clear();
    }
    // A draw of in-between frame `frame` into `target`: if it is one of the
    // reference frame's sources and `target` has no camera change yet, the
    // change is taken from it. True when `target` has one now.
    bool InBetweenDraw(uint64_t frame, uint64_t drawKey, uint64_t target, std::span<const uint8_t> vertexUniforms,
                       bool framebufferFlipped) {
        if (mChangesFrame != frame) {
            mChangesFrame = frame;
            mChanges.clear();
        }
        if (Change(frame, target) != nullptr) return true;
        const auto reference = mReference.find(drawKey);
        if (reference == mReference.end()) return false;
        const auto now = GrassRigidModelToClip(vertexUniforms, framebufferFlipped);
        if (!now) return false;
        const auto change = GrassClipChange(reference->second, *now);
        if (!change) return false;
        mChanges.emplace_back(target, *change);
        return true;
    }
    // The camera change of `target` in in-between frame `frame` (null: none
    // of the reference frame's sources was drawn into it yet).
    [[nodiscard]] const std::array<float, 16>* Change(uint64_t frame, uint64_t target) const noexcept {
        if (mChangesFrame != frame) return nullptr;
        for (const auto& [key, change] : mChanges)
            if (key == target) return &change;
        return nullptr;
    }
    // The real frame the reference sources were drawn in (~0 before any).
    [[nodiscard]] uint64_t ReferenceFrame() const noexcept { return mReferenceFrame; }
    void Clear() {
        mPending.clear();
        mReference.clear();
        mChanges.clear();
        mReferenceFrame = ~0ULL;
        mChangesFrame = ~0ULL;
    }

  private:
    std::unordered_map<uint64_t, std::array<float, 16>> mPending, mReference;
    std::vector<std::pair<uint64_t, std::array<float, 16>>> mChanges; // per target, this in-between frame
    uint64_t mReferenceFrame = ~0ULL;
    uint64_t mChangesFrame = ~0ULL;
};
// In-between frames that may follow one real frame (Fixed3x: two).
inline constexpr uint64_t kGrassInBetweenReach = 3U;

} // namespace Fast::Oot3d
