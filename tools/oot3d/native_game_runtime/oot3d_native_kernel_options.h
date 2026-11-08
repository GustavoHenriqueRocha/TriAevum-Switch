#pragma once

// Guest kernel fast paths (SWITCH_PORT.md "Guest kernel fast paths"), each
// switchable for A/B on the console. Neither changes what the guest sees:
// registers, memory, kernel objects, waits and the order threads run in stay
// the same (tests: oot3d_native_a32_ctr_host_tests,
// oot3d_native_a32_second_core_tests).
//
// Switch: sdmc:/switch/oot3dre/config/kernel_<part>; absent = on, "0" = the
// previous code path. Desktop: TRIAEVUM_KERNEL_FAST=1 turns both on,
// TRIAEVUM_KERNEL_<PART>=0/1 one part (both off by default off the Switch).
//
//  wake: config/second_core only. The kernel lock's waiters are woken for
//        the changes they wait for, not at every guest exit: the idle second
//        core used to wake at each of the game thread's SVCs (and each time
//        the game thread readied one of its own threads), take the kernel
//        lock the game thread then needed back, and go back to sleep
//        (oot3d_native_a32_second_core.h, gQuietWakes). perf.txt second_core:
//        core1_wakes / core1_empty_wakes.
//  svc:  the hot SVCs (ArbitrateAddress, SleepThread, WaitSynchronizationN)
//        build no strings per call: their SVC event keeps its numbers and its
//        name is written when the diagnostics read it; handle lookups go
//        through a small exact cache (NativeA32CtrHostConfig::FastSvc).
//        perf.txt guest_kernel: top= (microseconds per call).
//  ui_lock: config/second_core only. The UI submission (HUD build, texture
//        preparation, its render-stream submit) keeps the kernel lock the
//        frame loop holds instead of releasing it and taking it once per
//        guest memory read: on Horizon an unlock with the second core
//        waiting hands it the lock, and the next read then waits for that
//        core's whole SVC or DSP frame. The second core waits at most the
//        submission instead. perf.txt untimed: ui_lock_takes /
//        ui_lock_waits / ui_lock_wait (0 takes with it on).
//  retake: config/second_core with the host audio stream only. The second
//        core's DSP frame (idle work) holds the kernel lock only to claim the
//        frame, resolve the shared regions and signal the interrupt, and to
//        account the regions and statistics afterwards; the mix and the host
//        audio submit run with it released (the game thread used to wait at
//        its next guest exit for the rest of the frame: guest_kernel: lock=
//        of thread 0, ~72 % of dsp_core1_ms in test.11). Only when the
//        interrupt woke no game-thread thread and left nothing for one to
//        take (otherwise the frame keeps the lock). perf.txt second_core:
//        dsp_released / dsp_held_ms / core0_gated / core0_lock_wait_ms.

#include "oot3d_native_offload.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

struct NativeKernelOptions {
    bool Wake = false;
    bool Svc = false;
    bool UiLock = false;
    bool Retake = false;
};

inline NativeKernelOptions ReadNativeKernelOptions() {
    NativeKernelOptions options;
#if defined(__SWITCH__)
    const auto part = [](const char* name) {
        std::ifstream input(std::string("sdmc:/switch/oot3dre/config/kernel_") + name);
        if (!input) return true;
        std::string text;
        std::getline(input, text);
        return ParseOffloadSwitch(text);
    };
#else
    const char* all = std::getenv("TRIAEVUM_KERNEL_FAST");
    const bool allOn = all != nullptr && std::string_view(all) != "" && ParseOffloadSwitch(all);
    const auto part = [allOn](const char* name) {
        std::string variable = "TRIAEVUM_KERNEL_";
        for (const char* c = name; *c != '\0'; ++c)
            variable.push_back(static_cast<char>(*c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c));
        const char* value = std::getenv(variable.c_str());
        return value != nullptr ? ParseOffloadSwitch(value) : allOn;
    };
#endif
    options.Wake = part("wake");
    options.Svc = part("svc");
    options.UiLock = part("ui_lock");
    options.Retake = part("retake");
    return options;
}

} // namespace Oot3dNativeGame
