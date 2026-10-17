#pragma once

#include "fast/oot3d/graphics_settings.h"
#include "oot3d_native_frame_rate.h"

#include <cstdint>
#include <optional>

namespace Oot3dNativeGame {

// The presentation cadence of a launch whose command line fixes it.
//
// Pacing (the realtime pacer's target rate) and the visual sample cadence
// come from the graphics settings' FrameRate mode, which defaults to
// Original30 and is chosen in the F1 menu on the PC. The Switch has no such
// menu: its homebrew arguments carry --gameplay-timing and
// --presentation-rate (config/fps60 turns them into native30_interpolated at
// 60), but neither reached the pacer, so fps60 still presented 30 frames a
// second (each running two 60 Hz guest refreshes) and never drew the
// interpolated samples. This maps such a launch onto the FrameRate mode that
// presents at its rate. No value: keep the graphics settings.
[[nodiscard]] inline std::optional<Fast::Oot3d::FrameRateMode>
ResolveLaunchFrameRateMode(GameplayTimingMode mode,
                           uint32_t presentationRateHz) noexcept {
    using Fast::Oot3d::FrameRateMode;
    switch (mode) {
    case GameplayTimingMode::Native30NoInterpolation:
        return FrameRateMode::Original30;
    case GameplayTimingMode::Native30Interpolated:
        switch (presentationRateHz) {
        case 0U:
            return FrameRateMode::Uncapped;
        case 30U:
            return FrameRateMode::Original30;
        case 60U:
            return FrameRateMode::Interpolated2x;
        case 90U:
            return FrameRateMode::Interpolated3x;
        default:
            return std::nullopt;
        }
    case GameplayTimingMode::Enhanced60:
        // 60 simulation steps a second need 60 presentations; interpolation
        // stays off (the contract disallows it for this mode).
        return presentationRateHz == 60U
            ? std::optional<FrameRateMode>(FrameRateMode::Interpolated2x)
            : std::nullopt;
    }
    return std::nullopt;
}

} // namespace Oot3dNativeGame
