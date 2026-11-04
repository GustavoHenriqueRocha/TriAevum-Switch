#pragma once

// The console's current clocks for logs/perf.txt (oot3d_switch_clocks.cpp).
//
// Read-only: the MHz come from sys-clk's public IPC ("sys:clk",
// GetCurrentContext), the handheld/dock mode and the performance mode come from the
// applet (values libnx already keeps, no IPC). The app never opens clkrst
// nor pcv and never changes clocks (SwitchWakerHD issue #33: clkrst in the app
// froze the console on wake from sleep; sys-clk is what changes clocks).
//
// Off the Switch everything here is empty.

#include <string>

namespace Oot3dNativeGame {

#if defined(__SWITCH__)
// " clocks: cpu_mhz=1785 gpu_mhz=768 mem_mhz=1600 clk_src=sysclk_real
// op_mode=handheld perf_mode=normal sysclk_profile=handheld sysclk_enabled=1
// sysclk_api=4 clk_ipc_us=40 clk_busy=0" for the window line. Returns the
// previous reading and requests the next one from its own thread (the IPC never runs on the
// game thread; clk_busy=1 if the previous one hasn't returned yet); the session with
// sys-clk is opened the first time and kept. A field that couldn't be read
// is -1 (clk_src=none when sys-clk isn't running).
std::string SwitchClockPerfSummary();
#else
inline std::string SwitchClockPerfSummary() { return {}; }
#endif

} // namespace Oot3dNativeGame
