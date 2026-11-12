#pragma once
// "Simple shadow" (L3+R3 > Video, config/simple_shadow; issue #8): Link's
// body flattened onto the floor under him in place of his two foot spots, as
// "OoT3D: Single Screen Experience" 3.2.2 by M-1 shows it. The game side
// (oot3d_simple_shadow.h) publishes, per native frame, his actor, the floor
// under him and the shadow's opacity; the deko3d backend draws his opaque
// meshes of the frame a second time, flattened along the scene's light
// (gfx_deko3d_player_shadow.cpp), one of four ways (PlayerShadowMethod). The
// mod renders his silhouette into a small texture laid on the floor (method
// Texture); the others draw the flattened meshes themselves, so their edges
// are sharp. Pure math and bookkeeping, unit-tested on the host
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
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

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

// How the deko3d backend draws it (config/simple_shadow_method, L3+R3 > Video
// > Shadow method; a 0.2.3 test, each method's GPU time in the dkperf lines'
// shadow_<word>_ms):
// - TwoPass: the meshes darken where the top stencil bit is clear and set it,
//   then the same meshes without colour zero it (0.2.3 build 33);
// - OnePass: the meshes darken without the stencil, overlapping limbs darker;
// - Cover: the meshes only set the bit, inside the screen rectangle of the
//   floor region (PlayerShadowFloorRegion); one triangle over that rectangle
//   darkens where it is set and zeroes it;
// - Texture: the meshes go flat into a silhouette texture of the floor region,
//   which one quad on the floor samples (the mod's way: soft edges).
enum class PlayerShadowMethod : uint8_t { TwoPass, OnePass, Cover, Texture };
inline constexpr size_t kPlayerShadowMethodCount = 4U;

// Its word in config/simple_shadow_method and in the dkperf keys.
[[nodiscard]] constexpr std::string_view PlayerShadowMethodWord(PlayerShadowMethod method) noexcept {
    switch (method) {
    case PlayerShadowMethod::TwoPass: return "two_pass";
    case PlayerShadowMethod::OnePass: return "one_pass";
    case PlayerShadowMethod::Cover: return "cover";
    case PlayerShadowMethod::Texture: return "texture";
    }
    return "two_pass";
}

