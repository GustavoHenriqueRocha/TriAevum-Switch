#pragma once

#include "fast/oot3d/scene_presentation_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

// Widescreen for the OpenGL backend (Switch and desktop GL).
//
// PC Vulkan already renders the top-screen target in the window's shape and the
// projection hook (ResolveScenePresentationPolicy) widens the frustum in the same
// proportion. GL renders the native 400x240 target (x scale N) and presents in
// 5:3 with bars; with this option on it widens only the horizontally presented
// axis of the top-screen target (480x400 stored rotated), to the
// window's proportion, and the hook widens the FOV by the same ratio. With it
// off, the hook must use the native aspect, otherwise the scene comes out squeezed
// inside the 5:3.
//
// Read once: on the Switch from sdmc:/switch/oot3dre/config/widescreen ("1" on,
// "0" off; without the file the console default applies, on); elsewhere from the
// TRIAEVUM_GL_WIDESCREEN variable (without it, off).
[[nodiscard]] inline bool ParseGlWidescreenSetting(const std::string& text,
                                                   bool fallback) noexcept {
    for (const char c : text) {
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T')
            return true;
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
            return false;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }
    return fallback;
}

[[nodiscard]] inline bool GlWidescreenEnabled() {
    static const bool enabled = [] {
#if defined(__SWITCH__)
        constexpr bool kConsoleDefault = true;
        std::ifstream file("sdmc:/switch/oot3dre/config/widescreen");
        std::string text;
        if (!file || !std::getline(file, text))
            return kConsoleDefault;
        return ParseGlWidescreenSetting(text, kConsoleDefault);
#else
        const char* value = std::getenv("TRIAEVUM_GL_WIDESCREEN");
        return value != nullptr && ParseGlWidescreenSetting(value, false);
#endif
    }();
    return enabled;
}

// How much the top screen's presented horizontal axis grows relative to the
// native 400 to fill a `width`x`height` output (1 = no widening).
// Only widens (outputs narrower than 5:3 keep bars), and at most
// up to 21:9 so as not to overflow the target.
[[nodiscard]] inline float GlWidescreenFactor(std::uint32_t width,
                                              std::uint32_t height) noexcept {
    if (width == 0U || height == 0U)
        return 1.0F;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (!std::isfinite(aspect) || aspect <= kNativeTopScreenAspect + 1.0e-4F)
        return 1.0F;
    return std::min(aspect / kNativeTopScreenAspect, (21.0F / 9.0F) / kNativeTopScreenAspect);
}

} // namespace Fast::Oot3d
