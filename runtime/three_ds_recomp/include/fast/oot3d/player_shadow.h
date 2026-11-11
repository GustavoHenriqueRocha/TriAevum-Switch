#pragma once
// "Simple shadow" (L3+R3 > Game > Gameplay, config/simple_shadow; issue #8):
// Link's body flattened onto the floor under him in place of his two foot
// spots, as "OoT3D: Single Screen Experience" 3.2.2 by M-1 shows it. The game
// side (oot3d_simple_shadow.h) publishes, per native frame, his actor, the
// floor under him and the shadow's opacity; the deko3d backend draws his
// opaque meshes of the frame a second time, flattened along the scene's light
// (gfx_deko3d_player_shadow.cpp). The mod renders his silhouette into a small
// texture laid on the floor; here the meshes themselves are projected, so the
// edges are sharp. Pure math and bookkeeping, unit-tested on the host
// (oot3d_player_shadow_tests).

#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_frame_timing.h"
#include "fast/renderer3ds/pica_light_binding.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <span>

namespace Fast::Oot3d {

struct PlayerShadowFrame {
    // Link's actor; his draws carry (actor id 0 << 32) | address as their
    // NativeActorIdentity.
    uint32_t Actor = 0;
    // A point of the floor under him and its unit normal (world).
    std::array<float, 3> FloorPoint{};
    std::array<float, 3> FloorNormal{0.0F, 1.0F, 0.0F};
    // His height above that floor.
    float Height = 0.0F;
    // 0..1.
    float Opacity = 0.0F;

    bool operator==(const PlayerShadowFrame&) const = default;
};

[[nodiscard]] constexpr uint64_t PlayerShadowActorIdentity(uint32_t actor) noexcept { return actor; }

// The mod's rules (EUR 3.2.2 payload 0x005FDA54): lights weaker than 0.02 do not
// count, a strongest light under 0.35 tilts the shadow toward the vertical, and
// so does a rise from 40 to 140 units (a smoothstep, 0x005F96F8). Below the
// minimum elevation the projected body would stretch to the horizon; the mod
// has no bound, this is the port's.
inline constexpr float kPlayerShadowLightFloor = 0.02F;
inline constexpr float kPlayerShadowLightFull = 0.35F;
inline constexpr float kPlayerShadowRiseStart = 40.0F;
inline constexpr float kPlayerShadowRiseLength = 100.0F;
inline constexpr float kPlayerShadowMinimumElevation = 0.3F;
// The flattened body sits this far above the floor, against depth fighting.
inline constexpr float kPlayerShadowLift = 0.75F;
// The mod's darkness at its default 100% (0x005F9E58), and its final factor.
inline constexpr float kPlayerShadowBaseAlpha = 180.0F;
inline constexpr float kPlayerShadowAlphaScale = 0.8F;

namespace PlayerShadowDetail {
using Vec3 = std::array<float, 3>;
[[nodiscard]] inline float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
[[nodiscard]] inline std::optional<Vec3> Normalized(const Vec3& v) noexcept {
    const float length = std::sqrt(Dot(v, v));
    if (!std::isfinite(length) || length <= 1.0e-6F) return std::nullopt;
    return Vec3{v[0] / length, v[1] / length, v[2] / length};
}
// The mod's tilt: x and z scaled by 1 - k, y moved toward 1 by k, normalized.
[[nodiscard]] inline Vec3 TiltToward(const Vec3& direction, const Vec3& up, float k) noexcept {
    const Vec3 mixed{direction[0] * (1.0F - k) + up[0] * k, direction[1] * (1.0F - k) + up[1] * k,
                     direction[2] * (1.0F - k) + up[2] * k};
    return Normalized(mixed).value_or(up);
}
[[nodiscard]] inline bool Finite(std::span<const float> values) noexcept {
    return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
}
using Mat4 = std::array<float, 16>; // row-major, column vectors
[[nodiscard]] inline Mat4 Multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 out{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column) {
            float sum = 0.0F;
            for (size_t k = 0; k < 4U; ++k) sum += a[row * 4U + k] * b[k * 4U + column];
            out[row * 4U + column] = sum;
        }
    return out;
}
// The inverse of an affine matrix (last row 0 0 0 1).
[[nodiscard]] inline std::optional<Mat4> InverseAffine(const Mat4& m) noexcept {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float c00 = e * i - f * h, c01 = c * h - b * i, c02 = b * f - c * e;
    const float c10 = f * g - d * i, c11 = a * i - c * g, c12 = c * d - a * f;
    const float c20 = d * h - e * g, c21 = b * g - a * h, c22 = a * e - b * d;
    const float determinant = a * c00 + b * c10 + c * c20;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-12F) return std::nullopt;
    const float s = 1.0F / determinant;
    Mat4 out{c00 * s, c01 * s, c02 * s, 0.0F, c10 * s, c11 * s, c12 * s, 0.0F,
             c20 * s, c21 * s, c22 * s, 0.0F, 0.0F,    0.0F,    0.0F,    1.0F};
    for (size_t row = 0; row < 3U; ++row)
        out[row * 4U + 3U] = -(out[row * 4U] * m[3] + out[row * 4U + 1U] * m[7] + out[row * 4U + 2U] * m[11]);
    return out;
}
} // namespace PlayerShadowDetail

