#pragma once

// The Controllers applet that devkitPro's SDL2 launches from the game loop
// (oot3d_switch_controller_applet.cpp).
//
// switch-sdl2 2.28.5 (SDL_sysjoystick.c, SWITCH_JoystickUpdate) compares
// every open pad's npad device type and style set with the previous update
// and, on any change, calls SWITCH_UpdateControllerSupport, which for a pad
// that is not handheld runs hidLaShowControllerSupportForSystem: a library
// applet launch that waits (libnx libappletLaunch: appletCreateLibraryApplet,
// then appletHolderJoin, no timeout) until the applet closes. It runs inside
// SDL_PumpEvents on the game thread, with SDL's joystick lock held.
// Removing or attaching a Joy-Con, or reconnecting
// controllers from HOME > Controllers while the game runs, froze the game and
// the console (issue #11 and a later report). The link wraps that function
// (-Wl,--wrap=hidLaShowControllerSupportForSystem, CMake/oot3d/game.cmake):
// the applet is never launched from the game, SDL refreshes its pads as after
// a closed applet, and the player pairs controllers from HOME > Controllers.

#include <cstdint>

namespace Oot3dNativeGame {

// How many times SDL asked for the applet since the last call. Game thread
// (the input poll, which writes the game log line).
[[nodiscard]] std::uint32_t TakeSkippedControllerApplets() noexcept;

} // namespace Oot3dNativeGame
