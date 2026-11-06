#pragma once

// Distribution of the process's threads across the Switch cores (Horizon).
// Policy only: pure and testable on the host (oot3d_switch_thread_layout_tests);
// oot3d_switch_threads.cpp applies it.
//
// Horizon scheduler rules the policy respects:
// - lower priority = more urgent; only 0x3B time-slices between threads of
//   the same priority. At any other, a busy thread blocks those of
//   equal or lower priority on the same core, even with other cores
//   idle (migration only happens at rescheduling points);
// - a thread with a multi-core mask can be moved to an idle
//   core; with a single-core mask, never.
//
// Layouts (file sdmc:/switch/oot3dre/config/thread_layout, first
// word; absent = fixed):
// - fixed  : game alone on core 0; render thread pinned to 2; plan
//            worker, host audio and SDL audio on 1; service threads
//            (log, Mesa shader cache, resources, loaders) at 0x3B
//            with mask 1|2: they never take the place of what draws or plays sound.
//            The second guest core (config/second_core) is pinned to 1
//            above the plan worker, in every layout.
// - float  : like fixed, but render thread and plan worker with mask
//            1|2 (ideal core 2 and 1): the kernel can move them to the free core.
//            (StevensND/nfsmw-nx: pinning the render thread made the worst case worse.)
// - legacy : the previous behavior, for A/B: every new pthread at the
//            game's priority (0x2C), alternating cores 1/2; SDL audio
//            on the core it was born on; 500 us spin in the pacer.
// Options after the mode: pacer_spin_us=N (busy-wait before the pacer
// deadline; default 0 in fixed/float — svcSleepThread wakes on time — and 500
// in legacy).

#include <cstdint>
#include <string_view>

namespace Oot3dNativeGame {

enum class SwitchThreadLayout : uint8_t { Fixed, Float, Legacy };

enum class SwitchThreadRole : uint8_t {
    Game,       // main thread: the game (guest CPU, DSP HLE, submission)
    Render,     // NativeRenderCommandStream: all of GL
    PicaPlan,   // pre-assembly of the PICA plans
    HostAudio,  // HostAudioStream: stretch, resampling, SDL queue
    SdlAudio,   // SDL audio thread (audren)
    Service,    // any other pthread (log, Mesa, resources, loaders)
    GuestCore1, // config/second_core: the guest threads that asked for processor 1
    Count
};

inline constexpr int kSwitchGamePriority = 0x2C;
inline constexpr int kSwitchRenderPriority = 0x2D;
inline constexpr int kSwitchPicaPlanPriority = 0x2E;
// The second guest core (the game's sound thread and the DSP frames): on
// core 1 under the audio threads (0x2B preempt it), above the plan worker
// (0x2E, 26-31 ms idle per 40 ms frame: thread 6's ~1 ms bursts per DSP
// frame leave it ample time, while its preemption had doubled thread 6's
// wall time), at the render thread's priority (on core 2), above the
// time-sliced service threads.
inline constexpr int kSwitchGuestCore1Priority = 0x2D;
inline constexpr int kSwitchAudioPriority = 0x2B;
// The only priority with time-slicing; below all the others.
inline constexpr int kSwitchServicePriority = 0x3B;

struct SwitchThreadPlacement {
    int Priority = kSwitchServicePriority;
    // Ideal core (0-2); -1 = leave affinity alone; -2 = alternate 1/2 on
    // creation (legacy).
    int IdealCore = -1;
    uint32_t CoreMask = 0U;  // bits 0-2; 0 = leave alone
};

struct SwitchThreadLayoutConfig {
    SwitchThreadLayout Layout = SwitchThreadLayout::Fixed;
    // Pacer busy-wait before the deadline, in microseconds.
    int PacerSpinMicroseconds = 0;
    bool Valid = true;  // false: unrecognized text (falls back to fixed)
};

[[nodiscard]] constexpr const char* SwitchThreadLayoutName(
    SwitchThreadLayout layout) noexcept {
    switch (layout) {
    case SwitchThreadLayout::Fixed: return "fixed";
    case SwitchThreadLayout::Float: return "float";
    case SwitchThreadLayout::Legacy: return "legacy";
    }
    return "?";
}

[[nodiscard]] constexpr const char* SwitchThreadRoleName(
    SwitchThreadRole role) noexcept {
    switch (role) {
    case SwitchThreadRole::Game: return "game";
    case SwitchThreadRole::Render: return "render";
    case SwitchThreadRole::PicaPlan: return "plan";
    case SwitchThreadRole::HostAudio: return "audio";
    case SwitchThreadRole::SdlAudio: return "sdl_audio";
    case SwitchThreadRole::Service: return "service";
    case SwitchThreadRole::GuestCore1: return "guest_core1";
    case SwitchThreadRole::Count: break;
    }
    return "?";
}

[[nodiscard]] constexpr int DefaultSwitchPacerSpinMicroseconds(
    SwitchThreadLayout layout) noexcept {
    return layout == SwitchThreadLayout::Legacy ? 500 : 0;
}

// Contents of the config/thread_layout file. Empty = fixed.
[[nodiscard]] constexpr SwitchThreadLayoutConfig ParseSwitchThreadLayoutConfig(
    std::string_view text) noexcept {
    SwitchThreadLayoutConfig config;
    bool first = true;
    bool spinGiven = false;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' ||
                text[position] == '\r' || text[position] == '\n' ||
                text[position] == ',')) {
            ++position;
        }
        const size_t start = position;
        while (position < text.size() && text[position] != ' ' &&
               text[position] != '\t' && text[position] != '\r' &&
               text[position] != '\n' && text[position] != ',') {
            ++position;
        }
        const std::string_view token = text.substr(start, position - start);
        if (token.empty()) {
            break;
        }
        constexpr std::string_view kSpin = "pacer_spin_us=";
        if (token.substr(0, kSpin.size()) == kSpin) {
            const std::string_view digits = token.substr(kSpin.size());
            int value = 0;
            bool ok = !digits.empty() && digits.size() <= 5U;
            for (const char c : digits) {
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                value = value * 10 + (c - '0');
            }
            if (ok && value <= 4000) {
                config.PacerSpinMicroseconds = value;
                spinGiven = true;
            } else {
                config.Valid = false;
            }
        } else if (first && token == "fixed") {
            config.Layout = SwitchThreadLayout::Fixed;
        } else if (first && token == "float") {
            config.Layout = SwitchThreadLayout::Float;
        } else if (first && token == "legacy") {
            config.Layout = SwitchThreadLayout::Legacy;
        } else {
            config.Valid = false;
        }
        first = false;
    }
    if (!spinGiven) {
        config.PacerSpinMicroseconds =
            DefaultSwitchPacerSpinMicroseconds(config.Layout);
    }
    return config;
}

