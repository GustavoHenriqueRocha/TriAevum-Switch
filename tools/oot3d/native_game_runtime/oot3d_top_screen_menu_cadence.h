#pragma once

// The TopScreen pause menu at 60 FPS (config/fps60; SWITCH_PORT.md "The pause
// menu at the game's own cadence at 60 FPS").
//
// TopScreen draws the pause pages (Items, Gear, Map, System) as the game's
// own lower-screen draws moved onto the upper screen: they change with the
// game, 30 times a second, over a still world. At 60 FPS every in-between
// frame interpolated them like the world (oot3d_native_pica_visual_frame.cpp):
// the matched positions and uniform fades at 60, the vertex alpha, the UVs and
// the pieces that appear or go at 30, the UI draws (no temporal identity)
// paired by structure and order only; and the pacing governor dropped the
// in-between frames for up to a second after an opening (logs27/28:
// interp_dropped 15-31 in the opening's window, a ratio learnt in its cost
// spike), so the menu switched between 60 and 30 as it opened.
//
// While a page is up, each in-between frame shows the step image again, as a
// dropped one does, without asking the governor: the menu at the game's own
// 30 Hz, as the 3DS draws it. The step frames are unchanged (the delayed
// presentation of oot3d_native_frame_rate.h), so entering and leaving the
// menu neither skips nor repeats a picture. 30 FPS, the other UI profiles and
// everything outside the pause pages are unchanged. config/interp_menu "1"
// (desktop: TRIAEVUM_INTERP_MENU) interpolates the menu as before (A/B).

#include "oot3d_native_a32_memory.h"
#include "oot3d_native_interpolation_pacing.h"
#include "oot3d_top_screen_mod_profile.h"

#include <cstdint>

namespace Oot3dNativeGame {

struct TopScreenMenuCadenceInputs {
  bool TopScreenProfile = false;
  // The 60 FPS presentation (interpolated).
  bool Interpolation = false;
  // config/interp_menu = 1: the menu interpolated as before.
  bool MenuInterpolation = false;
  // A pause page owner (Gear, Items, Map, System) is active.
  bool PausePageActive = false;
};

[[nodiscard]] constexpr bool HoldTopScreenMenuInBetween(
    const TopScreenMenuCadenceInputs &inputs) noexcept {
  return inputs.TopScreenProfile && inputs.Interpolation &&
         !inputs.MenuInterpolation && inputs.PausePageActive;
}

// Game thread, after the host frame's guest refreshes: whether this host
// frame's in-between frame holds the step image. The page owners are the ones
// that hide the HUD and route touch to the page (ReadTopScreenPauseDrawInputs);
// they are read only when the rest allows a hold, and an unreadable state
// holds nothing.
[[nodiscard]] inline bool ResolveTopScreenMenuHold(NativeA32Memory &memory,
                                                   TopScreenMenuCadenceInputs inputs) {
  inputs.PausePageActive = false;
  if (inputs.TopScreenProfile && inputs.Interpolation && !inputs.MenuInterpolation) {
    TopScreenPauseDrawInputs pause;
    inputs.PausePageActive =
        ReadTopScreenPauseDrawInputs(memory, &pause) && pause.AnyPageGateActive;
  }
  return HoldTopScreenMenuInBetween(inputs);
}

// Who decides an in-between frame of the 60 FPS presentation.
enum class InBetweenSource : std::uint8_t {
  Governor,      // the pacing governor: full, reduced or dropped
  StepImage,     // the cadence locked at 30 (config/fps60_mode adaptive)
  MenuStepImage, // the TopScreen pause menu at its own cadence
};

[[nodiscard]] constexpr InBetweenSource ResolveInBetweenSource(
    bool cadenceLockedThirty, bool topScreenMenuHold) noexcept {
  return topScreenMenuHold     ? InBetweenSource::MenuStepImage
         : cadenceLockedThirty ? InBetweenSource::StepImage
                               : InBetweenSource::Governor;
}

// What an in-between frame showing the step image again counts as: the
// governor's and the locked cadence's drops stay Dropped (perf interp_dropped,
// a cost the governor learns); the menu's is Other, outside the governor (it
// neither learns the menu's frames nor counts them as drops).
[[nodiscard]] constexpr RenderFrameKind StepImageFrameKind(
    InBetweenSource source) noexcept {
  return source == InBetweenSource::MenuStepImage ? RenderFrameKind::Other
                                                  : RenderFrameKind::Dropped;
}

} // namespace Oot3dNativeGame
