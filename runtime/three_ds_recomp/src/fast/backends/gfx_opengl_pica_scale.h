#pragma once

// Pure helpers of the OpenGL PICA backend (gfx_opengl_pica.cpp) that decide
// GPU work: the internal resolution scale (integer or fractional) and the
// switches of the exact GPU savings. Kept free of GL so they can be checked
// offline (tests/gfx_opengl_pica_scale_tests.cpp).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace Fast::PicaGlScale {

inline constexpr float kMaxResolutionScale = 3.0F;
// Docked (1920x1080 window): 4.5 renders the 240-line screen at 1080 lines,
// so FSR EASU runs 1:1; lower values are enlarged by EASU.
inline constexpr float kMaxDockedResolutionScale = 4.5F;
// Without any config file (perf.txt: 3x made the handheld GPU the bottleneck).
inline constexpr float kDefaultResolutionScale = 2.0F;

// config/resolution_scale (Switch) or TRIAEVUM_GL_RESOLUTION_SCALE: "1", "2"
// and "3" as before, plus a decimal between them ("2.5" or "2,5"), rounded to
// a multiple of 1/4 so every host edge stays a short, exact binary fraction.
// Values above 3 are clamped to 3; anything unreadable or below 1 is 1 (the
// integer behaviour of the former parser: "10" was 1, "7" was 3).
[[nodiscard]] inline float ParseResolutionScale(std::string_view text,
                                               float maxScale = kMaxResolutionScale) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\n' || text.front() == '\r')) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1U);
    }
    if (text.empty() || text.front() < '1' || text.front() > '9') {
        return 1.0F;
    }
    if (text.size() > 1U && text[1] >= '0' && text[1] <= '9') {
        return 1.0F;
    }
    float value = static_cast<float>(text.front() - '0');
    if (text.size() > 2U && (text[1] == '.' || text[1] == ',')) {
        float weight = 0.1F;
        for (size_t index = 2U; index < text.size(); ++index) {
            const char c = text[index];
            if (c < '0' || c > '9') {
                break;
            }
            value += static_cast<float>(c - '0') * weight;
            weight *= 0.1F;
        }
        value = std::round(value * 4.0F) / 4.0F;
    }
    return std::clamp(value, 1.0F, maxScale);
}

// Switch: config/resolution_scale_docked wins in the dock (up to 4.5), then
// config/resolution_scale (both modes, up to 3), then the default. A null
// pointer is a missing file.
[[nodiscard]] inline float SelectResolutionScale(const std::string* docked, const std::string* common,
                                                 bool isDocked) noexcept {
    if (isDocked && docked != nullptr) {
        return ParseResolutionScale(*docked, kMaxDockedResolutionScale);
    }
    if (common != nullptr) {
        return ParseResolutionScale(*common);
    }
    return kDefaultResolutionScale;
}

[[nodiscard]] inline bool IsIntegerScale(float scale) noexcept {
    return scale == std::floor(scale);
}

// Host pixel edge of a native pixel edge. Both ends of every viewport,
// scissor, target and copy go through this, so neighbouring rectangles tile
// without gaps at a fractional scale; at an integer scale it is exactly
// native * N (the former integer arithmetic).
[[nodiscard]] inline int32_t ScaledEdge(int64_t native, float scale) noexcept {
    return static_cast<int32_t>(
        std::llround(static_cast<double>(native) * static_cast<double>(scale)));
}

[[nodiscard]] inline uint32_t ScaledExtent(uint32_t native, float scale) noexcept {
    return static_cast<uint32_t>(
        std::max<int32_t>(native == 0U ? 0 : 1, ScaledEdge(native, scale)));
}

// Largest scale whose biggest PICA target (1024 per axis) fits the driver.
[[nodiscard]] inline float FitResolutionScale(float scale,
                                              int64_t limit) noexcept {
    if (scale <= 1.0F || limit <= 0) {
        return std::max(scale, 1.0F);
    }
    while (scale > 1.0F &&
           static_cast<int64_t>(std::ceil(1024.0 * scale)) > limit) {
        // Integer scales step down by one as before; fractional ones fall to
        // the integer below.
        scale = IsIntegerScale(scale) ? scale - 1.0F : std::floor(scale);
    }
    return std::max(scale, 1.0F);
}

// Dynamic resolution (gl_pica_dynamic_scale.h): the drawn extent of a
// target allocated at allocWidth x allocHeight when drawn at `scale` (its
// bottom-left sub-rectangle). A widescreen target widens its height by
// wideFactor as at allocation. Never larger than the allocation.
struct SubRectangle {
    uint32_t Width = 0U;
    uint32_t Height = 0U;
};
[[nodiscard]] inline SubRectangle DynamicSubRectangle(
    uint32_t nativeWidth, uint32_t nativeHeight, float scale,
    uint32_t allocWidth, uint32_t allocHeight, bool wide,
    float wideFactor) noexcept {
    SubRectangle extent;
    extent.Width = std::min(allocWidth, ScaledExtent(nativeWidth, scale));
    uint32_t height = ScaledExtent(nativeHeight, scale);
    if (wide) {
        height = static_cast<uint32_t>(
            std::lround(static_cast<float>(height) * wideFactor));
    }
    extent.Height = std::min(allocHeight, std::max<uint32_t>(1U, height));
    return extent;
}

// config/gl_lean: exact GPU savings of the backend, on unless the file holds
// "0" (TRIAEVUM_GL_LEAN=0 elsewhere): skip a display copy whose source has not
// changed since the same copy, skip the window clear a full-window scanout
// overwrites, and clear a target's Shadow2D image only when it is used.
[[nodiscard]] inline bool ParseLeanSetting(std::string_view text,
                                           bool fallback) noexcept {
    for (const char c : text) {
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T') {
            return true;
        }
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F') {
            return false;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
    }
    return fallback;
}

// The Replace-mode scanout writes every pixel of its viewport with blending
// off, so a clear of the window is dead when that viewport is the window.
[[nodiscard]] inline bool ScanoutCoversWindow(int viewportX, int viewportY,
                                              int viewportWidth,
                                              int viewportHeight,
                                              int windowWidth,
                                              int windowHeight) noexcept {
    return viewportX <= 0 && viewportY <= 0 &&
           viewportX + viewportWidth >= windowWidth &&
           viewportY + viewportHeight >= windowHeight;
}

// Identity of one display-transfer copy: the source target and its content
// version, the copied source extent and the filter. A copy whose identity
// equals the last one into the same display image writes the same pixels.
struct DisplayCopyIdentity {
    uint64_t SourceTarget = 0U;
    uint64_t SourceVersion = 0U;
    int32_t SourceWidth = 0;
    int32_t SourceHeight = 0;
    uint32_t Filter = 0U;

    [[nodiscard]] bool Valid() const noexcept { return SourceTarget != 0U; }
    bool operator==(const DisplayCopyIdentity&) const = default;
};

} // namespace Fast::PicaGlScale
