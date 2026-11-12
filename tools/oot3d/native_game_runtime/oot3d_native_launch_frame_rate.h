#pragma once

#include "fast/oot3d/graphics_settings.h"
#include "oot3d_native_frame_rate.h"

#include <atomic>
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

// L3+R3 > Video > FPS while the game runs (oot3d_video_settings_panel.cpp;
// config/fps60 keeps the choice for the next start). "30 FPS" and "60 FPS
// (interpolated)" both simulate at 30 Hz (native30_interpolated presented
// at 30 or 60): only the presentation changes, so the session applies it at
// its next frame boundary (RunFrame, before the pacing policy is resolved).
// Published by the menu (game thread), read by the session once per frame.
struct LiveFrameRateChoice {
    std::atomic<bool> Fps60{false};
    std::atomic<uint64_t> Revision{0U};

    void Publish(bool fps60) noexcept {
        Fps60.store(fps60, std::memory_order_relaxed);
        Revision.fetch_add(1U, std::memory_order_release);
    }
};

inline LiveFrameRateChoice& LiveFrameRate() noexcept {
    static LiveFrameRateChoice choice;
    return choice;
}

// The FrameRate mode a newly published choice asks of a launch with timing
// `mode` (seenRevision: the last revision the session applied; updated).
// No value: nothing new, or a launch the choice cannot change live
// (enhanced60 simulates at 60 Hz; native30_no_interpolation never
// interpolates and stays at Original30).
[[nodiscard]] inline std::optional<Fast::Oot3d::FrameRateMode>
TakeLiveFrameRateMode(const LiveFrameRateChoice& choice, uint64_t& seenRevision,
                      GameplayTimingMode mode) noexcept {
    const uint64_t revision = choice.Revision.load(std::memory_order_acquire);
    if (revision == seenRevision) {
        return std::nullopt;
    }
    seenRevision = revision;
    if (mode == GameplayTimingMode::Enhanced60) {
        return std::nullopt;
    }
    return ResolveLaunchFrameRateMode(
        mode, choice.Fps60.load(std::memory_order_relaxed) ? 60U : 30U);
}

} // namespace Oot3dNativeGame