// processMask: cores allowed to the process (bits 0-2). Cores outside it
// leave the mask; with no core left, the affinity stays as is.
[[nodiscard]] constexpr SwitchThreadPlacement ResolveSwitchThreadPlacement(
    SwitchThreadLayout layout, SwitchThreadRole role,
    uint32_t processMask) noexcept {
    SwitchThreadPlacement placement;
    const bool legacy = layout == SwitchThreadLayout::Legacy;
    const bool floating = layout == SwitchThreadLayout::Float;
    switch (role) {
    case SwitchThreadRole::Game:
        placement = {kSwitchGamePriority, 0, 0x1U};
        break;
    case SwitchThreadRole::Render:
        placement = {kSwitchRenderPriority, 2, floating ? 0x6U : 0x4U};
        break;
    case SwitchThreadRole::PicaPlan:
        placement = {kSwitchPicaPlanPriority, 1, floating ? 0x6U : 0x2U};
        break;
    case SwitchThreadRole::HostAudio:
        placement = {kSwitchAudioPriority, 1, 0x2U};
        break;
    case SwitchThreadRole::SdlAudio:
        placement = legacy ? SwitchThreadPlacement{kSwitchAudioPriority, -1, 0U}
                           : SwitchThreadPlacement{kSwitchAudioPriority, 1, 0x2U};
        break;
    case SwitchThreadRole::GuestCore1:
        placement = {kSwitchGuestCore1Priority, 1, 0x2U};
        break;
    case SwitchThreadRole::Service:
    case SwitchThreadRole::Count:
        placement = legacy
                        ? SwitchThreadPlacement{kSwitchGamePriority, -2, 0U}
                        : SwitchThreadPlacement{kSwitchServicePriority, 1, 0x6U};
        break;
    }
    if (placement.CoreMask != 0U) {
        placement.CoreMask &= processMask & 0x7U;
        if (placement.CoreMask == 0U) {
            placement.IdealCore = -1;
        } else if ((placement.CoreMask & (1U << placement.IdealCore)) == 0U) {
            // The ideal core must be in the mask: the lowest one left.
            for (int core = 0; core < 3; ++core) {
                if ((placement.CoreMask & (1U << core)) != 0U) {
                    placement.IdealCore = core;
                    break;
                }
            }
        }
    }
    return placement;
}

} // namespace Oot3dNativeGame
