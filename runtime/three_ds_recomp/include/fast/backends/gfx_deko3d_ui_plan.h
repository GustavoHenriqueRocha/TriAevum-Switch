#pragma once

// Pure parts of the deko3d UI path (package 6, docs/DEKO3D_PROGRESSO.md):
// the TopScreen UI (Fast3dUiRenderBackend) and ImGui drawn on the window.
// No deko3d types here, so the host tests cover them.

#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/renderer3ds/pica_native_state.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Fast::Deko3d {

// ---- rects ----------------------------------------------------------------

struct UiRect {
    uint32_t X = 0;
    uint32_t Y = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    bool operator==(const UiRect&) const = default;
};

// A glViewport/glScissor rect (origin bottom-left unless the framebuffer is
// drawn with invertY) as a deko3d rect (origin upper-left), clamped to the
// framebuffer.
[[nodiscard]] constexpr UiRect GlRectToUpperLeft(int x, int y, int width, int height, uint32_t fbWidth,
                                                 uint32_t fbHeight, bool invertY) noexcept {
    const int64_t left = std::clamp<int64_t>(x, 0, fbWidth);
    const int64_t right = std::clamp<int64_t>(static_cast<int64_t>(x) + std::max(width, 0), 0, fbWidth);
    int64_t top = y;
    int64_t bottom = static_cast<int64_t>(y) + std::max(height, 0);
    if (!invertY) {
        top = static_cast<int64_t>(fbHeight) - bottom;
        bottom = static_cast<int64_t>(fbHeight) - y;
    }
    top = std::clamp<int64_t>(top, 0, fbHeight);
    bottom = std::clamp<int64_t>(bottom, 0, fbHeight);
    return {static_cast<uint32_t>(left), static_cast<uint32_t>(top), static_cast<uint32_t>(right - left),
            static_cast<uint32_t>(bottom - top)};
}

// Clip-space y of the UI follows GL for the framebuffer's invertY (top row
// = +1 without it); with OriginUpperLeft the top row is -1, so y is negated
// unless the framebuffer is drawn inverted.
[[nodiscard]] constexpr float UiClipYScale(bool invertY) noexcept {
    return invertY ? 1.0F : -1.0F;
}

// deko3d's clip space has y up (GL) while OriginUpperLeft puts image row 0
// at the top: NDC y = -1 lands on the last row, the opposite of GL memory
// (row 0 = NDC -1) that the whole backend assumes (p6 on the console: UI
// upside down, the top screen turned 180 degrees). A negative-height
// viewport puts NDC -1 on row 0 again, everywhere; it also mirrors the
// winding in window space, which corrects the culling seen on p6 (sky dome
// culled, so the uncleared top scene smeared).
struct ViewportRows {
    float Y = 0.0F;
    float Height = 0.0F;
    bool operator==(const ViewportRows&) const = default;
};

[[nodiscard]] constexpr ViewportRows FlipViewportRows(float y, float height, bool flip) noexcept {
    return flip ? ViewportRows{y + height, -height} : ViewportRows{y, height};
}

// ---- vertices ---------------------------------------------------------------

// The two UI programs (oot3d_legacy_ui_adapters.cpp): solid = shade colour,
// textured = TEXEL0 * shade. Their DrawTriangles layouts: position xyzw,
// [uv], rgba.
enum class UiVertexLayout : uint8_t { Unsupported, Solid, Textured };

inline constexpr uint32_t kUiSolidFloats = 8U;
inline constexpr uint32_t kUiTexturedFloats = 10U;

[[nodiscard]] constexpr UiVertexLayout ClassifyUiVertices(size_t floats, size_t triangles) noexcept {
    if (triangles == 0U || floats == 0U || floats % (triangles * 3U) != 0U) {
        return UiVertexLayout::Unsupported;
    }
    switch (floats / (triangles * 3U)) {
        case kUiSolidFloats: return UiVertexLayout::Solid;
        case kUiTexturedFloats: return UiVertexLayout::Textured;
        default: return UiVertexLayout::Unsupported;
    }
}