[[nodiscard]] inline float PlayerShadowRise(float height) noexcept {
    const float t = std::clamp((height - kPlayerShadowRiseStart) / kPlayerShadowRiseLength, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Toward the light (world, unit), from the scene's directional lights.
[[nodiscard]] inline std::array<float, 3> PlayerShadowLightDirection(
    const Renderer3ds::PicaSceneDirectionalLights* lights, const std::array<float, 3>& floorNormal,
    float height) noexcept {
    using namespace PlayerShadowDetail;
    const Vec3 up = lights != nullptr ? Normalized(lights->WorldUp).value_or(Vec3{0.0F, 1.0F, 0.0F})
                                      : Vec3{0.0F, 1.0F, 0.0F};
    Vec3 sum{};
    float strongest = 0.0F;
    if (lights != nullptr) {
        for (uint32_t index = 0; index < std::min(lights->Count, Renderer3ds::kMaximumPicaSceneDirectionalLights);
             ++index) {
            const auto& light = lights->Lights[index];
            const float weight = Renderer3ds::PicaSceneDirectionalEnergy(light);
            const auto direction = Renderer3ds::PicaSceneDirectionalDirection(light);
            if (!direction || !(weight >= kPlayerShadowLightFloor)) continue;
            for (size_t axis = 0; axis < 3U; ++axis) sum[axis] += weight * (*direction)[axis];
            strongest = std::max(strongest, weight);
        }
    }
    Vec3 direction = Normalized(sum).value_or(up);
    if (strongest < kPlayerShadowLightFull)
        direction = TiltToward(direction, up, (kPlayerShadowLightFull - strongest) / kPlayerShadowLightFull);
    if (const float rise = PlayerShadowRise(height); rise > 0.0F) direction = TiltToward(direction, up, rise);
    const Vec3 normal = Normalized(floorNormal).value_or(up);
    const float elevation = Dot(direction, normal);
    if (elevation < kPlayerShadowMinimumElevation) {
        const Vec3 along{direction[0] - elevation * normal[0], direction[1] - elevation * normal[1],
                         direction[2] - elevation * normal[2]};
        const auto horizontal = Normalized(along);
        if (!horizontal) return normal;
        const float level = std::sqrt(1.0F - kPlayerShadowMinimumElevation * kPlayerShadowMinimumElevation);
        for (size_t axis = 0; axis < 3U; ++axis)
            direction[axis] = kPlayerShadowMinimumElevation * normal[axis] + level * (*horizontal)[axis];
    }
    return direction;
}

// Row-major 4x4 (column vectors): a world point goes along `towardLight` to
// the floor plane lifted by kPlayerShadowLift. Points of the plane stay put.
[[nodiscard]] inline std::array<float, 16> PlayerShadowProjection(const PlayerShadowFrame& frame,
                                                                  const std::array<float, 3>& towardLight) noexcept {
    using namespace PlayerShadowDetail;
    const Vec3 n = Normalized(frame.FloorNormal).value_or(Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 point{frame.FloorPoint[0] + n[0] * kPlayerShadowLift, frame.FloorPoint[1] + n[1] * kPlayerShadowLift,
                     frame.FloorPoint[2] + n[2] * kPlayerShadowLift};
    const float nd = std::max(Dot(n, towardLight), 1.0e-3F);
    const float np = Dot(n, point);
    Mat4 out{};
    for (size_t row = 0; row < 3U; ++row) {
        for (size_t column = 0; column < 3U; ++column)
            out[row * 4U + column] = (row == column ? 1.0F : 0.0F) - towardLight[row] * n[column] / nd;
        out[row * 4U + 3U] = towardLight[row] * np / nd;
    }
    out[15] = 1.0F;
    return out;
}

// The PICA vertex programs of the title map a vertex through the model or
// bone rows (f[20..]), the view rows f[4..6] and the projection f[0..3].
inline constexpr size_t kPlayerShadowViewRow = 4U;

// Replaces f[4..6] (rows of F) with the rows of V·S·V⁻¹·F, V the draw's
// world-to-view (column-major, as captured with the geometry): the vertex
// lands where `projection` puts it in world space, whichever way the title
// split the view between those rows and the bones. False (bytes untouched)
// when the block is short, V is not invertible or a value is not finite.
[[nodiscard]] inline bool PatchPlayerShadowViewRows(std::span<uint8_t> uniforms,
                                                    const std::array<float, 16>& worldToViewColumnMajor,
                                                    const std::array<float, 16>& projection) noexcept {
    using namespace PlayerShadowDetail;
    const size_t offset = kPicaPackedVertexFloatOffset + kPlayerShadowViewRow * kPicaPackedVec4Bytes;
    if (uniforms.size() < offset + 3U * kPicaPackedVec4Bytes) return false;
    Mat4 view{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column)
            view[row * 4U + column] = worldToViewColumnMajor[column * 4U + row];
    if (!Finite(view) || std::abs(view[12]) > 1.0e-4F || std::abs(view[13]) > 1.0e-4F ||
        std::abs(view[14]) > 1.0e-4F || std::abs(view[15] - 1.0F) > 1.0e-4F)
        return false;
    const auto inverse = InverseAffine(view);
    if (!inverse) return false;
    Mat4 rows{};
    std::memcpy(rows.data(), uniforms.data() + offset, 12U * sizeof(float));
    rows[15] = 1.0F;
    const Mat4 patched = Multiply(Multiply(Multiply(view, projection), *inverse), rows);
    if (!Finite(patched)) return false;
    std::memcpy(uniforms.data() + offset, patched.data(), 12U * sizeof(float));
    return true;
}

// The mod's opacity (0x005FDC4C..0x005FDCD4) for the actor's shadow alpha at
// `height`: 180 at its default darkness, the game's round-shadow fade over
// 150 of 350 units that the rise undoes, a near fade over 60 units that the
// rise also takes over, times 0.8; truncated as the payload truncates.
[[nodiscard]] inline float PlayerShadowOpacity(uint8_t shadowAlpha, float height) noexcept {
    const float rise = PlayerShadowRise(height);
    const float fade = 1.0F - std::clamp(height, 0.0F, 150.0F) / 350.0F;
    const float lift = fade + (1.0F - fade) * rise;
    const int alpha = static_cast<int>(static_cast<float>(shadowAlpha) / 255.0F * lift * kPlayerShadowBaseAlpha);
    const float near = std::max(1.0F - std::max(height, 0.0F) / 60.0F, rise);
    if (!(near > 0.0F)) return 0.0F;
    return static_cast<float>(static_cast<int>(static_cast<float>(alpha) * kPlayerShadowAlphaScale * near)) / 255.0F;
}

// Native frames (game thread) to the sample being drawn (render thread), as
// GrassInteractionBridge carries the grass colliders: a 60 FPS in-between
// frame blends the floor and opacity of its two native frames.
class PlayerShadowBridge final {
  public:
    static PlayerShadowBridge& Instance() {
        static PlayerShadowBridge bridge;
        return bridge;
    }
    void PublishFrame(uint64_t sourceFrame, const std::optional<PlayerShadowFrame>& shadow) {
        std::scoped_lock lock(mMutex);
        if (!mFrames.empty() && sourceFrame <= mFrames.back().Id) {
            if (sourceFrame == mFrames.back().Id) return;
            mFrames.clear(); // a reset (state load, scene restart)
        }
        mFrames.push_back({sourceFrame, shadow});
        while (mFrames.size() > 4U) mFrames.pop_front();
    }
    void SelectSample(const Renderer3ds::PicaFrameTemporalSample& sample) {
        std::scoped_lock lock(mMutex);
        const auto find = [&](uint64_t id) -> const std::optional<PlayerShadowFrame>* {
            for (const auto& frame : mFrames)
                if (frame.Id == id) return &frame.Shadow;
            return nullptr;
        };
        const auto* current = find(sample.CurrentSourceFrameId);
        mSelected = current != nullptr ? *current : std::nullopt;
        if (!mSelected || sample.HistoryReset || !(sample.Alpha > 0.0F && sample.Alpha < 1.0F)) return;
        const auto* previous = find(sample.PreviousSourceFrameId);
        if (previous == nullptr || !previous->has_value() || (*previous)->Actor != mSelected->Actor) return;
        const float t = static_cast<float>(sample.Alpha);
        const auto blend = [t](float from, float to) { return from + (to - from) * t; };
        PlayerShadowFrame mixed = *mSelected;
        for (size_t axis = 0; axis < 3U; ++axis) {
            mixed.FloorPoint[axis] = blend((*previous)->FloorPoint[axis], mSelected->FloorPoint[axis]);
            mixed.FloorNormal[axis] = blend((*previous)->FloorNormal[axis], mSelected->FloorNormal[axis]);
        }
        mixed.FloorNormal = PlayerShadowDetail::Normalized(mixed.FloorNormal).value_or(mSelected->FloorNormal);
        mixed.Height = blend((*previous)->Height, mSelected->Height);
        mixed.Opacity = blend((*previous)->Opacity, mSelected->Opacity);
        mSelected = mixed;
    }
    [[nodiscard]] std::optional<PlayerShadowFrame> Selected() const {
        std::scoped_lock lock(mMutex);
        return mSelected;
    }
    void Reset() {
        std::scoped_lock lock(mMutex);
        mFrames.clear();
        mSelected.reset();
    }
    // Set by a renderer that draws the shadow (deko3d, once its program is
    // loaded): until then the game keeps Link's own foot shadows.
    void SetRendererReady(bool ready) noexcept { mRendererReady.store(ready, std::memory_order_relaxed); }
    [[nodiscard]] bool RendererReady() const noexcept { return mRendererReady.load(std::memory_order_relaxed); }

  private:
    struct Entry {
        uint64_t Id = 0;
        std::optional<PlayerShadowFrame> Shadow;
    };
    mutable std::mutex mMutex;
    std::deque<Entry> mFrames;
    std::optional<PlayerShadowFrame> mSelected;
    std::atomic<bool> mRendererReady{false};
};

} // namespace Fast::Oot3d
