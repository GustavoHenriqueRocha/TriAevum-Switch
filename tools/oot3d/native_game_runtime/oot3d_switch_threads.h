#pragma once

// Switch thread placement (oot3d_switch_threads.cpp). New threads start on
// core 1 or 2, alternating by creation order; the game thread owns core 0.
// A thread whose load matters asks for its core explicitly at its start:
// render worker -> core 2; PICA plan worker and audio -> core 1.
//
// Pins the calling thread to `core` (0-2) when the process core mask allows
// it. False (placement unchanged) when it does not, or off the Switch.
#if defined(__SWITCH__)
bool SetSwitchThreadCore(int core);
#else
inline bool SetSwitchThreadCore(int) { return false; }
#endif

inline constexpr int kSwitchRenderThreadCore = 2;
inline constexpr int kSwitchPicaPlanThreadCore = 1;
inline constexpr int kSwitchAudioThreadCore = 1;