// ImGui vertex (pixels, origin upper-left of the display) as a textured UI
// vertex in GL clip space of a non-inverted framebuffer.
[[nodiscard]] constexpr std::array<float, kUiTexturedFloats>
ImGuiVertexToUi(float px, float py, float u, float v, uint32_t rgba, float displayX, float displayY,
                float displayWidth, float displayHeight) noexcept {
    const float w = displayWidth > 0.0F ? displayWidth : 1.0F;
    const float h = displayHeight > 0.0F ? displayHeight : 1.0F;
    return {(px - displayX) / w * 2.0F - 1.0F,
            1.0F - (py - displayY) / h * 2.0F,
            0.0F,
            1.0F,
            u,
            v,
            static_cast<float>(rgba & 0xFFU) / 255.0F,
            static_cast<float>((rgba >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>((rgba >> 16U) & 0xFFU) / 255.0F,
            static_cast<float>((rgba >> 24U) & 0xFFU) / 255.0F};
}

// An ImGui clip rect (pixels, upper-left) as a scissor in the window.
[[nodiscard]] constexpr UiRect ImGuiClipToScissor(float x0, float y0, float x1, float y1, float displayX,
                                                  float displayY, uint32_t fbWidth, uint32_t fbHeight) noexcept {
    const auto clampTo = [](float value, uint32_t limit) {
        return static_cast<uint32_t>(std::clamp(value, 0.0F, static_cast<float>(limit)));
    };
    const uint32_t left = clampTo(x0 - displayX, fbWidth);
    const uint32_t top = clampTo(y0 - displayY, fbHeight);
    const uint32_t right = clampTo(x1 - displayX, fbWidth);
    const uint32_t bottom = clampTo(y1 - displayY, fbHeight);
    return {left, top, right > left ? right - left : 0U, bottom > top ? bottom - top : 0U};
}

// ---- samplers ---------------------------------------------------------------

[[nodiscard]] constexpr PicaWrap UiWrap(Renderer3ds::NativeTextureWrap wrap) noexcept {
    switch (wrap) {
        case Renderer3ds::NativeTextureWrap::Repeat: return PicaWrap::Repeat;
        case Renderer3ds::NativeTextureWrap::MirroredRepeat: return PicaWrap::MirroredRepeat;
        default: return PicaWrap::ClampToEdge;
    }
}

[[nodiscard]] constexpr bool UiFilterLinear(Renderer3ds::NativeTextureFilter filter) noexcept {
    return filter == Renderer3ds::NativeTextureFilter::Linear ||
           filter == Renderer3ds::NativeTextureFilter::LinearMipmapNearest ||
           filter == Renderer3ds::NativeTextureFilter::LinearMipmapLinear;
}

// SetNativeSamplerParameters: UI textures have one level (no mips).
[[nodiscard]] constexpr PicaSamplerKey UiSamplerKey(const Renderer3ds::NativeSamplerState& state) noexcept {
    PicaSamplerKey key;
    key.MinLinear = UiFilterLinear(state.MinFilter);
    key.MagLinear = UiFilterLinear(state.MagFilter);
    key.WrapS = UiWrap(state.WrapS);
    key.WrapT = UiWrap(state.WrapT);
    return key;
}

// SetSamplerParameters (Fast3D, N64 cms/cmt bits: 1 mirror, 2 clamp).
[[nodiscard]] constexpr PicaSamplerKey UiLegacySamplerKey(bool linear, uint32_t cms, uint32_t cmt) noexcept {
    const auto wrap = [](uint32_t bits) {
        return (bits & 2U) != 0U ? PicaWrap::ClampToEdge
                                 : ((bits & 1U) != 0U ? PicaWrap::MirroredRepeat : PicaWrap::Repeat);
    };
    PicaSamplerKey key;
    key.MinLinear = linear;
    key.MagLinear = linear;
    key.WrapS = wrap(cms);
    key.WrapT = wrap(cmt);
    return key;
}

} // namespace Fast::Deko3d