// The file's first word; anything else (no file included) is TwoPass.
[[nodiscard]] constexpr PlayerShadowMethod ParsePlayerShadowMethod(std::string_view text) noexcept {
    constexpr std::string_view blank = " \t\r\n";
    const size_t first = text.find_first_not_of(blank);
    if (first == std::string_view::npos) return PlayerShadowMethod::TwoPass;
    text.remove_prefix(first);
    text = text.substr(0, text.find_first_of(blank));
    for (size_t index = 0; index < kPlayerShadowMethodCount; ++index) {
        const auto method = static_cast<PlayerShadowMethod>(index);
        if (text == PlayerShadowMethodWord(method)) return method;
    }
    return PlayerShadowMethod::TwoPass;
}

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
[[nodiscard]] inline Vec3 Cross(const Vec3& a, const Vec3& b) noexcept {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
// m · (v, 1).
[[nodiscard]] inline std::array<float, 4> Transform(const Mat4& m, const Vec3& v) noexcept {
    std::array<float, 4> out{};
    for (size_t row = 0; row < 4U; ++row)
        out[row] = m[row * 4U] * v[0] + m[row * 4U + 1U] * v[1] + m[row * 4U + 2U] * v[2] + m[row * 4U + 3U];
    return out;
}
// A draw's world-to-view (column-major) as rows, if finite and affine (last
// row 0 0 0 1).
[[nodiscard]] inline std::optional<Mat4> AffineView(const std::array<float, 16>& columnMajor) noexcept {
    Mat4 view{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column) view[row * 4U + column] = columnMajor[column * 4U + row];
    if (!Finite(view) || std::abs(view[12]) > 1.0e-4F || std::abs(view[13]) > 1.0e-4F ||
        std::abs(view[14]) > 1.0e-4F || std::abs(view[15] - 1.0F) > 1.0e-4F)
        return std::nullopt;
    return view;
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
    const auto view = AffineView(worldToViewColumnMajor);
    if (!view) return false;
    const auto inverse = InverseAffine(*view);
    if (!inverse) return false;
    Mat4 rows{};
    std::memcpy(rows.data(), uniforms.data() + offset, 12U * sizeof(float));
    rows[15] = 1.0F;
    const Mat4 patched = Multiply(Multiply(Multiply(*view, projection), *inverse), rows);
    if (!Finite(patched)) return false;
    std::memcpy(uniforms.data() + offset, patched.data(), 12U * sizeof(float));
    return true;
}

// Methods Cover and Texture: the rectangle of the shadow plane that his
// flattened body can cover, the box about his position (kPlayerShadowBodyRadius
// around him, kPlayerShadowBodyAbove over it and kPlayerShadowBodyBelow under
// it, hanging from a ledge, but not under the floor, which would only stretch
// the region toward the light) projected along the light. A pose leaving the
// box is cut at its edge.
inline constexpr float kPlayerShadowBodyRadius = 60.0F;
inline constexpr float kPlayerShadowBodyBelow = 60.0F;
inline constexpr float kPlayerShadowBodyAbove = 110.0F;

struct PlayerShadowRegion {
    std::array<float, 3> Origin{}; // on the shadow plane: the floor point, lifted
    std::array<float, 3> Along{};  // unit, in the plane: the way the shadow falls
    std::array<float, 3> Across{}; // unit, in the plane
    std::array<float, 2> AlongRange{}; // from Origin: first, last
    std::array<float, 2> AcrossRange{};

    // (first, first), (last, first), (first, last), (last, last) along and
    // across: a triangle strip, whose silhouette coordinates are 0 and 1 in
    // that order.
    [[nodiscard]] std::array<std::array<float, 3>, 4> Corners() const noexcept {
        std::array<std::array<float, 3>, 4> corners{};
        for (size_t corner = 0; corner < 4U; ++corner) {
            const float along = AlongRange[corner & 1U];
            const float across = AcrossRange[(corner >> 1U) & 1U];
            for (size_t axis = 0; axis < 3U; ++axis)
                corners[corner][axis] = Origin[axis] + along * Along[axis] + across * Across[axis];
        }
        return corners;
    }
};

[[nodiscard]] inline std::optional<PlayerShadowRegion> PlayerShadowFloorRegion(
    const PlayerShadowFrame& frame, const std::array<float, 3>& towardLight) noexcept {
    using namespace PlayerShadowDetail;
    const Vec3 n = Normalized(frame.FloorNormal).value_or(Vec3{0.0F, 1.0F, 0.0F});
    PlayerShadowRegion region;
    for (size_t axis = 0; axis < 3U; ++axis)
        region.Origin[axis] = frame.FloorPoint[axis] + n[axis] * kPlayerShadowLift;
    // Away from the light, in the plane; under a light straight above, any
    // direction of the plane.
    const float rise = Dot(towardLight, n);
    auto along = Normalized(
        Vec3{rise * n[0] - towardLight[0], rise * n[1] - towardLight[1], rise * n[2] - towardLight[2]});
    if (!along) {
        const Vec3 axis = std::abs(n[0]) < 0.9F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 0.0F, 1.0F};
        const float d = Dot(axis, n);
        along = Normalized(Vec3{axis[0] - d * n[0], axis[1] - d * n[1], axis[2] - d * n[2]});
        if (!along) return std::nullopt;
    }
    region.Along = *along;
    region.Across = Cross(n, *along);
    const Mat4 projection = PlayerShadowProjection(frame, towardLight);
    const Vec3 position{frame.FloorPoint[0], frame.FloorPoint[1] + frame.Height, frame.FloorPoint[2]};
    const float bottom = std::max(position[1] - kPlayerShadowBodyBelow, frame.FloorPoint[1]);
    region.AlongRange = {std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    region.AcrossRange = region.AlongRange;
    for (uint32_t corner = 0; corner < 8U; ++corner) {
        const Vec3 point{position[0] + ((corner & 1U) != 0U ? kPlayerShadowBodyRadius : -kPlayerShadowBodyRadius),
                         (corner & 2U) != 0U ? position[1] + kPlayerShadowBodyAbove : bottom,
                         position[2] + ((corner & 4U) != 0U ? kPlayerShadowBodyRadius : -kPlayerShadowBodyRadius)};
        const auto landed = Transform(projection, point);
        const Vec3 offset{landed[0] - region.Origin[0], landed[1] - region.Origin[1], landed[2] - region.Origin[2]};
        const float a = Dot(offset, region.Along);
        const float b = Dot(offset, region.Across);
        region.AlongRange = {std::min(region.AlongRange[0], a), std::max(region.AlongRange[1], a)};
        region.AcrossRange = {std::min(region.AcrossRange[0], b), std::max(region.AcrossRange[1], b)};
    }
    if (!Finite(region.AlongRange) || !Finite(region.AcrossRange) ||
        !(region.AlongRange[1] - region.AlongRange[0] >= 1.0F) ||
        !(region.AcrossRange[1] - region.AcrossRange[0] >= 1.0F))
        return std::nullopt;
    return region;
}

// The world-to-clip of his draws (rows): their projection rows f[0..3] times
// V (column-major, the draw's world-to-view), y negated where the block flips
// the viewport, as their vertex programs end; z is still PICA's (-w..0).
[[nodiscard]] inline std::optional<std::array<float, 16>> PlayerShadowWorldToClip(
    std::span<const uint8_t> uniforms, const std::array<float, 16>& worldToViewColumnMajor) noexcept {
    using namespace PlayerShadowDetail;
    if (uniforms.size() < kPicaPackedVertexFloatOffset + 4U * kPicaPackedVec4Bytes) return std::nullopt;
    const auto view = AffineView(worldToViewColumnMajor);
    if (!view) return std::nullopt;
    Mat4 projection{};
    std::memcpy(projection.data(), uniforms.data() + kPicaPackedVertexFloatOffset, sizeof(projection));
    Mat4 clip = Multiply(projection, *view);
    int32_t flip = 0;
    std::memcpy(&flip, uniforms.data() + kPicaPackedVertexFlipViewportOffset, sizeof(flip));
    if (flip != 0)
        for (size_t column = 0; column < 4U; ++column) clip[4U + column] = -clip[4U + column];
    if (!Finite(clip)) return std::nullopt;
    return clip;
}

// gl_Position of a world point through it, with the deko3d dialect's depth
// (z' = -2z - w, TranslatePicaShaderToOpenGl43).
[[nodiscard]] inline std::array<float, 4> PlayerShadowGlPosition(const std::array<float, 16>& worldToClip,
                                                                 const std::array<float, 3>& world) noexcept {
    const auto clip = PlayerShadowDetail::Transform(worldToClip, world);
    return {clip[0], clip[1], -2.0F * clip[2] - clip[3], clip[3]};
}

// Method Cover: the target pixels (x, y, width, height) that the region spans
// through his camera and viewport (x, y, width, height), with a pixel of
// margin; `minusOneFirst`: NDC y = -1 is the viewport's first row (deko3d's
// NegativeY swizzle). Nothing when a corner is not in front of the camera:
// the caller covers his draws' whole scissor then.
[[nodiscard]] inline std::optional<std::array<int32_t, 4>> PlayerShadowScreenRect(
    const std::array<float, 16>& worldToClip, const PlayerShadowRegion& region, const std::array<float, 4>& viewport,
    bool minusOneFirst) noexcept {
    std::array<float, 2> columns{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
    std::array<float, 2> rows = columns;
    for (const auto& corner : region.Corners()) {
        const auto clip = PlayerShadowDetail::Transform(worldToClip, corner);
        if (!(clip[3] > 1.0e-3F)) return std::nullopt;
        const float column = viewport[0] + (clip[0] / clip[3] + 1.0F) * 0.5F * viewport[2];
        const float y = clip[1] / clip[3];
        const float row = viewport[1] + (minusOneFirst ? y + 1.0F : 1.0F - y) * 0.5F * viewport[3];
        columns = {std::min(columns[0], column), std::max(columns[1], column)};
        rows = {std::min(rows[0], row), std::max(rows[1], row)};
    }
    if (!PlayerShadowDetail::Finite(columns) || !PlayerShadowDetail::Finite(rows)) return std::nullopt;
    // Far outside the target is as good as at its edge, and fits an int.
    const auto bound = [](float value) { return std::clamp(value, -1.0e6F, 1.0e6F); };
    const auto x0 = static_cast<int32_t>(std::floor(bound(columns[0]))) - 1;
    const auto y0 = static_cast<int32_t>(std::floor(bound(rows[0]))) - 1;
    const auto x1 = static_cast<int32_t>(std::ceil(bound(columns[1]))) + 1;
    const auto y1 = static_cast<int32_t>(std::ceil(bound(rows[1]))) + 1;
    return std::array<int32_t, 4>{x0, y0, x1 - x0, y1 - y0};
}

// Method Texture, after PatchPlayerShadowViewRows: projection rows f[0..3] of
// T·V⁻¹, T taking the region to x, y = -1..1 (along, across), z = -w/2 (PICA
// depth, halfway) and w = 1, and the viewport flip off: each flattened vertex
// lands at its place in the silhouette. False (bytes untouched) when the
// block is short or V is not an invertible affine.
[[nodiscard]] inline bool PatchPlayerShadowSilhouetteRows(std::span<uint8_t> uniforms,
                                                          const std::array<float, 16>& worldToViewColumnMajor,
                                                          const PlayerShadowRegion& region) noexcept {
    using namespace PlayerShadowDetail;
    if (uniforms.size() < kPicaPackedVertexFloatOffset + 4U * kPicaPackedVec4Bytes) return false;
    const auto view = AffineView(worldToViewColumnMajor);
    if (!view) return false;
    const auto inverse = InverseAffine(*view);
    if (!inverse) return false;
    const float alongScale = 2.0F / (region.AlongRange[1] - region.AlongRange[0]);
    const float acrossScale = 2.0F / (region.AcrossRange[1] - region.AcrossRange[0]);
    Mat4 toSilhouette{};
    for (size_t axis = 0; axis < 3U; ++axis) {
        toSilhouette[axis] = alongScale * region.Along[axis];
        toSilhouette[4U + axis] = acrossScale * region.Across[axis];
    }
    toSilhouette[3] = -alongScale * (Dot(region.Along, region.Origin) + region.AlongRange[0]) - 1.0F;
    toSilhouette[7] = -acrossScale * (Dot(region.Across, region.Origin) + region.AcrossRange[0]) - 1.0F;
    toSilhouette[11] = -0.5F;
    toSilhouette[15] = 1.0F;
    const Mat4 rows = Multiply(toSilhouette, *inverse);
    if (!Finite(rows)) return false;
    std::memcpy(uniforms.data() + kPicaPackedVertexFloatOffset, rows.data(), sizeof(rows));
    const int32_t noFlip = 0;
    std::memcpy(uniforms.data() + kPicaPackedVertexFlipViewportOffset, &noFlip, sizeof(noFlip));
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
    // config/simple_shadow_method: set by the Video panel, read by the
    // renderer at each frame.
    void SetMethod(PlayerShadowMethod method) noexcept { mMethod.store(method, std::memory_order_relaxed); }
    [[nodiscard]] PlayerShadowMethod Method() const noexcept { return mMethod.load(std::memory_order_relaxed); }

  private:
    struct Entry {
        uint64_t Id = 0;
        std::optional<PlayerShadowFrame> Shadow;
    };
    mutable std::mutex mMutex;
    std::deque<Entry> mFrames;
    std::optional<PlayerShadowFrame> mSelected;
    std::atomic<bool> mRendererReady{false};
    std::atomic<PlayerShadowMethod> mMethod{PlayerShadowMethod::TwoPass};
};

} // namespace Fast::Oot3d
