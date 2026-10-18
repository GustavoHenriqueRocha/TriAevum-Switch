#pragma once

#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_top_screen_mod_profile.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

// The mod swaps the two native targets. The desktop frontend instead retains
// the upper scene and draws the relocated menu over it, in native draw order.
inline TopScreenPauseTargetPlan ResolveTopScreenFrontendTargetCommand(
    const TopScreenPauseTargetCommand& command, bool frontendActive) noexcept {
    TopScreenPauseTargetPlan plan;
    if (!frontendActive || !command.RendererEnabled ||
        (command.NativeCommand != 0x400U && command.NativeCommand != 0x401U)) {
        return plan;
    }
    plan.Handled = true;
    plan.BindTopTarget = true;
    plan.StoredCommand = command.NativeCommand;
    plan.FramebufferBindingOffset = 0x38U;
    plan.ViewportWidth = command.HalfHeightMode ? 240U : 480U;
    plan.ViewportY = command.NativeCommand == 0x401U ? 40U : 0U;
    plan.ViewportHeight = command.NativeCommand == 0x401U ? 320U : 400U;
    return plan;
}

// The relocated lower menu's canvases on the upper target, in both renderer
// width modes. The guest draws that menu after its upper snapshot, so these
// draws continue the snapshot's visual frame (see FinishFrames).
inline std::array<Oot3dPicaViewportState, 2> TopScreenFrontendMenuCanvases() noexcept {
    std::array<Oot3dPicaViewportState, 2> canvases{};
    for (size_t index = 0; index < canvases.size(); ++index) {
        const auto plan =
            ResolveTopScreenFrontendTargetCommand({true, 0x401U, index != 0U}, true);
        canvases[index].CornerX = static_cast<int16_t>(plan.ViewportX);
        canvases[index].CornerY = static_cast<int16_t>(plan.ViewportY);
        canvases[index].HalfWidth = static_cast<float>(plan.ViewportWidth) * 0.5F;
        canvases[index].HalfHeight = static_cast<float>(plan.ViewportHeight) * 0.5F;
    }
    return canvases;
}

struct TopScreenFrontendCanvas {
    uint32_t ColorAddress = 0;
    Oot3dPicaViewportState Viewport;

    bool Matches(const Oot3dPicaDecodedDrawState& state) const noexcept {
        // A shared target does not imply a shared canvas: the upper scene's
        // clear must survive removal of the relocated lower menu's clear.
        return ColorAddress == state.Framebuffer.ColorPhysicalAddress &&
               Viewport.CornerX == state.Viewport.CornerX &&
               Viewport.CornerY == state.Viewport.CornerY &&
               Viewport.HalfWidth == state.Viewport.HalfWidth &&
               Viewport.HalfHeight == state.Viewport.HalfHeight;
    }
};

// The relocated menu's own backdrop (CommonBackground00 of File Select and
// Name Entry, common_bg01 of the version choice). On the 3DS that backdrop and
// the menu form the lower screen, which the mod moves to the upper screen as a
// unit (target swap above): the upper scene - the title logo and copyright,
// File Select's 3D Link and its caption panel - is not behind the menu there.
// Removing the backdrop showed exactly that scene between and through the
// cards. Only the menu's opaque canvas clear stays removed, so the upper sky
// remains beside the 4:3 menu on a widened target, as it is beside the 320-wide
// menu on the 400-wide upper screen.
// Read once: on the Switch sdmc:/switch/oot3dre/config/topscreen_menu_backdrop
// ("0" restores the previous sky-behind-the-cards composition; missing = on);
// elsewhere TRIAEVUM_TOPSCREEN_MENU_BACKDROP with the same format.
[[nodiscard]] inline bool ParseTopScreenMenuBackdropSetting(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    return text.empty() || text.front() != '0';
}

namespace TopScreenMenuBackdropDetail {
inline int& TestOverride() noexcept {
    static int value = -1;
    return value;
}
} // namespace TopScreenMenuBackdropDetail

inline void SetTopScreenMenuBackdropForTesting(std::optional<bool> enabled) noexcept {
    TopScreenMenuBackdropDetail::TestOverride() = enabled ? (*enabled ? 1 : 0) : -1;
}

[[nodiscard]] inline bool TopScreenMenuBackdropEnabled() {
    if (const int forced = TopScreenMenuBackdropDetail::TestOverride(); forced >= 0)
        return forced != 0;
    static const bool enabled = [] {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/topscreen_menu_backdrop");
        std::string text;
        if (!file || !std::getline(file, text))
            return true;
        return ParseTopScreenMenuBackdropSetting(text);
#else
        const char* value = std::getenv("TRIAEVUM_TOPSCREEN_MENU_BACKDROP");
        return value == nullptr || ParseTopScreenMenuBackdropSetting(value);
#endif
    }();
    return enabled;
}

// Which draws on a relocated menu canvas the composition omits: always the
// menu's opaque canvas clear (it would cover the upper scene that the shared
// target keeps beside the menu); the backdrop texture only when the backdrop
// is disabled.
[[nodiscard]] inline bool OmitTopScreenFrontendMenuDraw(bool relocationActive,
                                                        bool targetsMenuCanvas,
                                                        bool usesMenuBackdrop,
                                                        bool isOpaqueCanvasClear,
                                                        bool backdropEnabled) noexcept {
    return relocationActive && targetsMenuCanvas &&
           (isOpaqueCanvasClear || (usesMenuBackdrop && !backdropEnabled));
}

inline bool ComposeTopScreenFrontendFrame(Oot3dPicaVisualFrame& frame,
                                         bool frontendActive) {
    if (!frontendActive || frame.Draws.empty()) {
        return false;
    }
    uint64_t lastTopDraw = frame.TopTransfer.AfterDrawSubmissionId;
    for (const auto& draw : frame.Draws) {
        if (draw.State.Framebuffer.ColorPhysicalAddress ==
            frame.TopTransfer.InputPhysicalAddress) {
            lastTopDraw = std::max(lastTopDraw, draw.SubmissionId);
        }
    }
    if (lastTopDraw == frame.TopTransfer.AfterDrawSubmissionId) {
        return false;
    }
    // Only the host scanout is deferred until the relocated menu is complete.
    // Guest GSP commands, interrupts, memory and the other transfer stay native.
    for (auto& transfer : frame.DisplayTransfers) {
        if (transfer.CompletionId == frame.TopTransfer.CompletionId &&
            transfer.OutputPhysicalAddress == frame.TopTransfer.OutputPhysicalAddress &&
            transfer.InputPhysicalAddress == frame.TopTransfer.InputPhysicalAddress) {
            transfer.AfterDrawSubmissionId = lastTopDraw;
        }
    }
    frame.TopTransfer.AfterDrawSubmissionId = lastTopDraw;
    std::stable_sort(frame.DisplayTransfers.begin(), frame.DisplayTransfers.end(),
                     [](const auto& a, const auto& b) {
                         return a.AfterDrawSubmissionId < b.AfterDrawSubmissionId;
                     });
    return true;
}

} // namespace Oot3dNativeGame
