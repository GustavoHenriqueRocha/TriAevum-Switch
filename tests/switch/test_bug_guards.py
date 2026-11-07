#!/usr/bin/env python3
"""Regression guards for bugs already fixed in the Switch port.

Each guard checks in the source code that the fix for a bug seen on the console
is still in place. They are static checks (they run in seconds, no build): they
don't prove the code is right, but they fail if an upstream merge, a refactor
or a hastily resolved conflict erases the fix. The behavior test for each bug,
when there is one, lives in the target named in `test`.

Usage: python3 tests/switch/test_bug_guards.py [--list]
"""
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@dataclass
class Guard:
    bug: str          # what was seen on the console
    path: str         # file where the fix lives
    pattern: str      # regex that must exist (re.S)
    test: str = ""    # behavior test, when there is one
    forbid: str = ""  # regex that must NOT exist in the same file


GUARDS = [
    # --- 11/07: Upscaling looked live on deko3d; only the OpenGL backend reads config/upscaler ---
    Guard("Video > Upscaling: disabled with a note on deko3d (no FSR step there)",
          "tools/oot3d/native_game_runtime/oot3d_video_settings_panel.cpp",
          r'const bool upscalerApplies = VideoSettings::UpscalerApplies\(Fast::Deko3dRendererSelected\(\)\);\s+'
          r'ImGui::BeginDisabled\(!upscalerApplies\);\s+if \(ImGui::Combo\("Upscaling".*?ImGui::EndDisabled\(\);\s+'
          r'if \(!upscalerApplies\) ImGui::TextDisabled\("%s", VideoSettings::kUpscalerDeko3dNote\);',
          test="oot3d_video_settings_tests (UpscalerApplies)"),
    # --- 11/06: fastmem phase 1, config/fastmem_window (oot3d_switch_guest_window.h) ---
    # Absent must stay today's allocation bit for bit; phase 1 must not touch a shard-visible
    # header (the 256 shards and the PGO profile stay valid).
    Guard("fastmem window, switch absent: a region is allocated as before unless the window takes it",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'const bool windowOwner =\s+GuestWindow::Accepting\(\) &&.*?'
          r'if \(windowOwner\) GuestWindow::Reserve\(region\.Bytes, config\.BaseAddress, config\.Size\);\s+'
          r'region\.Bytes\.assign\(config\.Size, 0\);',
          test="oot3d_switch_guest_window_tests (TestAbsent), sweep of the memory tests with the window off"),
    Guard("fastmem window, switch absent: a copy assignment copies the regions as before",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'if \(GuestWindow::IsOwner\(this\)\) \{\s+CopyRegionsHome\(mRegions, other\.mRegions\);\s+'
          r'\} else \{\s+mRegions = other\.mRegions;',
          test="oot3d_switch_guest_window_tests (TestAbsent, TestRehome)"),
    Guard("fastmem window: on by default on Switch (the file only turns it off or asks for verify), "
          "and no process handle (emulator) keeps it off",
          "tools/oot3d/native_game_runtime/oot3d_switch_guest_window.cpp",
          r'"sdmc:/switch/oot3dre/config/fastmem_window".*?if \(file == nullptr\) \{\s+mode = Mode::On;.*?'
          r'gProcess = envGetOwnProcessHandle\(\);\s+if \(gProcess == INVALID_HANDLE\) \{'),
    Guard("fastmem window: a refused slab mapping stops every new placement (today's allocation from then on)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guest_window.cpp",
          r'if \(!PlatformMapSlab\(index\)\) \{.*?gAccepting\.store\(false, std::memory_order_release\);',
          test="oot3d_switch_guest_window_tests (TestRefusedMapping)"),
    Guard("fastmem window: newlib's free never gets a window buffer",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'void __wrap__free_r\(struct _reent\* reent, void\* pointer\) \{.*?'
          r'if \(Oot3dNativeGame::GuestWindow::Contains\(pointer\)\) \{\s+'
          r'Oot3dNativeGame::GuestWindow::ReleaseAllocation\(pointer\);\s+return;'),
    # --- 11/06: fastmem phase 2, checked window access (ABI 5, every shard rebuilt) ---
    # One flag byte per guest page decides whether a typed access completes inline at window
    # base + address; everything else takes the original slow path (exact faults, observers,
    # deferred writes, watches, traces). An image outside the window (file absent, emulator,
    # refused mapping, copies) points at the all-trap table. mWritten is gone from the stores.
    Guard("fastmem phase 2: a typed load completes inline only where its page's flag allows it, at window base + address",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'if \(value != nullptr && \(address & PageMask\) <= PageSize - sizeof\(T\) &&\s*'
          r'PageAllows\(address, kGuestPageLoadFast\)\) \[\[likely\]\] \{\s*'
          r'\*value = LoadGuestValue<T>\(mWindowBase \+ address\);\s*return true;\s*\}\s*'
          r'return ReadFastSlow<T>\(address, value, faultAddress\);',
          test="oot3d_switch_guest_window_tests (TestPageFlags), memory_lean_diff/run_store_sequence_diff.sh"),
    Guard("fastmem phase 2: a typed store completes inline only where its page's flag allows it, then marks its card",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'if \(\(address & PageMask\) <= PageSize - sizeof\(T\) &&\s*'
          r'PageAllows\(address, kGuestPageStoreFast\)\) \[\[likely\]\] \{\s*'
          r'StoreGuestValue<T>\(mWindowBase \+ address, value\);\s*MarkCard\(address >> PageShift\);\s*'
          r'return true;\s*\}\s*return WriteFastSlow<T>\(address, value, faultAddress\);',
          test="oot3d_switch_guest_window_tests (TestPageFlags), memory_lean_diff/run_store_sequence_diff.sh"),
    Guard("fastmem phase 2: page flags keep their own alias set (union of an enum class), not uint8_t",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'enum class GuestPageFlags : uint8_t \{\};\s*union GuestPageFlag \{\s*GuestPageFlags Bits;'),
    Guard("fastmem phase 2: a flag allows an access only when the page entry's own pointer is the page's window address",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'\(entry\.Read == home \? kGuestPageLoadFast : 0U\) \|\s*'
          r'\(entry\.Write == home \? kGuestPageStoreFast : 0U\)',
          test="oot3d_switch_guest_window_tests (TestPageFlags, TestConflicts), TRIAEVUM_FASTMEM_WINDOW=verify sweep"),
    Guard("fastmem phase 2, window inactive: an image outside the window points at the all-trap table",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'if \(!inWindow \|\| !mOwnPageFlags\) \{\s*'
          r'if \(mPageFlags != sNoFastPages\) mPageFlags = sNoFastPages;',
          test="oot3d_switch_guest_window_tests (TestAbsent, TestConflicts, TestRefusedMapping), sweep with the window off"),
    Guard("fastmem phase 2: a store marks its card only when it is not Dirty already (no memory-wide written mark)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'OOT3D_AOT_HOT void MarkCard\(size_t page\) noexcept \{\s*GuestCard& card = mCards\[page\];\s*'
          r'if \(card\.State != GuestCardState::Dirty\) \[\[unlikely\]\] \{',
          test="oot3d_switch_guest_window_tests (TestPageFlags), oot3d_native_a32_process_tests (TestMemoryStoreCards)",
          forbid=r'mWritten'),
    Guard("fastmem phase 2: the inline paths changed, so the title-module ABI is 5 (rebuild every shard)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'WriteObservationAbiVersion\(\) noexcept \{ return 5U; \}'),
    Guard("fastmem phase 2: the global exclusive monitor serves the single-core path too (value token, no write-generation token)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'bool NativeA32Memory::LoadExclusive\(uint32_t address, uint8_t size,\s*'
          r'uint64_t\* value, uint64_t\* token,\s*uint32_t\* faultAddress\) \{\s*'
          r'if \(token == nullptr \|\| value == nullptr\) \{',
          test="oot3d_native_a32_second_core_tests (TestMonitorInactive), oot3d_native_a32_process_tests (TestMemoryStoreCards)",
          forbid=r'if \(!SecondCore::Active\(\)\)|token != WriteGeneration\(\)'),
    Guard("fastmem phase 2: every guest context switch drops the reservation, every savestate load resets the monitor",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'if \(selectedId != mCurrentThreadId\) \{\s*SecondCore::ClearExclusive\(\);\s*\}.*?'
          r'SecondCore::ResetExclusiveMonitor\(\);\s*if \(SecondCore::Active\(\) && secondCoreReady\)',
          test="oot3d_native_a32_second_core_tests (TestMonitorInactive, TestSchedulerExclusion)"),
    Guard("fastmem phase 2: the fastmem_window log line says whether the game's image takes the window (access=window|slow)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'process\.Memory\(\)\.InGuestWindow\(\) \? " access=window" : " access=slow"'),
    Guard("fastmem phase 2: the store-sequence diff under qemu has no guarded static (its condition variable aborts there)",
          "tools/oot3d/native_a32_runtime/memory_lean_diff/store_sequence_side.cpp",
          r'static bool active = false;\s*if \(!active\) \{\s*SEQ_MONITOR::SetActive\(true\);',
          test="memory_lean_diff/run_store_sequence_diff.sh (TARGET=switch)",
          forbid=r'static const bool active = \['),
    # --- 06/11: second guest core, config/second_core (oot3d_native_a32_second_core.h) ---
    # With it on, STREX must go through the global exclusive monitor (the write-generation token
    # fails under a second writer); since fastmem phase 2 the monitor serves single-core too.
    Guard("second core: STREX is a compare-and-swap under the global exclusive monitor (reservation per host core)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'if \(gExclusiveGranule\[processor\] != granule\) \{.*?ClearExclusiveGranule\(granule\);\s+'
          r'if \(!AtomicCompareExchangeGuest\(bytes, size, token, value\)\)',
          test="oot3d_native_a32_second_core_tests (TestMonitorSemantics, TestMonitorTwoThreads)"),
    Guard("second core: the cooperative scheduler never selects a second-core thread",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'thread\.HostCore == 0U && thread\.Priority < selectedPriority',
          test="oot3d_native_a32_second_core_tests (TestSchedulerExclusion)"),
    Guard("second core: only config/second_core starts it",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'"sdmc:/switch/oot3dre/config/second_core".*?if \(secondCore\) StartSecondCore\(\);'),
    Guard("second core: Run() drops the kernel lock only for the dispatch",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'if \(concurrent\) SecondCore::EnterGuest\(\);.*?const SecondCore::KernelRelease release;\s+'
          r'exit = DispatchGuest\(state, dispatchBudget, false\);\s+\}\s+if \(concurrent\) SecondCore::LeaveGuest\(\);'),
    Guard("second core: the DSP mix updates the shared regions in place only while the sound thread is parked",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(secondCore && runGuest\) \{.*?process\.WaitSecondCoreIdle\(kSecondCoreDspWait\)',
          test="oot3d_native_a32_second_core_tests (TestEventPingPong, TestSleepTimeout)"),
    Guard("second core: savestates hold it at a dispatch boundary",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_savestate.cpp",
          r'renderStream\.Drain\(\);.*?const Oot3dNativeGame::SecondCore::WorldStop secondCoreStop;.*?'
          r'renderStream\.Drain\(\);.*?const Oot3dNativeGame::SecondCore::WorldStop secondCoreStop;',
          test="oot3d_native_a32_second_core_tests (TestSavestate)"),
    # 06/11 (logs13sc): core 0 waited 6-7 ms per frame in the Forest Temple for the sound
    # thread to park before each DSP frame, and audio got worse; the second core now delivers
    # the frames itself, only with its threads parked and nothing an interrupt woke still ready.
    Guard("second core: DSP frames run on the second core only while its threads are parked and no auxiliary work is ready",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'SecondCoreIdleWorkDue\(\) const \{.*?SecondCore::gStopRequests == 0U && !mSecondCoreFault\.has_value\(\) &&\s+'
          r'!AuxiliaryWorkPending\(\) && mSecondCoreIdleWork\.Pending\(\);.*?'
          r'thread = SelectSecondCoreThread\(\);\s+idleWork = thread == nullptr && SecondCoreIdleWorkDue\(\);',
          test="oot3d_native_a32_second_core_tests (TestDspOnSecondCore)"),
    Guard("second core: with the host audio stream the game thread only adds DSP frames (no pump, no flush wait)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(deferredDspAudioFrames != 0U && !dspOnSecondCore &&.*?'
          r'if \(deferredDspAudioFrames != 0U && !dspOnSecondCore\) \{.*?'
          r'if \(dspOnSecondCore && dspAudioFrames != 0U\)\s+Oot3dNativeGame::SecondCore::NotifyKernel\(\);',
          test="oot3d_native_a32_second_core_tests (TestDspOnSecondCore)"),
    Guard("second core: the DSP frames go to the second core only with the host audio stream",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window.cpp",
          r'dspOnSecondCore = hostAudioStream != nullptr;\s+if \(dspOnSecondCore\) \{\s+'
          r'process\.SetSecondCoreIdleWork\('),
    Guard("second core: a core-0 thread above the main thread parking wakes the second core's idle work",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'case NativeA32HostAction::Wait:\s+state\.r\[15\] = resumePc;\s+status = NativeA32ThreadStatus::Waiting;\s+'
          r'NoteCore0ThreadParked\(\);'),
    # --- 07/11: L3+R3 > Video > GPU clock (config/gpu_performance, off by default) ---
    # Only the handheld (Normal) configuration, only the official 0x92220008, only when asked.
    Guard("GPU clock High: only the handheld apm configuration, the official 0x92220008",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'apmSetPerformanceConfiguration\(ApmPerformanceMode_Normal, status\.Requested\)',
          test="oot3d_fps_counter_tests (GpuPerformance)",
          forbid=r'apmSetPerformanceConfiguration\(ApmPerformanceMode_Boost|clkrstInitialize|pcvInitialize'),
    Guard("GPU clock: Standard without a prior High opens no apm session (default unchanged)",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'if \(!high && !sessionOpen\) \{\s+return status;'),
    Guard("GPU clock: config/gpu_performance applied at start and from the menu",
          "tools/oot3d/native_game_runtime/oot3d_video_settings_panel.cpp",
          r'mGpuHigh = ReadGpuPerformanceHigh\(mConfig\);\s+if \(mGpuHigh\) mGpuStatus = ApplyGpuPerformance\(true\);.*?'
          r'mGpuStatus = ApplyGpuPerformance\(mGpuHigh\);'),
    # --- 07/11: ZCULL on deko3d only with config/deko3d_zcull (decal tiles lost in Aurora's renderer) ---
    Guard("ZCULL: off unless config/deko3d_zcull exists (decal tiles lost with it on)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'"sdmc:/switch/oot3dre/config/deko3d_zcull", "rb"\)\) \{\s+std::fclose\(file\);\s+mZcull = true;\s+\}.*?'
          r'if \(!mZcull\) \{\s+queueMaker\.flags \|= DkQueueFlags_DisableZcull;',
          test="oot3d_deko3d_backend_tests (TestDepthBreakdown: the depth line that measures it)"),
    # --- 07/11: depth reset as fast clears (config/deko3d_clear_draw), off by default, only where provable ---
    Guard("clear draw: the depth reset becomes clears only with config/deko3d_clear_draw (off by default)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'const auto action = mClearDraw \? plan\.Action : Deko3d::PicaClearDrawAction::Draw;',
          test="oot3d_deko3d_clear_draw_tests, test_clear_draw_tev.py"),
    Guard("clear draw: a constant colour only away from 8-bit rounding boundaries (float32 must agree)",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_clear_draw.h",
          r'inline constexpr double kPicaTevRoundMargin = 0\.01;.*?'
          r'if \(std::fabs\(scaled - std::floor\(scaled\) - 0\.5\) < kPicaTevRoundMargin\) \{\s+return false;',
          test="oot3d_deko3d_clear_draw_tests (TestConstantTevAgainstFloat32), test_clear_draw_tev.py"),
    Guard("ZCULL: draws whose program writes gl_FragDepth invalidate ZCULL around them (decal tiles)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'const bool zcullGuard =\s+mDevice\.Zcull\(\) && writesFragDepth.*?'
          r'if \(zcullGuard\) \{\s+mDevice\.InvalidateZcull\(\);\s+\}\s+if \(draw\.Indexed\).*?'
          r'if \(zcullGuard\) \{\s+mDevice\.InvalidateZcull\(\);'),
    # --- 07/11: deko3d barriers. Uploads still need the Full barrier (copy engine before 3D sampling). ---
    Guard("deko3d uploads: a Full barrier with Image|Descriptors invalidation before the next draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'DkBarrier mImageBarrierMode = DkBarrier_Full;\s+'
          r'uint32_t mImageBarrierFlags = DkInvalidateFlags_Image \| DkInvalidateFlags_Descriptors;'),
    Guard("deko3d: every queue submit is flushed (postSubmitFlush is what replaces the frame-top barrier)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'dkQueueSubmitCommands\(mQueue, list\);\s+dkQueueFlush\(mQueue\);'),
    Guard("deko3d pack images: finished ones upload at frame top, never one barrier per draw mid-frame",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'Evict\(\);\s+BeginScene\(\);\s+UploadReadyReplacements\(\);\s+\}',
          forbid=r'PollOrQueue\(\s*native\.PackHash, static_cast<uint16_t>\(view\.Width\)'),
    # --- 07/11: colour compression (config/deko3d_compression "0" = off). The display controller can't
    # read compressed images: window images handed to nwindow without dkQueuePresentImage stay uncompressed.
    Guard("deko3d compression: window images compressed only when dkQueuePresentImage decompresses them",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'layoutMaker\.flags = DkImageFlags_UsageRender \| DkImageFlags_UsagePresent;\s+'
          r'if \(!mPresentThreaded\) \{\s+layoutMaker\.flags \|= DkImageFlags_HwCompression;'),
    Guard("deko3d compression: scene colour and display images follow config/deko3d_compression",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_targets.cpp",
          r'DkImageFormat_RGBA8_Unorm, ColorFlags\(\*mDevice\), target\.Extent\.Width.*?'
          r'DkImageFormat_RGBA8_Unorm, ColorFlags\(\*mDevice\), allocWidth'),
    # --- 06/11: L3+R3 > Video > FPS (30 <-> 60 interpolated) applied only after a restart ---
    # Both choices simulate at 30 Hz; the launch fixed the pacing mode and the 30 FPS launch's
    # native30_no_interpolation contract forbade interpolation, so the menu only wrote config/fps60.
    Guard("FPS in L3+R3 > Video applied only after restarting: the menu publishes it, the frame loop applies it at the next frame boundary",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'TakeLiveFrameRateMode\(\s*Oot3dNativeGame::LiveFrameRate\(\), liveFrameRateRevision,.*?'
          r'launchFrameRateMode = \*live;.*?'
          r'ResolvePresentationPacingPolicy\(EffectiveFrameRateMode\(\)\)',
          test="triaevum_presentation_clock_tests (live frame rate choice, no catch-up across the switch)"),
    Guard("FPS in L3+R3 > Video: the Switch launches native30_interpolated at both rates (30 FPS presents Original30)",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'const_cast<char\*>\("--gameplay-timing"\),\s+const_cast<char\*>\("native30_interpolated"\),',
          forbid=r'"native30_no_interpolation"'),
    Guard("FPS in L3+R3 > Video: the menu publishes the choice (and still writes config/fps60)",
          "tools/oot3d/native_game_runtime/oot3d_video_settings_panel.cpp",
          r'if \(changed\) mError = mSettings\.Save\(mConfig\);.*?'
          r'if \(rateChanged\) LiveFrameRate\(\)\.Publish\(mSettings\.Fps60\);'),
    Guard("FPS switched live: every governor starts over at the new rate (no lock, estimate or interval of the old one)",
          "tools/oot3d/native_game_runtime/oot3d_native_cadence_governor.h",
          r'void Configure\(const Fps60ModeConfig &config, double refreshMs\) noexcept \{.*?'
          r'mHasPresent = false;.*?mOverheadEmaMs = 0\.0;\s+\}',
          test="oot3d_native_cadence_governor_tests (TestLiveRateSwitchMatchesFreshStart, TestLiveRateSwitchFirstPresents)"),
    Guard("FPS switched live: the in-between governor forgets the step cost of the old rate",
          "tools/oot3d/native_game_runtime/oot3d_native_interpolation_pacing.h",
          r'mMeasured = \{\};\s+mStepCostMs = 0\.0;',
          test="oot3d_native_cadence_governor_tests (TestLiveRateSwitchMatchesFreshStart)"),
    Guard("FPS switched live: the render thread reconfigures pacing, DRS and cadence through one function",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'drsRenderBusyAverageMs = 0\.0;.*?Oot3dNativeGame::ConfigureRatePacing\(rateHz, pacing, interpolationPacing,',
          test="oot3d_native_cadence_governor_tests (TestLiveRateSwitchMatchesFreshStart)"),
    Guard("FPS switched live: grass takes the new rate's budgets; its stream memory holds both caps",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'void Deko3dSetGrassFrameRate\(bool fps60\) \{.*?ApplyProfile\(\*state, fps60, state->Distance\);.*?'
          r'state->SlotInstances = Oot3d::GrassSwitchStreamCapacity\(\);',
          test="oot3d_grass_switch_tests (FrameRateSwitchesLiveAsAStartWould)"),
    Guard("config/pacer_vsync (shipped on): the anchored pacer waited one display period whatever the target, so 30 FPS presented every vsync",
          "tools/oot3d/native_game_runtime/oot3d_native_presentation_pacer.cpp",
          r'std::max\(periods, 1U\) \*\s+VsyncPeriodsPerPresentation\(mTargetRateHz, vsync\.PeriodNs\);',
          test="oot3d_frame_spike_tests (TestAnchoredPacerHonoursTargetRate)"),
    # --- 05/11: optional deko3d lighting, config/lighting (gfx_deko3d_lighting.h) ---
    Guard("0.1.2-test.2: Scene lights said 'needs the Modern lighting model' under deko3d per-pixel (the model lived outside the graphics settings)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'SetCapability\(Oot3d::GraphicsCapability::NativeSurfaceLighting, surfaceLighting,.*?'
          r'graphics\.Apply\(Deko3d::WithLightingMode\(graphics\.Snapshot\(\), mode\), false\);.*?'
          r'Deko3d::LightingModeOf\(rendering\.Value\)',
          test="oot3d_deko3d_lighting_tests (TestModelCapabilityAndPanel)"),
    Guard("0.1.2-test.2: Scene lights tab: one activity rule, and controls the Switch renderer cannot draw are marked, not ignored",
          "tools/oot3d/native_game_runtime/oot3d_scene_environment_panel.cpp",
          r'SceneEnvironmentActive\(supported, settings, graphics\.NativePresentationOverrideActive\(\)\).*?'
          r'Unavailable on the Switch renderer: placed lights.*?Unavailable on the Switch renderer: it draws no',
          test="oot3d_deko3d_lighting_tests (TestControlsReachTheBlock)"),
    Guard("[decision] deko3d lighting off: native keys and shaders untouched (the variant path only runs with a mode on)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'mCounters\.Programs\.Count\(program\.Pick\);\s*'
          r'if \(mLightingMode != Deko3d::LightingMode::Off && program\.Vertex != nullptr && program\.Fragment != nullptr\)',
          test="oot3d_deko3d_lighting_tests (TestOffIsNative)"),
    Guard("[decision] deko3d lighting: a variant missing from the pack is recorded and the draw keeps both native shaders (never a stall, never a mixed pair)",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_lighting.h",
          r'const T\* shader = lookup\(ShaderTextKey\(stage, \*variant\).*?'
          r'if \(variantVertex != nullptr && variantFragment != nullptr\).*?'
          r'\+\+counters\.DrawsNative;\s*return \{nativeVertex, nativeFragment, false\};',
          test="oot3d_deko3d_lighting_tests (TestLookupAndFallback)"),
    Guard("[decision] deko3d per-pixel lighting: upstream's surface block at binding 16 is remapped to fragment slot 5 (deko3d has 0-15)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting.cpp",
          r'layout\(binding=5, std140\) uniform PicaSurfaceLighting \{',
          test="oot3d_deko3d_lighting_tests (TestBlockLayoutAndBinding)",
          forbid=r'binding\s*=\s*16'),
    Guard("deko3d per-pixel lighting: the draw binds the surface block at kSurfaceLightingBinding (5)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'dkCmdBufBindUniformBuffer\(cmd, DkStage_Fragment, Deko3d::kSurfaceLightingBinding, res\.SurfaceLighting,'),
    # --- 0.1.2-test.7: per-pixel dots, 8x4-pixel (one warp) tiles of wrong lighting on Link and floors ---
    Guard("per-pixel dots: the interpolants are read once at the top of the per-pixel body, never per use inside the light loops or lane-dependent branches",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting.cpp",
          r'PicaSurfaceFragment pica_surface_fragment\(\) \{.*?fragment\.view = clamp\(pica_view, -finite, finite\);.*?'
          r'PicaSurfaceFragment pica_surface_inputs = pica_surface_fragment\(\);',
          test="test_perpixel_lighting.py (test_interpolants_read_once_at_the_top, test_branches_are_uniform)",
          forbid=r'pica_surface_unit\(pica_view\)|if \(dot\(n, n\) < 0\.5'),
    # --- 0.1.2-test.8: per-pixel is the GPU limit in Forest Temple room 8 (busy_ms 14.0 at 2.25, 305 render-late frames) ---
    Guard("per-pixel cost: the rim's view and Fresnel terms are computed once per pixel, not per light; the light loops are written out",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting.cpp",
          r'if \(pica_surface\.rim\.x > 0\.0\) \{\s*fragment\.view_unit = pica_surface_unit\(fragment\.view\);\s*'
          r'fragment\.rim_fresnel = pica_surface_rim_fresnel\(fragment\.normal, fragment\.view_unit\);',
          test="test_perpixel_lighting.py (test_valid_inputs_match_test8_exactly)",
          forbid=r'for \((int|uint) slot = 0u?; .*?slot\)|pica_surface_rim_fresnel\(n, v\) \* back'),
    Guard("per-pixel cost: the draw uses its block's path variant (environment, baked) when the pack has it, the generic one otherwise",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'Deko3d::BuildPerPixelSurface\(draw, mLightingEnvironment, mLightingView,.*?'
          r'path = Deko3d::ClassifyPerPixelSurface\(mSurfaceLighting\);.*?'
          r'ResolveLightingFragment\(Deko3d::LightingMode::PerPixel, fragmentSource,\s*fragmentIdentity, path\)\) \{\s*'
          r'fragment = special;\s*\} else \{\s*path = Deko3d::PerPixelPath::Generic;',
          test="test_perpixel_lighting.py (test_path_variants_match_test8_exactly), oot3d_deko3d_lighting_tests (TestPerPixelPaths)"),
    Guard("per-pixel cost: the pack tool writes every per-pixel path variant of every fragment text",
          "tools/oot3d/deko3d_lighting_variants.cpp",
          r'static_cast<size_t>\(Fast::Deko3d::PerPixelPath::Count\).*?LightingFragmentVariant\(mode, text, path\)'),
    # --- lighting3: Forest Temple room 8, 16.7 ms off / 38.4 ms per-pixel busy_ms (spilling parametric programs) ---
    Guard("lighting3: the parametric lighting copied its 256-byte program to a local; the loop's dynamic light index put it in scratch (105 GPRs, 4096 B/warp)",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_fragment_lighting_gen.cpp",
          r'parametric \? Fast::Renderer3ds::PicaLightingUniformProgramFunctions.*?'
          r'parametric \? UniformProgramBody\(\)',
          test="oot3d_native_pica_program_descriptor_tests (TestParametricRegisterPressure)"),
    Guard("lighting3: the parametric TEV copied its constants and selected sources with switches (97 GPRs unlit)",
          "runtime/three_ds_recomp/src/fast/renderer3ds/pica_tev_program.cpp",
          r'vec4 pica_evaluate_tev_uniform\(PicaTevInputs inputs\).*?fragment_uniforms\.tev_constants\[stage\]',
          test="oot3d_native_pica_program_descriptor_tests (TestParametricRegisterPressure)",
          forbid=r'case 14u: return inputs\.constants\[stage\];'),
    Guard("lighting3: the no-LUT lit parametric base (console-recorded 08743ed174a0fce1) is dumped into the pack",
          "tools/oot3d/deko3d_parametric_glsl_dump.cpp",
          r'\{true, false, false\}, \{true, true, false\}'),
    Guard("lighting3: a draw whose base fragment program spills keeps native shading (its variants spill too)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'if \(program\.Base == Deko3d::PicaBaseClass::Spilling\) \{.*?\+\+mLightingCounters\.DrawsSpillingBase;',
          test="oot3d_deko3d_backend_tests (TestFrameBreakdown)"),
    Guard("lighting3: per-pixel on every draw that writes depth; the blend flag must not exclude it (logs14: 0 lit draws, 81% blended)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting_draw.cpp",
          r'if \(!PerPixelCostAdmits\(draw\.DepthWriteEnabled\)\) return PerPixelRefusal::NoDepthWrite;',
          test="oot3d_deko3d_lighting_tests (TestPerPixel)",
          forbid=r'PerPixelCostAdmits\(draw\.Blend\.Enabled'),
    Guard("lighting by frame kind: the fps60 in-between frames' per-pixel draws are counted apart from the real ones, through the host-tested decision (0.1.2-test.7 blink)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'auto& kind = mInBetweenSample \? mInBetweenLighting : mRealLighting;.*?'
          r'Deko3d::AdmitPerPixelDraw\(draw\).*?Deko3d::BuildPerPixelSurface\(draw, mLightingEnvironment, mLightingView,.*?'
          r'kind\.Light\(mSurfaceLighting\);.*?"lighting in-between \(%s\): %s"',
          test="oot3d_deko3d_inbetween_lighting_tests"),
    Guard("lighting by frame kind: deko3d learns the frame kind from the published temporal sample",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'BeginLightingSample\(sample\.Synthetic\);'),
    Guard("per-pixel blink (test.8): every per-pixel decision of a presented frame feeds the transient watch, "
          "and a one-frame transient is logged with its device frame",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'const auto refuse = \[&\]\(Deko3d::PerPixelRefusal refusal\) \{.*?'
          r'Deko3d::ClassifyLostWorldComposition\(draw, refusal\).*?mLightingWatch\.Refused\(draw, refusal\);.*?'
          r'mLightingWatch\.Lit\(draw, mSurfaceLighting\);.*?mLightingWatch\.Native\(draw\);.*?'
          r'void GfxRenderingAPIDeko3d::BeginLightingSample\(bool inBetween\) \{\s+mInBetweenSample = inBetween;.*?'
          r'mLightingWatch\.Begin\(mDevice\.Frame\(\), inBetween\).*?"lighting transient: %s"',
          test="oot3d_deko3d_inbetween_lighting_tests (TestLightingTransients)"),
    Guard("per-pixel blink (test.8): a world model refused as not_scene_world is classified by its composition "
          "(a list submitted in the HUD phase, a UI span, no span, control flow)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting_draw.cpp",
          r'std::optional<LostWorldComposition> ClassifyLostWorldComposition\(.*?'
          r'refusal != PerPixelRefusal::NotSceneWorld \|\| hooks\.SurfaceProgram\(\) == nullptr \|\|.*?'
          r'LostWorldComposition::UiList.*?LostWorldComposition::NoSpan',
          test="oot3d_deko3d_inbetween_lighting_tests (TestLostWorldComposition)"),
    Guard("per-pixel blink (test.8): perf.txt reports the composition spans lost per window",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'perf << " composition: spans_consumed=".*?" spans_partial=".*?" spans_overlapping=".*?'
          r'" spans_unattributed=".*?" spans_evicted=".*?" cmb_invalid="'),
    Guard("lighting3: logs/deko3d.txt counts each issued draw by base program class (per-frame breakdown)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'mFrameDraws\.Current\.Count\(program\.Base, program\.Lit, draw\.Blend\.Enabled, pixels\);',
          test="oot3d_deko3d_backend_tests (TestFrameBreakdown)"),
    # --- 05/11: a guest memory fault in the final battle said only "kind=4 pc=0x004C0A8C" ---
    # The text parser (TextTokenUtf8_ParseNext) met 7f + a code past its table and copied argument words
    # over the stack; the log could not tell which thread, which text or which caller.
    Guard("guest fault: every fault carries the report (thread, registers, backtrace, memory, area)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'NativeA32ProcessRunResult NativeA32Process::Fault\(.*?'
          r'result\.GuestReport = FormatGuestFaultReport\(mMemory, facts\);',
          test="oot3d_native_guest_fault_report_tests, oot3d_native_a32_process_tests (TestFallbackBoundary)"),
    Guard("guest fault: the report and the last perf windows go to the game log before the fatal error",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window.cpp",
          r'if \(!result\.GuestReport\.empty\(\)\) \{\s+LogFatalErrorForDemo\(\s+'
          r'\(result\.GuestReport \+ Oot3dNativeGame::GuestFaultPerf\(\)\.Format\(\)\)\.c_str\(\)\);\s+\}\s+'
          r'throw std::runtime_error\("native A32 process faulted: "'),
    Guard("guest fault: the perf windows of the report are recorded with the GPU numbers of the dkperf line",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'Oot3dNativeGame::GuestFaultPerf\(\)\.Record\(.*?PerfLineValue\(dkperf, "busy_ms"\)'),
    Guard("guest fault: the report gives the area as numbers, never the scene's name",
          "tools/oot3d/native_game_runtime/oot3d_native_guest_fault_report.h",
          r'"  area: scene=0x%02X room=%d\\n"',
          forbid=r'FormatNativeArea|NativeSceneStem'),
    Guard("PT-BR patch: 7f0e/7f0d carry the 5 argument bytes of the game's table (6 put the next 7f in the sound code)",
          "tools/oot3d/ptbr/msgs.py",
          r'0x0d:5,0x0e:5,',
          test="tools/oot3d/ptbr/test_ptbr.py",
          forbid=r'0x0e:6'),
    Guard("PT-BR patch: a message the game cannot parse stops the romfs build",
          "tools/oot3d/ptbr/assemble.py",
          r'error = grammar_error\(new_payload\).*?raise SystemExit\(f\'\{len\(unparsable\)\} PT-BR messages'),
    # --- 05/11: PGO collection on the console (oot3d_whole_aot_pgo.cpp, from sa2-nx/nfsmw-nx) ---
    Guard("PGO generate: value profiling read tpidr_el0 (0 on Horizon) and needs atomics; counters go in a section",
          "CMake/oot3d/native_runtime.cmake",
          r'-fprofile-update=single\s+-fno-profile-values\s+-fprofile-info-section=gcov_info\)',
          test="test_whole_aot_pgo_merge"),
    Guard("PGO use: counts taken without atomics are not flow-consistent; GCC stops without -fprofile-correction",
          "CMake/oot3d/native_runtime.cmake",
          r'-fprofile-partial-training\s+-fprofile-correction\s+-fno-profile-values'),
    Guard("PGO dumps: a HOME close mid-dump must keep the last complete stream (.tmp, then remove + rename: Horizon cannot rename over a file)",
          "tools/oot3d/native_game_runtime/oot3d_whole_aot_pgo.cpp",
          r'const std::string temporary = gStream \+ ".tmp";.*?std::remove\(gStream\.c_str\(\)\);\s+'
          r'ok = std::rename\(temporary\.c_str\(\), gStream\.c_str\(\)\) == 0;',
          test="test_whole_aot_pgo_merge (SessionStreamsTest)"),
    Guard("PGO dumps: the focus-loss hook (game thread) only wakes the writer thread, never writes",
          "tools/oot3d/native_game_runtime/oot3d_whole_aot_pgo.cpp",
          r'void OnAppletHook\(AppletHookType hook, void\*\) \{.*?gDumpRequested = true;\s+\}\s+gWake\.notify_one\(\);\s+\}',
          forbid=r'void OnAppletHook[^}]*DumpNow'),
    # --- 05/11: NRO code on 2 MB blocks (oot3d_switch_code_pages.cpp, from sa2-nx) ---
    # The swap unmaps the code while it runs: it must be the first constructor
    # (no other code or thread may run meanwhile), every later constructor that
    # depends on the final mapping runs at 102, and once moved hbloader must
    # never get control back (its own unmap fails and it aborts).
    Guard("code pages: the swap must be the first constructor (101), before any other start-up code",
          "tools/oot3d/native_game_runtime/oot3d_switch_code_pages.cpp",
          r'\[\[gnu::constructor\(101\)\]\] void RemapCodeOnBlocks\(\)',
          test="oot3d_switch_code_pages_tests"),
    Guard("code pages: the TLS mirror must run after the swap (102, not 101)",
          "tools/oot3d/native_game_runtime/oot3d_switch_threads.cpp",
          r'__attribute__\(\(constructor\(102\)\)\) void MirrorMainThreadTlsPointer\(\)',
          forbid=r'constructor\(10[01]\)'),
    Guard("code pages: guard_alloc's early start must run after the swap (102, not 101)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'\[\[gnu::constructor\(102\)\]\] void StartEarly\(\)',
          forbid=r'constructor\(10[01]\)'),
    Guard("code pages: after the text moved, a return to hbloader aborts (hbl error 24): exit mode 1 for the whole process",
          "tools/oot3d/native_game_runtime/oot3d_switch_code_pages.cpp",
          r'if \(textResult != 0\) \{.*?return Fallback\("the kernel refused the text swap", textResult\);\s+\}'
          r'(\s+//[^\n]*)*\s+__nx_applet_exit_mode = 1;'),
    Guard("code pages: the swap runs from a page of its own (the code cannot unmap itself)",
          "tools/oot3d/native_game_runtime/oot3d_switch_code_pages.cpp",
          r'MapCodePage\(process, kStub, sizeof\(kStub\), &stub\).*?reinterpret_cast<SwapFunction>\(stub\.Va\)'),
    Guard("code pages: config/no_code_hugepages turns it off and the game log says what happened",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'SPDLOG_WARN\("\{\}", Oot3dNativeGame::FormatCodePagesStatus\(',
          test="oot3d_switch_code_pages_tests (TestFormat)"),
    # --- 05/11: every whole-AOT indirect call searched 162,757 entries ---
    # The flat dispatch table (one slot per guest code word, filled once at
    # static initialization) replaces the page bucket and std::lower_bound;
    # the indirect profile stays opt-in (config/indirect_profile).
    Guard("whole-AOT indirect calls must look up the flat dispatch table, not search the entries",
          "tools/oot3d/native_a32_runtime/whole_aot_cpp.py",
          r'"    const uint32_t slot = std::rotr\(pc - kDispatchCodeBase, 2\);",.*?'
          r'"    const uint32_t index = gDispatchSlots\[slot\];",',
          test="tools/oot3d/native_a32_runtime/test_whole_aot_cpp.py (test_flat_dispatch_table_finds_exactly_the_entries)",
          forbid=r'kDispatchPageOffsets|"        begin, end, pc,"'),
    Guard("the indirect call profile must stay opt-in (config/indirect_profile): the dispatcher records only while it is installed",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'exists\("sdmc:/switch/oot3dre/config/indirect_profile", profileError\)\) \{\s+indirectProfile\.emplace\(\);',
          test="oot3d_native_perf_diagnostics_tests"),
    # VLDM/VSTM/VPUSH/VPOP walked the page table once per word; the span path
    # walks it once when the whole range lies on one page.
    Guard("VFP multiple transfers must take the single-page span path before the word-by-word one",
          "tools/oot3d/native_a32_runtime/whole_aot_cpp.py",
          r'body\.extend\(_vfp_word_span\(False, first, words, per_word\)\).*?'
          r'body\.extend\(_vfp_word_span\(True, first, words, per_word\)\).*?'
          r'body\.extend\(_vfp_word_span\(load, first, words, per_word\)\)',
          test="tools/oot3d/native_a32_runtime/test_whole_aot_vfp_memory_execution.py"),
    # Store cards replaced the per-store counter. A u8 card would alias every
    # host value in GCC (reloads after each guest store); a crossing store
    # must mark both pages; the write generation must still change on any write.
    # Since fastmem phase 2: one flat two-bit card per page (bit 0 for the range
    # certificates, bit 1 for the write generation), each taken atomically.
    Guard("store cards must keep their own alias set (union of an enum class), not uint8_t",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'enum class GuestCardState : uint8_t \{ Clean = 0, RangeWritten = 1, Written = 2, Dirty = 3 \};\s*'
          r'union GuestCard \{\s*GuestCardState State;',
          test="oot3d_native_a32_process_tests (TestMemoryStoreCards)",
          forbid=r'\+\+mRangeGeneration;'),
    Guard("a write must mark the card of every page it covers (crossing stores mark both pages)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.h",
          r'void NoteWrite\(uint32_t address, size_t size\) noexcept \{\s*'
          r'const size_t last = \(uint64_t\{address\} \+ size - 1U\) >> PageShift;\s*'
          r'for \(size_t page = address >> PageShift; page <= last; \+\+page\)',
          test="oot3d_native_a32_process_tests (TestMemoryStoreCards)"),
    Guard("WriteGeneration must step whenever a write happened since the last observation (savestates)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'if \(TakeWrittenCards\(\)\) \{\s*__atomic_add_fetch\(&mWriteGeneration, 1U, __ATOMIC_RELAXED\);',
          test="oot3d_native_a32_process_tests (TestMemoryStoreCards), oot3d_switch_guest_window_tests (TestPageFlags)"),
    Guard("the two generations take their own card bit with an atomic and-fetch (a racing store's mark survives)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'__atomic_fetch_and\(&words\[word\], ~written, __ATOMIC_ACQ_REL\).*?'
          r'TakeCardBit\(mCards\[page\], GuestCardState::RangeWritten\)',
          test="oot3d_switch_guest_window_tests (TestPageFlags, TestTwoCores)"),
    # --- 05/11: dsp_mix 0.45 -> 1.3-1.6 ms per frame in battles (logs8, Forest Temple Wolfos) ---
    # The DSP frame runs on core 0 while the guest waits. The shared regions are
    # processed in the guest's bytes (no 4 x 32 KiB of copies per frame) with the
    # same read/write accounting; ADPCM decodes are shared between sources.
    Guard("dsp_mix: the DSP shared regions were copied in and out whole every frame (128 KiB of memcpy on core 0)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_dsp_hle.cpp",
          r'memory\.BeginUpdate\(kDspRegion0Address, regionBytes\).*?'
          r'memory\.EndUpdate\(readAddress, regionBytes\) \|\|\s*!memory\.EndUpdate\(writeAddress, regionBytes\)',
          test="oot3d_native_audio_dsp_mix_tests",
          forbid=r'memory\.(Read|Write)\('),
    Guard("the in-place DSP region update must account its write-back exactly as WriteBytes (store cards, observers)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_memory.cpp",
          r'bool NativeA32Memory::EndHostUpdate\(.*?RecordTracedWrite\(address, [^;]*;\s*\}\s*'
          r'NoteWrite\(address, size\);\s*NotifyObservedWrite\(address, size\);',
          test="oot3d_native_audio_storage_tests"),
    Guard("shared ADPCM decodes must compare the encoded bytes in full and stay within their memory budget",
          "tools/oot3d/third_party/azahar_audio/audio_core/adpcm_decode_cache.h",
          r'std::memcmp\(entry->encoded\.data\(\), encoded, bytes\) == 0.*?while \(samples_ > kBudgetSamples',
          test="oot3d_native_audio_dsp_mix_tests"),
    # --- 05/11: console logs could not name the slow area nor the actors' cost ---
    # Area (scene, room) read once per host frame into hitches.txt and perf.txt;
    # the actor cost profile adds its block-entry hooks only with
    # config/actor_profile, so the default game observes nothing new.
    Guard("hitches.txt and perf.txt lines did not say which area was slow",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'frameArea = Oot3dNativeGame::ReadNativeArea\(processStorage->Memory\(\)\).*?'
          r'hitchArea = area;.*?perf << \' \' << areaText << " scene_changes="',
          test="oot3d_native_perf_diagnostics_tests"),
    Guard("hitches.txt lines must end with the area of their frame",
          "tools/oot3d/native_game_runtime/oot3d_hitch_recorder.h",
          r'entry \+= FormatNativeArea\(sample\.Area\);',
          test="oot3d_hitch_recorder_tests"),
    Guard("the actor profile's hooks must stay opt-in (config/actor_profile): each hook costs a guest flush and a policy call",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'exists\("sdmc:/switch/oot3dre/config/actor_profile", profileError\)\) \{.*?'
          r'NativeActorProfile::HookPcs\(\);\s+blockEntryHooks\.insert',
          test="oot3d_native_perf_diagnostics_tests"),
    Guard("perf.txt guest_in_pica was the running total in seconds, not ms per frame",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'guest_in_pica="\s*<< \(\(phaseTiming\.GuestInsidePicaSubmitSeconds - windowGuestInPica\) \* 1000\.0 / hostFrames\)',
          forbid=r'guest_in_pica=" << phaseTiming\.GuestInsidePicaSubmitSeconds'),
    Guard("perf.txt threads: printed na for every thread in a session's first window",
          "tools/oot3d/native_game_runtime/oot3d_switch_threads.cpp",
          r'record\.HasLastTicks = ReadThreadTicks\(self, &record\.LastTicks\);',
          forbid=r'gLastSummaryTick'),
    # --- 04/11: dungeon map (pause, TopScreen) stuck on the floor it opens on ---
    # 0x00442F38 (PauseDungeonMap_HandleCursorInput) is the floor cursor for
    # D-pad/analog stick (Up/Down change the floor via SetSelectedFloor); touch is
    # 0x004435B0. The TopScreen port made 0x00442F38 return immediately.
    Guard("dungeon map in the pause menu didn't change floors with the analog stick (TopScreen skipped 0x00442F38)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_candidate_dispatch.cpp",
          r'PauseDungeonMap_HandleCursorInput \(0x00442F38\) is not intercepted',
          forbid=r'kPauseDungeonMapCursorInput|0x00442F38U'),
    Guard("dungeon map: 0x00442F38 must not go back into the TopScreen block lists",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'kPauseLowerScreenBlock,\s+kPauseUiNativeUpdateCall,',
          forbid=r'kPauseDungeonMapCursorInput|0x00442F38'),
    # --- 04/11: "cannot replace native control config" when saving controls in the dev menu ---
    # The Horizon FS doesn't rename over an existing file (EEXIST): delete the
    # destination and rename again (as Ship::WriteConfigFileAtomically already did).
    Guard("saving controls in the dev menu failed on Switch (rename over oot3d_controls.json gives EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_native_control_config.cpp",
          r'#if defined\(__SWITCH__\).*?replaceError == std::errc::file_exists.*?std::filesystem::remove\(path'),
    Guard("dev warp didn't save developer-tools.json on Switch (the .tmp was left behind)",
          "tools/oot3d/native_game_runtime/oot3d_dev_warp.cpp",
          r'Ship::WriteConfigFileAtomically\(mStorage,',
          forbid=r'std::filesystem::rename\(temporary, mStorage'),
    Guard("savestate preferences didn't save on Switch (rename over an existing file gives EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_savestate_library.cpp",
          r'Ship::WriteConfigFileAtomically\(mPreferencesPath,',
          forbid=r'std::filesystem::rename\('),
    Guard("savestate didn't overwrite the existing file on Switch (EEXIST)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_savestate.cpp",
          r'#if defined\(__SWITCH__\).*?renameError == std::errc::file_exists.*?std::filesystem::remove\(destination'),
    # --- 05/11: "native PICA color capture failed" on every save state with deko3d ---
    Guard("deko3d: save states failed (\"native PICA color capture failed\"): the backend kept the "
          "interface defaults, which refuse to capture and restore render targets",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'bool CapturePicaColorTargets\(std::vector<GfxNativePicaRenderTargetColorSnapshot>& snapshots,'
          r'\s+std::string\* error = nullptr\) override;'
          r'\s+bool RestorePicaColorTargets\(std::span<const GfxNativePicaRenderTargetColorSnapshot> snapshots,'
          r'\s+std::string\* error = nullptr\) override;',
          "oot3d_deko3d_backend_tests (TestSavestateRoundTrip)"),
    Guard("deko3d save states: depth/stencil read back in a probed Z24S8 layout (an unrecognised one fails the "
          "save), and Zcull forgets the creation clear after a restore writes the depth",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_targets.cpp",
          r'dkCmdBufCopyBufferToImage\(cmd, &depth, &target\.DepthView, &rect, 0U\);.*?'
          r'DkInvalidateFlags_Image \| DkInvalidateFlags_Zcull\);.*?'
          r'const auto layout = IdentifyDepthStencilWordLayout\(clearedWords\.front\(\)\);',
          "oot3d_deko3d_backend_tests (TestSavestateDepthWords)"),
    # --- 07/11: deko3d top screen at one sample per presented line (config/deko3d_line_samples) ---
    # GpuScout: at 3x the top target was 1440x1280 for 720 lines; the game's second sample per line
    # was rendered at host resolution and averaged away by the display transfer.
    Guard("[decision] deko3d top screen: one sample per presented line unless config/deko3d_line_samples is 2",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'return c == \'2\' \? TopScreenLineSamples::Two : TopScreenLineSamples::One;\s+\}\s+'
          r'return TopScreenLineSamples::One;',
          test="oot3d_deko3d_backend_tests (TestTopScreenLineSamples)"),
    Guard("deko3d top screen at one sample per line: the Vulkan policy decides the sampling, X scales by it",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'return Oot3d::NativeTopScreenStorageSamplingX\(\{width, height\}, PresentedTopScreenExtent\(allocScale\)\);.*?'
          r'extent\.ScaleX = scale \* samplingX;\s+extent\.Width = ScaledExtent\(width, extent\.ScaleX\);.*?'
          r'extent\.ScaleX = scale \* alloc\.SamplingX;',
          test="oot3d_deko3d_backend_tests (TestTopScreenLineSamples)"),
    Guard("deko3d top screen at one sample per line: the game's 2:1 transfer filter becomes a 1:1 copy",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'const uint32_t collapsedH = drawn\.SamplingX < 1\.0F \? horizontalSamples : 1U;\s+'
          r'plan\.Linear = horizontalSamples / collapsedH == 2U \|\| verticalSamples == 2U;',
          test="oot3d_deko3d_backend_tests (TestTopScreenLineSamples)"),
    Guard("deko3d top screen at one sample per line: targets and dynamic resolution keep the allocation's sampling",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_targets.cpp",
          r'PicaTargetSamplingX\(key\.Width, key\.Height, mScale, mLines\)\);.*?'
          r'target\.Extent = PlanPicaDynamicExtent\(key\.Width, key\.Height, desired, target\.Alloc\);',
          test="oot3d_deko3d_backend_tests (TestTopScreenLineSamples)"),
    Guard("deko3d top screen at one sample per line: viewport, scissor and widened X edges use ScaleX",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'PlanPicaViewport\(draw\.ViewportX, draw\.ViewportY, draw\.ViewportWidth, draw\.ViewportHeight, scaleX,\s+scale\);.*?'
          r'scaleX, scale, target\.Extent\.Width, target\.Extent\.Height\);.*?'
          r'const auto edgeX = \[&\]\(float native\) \{ return static_cast<int32_t>\(std::lround\(native \* scaleX\)\); \};',
          test="oot3d_deko3d_backend_tests (TestTopScreenLineSamples)",
          forbid=r'native \* scale\)\)'),
    # --- 05/11: grass on deko3d (config/grass, SWITCH_PORT.md "Grass on deko3d") ---
    Guard("[decision] grass off without config/grass: nothing is configured, every hook returns at once",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'const std::filesystem::path config = "sdmc:/switch/oot3dre/config";\s+'
          r'const auto mode = Oot3d::ReadGrassSwitchMode\(config\);\s+'
          r'if \(mode == Oot3d::GrassSwitchMode::Off\) \{\s+return;\s+\}',
          test="oot3d_grass_switch_tests (ConfigFileMeaning)"),
    Guard("[decision] grass: absent config/grass means Off",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_switch_profile.cpp",
          r'if \(!fileText\.has_value\(\)\) return GrassSwitchMode::Off;',
          test="oot3d_grass_switch_tests (ConfigFileMeaning)"),
    Guard("[decision] grass: the deko3d draw hooks run only with config/grass = draw (mGrassDraws)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'if \(mGrassDraws && draw\.CompositionDomain == Renderer3ds::PicaCompositionDomain::Scene &&\s+'
          r'draw\.Composition\.Layer == Renderer3ds::PicaCompositionLayer::TransparentWorld\) \{.*?DrawGrass\(key, \*target\);'),
    Guard("grass popped (0/600 frames drawn at 60 fps): the draw point came before the ground's sources, at a sky draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'state->SourceFrame != frame \|\|\s+state->SourceNamespace != key\.Namespace \|\| state->SourceColorAddress != key\.ColorAddress\)',
          test="oot3d_grass_switch_tests (Oot3dGrassHandoff)"),
    Guard("grass popped: a frame must draw the newest finished cull, a late worker never empties it",
          "runtime/three_ds_recomp/include/fast/oot3d/grass_stream_handoff.h",
          r'if \(fit != GrassCullFit::Draw\) return \{std::nullopt, fit\};\s+return \{slots\.Draw\(frame\), fit\};',
          test="oot3d_grass_switch_tests (Oot3dGrassHandoff)"),
    Guard("grass GPU: the PICA depth goes into clip z; gl_FragDepth (no early-Z) only for w-buffered draws",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'result\.ExplicitDepth = push\.DepthState\[3\] > 0\.5F && push\.DepthState\[2\] > 0\.5F;'),
    Guard("[decision] grass: game thread reads colliders only when grass draws (Link and Epona, not every actor)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'\} else if \(Fast::Deko3dGrassWantsColliders\(\)\) \{\s+//[^\n]*\n\s+'
          r'frameActors = Oot3dNativeGame::CollectNativeGrassColliders\('),
    Guard("[decision] grass: collider flag is set only for config/grass = draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'gGrassWantsColliders = mode == Oot3d::GrassSwitchMode::Draw;'),
    Guard("[decision] grass: culling runs on the grass worker thread, never in the render or game thread's draw path",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'GrassCullWorker\(\) : mThread\(\[this\] \{ Run\(\); \}\) \{\}.*?result\.Stats = builder\.Build\(',
          forbid=r'GrassInstanceStreamBuilder builder;(?![^\n]*\n\s+for \(;;\))'),
    Guard("grass: a placement cap must thin evenly, not cut the index buffer short (bald patches)",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_surface_extractor.cpp",
          r'if \(kept\[index\]\) anchors\.push_back\(pending\[index\]\.Anchor\);\s+'
          r'ThinGrassAnchorsToLimit\(anchors, limit\);',
          test="oot3d_grass_switch_tests (Oot3dGrassThinning)",
          forbid=r'index < pending\.size\(\) && anchors\.size\(\) < limit'),
    Guard("grass: the display copy must see the grass draw's Version (else a lean repeat skips the copy)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'DrawGrass\(\*key, \*source\);\s+\}\s+const PicaDisplayCopy copy\{source->Id, source->Version'),
    Guard("grass of the title demo drawn in the temple entrance: a cull of another scene is dropped, never drawn",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'const auto pick = Oot3d::GrassCullForDrawPoint\(state->Slots, frame, state->Current\.Area, state->Area,.*?'
          r'if \(pick\.Fit == Oot3d::GrassCullFit::OtherScene\) \{\s+\+\+state->ForeignOtherScene;\s+'
          r'state->Current = \{\};.*?const auto drawnSlot = pick\.Slot;',
          test="oot3d_grass_switch_tests (Oot3dGrassAreaChange, Oot3dGrassForeignCull)"),
    Guard("grass: a scene change drops the drawn cull, the sources' memo and the placements",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'if \(change == Oot3d::GrassAreaChange::Room\) return;\s+\+\+state\.SceneChanges;\s+'
          r'state\.Slots\.Reset\(\);\s+state\.Current = \{\};\s+state\.Sources = std::make_unique'),
    # --- 07/11: the grass flickered once entering the Forest Temple (log16, test.8) ---
    Guard("grass flickered at a room change: a room change within a scene must keep the drawn cull (the old room's ground stays drawn)",
          "runtime/three_ds_recomp/include/fast/oot3d/grass_stream_handoff.h",
          r'return GrassSameScene\(before, now\) \? GrassAreaChange::Room : GrassAreaChange::Scene;',
          test="oot3d_grass_switch_tests (EnteringTheForestTempleDrawsItsGrassOnceAndKeepsIt)"),
    Guard("grass flickered: a cull without a shared placement is left out of one frame, never forgotten",
          "runtime/three_ds_recomp/include/fast/oot3d/grass_stream_handoff.h",
          r'if \(fit == GrassCullFit::OtherScene\) slots\.Reset\(\);\s+'
          r'if \(fit != GrassCullFit::Draw\) return \{std::nullopt, fit\};',
          test="oot3d_grass_switch_tests (ACullOfAnotherSceneIsNeverDrawnEvenWithASharedPlacement)"),
    Guard("grass flickered at 60 fps: in-between frames must keep their sources (model-to-clip of the frame, world kept)",
          "runtime/three_ds_recomp/src/fast/oot3d/pica_grass_source.cpp",
          r'RefreshGrassMemoDraw\(mesh, draw, state\.Memo\.CameraInverse\(draw\)\) \|\| RefreshMemoClip\(mesh, draw\)',
          test="oot3d_grass_switch_tests (Oot3dGrassInterpolatedFrames)"),
    # --- 07/11: less grass around Link than further out in Hyrule Field (test.8 videos) ---
    Guard("grass thinner around Link: tufts near the camera bring fill tufts (FillDistance / distance in all)",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_instance_stream.cpp",
          r'const float fillReach = std::min\(settings\.FillDistance, fullDistance\);.*?'
          r'fillReach / \(static_cast<float>\(child\) \+ GrassFillStableValue\(tuft\.StableId, child\)\);',
          test="oot3d_grass_switch_tests (AroundLinkTheGrassCoversAsMuchAsFurtherOut)"),
    Guard("grass changed as Link walked: a thinned tuft grows in, never appears at full height",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_instance_stream.cpp",
          r'const float grown = std::min\(fade, GrassGrowth\(kept, distance, kGrassThinGrowth\)\);',
          test="oot3d_grass_switch_tests (WalkingChangesNoTuftAtOnce)"),
    Guard("grass changed as Link walked: single blades turn into tufts over a distance, not at once",
          "tools/oot3d/deko3d_grass/grass_vsh.glsl",
          r'float tuftShare = smoothstep\(grass_distance\.z, grass_distance\.w, distanceToEye\);',
          forbid=r'bool blades = grass_distance\.w > 0\.5;'),
    # --- grass round 4 (SWITCH_PORT.md "Grass on deko3d", Round 4) ---
    Guard("grass at 60 fps: an in-between frame publishes no sources; it draws the last real frame's placements with its own camera",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'if \(state->InBetweenFrame == frame\) \{.*?state->InBetween\.InBetweenDraw\(.*?\} else \{\s+try \{\s+'
          r'if \(Oot3d::PublishPicaGrassSource\(',
          test="oot3d_grass_switch_tests (AnInBetweenFrameDrawsTheRealFramesPlacementsWithItsCamera)"),
    Guard("grass at 60 fps: only a real frame's placements become the ones the in-between frames draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'if \(state->InBetweenFrame != frame\) \{.*?state->RealFrame = std::move\(state->Frame\);.*?'
          r'state->InBetween\.EndRealFrame\(frame\);',
          test="oot3d_grass_switch_tests (AnInBetweenFrameDrawsTheRealFramesPlacementsWithItsCamera)"),
    Guard("grass at 60 fps: the deko3d backend learns which frames are in-between from the temporal sample",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'GfxRenderingAPIDeko3d::PublishPicaFrameTemporalSample\(.*?Deko3d::GrassNoteSample\(sample\.Synthetic, mDevice\.Frame\(\)\);'),
    Guard("Grass distance (L3+R3 > Video) applies at once: the renderer takes a published distance at its next frame",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'const auto distance = Oot3d::PublishedGrassSwitchDistance\(\); distance && \*distance != state->Distance\) \{\s+'
          r'ApplyProfile\(\*state, state->Fps60, \*distance\);'),
    Guard("Grass distance: the menu saves and publishes the choice",
          "tools/oot3d/native_game_runtime/oot3d_video_settings_panel.cpp",
          r'SaveGrassSwitchDistance\(mConfig, mGrassDistance\);\s+Fast::Oot3d::PublishGrassSwitchDistance\(mGrassDistance\);'),
    Guard("Grass distance Whole area: every tuft, capped only by the stream memory (no distance thinning)",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_switch_profile.cpp",
          r'case GrassSwitchDistance::Area:\s+profile\.FullDensityDistance = kGrassWholeAreaDistance;.*?'
          r'profile\.MaxVisibleInstances = kGrassWholeAreaInstances;',
          test="oot3d_grass_switch_tests (WholeAreaDrawsEveryTuftAndTheCapKeepsTheNearest)"),
    # --- 06/11: a save that starts in the Forest Temple drew no grass at its entrance (logs15) ---
    Guard("grass (deko3d): rule textures' signatures are kept across sessions, so a twin in an area entered first finds its rule",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'SetSignatureStore\(\s+"sdmc:/switch/oot3dre/shadercache/grass_texture_signatures\.bin"\);',
          test="oot3d_graphics_foundation_tests (Oot3dGrassTextureTwins.RuleSignatureKeptByAnEarlierSessionServesItsTwin)"),
    Guard("grass (deko3d): a kept signature never skips the session's decode (the placement samples its pixels)",
          "runtime/three_ds_recomp/src/fast/oot3d/pica_grass_source.cpp",
          r'texture->Height >= 16 && !sources\.Contains\(\*textureHash\)\) \{.*?'
          r'sources\.ObserveDecoded\(\*textureHash, texture->Width, texture->Height, rgba\);\s+\}\s+\}'),
    # --- 07/11: still no grass at the Forest Temple entrance on a first load-in (test.7): the kept
    # signature needs one visit to Sacred Forest Meadow. The preset's twins are built in. ---
    Guard("grass: a twin ground takes its rule from the built-in twins while the rule texture's signature is missing",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_texture_twins.cpp",
          r'const auto known = std::equal_range\(\s+GrassKnownTwins\(\)\.begin\(\), GrassKnownTwins\(\)\.end\(\).*?'
          r'if \(original != nullptr\) \{.*?\} else if \(offline != known\.second\) \{\s+score = offline->Score;',
          test="oot3d_grass_switch_tests (Oot3dGrassTwinsAtLoad)"),
    Guard("grass: the built-in twins hold the Forest Temple entrance's ground (rule 10, Sacred Forest Meadow)",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_texture_twin_table.cpp",
          r'\{0xd885db352c997c7bULL, 256, 256, 0x7a41e65008d2ea50ULL, 256, 256, GrassSampleChannel::Luminance,',
          test="oot3d_grass_switch_tests (TheFirstFrameOfAFreshSessionGivesTheForestTempleEntranceItsRule)"),
    # --- 07/11: grass cost per tuft, config/grass_lod (SWITCH_PORT.md "Grass on deko3d: cost per tuft") ---
    Guard("[decision] grass_lod: absent config/grass_lod means on; 0/off/false restore test.9's grass",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_switch_profile.cpp",
          r'bool ParseOnUnlessOff\(std::optional<std::string_view> fileText\) noexcept \{\s+'
          r'if \(!fileText\.has_value\(\)\) return true;.*?'
          r'return !\(word == "0" \|\| word == "off" \|\| word == "false"\);.*?'
          r'bool ParseGrassSwitchLod\(std::optional<std::string_view> fileText\) noexcept \{\s+'
          r'return ParseOnUnlessOff\(fileText\);',
          test="oot3d_grass_switch_tests (Oot3dGrassLod.ConfigFileMeaning)"),
    Guard("[decision] grass_lod off: a cull culled without levels is drawn by grass_vsh.glsl, instanced, as test.9",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'bool lod = drawnSlot && current\.Valid && current\.Lod && !state->LodFailed;.*?'
          r'const DkShader\* shaders\[\] = \{state->Vertex,.*?'
          r'dkCmdBufDraw\(cmd, DkPrimitive_Triangles, 3U \* state->Profile\.BladesPerTuft, range\.Count, 0, 0\);'),
    Guard("grass_lod: no pop at a level change: the cull picks a tuft's draw at its distance less the cull margin",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_instance_stream.cpp",
          r'if \(own < nearEnd \+ margin\) \{\s+kind = GrassStreamKind::Blades;.*?'
          r'kind = GrassLodKind\(GrassLodCoordinate\(lod, own - margin, halfWidth\)\);',
          test="oot3d_grass_switch_tests (Oot3dGrassLod.LevelsRegroupTheSameInstancesByDistance), "
               "tests/switch/test_grass_lod.py (GrassLodLayout)"),
    Guard("grass_lod: the tuft pass's writes land before a blade program reads them",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'dkCmdBufDraw\(cmd, DkPrimitive_Points, count, 1, 0, 0\);.*?'
          r'dkCmdBufBarrier\(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image\);'),
    Guard("grass_lod: the blade programs read the tuft buffer past the L1 (coherent)",
          "tools/oot3d/deko3d_grass/grass_lod_vsh.glsl",
          r'layout\(std430, binding = 0\) coherent readonly buffer GrassTufts'),
    # --- 07/11: test.9's grass grew around Link; still flickered entering the Forest Temple (logs17) ---
    Guard("[decision] grass_fill: absent config/grass_fill means fixed; 0/off/false restore test.9's fill",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_switch_profile.cpp",
          r'bool ParseGrassSwitchFill\(std::optional<std::string_view> fileText\) noexcept \{\s+'
          r'return ParseOnUnlessOff\(fileText\);.*?'
          r'bool ReadGrassSwitchFill\(const std::filesystem::path& configDirectory\) \{\s+'
          r'return ReadOnUnlessOff\(configDirectory / "grass_fill"\);',
          test="oot3d_grass_switch_tests (Oot3dGrassNearLink.FillConfigFileMeaning)"),
    Guard("grass grew around Link as he walked (test.9): fixed fill tufts keep their full height and narrow, "
          "far away, instead of growing; thinned tufts widen in",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_instance_stream.cpp",
          r'if \(settings\.FillFixed\) \{.*?'
          r'static_cast<uint8_t>\(std::lround\(fade \* 255\.0F\)\),\s+'
          r'static_cast<uint8_t>\(std::lround\(widest \* thinGrowth \* 64\.0F\)\), 0U\);.*?'
          r'const float rank = GrassFixedFillRank\(settings, distance\);.*?'
          r'GrassFixedFillWidth\(rank, child, GrassFillStableValue\(tuft\.StableId, child\)\)',
          test="oot3d_grass_switch_tests (Oot3dGrassNearLink.NothingGrowsNearLinkAsHeWalks)"),
    Guard("grass grew around Link: the fixed fill is constant within kGrassFixedFillNear of the camera",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_instance_stream.cpp",
          r'const float count = GrassFixedFillCount\(settings\);\s+'
          r'if \(!\(distance > kGrassFixedFillNear\)\) return count;',
          test="oot3d_grass_switch_tests (Oot3dGrassNearLink.FixedFillMultiplierAndWidths)"),
    Guard("[decision] grass_fill is applied live: the renderer takes the menu's published choice at its next frame",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'if \(const auto fill = Oot3d::PublishedGrassSwitchFill\(\); fill && \*fill != state->Fill\) \{.*?'
          r'state->Stream\.FillFixed = \*fill;'),
    Guard("grass flickered in the Forest Temple entrance: new signatures must not erase a twin's rule "
          "(it stays until the texture is resolved again)",
          "runtime/three_ds_recomp/src/fast/oot3d/grass_texture_twins.cpp",
          r'for \(auto& \[key, entry\] : mEntries\) entry\.Stale \|= !entry\.Complete;.*?'
          r'if \(found != mEntries\.end\(\) && !found->second\.Stale\) return found->second;',
          test="oot3d_grass_switch_tests (Oot3dGrassTempleEntry.ATextureSeenAfterTheGroundKeepsTheEntrancesRule)",
          forbid=r'std::erase_if\(mEntries, \[\]\(const auto& entry\) \{ return !entry\.second\.Complete; \}\);'),
    # --- 06/11: floors shimmered in "square pixels" on deko3d (Forest Temple, since test.3; logs15) ---
    # The shaders add the PICA LOD bias to their explicit LOD; a sampler bias adds to it again
    # (-1.7 became -3.4: level 0 far away). SWITCH_PORT.md "Floor shimmer on deko3d".
    Guard("deko3d: the PICA LOD bias is applied once, in the shader: the sampler keeps lodBias 0",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'dkSamplerDefaults\(&sampler\);.*?sampler\.lodClampMax = .*?dkSamplerDescriptorInitialize\(&descriptor, &sampler\);',
          test="oot3d_deko3d_backend_tests (TestSamplerAndTextureKeys)",
          forbid=r'\.lodBias\s*='),
    Guard("deko3d: the shader is where the PICA LOD bias is applied (the sampler has none)",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_fragment_shader_gen.cpp",
          r'return textureLod\(pica_texture0, coord, lod \+ fragment_uniforms\.texture_lod_bias\[0\]\);.*?'
          r'return textureLod\(pica_texture1, coord, lod \+ fragment_uniforms\.texture_lod_bias\[1\]\);.*?'
          r'return textureLod\(pica_texture2, coord, lod \+ fragment_uniforms\.texture_lod_bias\[2\]\);'),
    # --- 26/10: dev menu without buttons on deko3d ---
    Guard("dev menu (L3+R3) opened on deko3d but didn't receive controller input (no ImGui SDL backend)",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'void Fast3dGui::ImGuiWMNewFrame\(\) \{\s+if \(Deko3dRendererSelected\(\)\) \{.*?FeedDeko3dGamepad\(io\);.*?return;'),
    # --- 24/10: deko3d backend (branch deko3d, docs/DEKO3D_PROGRESS.md) ---
    Guard("[decision] deko3d only with config/renderer = deko3d; without the file, the usual GL",
          "runtime/three_ds_recomp/src/fast/Fast3dWindow.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{\s+mRenderingApi = CreateDeko3dRenderingApi\(\);\s+\}\s+'
          r'if \(mRenderingApi == nullptr\) \{\s+mRenderingApi = new GfxRenderingAPIOGL\(\);',
          "oot3d_deko3d_backend_tests (TestRendererConfigSelection)"),
    Guard("[decision] deko3d: no SDL window (Switch SDL puts an EGL surface on the swapchain's native window)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_sdl2.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{.*?mNoWindow = true;.*?return;'),
    Guard("deko3d: glFlush (config/gl_pacing_flush) without a GL context would call a null pointer",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(gFlushBeforePacing && !Fast::Deko3dRendererSelected\(\)\)\s+glFlush\(\);'),
    Guard("deko3d: GL context is not moved to the render thread (it doesn't exist)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'if \(openGlWorker && !Fast::Deko3dRendererSelected\(\)\) \{'),
    Guard("deko3d: ImGui without SDL/OpenGL3 backend (null GL pointers, ImplSDL2 without a window)",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'void Fast3dGui::ImGuiRenderDrawData\(ImDrawData\* data\) \{\s+if \(Deko3dRendererSelected\(\)\) \{\s+return;'
          r'.*?void Fast3dGui::ImGuiWMNewFrame|void Fast3dGui::ImGuiWMNewFrame\(\) \{\s+if \(Deko3dRendererSelected\(\)\)'),
    Guard("deko3d: GL profiler (config/gpu_timing) would call a null glGenQueries",
          "runtime/three_ds_recomp/src/fast/backends/gl_frame_profiler.cpp",
          r'if \(Deko3dRendererSelected\(\)\) return;\s+const auto config = ReadConfig\(\);'),
    Guard("deko3d: dkCmdBufClear goes back to the previous frame's memory: the slot feeds its own slice",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'dkCmdBufClear\(mCmd\);\s+dkCmdBufAddMemory\(mCmd, mCommandMemory, mSlot \* kCommandSliceBytes'),
    Guard("[decision] deko3d: draws use the parametric (uber) program and nothing compiles on the render thread",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'bool UsesAlwaysAvailablePrograms\(\) const override \{\s+return true;'),
    Guard("deko3d: stage without DKSH in the pack is remembered (no retranslation on every draw)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'return cache\.Get\(identity, \[&\]\(\) -> const DkShader\* \{.*?TranslatePicaShaderToOpenGl43',
          "oot3d_deko3d_backend_tests (TestStageCacheResolvesOnce)"),
    Guard("deko3d: missing specialized program falls back to the uber one before skipping the draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'if \(specialized == nullptr\) \{\s+uber = ResolvePicaStage\(Deko3d::ShaderStage::Fragment, draw\.UberFragmentShaderSource',
          "oot3d_deko3d_backend_tests (TestProgramPick)"),
    Guard("deko3d: texture freed while the GPU was still reading it (eviction only after waiting for the slot's fence)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d.cpp",
          r'void GfxRenderingAPIDeko3d::EnsureFrame\(\) \{\s+mDevice\.BeginFrame\(\);.*?mResources\.BeginFrame\(mResourcesFrame\);'),
    Guard("deko3d: a texture that fails to decode is not decoded again on every draw",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'did not decode".*?return rememberFallback\(\);'),
    Guard("deko3d: missing vertex attribute reads (0,0,0,1) from the constant buffer, like a disabled GL array",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_plan.h",
          r'PicaAttributeKind Kind = PicaAttributeKind::Constant;.*?uint8_t Binding = kPicaConstantVertexBinding;',
          "oot3d_deko3d_backend_tests (TestVertexPlan)"),
    # --- 24/10: deko3d pack 5 (targets, state, draw, transfer, scanout) ---
    Guard("deko3d: constant attribute (0,0,0,1) was never allocated (invalid address on every missing attribute)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'mConstantGpu = dkMemBlockGetGpuAddr\(mConstant\);'),
    Guard("deko3d: widened projection as in GL (otherwise the frustum becomes 16:9 and the target ends up stretched 5:3)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'GetName\(\)\) == "OpenGL" \|\|\s*std::string_view\(apiPointer->GetName\(\)\) == "deko3d"'),
    Guard("deko3d: display transfer with a barrier before (fragments in memory) and after (texture cache) the blit",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'dkCmdBufBarrier\(cmd, DkBarrier_Full, 0\);.*?dkCmdBufBlitImage.*?dkCmdBufBarrier\(cmd, DkBarrier_Full, DkInvalidateFlags_Image\);'),
    Guard("[decision] deko3d: face winding inverted relative to GL (origin at the top, like Vulkan); config/deko3d_cull_invert undoes it",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'plan\.FrontCcw = invertWinding \? glCcw : !glCcw;',
          "oot3d_deko3d_backend_tests (TestCullPlan)"),
    Guard("deko3d: PICA logic ops are ordered differently from deko3d's (Set=4 on PICA, 15 on deko3d)",
          "runtime/three_ds_recomp/include/fast/backends/gfx_deko3d_pica_pipeline.h",
          r'case L::Set: return 15U;',
          "oot3d_deko3d_backend_tests (TestPipelineEnums)"),
    Guard("deko3d: scanout flips v (origin at the top) so the screen doesn't come out upside down",
          "tools/oot3d/deko3d_scanout/scanout_vsh.glsl",
          r'pica_uv = vec2\(position\.x \* 0\.5 \+ 0\.5, 0\.5 - position\.y \* 0\.5\);'),
    Guard("boot-status stayed at host_frames=1/guest_frames=1 without geometry cache (deko3d): looked like a hung guest",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'runFrameCount == 1U \|\| gSwitchPicaGeometryCacheObserved \|\|\s*runFrameCount % 600U == 0U'),
    Guard("deko3d.txt didn't say which NRO it came from or whether PICA reached the backend (p5 log looked like p3's)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'pica: first draw.*?pica: first %s.*?pica: first render-target clear.*?pica: first memory fill',
          "oot3d_deko3d_backend_tests (TestLogSchedule)"),
    Guard("deko3d p5/p5b: crash (Data Abort) on the 1st draw — target views pointed to the DkImage of the local copy on the stack",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_targets.cpp",
          r'auto& stored = mTargets\.emplace\(key, target\)\.first->second;.*?'
          r'dkImageViewDefaults\(&stored\.ColorView, &stored\.Color\);\s+'
          r'dkImageViewDefaults\(&stored\.DepthView, &stored\.DepthStencil\);',
          forbid=r'dkImageViewDefaults\(&target\.(ColorView|DepthView)'),
    Guard("deko3d p5c: game closed when entering gameplay ('renderer does not support the native UI sampler contract')",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_ui.cpp",
          r'bool GfxRenderingAPIDeko3d::SetNativeSamplerParameters\(int sampler, const GfxNativeSamplerState& state\) \{'
          r'.*?found->second\.Sampler = UiSamplerKey\(state\);\s+\}\s+return true;',
          "oot3d_deko3d_backend_tests (TestUiPlan)"),
    Guard("deko3d: without ImGui on the window there was no FPS meter (config/fps_counter) — copy drawn on the render thread",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'if \(Deko3dRendererSelected\(\)\) \{.*?mRendererRouting\.Deferred\(\s*'
          r'\[copy = CloneImGuiDrawData\(data\)\] \{ Deko3dRenderImGuiDrawData\(copy\.get\(\)\); \}\);',
          forbid=r'ImGui is not drawn on deko3d yet'),
    Guard("deko3d p6: UI upside down and scene rotated 180° (NDC -1 on the last row); sky vanished and the scene smeared",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'ApplyDkViewport\(cmd, static_cast<float>\(viewport\.X\).*?'
          r'ApplyDkViewport\(cmd, static_cast<float>\(placement\.Viewport\.X\)',
          "oot3d_deko3d_backend_tests (TestUiPlan: ClipYSwizzleNegates)",
          forbid=r'const DkViewport \w+ = \{'),
    Guard("deko3d p6b: black screen — negative-height viewport becomes an invalid scissor rectangle in deko3d 0.5.0",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_api.h",
          r'void ApplyDkViewport\(.*?dkCmdBufSetViewports\(cmd, 0, &viewport, 1\);.*?DkSwizzle_NegativeY.*?'
          r'dkCmdBufSetViewportSwizzles\(cmd, 0, &swizzle, 1\);',
          forbid=r'FlipViewportRows|\{x, y \+ height'),
    Guard("deko3d p6f: GPU rejected the command list when leaving Link's house — shaders with 4 KiB of scratch, queue with 2 KiB",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'queueMaker\.perWarpScratchMemorySize = kPerWarpScratchBytes;',
          "oot3d_deko3d_backend_tests (TestDkshScratch)"),
    Guard("deko3d p6k: crash when leaving Link's house — deko3d's BindVtxBuffers writes past the command block",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'slot\.CommandUsed \+ want \+ kCommandGuardBytes <= kCommandSliceBytes\).*?'
          r'slot\.CommandUsed \+= want \+ kCommandGuardBytes;.*?size - kCommandGuardBytes\);.*?'
          r'slot\.CommandUsed = kCommandChunkBytes \+ kCommandGuardBytes;',
          "oot3d_deko3d_backend_tests (TestCommandGuard)"),
    Guard("deko3d p6f: a shader with more scratch than the queue is rejected, it doesn't bring down the GPU",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_shaders.cpp",
          r'DkshPerWarpScratchBytes\(dksh\);\s+if \(!scratch\.has_value\(\) \|\| \*scratch > kPerWarpScratchBytes\)'),
    Guard("deko3d p6m: GPU ms included the acquire wait (14.5 ms × 8.5 on GL) — timestamps around the acquire",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'kTimestampSlotBytes \+ kTimestampBytes\);\s+\}\s+Submit\(\);.*?'
          r'dkQueueAcquireImage\(mQueue, mSwapchain\);.*?TimestampPipelineTop.*?2U \* kTimestampBytes\);'),
    Guard("deko3d p6m: governor and DRS without GPU time on deko3d (CPU only) — Deko3dTagFrame/TakeGpuFrame",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'Fast::Deko3dTagFrame\(kind, costCpuMs\);.*?timer\.Poll\(onGpuResult\);.*?Fast::Deko3dTakeGpuFrame\(frame\)'),
    Guard("deko3d p6m: DRS — display image at the allocation scale, content at the drawn scale (no reallocation)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_draw.cpp",
          r'PlanPicaTransfer\(transfer, key->Width, key->Height, source->Extent, source->Alloc\);',
          "oot3d_deko3d_backend_tests (DRS: content smaller than the allocation)"),
    Guard("deko3d p6p: present on its own thread without waiting for the previous one → nwindow loses the current slot (dequeue before queue)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'WaitPresent\(\);(?:(?!dkQueueAcquireImage|QueuePresent).){0,800}?'
          r'mWindowSlot = dkQueueAcquireImage\(mQueue, mSwapchain\);'),
    Guard("deko3d p6p: compressed window with present on its own thread (only dkQueuePresentImage decompresses)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'if \(!mPresentThreaded\) \{\s+layoutMaker\.flags \|= DkImageFlags_HwCompression;'),
    Guard("p6w: sys-clk IPC on the game thread hung for 4.4 s — SwitchClockPerfSummary only reads the cache",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'std::string SwitchClockPerfSummary\(\) \{(?:(?!serviceDispatch|ReadClocks\(\)).)*?return text \+'),
    Guard("p6x: std::thread::detach() throws 'Function not implemented' on Switch (crash on the first perf)",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'\A(?:(?!\.detach\(\)).)*\Z'),
    Guard("p6w: sys-clk IPC on the game thread hung for 4.4 s — the overlay's SwitchClockLatest only reads the cache",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'SwitchClockReading SwitchClockLatest\(\) \{'
          r'(?:(?!serviceDispatch|ReadClocks\(\)|appletGetCurrentPerformanceConfiguration).)*?return reading;'),
    Guard("SwitchWakerHD #33: clkrst in the app froze the console on wake from sleep — clocks are read without clkrst/pcv",
          "tools/oot3d/native_game_runtime/oot3d_switch_clocks.cpp",
          r'appletGetCurrentPerformanceConfiguration\(',
          forbid=r'\b(?:clkrst|pcv)[A-Z]\w*\('),
    Guard("p6x: std::thread::detach() throws on Switch — debug audio recording",
          "runtime/three_ds_recomp/src/ship/audio/SDLAudioPlayer.cpp",
          r'\A(?:(?!\.detach\(\)).)*\Z'),
    Guard("deko3d p6w: mailbox — the renderer must not reuse the image that is waiting to be copied or being copied",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'while \(image == mMailboxPending \|\| image == mMailboxCopying\)'),
    Guard("deko3d p6w: mailbox — window dequeue and queue only on the present thread (nwindow lends 1 image at a time)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'void Device::MailboxLoop\(\).*?dkSwapchainAcquireImage\(mSwapchain, &slot, &dequeued\);'
          r'.*?dkQueueWaitFence\(mPresentQueue, &dequeued\);.*?dkQueueWaitFence\(mPresentQueue, &drawn\);'
          r'.*?dkQueuePresentImage\(mPresentQueue, mSwapchain, slot\);'),
    Guard("deko3d p6w: mailbox — the render GPU waits for the previous copy to leave the image before drawing",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'if \(wait\) \{\s+dkQueueWaitFence\(mQueue, &copied\);'),
    Guard("deko3d p6q: deko3d_swapchain_images outside 2-4 would overflow the window image arrays",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'count >= 2U && count <= kMaxSwapchainImages\)'),
    # --- 24/10: spikes outside loading (worktree picos-logica) ---
    Guard("[decision] sound/DSP on another core discarded: would fix 3 of 95 guest spikes (tour20+22)",
          "docs/PORTING_GUIDE_3DS_SWITCH.md",
          r'would solve \*\*3 of 95\*\* guest spikes — \*\*discarded\*\*'),
    Guard("spikes.txt accumulates sessions: spike analysis only of the last one (tour20 had 428 lines from tour19)",
          "tools/oot3d/spike_report.py",
          r'if line\.startswith\("session_start"\):\s+rows = \[\]',
          "test_perf_tools.py (spikes: only the last session)"),
    Guard("[decision] early_frame_slot is opt-in (file; content 0 turns it off)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r"earlyFrameSlot = value\.empty\(\) \|\| value\[0\] != '0';"),
    Guard("early_frame_slot: slot returned when the pacer starts waiting (deadline announcement)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'PublishPresentationDeadline\(slot\.Serial, deadline\);\s+slot\.Session->renderStream\.ReleaseFrameSlot\(\);',
          "oot3d_native_render_command_stream_tests (TestEarlyFrameSlotRelease), oot3d_frame_spike_tests"),
    Guard("[decision] early_frame_slot without a clock driven by the pacer deadline worsens images/s (alternating refresh 0/2)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'PublishedPresentationDeadline\(presentationFrameCount - 2U\)',
          "oot3d_native_cadence_governor_tests (TestEarlyFrameSlotAbsorbsLongSteps)"),
    Guard("[decision] frames_in_flight=2 remains ignored (heap corrupted in 21 s, tour3b)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'frames_in_flight ignored.*?SetMaxFramesInFlight\(1U\);'),
    Guard("Pacer announces the deadline also when late/disabled (otherwise the slot only comes back after the swap)",
          "tools/oot3d/native_game_runtime/oot3d_native_presentation_pacer.cpp",
          r'if \(!mEnabled\) \{\s+announce\(std::chrono::steady_clock::now\(\)\);',
          "oot3d_frame_spike_tests (TestPacerAnnouncesDeadlineBeforeWait)"),
    Guard("[decision] heap scan with gpu_timing = 16-21 ms spike every 20 windows (artifact, documented)",
          "docs/PORTING_GUIDE_3DS_SWITCH.md",
          r'heap scan \(every 20 windows\) costs 16-21 ms'),
    # --- 24/10: texture/object "flicker" when turning the camera fast (only with fps60) ---
    Guard("Flicker when turning the camera (fps60): step frame (alpha 0) drew the draw set of the new frame",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp",
          r'if \(mImpl->Options\.ExactStep && alpha <= 0\.0F\) \{.*?return ViewOot3dPicaVisualFrame\(previous, output\);',
          test="oot3d_native_pica_visual_cull_tests"),
    Guard("Flicker when turning the camera (fps60): intermediate frame without the draws the game culled / with the new ones in the wrong camera",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_visual_frame.cpp",
          r'ApplyVisualUnmatchedCarry\(previous, current, mImpl->Carry, alpha, output\);',
          test="oot3d_native_pica_visual_cull_tests"),
    Guard("Flicker fix on by default (config/interp_exact_step and interp_carry_unmatched = 0 turn it off)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'transitionOptions\.ExactStep = true;\s*transitionOptions\.CarryUnmatched = true;.*?preparedVisualTransition\.SetOptions'),
    Guard("[decision] flicker is not DRS: the depth fill clears the whole attachment (no scissor) and the scale only changes 1x per FrameSerial",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'glBindFramebuffer\(GL_FRAMEBUFFER, target\.Framebuffer\);\s*glDisable\(GL_SCISSOR_TEST\);\s*GLbitfield mask = 0U;'),
    Guard("[decision] flicker is not the texture cache: key by content (hash), an evicted entry is decoded again from the plan's bytes",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'struct PicaGlTextureKey \{\s*uint64_t ContentHash = 0U;'),
    Guard("[decision] flicker is not the hold: a discarded intermediate re-presents the whole last presented frame",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(present\) \{\s*capturedTemporalSample = temporalSample;\s*lastPresentedVisual\.emplace\(sample\.TopTransfer, renderTargetNamespace\);'),
    # --- 23/10: decisions measured on the console (don't redo) and analysis bugs ---
    Guard("[decision] pacer_vsync only with the config/pacer_vsync file (tour22: missed refresh 3.7→4.1, interp 75→69)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'pacerVsyncConfigured = pacerVsyncFile\.good\(\);'),
    Guard("[decision] ETC2 off without config/gl_etc2=1 (nvc0/Mesa 20.1 has no ETC2 on GM20B: decodes on the CPU)",
          "runtime/three_ds_recomp/include/fast/oot3d/pica_etc1_transcode.h",
          r'config/gl_etc2.*?return false;\s*\}\s*return !text\.empty\(\) && text\[0\] == \'1\';'),
    Guard("[decision] governor counts the acquire wait (pgo5j without it: 58.52 fps, too optimistic)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'bool gCountRenderWait = true;'),
    Guard("[decision] aurora/Dusklight draw merging discarded (~1% of draws mergeable; the measurement inflated render by 3-4 ms)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'kGlCompressedRgb8Etc2', forbid=r'step_mergeable|merge_stats'),
    Guard("[decision] record of the discarded draw merging in the guide",
          "docs/PORTING_GUIDE_3DS_SWITCH.md",
          r'Draw merging \(aurora/Dusklight technique\): \*\*measured and discarded'),
    Guard("DRS decided but never applied (pgo5n, drs_applied=0): also applies at the frame's first draw",
          "runtime/three_ds_recomp/include/fast/backends/gl_pica_dynamic_scale.h",
          r'drs_applied stayed 0.*?first draw'),
    Guard("Vsync event fails with 0x1272 (vi desc 9): clock falls back to the acquire",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'!vsyncStarted && vsyncClock\.StartAcquireFallback\(\)', test="oot3d_frame_spike_tests"),
    Guard("perf.txt accumulates runs: per-area report uses only the last session",
          "tools/oot3d/lab_tour_report.py",
          r'if h < prev: wins = \[\]'),
    Guard("lab_tour_report broke on acks without a frame",
          "tools/oot3d/lab_tour_report.py",
          r'if "frame" in a'),
    Guard("perf.txt averages mixing sessions (tour22 counted with tour21: 48 instead of 4.1)",
          "tools/oot3d/perf_window_summary.py",
          r'if previous is not None and frames < previous:\s*windows = \[\]',
          test="tests/switch/test_perf_tools.py"),
    Guard("Saturated spike profile read as a spike profile (tour21: 97% of frames above 14 ms)",
          "tools/oot3d/perf_window_summary.py",
          r'def guest_spike_saturation', test="tests/switch/test_perf_tools.py"),
    Guard("[decision] guest spike profile only with config/guest_spike_ms (makes the game slow)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'config/guest_spike_ms; off by default'),
    Guard("[decision] audio effects 0x4A0338/0x4A022C are already native (don't rewrite again)",
          "tools/oot3d/native_game_runtime/oot3d_native_guest_overrides.cpp",
          r'\{0x004A0338U, "AudioEffect_ProcessStereoReverb"\}'),
    Guard("[decision] Per-guest-thread time always on: one counter read per thread switch, "
          "not two clock reads per dispatch and per SVC behind config/gpu_timing",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'mThreadProfileRunning && selectedId != mCurrentThreadId\) \{\s*'
          r'ChargeThreadProfile\(mCurrentThreadId, ReadThreadProfileCounter\(\)\);',
          forbid=r'AddThreadTime\(', test="oot3d_native_a32_process_tests"),
    Guard("guest_threads: perf.txt line per window (which guest thread holds the game thread)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'FormatNativeA32GuestThreadsLine\(\s*process, previousThreads, runFrameCount, hostFrames\)',
          test="oot3d_native_a32_process_tests"),
    Guard("CreateThread logs guest_thread: with the requested processor at warning level (release drops info)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host.cpp",
          r'FormatNativeA32GuestThreadCreated\(\s*\*threadId, state\.r\[1\], priority, processorId, state\.r\[3\]\);'
          r'.*?SPDLOG_WARN\("\{\}", line\);',
          test="oot3d_native_a32_process_tests"),
    Guard("config/audio_deferred \"0\" without a newline was read as on (good() false at EOF)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'const bool present = audioDeferredFile\.good\(\);.*?deferDspAudio = !present',
          forbid=r'deferDspAudio = !audioDeferredFile\.good\(\)'),
    Guard("Deferred audio: pending DSP frames delivered at the end of RunFrame (nothing is left behind)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'frameCount \+= advanceGuest \? 1U : 0U;\s*\}\s*if \(deferredDspAudioFrames != 0U && !dspOnSecondCore\)'),
    Guard("Deferred audio after presentation delayed the next frame (tour24): it goes into the render wait",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'deferredDspAudioFrames != 0U && !dspOnSecondCore &&\s+renderStream\.FrameSlotsFull\(\)',
          test="oot3d_native_render_command_stream_tests"),
    Guard("Crash at startup with config/gpu_timing (pgo5e): cntvct_el0 gives Data Abort at Horizon's EL0",
          "runtime/three_ds_recomp/include/fast/backends/gl_pica_draw_phases.h",
          r'mrs %0, cntpct_el0',
          forbid=r'mrs %0, cntvct_el0'),
    Guard("Code generated with -ffast-math diverges from exact VFP emulation",
          "CMake/oot3d/native_runtime.cmake",
          r'NintendoSwitch.*?oot3d_native_whole_aot PRIVATE\s+-fno-fast-math -ffp-contract=off'),
    Guard("std::thread::detach() throws on Horizon (pthread_detach = ENOSYS)",
          "tools/oot3d/native_demo_host/oot3d_demo_host_context.cpp",
          r'pthread_detach, so\s+// std::thread::detach\(\) always throws'),
    Guard("Giant polygon in 1st-person aiming (GL_DEPTH_CLAMP on PICA draws)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica_state_cache.h",
          r'Before a PICA draw: true means glDisable\(GL_DEPTH_CLAMP\)',
          "gfx_opengl_pica_near_clip_tests (EGL)"),
    Guard("HUD vanished: PICA VAO leaked into the legacy UI (attribute divisor)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'glVertexAttribDivisor\('),
    Guard("Game Over without the continue prompt (duplicated 0x00300544 dispatch)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_candidate_dispatch.cpp",
          r'TopScreenRendererVisibilityCallDefersToGameOverDraw\(',
          "config/debug_force_game_over on the console"),
    Guard("Freeze: mallinfo() holds the malloc lock and walks a corrupted heap",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'__malloc_current_mallinfo.*?config/heap_stats',
          "oot3d_hang_watchdog_tests"),
    Guard("Config.cpp 'unable to atomically save' (rename over an existing file)",
          "runtime/three_ds_recomp/src/ship/config/ConfigPersistence.cpp",
          r'__SWITCH__.*?errc::file_exists.*?filesystem::remove\(target',
          "config_atomic_save_tests"),
    Guard("Adaptive pacing got stuck at 30 FPS (default must be legacy)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'config/fps60_mode: legacy \(default\)',
          "oot3d_native_cadence_governor_tests"),
    Guard("Log rotated on open (upstream #72) broke reading the .o3tu on the SD",
          "runtime/three_ds_recomp/src/ship/Context.cpp",
          r'__SWITCH__.*?kRotateOnOpen = false',
          "tests/switch/eden_smoke.sh (status=failed + SD md5)"),
    Guard("FPS counter (and all ImGui) invisible on Switch: ImGui without a GL context on the game thread",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'OpenGlContextCurrent\(\)\) \{\s+(?:FlushDeferredWindowClear\(\);\s+)?ImGui_ImplOpenGL3_RenderDrawData\(data\);\s+\} else if \(mRendererRouting\.Deferred',
          "Eden with config/fps_counter (check the top-left corner)"),
    Guard("Hitches when entering a new area: textures decoded on the render thread",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'SharedPicaPredecodeCache\(\)\.Take\(',
          "oot3d_pica_texture_predecode_tests + logs/hitches.txt (cause=textures)"),
    Guard("Texture predecoding turned off in the plan worker",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'picaPlanBuilder\.SetPredecodeTextures\(predecodeTextures\)',
          "oot3d_pica_texture_predecode_tests"),
    Guard("Debug menu (L3+R3) unreachable on Switch: the settings window only exists on Vulkan",
          "runtime/three_ds_recomp/src/fast/Fast3dGui.cpp",
          r'__SWITCH__.*?AddGuiWindow\(std::make_shared<Oot3dDebugMenuWindow>\(\)\)',
          "oot3d_debug_menu_tests"),
    Guard("L3+R3 didn't open the menu on Switch: the shortcut was only in module_host (outside the .nro)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_input.cpp",
          r'__SWITCH__.*?pollingState\.DebugMenuCombo\.Update\(debugCombo\)\) Fast::Oot3d::ToggleDebugMenu\(\);\s+\}\s+if \(Fast::Oot3d::DebugMenuOpen\(\)\) \{\s+hostGuiVisible = true;',
          "Eden: F+G (L3+R3) opens the debug menu"),
    Guard("Game received button presses with the debug menu open",
          "tools/oot3d/native_game_runtime/triaevum_oot3d_input_backend.cpp",
          r'if \(Fast::Oot3d::DebugMenuOpen\(\)\) \{\s+const auto sequence = mState\.sampleSequence;\s+mState = \{\};',
          "oot3d_debug_menu_tests"),
    Guard("Keyboard shortcuts (F5/F8, F11, F2, game keys) active on Switch: the console is controller-only",
          "runtime/three_ds_recomp/src/fast/Fast3dWindow.cpp",
          r'IsKeyDown\(int32_t scancode\) const \{\s+#if defined\(__SWITCH__\)\s+\(void\)scancode;\s+return false;'
          r'.*?KeyUp\(int32_t scancode\) \{\s+#if defined\(__SWITCH__\).*?return true;'
          r'.*?KeyDown\(int32_t scancode\) \{\s+#if defined\(__SWITCH__\).*?return true;',
          "Eden: F5/F8 do nothing; L3+R3 menu still opens"),
    Guard("Switch debug menu showed PC shortcuts (F5/F8, F6, F2, keyboard/mouse)",
          "tools/oot3d/native_game_runtime/oot3d_savestate_panel.cpp",
          r'TRIAEVUM_KEY_HINT\("Save state", "Save state \(F5\)"\)',
          "grep for F[0-9] in the tabs: only inside TRIAEVUM_KEY_HINT or the #else of __SWITCH__"),
    Guard("Vertex ring without UNSYNCHRONIZED: nouveau (Mesa 20.1) creates staging on every map (nfsu2-sw PERF_NOTES)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'WritePicaGlStream.*?glMapBufferRange\(.*?GL_MAP_UNSYNCHRONIZED_BIT',
          "nouveau_buffer_transfer_map: GART + UNSYNCHRONIZED returns the direct map"),
    Guard("tour14: GPU 'before acquire' timestamp emitted after glFlush (subtracted gap ~0)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl.cpp",
          r'void GfxRenderingAPIOGL::FlushDeferredWindowClear\(\).*?gWindowAcquireHook\(false\);.*?glFlush\(\);.*?ClearFramebufferNow',
          "oot3d_native_gpu_frame_timer_tests"),
    Guard("tour14: acquire wait counted as render thread cost (intermediate frames discarded)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'NativeRenderThreadBusyMs\(cpuMs, waitMs\).*?timer\.EndFrame\(kind, costCpuMs\)',
          "oot3d_native_cadence_governor_tests (TestRenderWaitIsNotCost)"),
    Guard("tour15: measured 29-33 ms pair kept (margin 0) and the area lost 3-5 FPS (cost without the wait)",
          "tools/oot3d/native_game_runtime/oot3d_native_interpolation_pacing.h",
          r'kExcludedWaitMarginMs = 5\.0',
          "oot3d_native_cadence_governor_tests (TestMarginKeepsTheRefresh)"),
    Guard("tour16 (pgo5k): pacing defaults go back to pgo5i's (wait counted, margin 0, no glFlush before the pacer)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'bool gCountRenderWait = true;.*?bool gFlushBeforePacing = false;'
          r'.*?!std::ifstream\("sdmc:/switch/oot3dre/config/interp_cpu_exclude_wait"\)'
          r'.*?std::ifstream\("sdmc:/switch/oot3dre/config/gl_pacing_flush"\)\.good\(\)',
          "oot3d_native_interpolation_pacing_tests (default margin 0)"),
    Guard("tour16 (pgo5k): acquire timestamp in pgo5i's order unless config/gl_acquire_stamp_before_flush",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl.cpp",
          r'bool gAcquireStampBeforeFlush = false;.*?if \(!gAcquireStampBeforeFlush && gWindowAcquireHook != nullptr\)'),
    Guard("ETC2 on GM20B goes through the st/mesa fallback (malloc per mip + decoding on the render thread): off by default",
          "runtime/three_ds_recomp/include/fast/oot3d/pica_etc1_transcode.h",
          r'if \(!file \|\| !std::getline\(file, text\)\) \{\s+return false;\s+\}\s+return !text\.empty\(\) && text\[0\] == \'1\';',
          "nvc0_screen_is_format_supported: ETC/ASTC only on class 0xA297 (GK20A)"),
    Guard("tour16: Data Abort in st_FreeTextureImageBuffer under TrimPicaGlResidency (corrupted heap)",
          "runtime/three_ds_recomp/tests/gfx_opengl_pica_residency_tests.cpp",
          r'TEST\(PicaGlResidency, ReleasesEachNameOnceAndNeverAPinnedOne\)',
          "gfx_opengl_pica_residency_tests (ASan)"),
    Guard("tour19 (pgo5n): DRS went down to 2.25 but drs_applied=0 (the scene target never gets a color clear)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_opengl_pica.cpp",
          r'if \(ApplyPicaGlTargetScaleAtDraw\(target, targetKey\)\)',
          "oot3d_native_dynamic_resolution_tests (TestBackendAppliesAtFirstDraw)"),
    Guard("tour19 (pgo5n): FrameSerial advances on every step (without it the scale never changes at the first draw)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'\+\+Fast::PicaGlDynamicScale::gState\.FrameSerial;'),
    Guard("tour19 (pgo5n): DRS blind to hidden GPU time (pair 26 ms < 28.3 with the governor discarding)",
          "tools/oot3d/native_game_runtime/oot3d_native_dynamic_resolution.h",
          r'double HiddenGpuMs = 3\.0;.*?pairMs \+= std::max\(0\.0, mConfig\.HiddenGpuMs\);',
          "oot3d_native_dynamic_resolution_tests (TestTour19WaterTempleHiddenGpu)"),
    Guard("log16 (test.8): deko3d DRS sat at 2.25 at 60 FPS everywhere (GL calibration: 3 ms hidden, down 85%, up 80%)",
          "tools/oot3d/native_game_runtime/oot3d_native_dynamic_resolution.h",
          r'UseDeko3dDynamicResolutionTiming\(\s*DynamicResolutionConfig &config\) noexcept \{\s+'
          r'config\.HiddenGpuMs = 0\.0;\s+config\.DownFraction = 0\.92;\s+config\.UpFraction = 0\.82;',
          "oot3d_native_dynamic_resolution_tests (TestLog16Deko3dReplay, TestDeko3dSteadyAreasHoldOneLevel)"),
    Guard("log16 (test.8): the session gives deko3d its own DRS calibration",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'drsDeko3dTiming = Fast::Deko3dRendererSelected\(\);.*?'
          r'if \(drsDeko3dTiming\)\s+Oot3dNativeGame::UseDeko3dDynamicResolutionTiming\(pacing\.Resolution\);'),
    Guard("log16 (test.8): a 30 -> 60 switch restarted DRS at 3.00 (room 8: 16 and 23 in-between frames dropped)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'dynamicResolutionMemory\.Leave\(interpolationPacingRateHz, dynamicResolution\);.*?'
          r'pacing\.ResolutionStartScale = dynamicResolutionMemory\.StartScale\(\s*rateHz, !firstConfiguration, pacing\.Resolution\);',
          "oot3d_native_cadence_governor_tests (TestLiveRateSwitchKeepsSixtyScale)"),
    Guard("tour19 (pgo5n): vsync clock failed (vsync_rc=0x1272) and pacer_vsync was left without a source",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'if \(!vsyncStarted && vsyncClock\.StartAcquireFallback\(\)\)',
          "oot3d_frame_spike_tests (TestAcquireFallbackFindsVsync)"),
    Guard("tour19 (pgo5n): the acquire that blocked feeds the fallback clock",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'clock->ObserveAcquire\(nowNs, waitMs\);'),
    Guard("SELECT stopped opening Save/Quit (upstream #80)",
          "tools/oot3d/native_game_runtime/oot3d_top_screen_pause_transaction.cpp",
          r'.',
          "oot3d_top_screen_pause_transaction_tests"),
    # --- 26/10: 14-18 ms spikes every 1800 frames (deko3d log on the render thread) ---
    Guard("deko3d: Log() wrote logs/deko3d.txt (fopen/fclose on the SD) on the render thread every 1800 frames",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_device.cpp",
          r'void Log\(const char\* format, \.\.\.\) \{.*?gLogQueue\.emplace_back\(line\);',
          "oot3d_frame_spike_tests (TestDeko3dPresentFieldsAtTheEnd)",
          forbid=r'void Log\(const char\* format, \.\.\.\) \{\s+std::lock_guard lock\(gLogMutex\);\s+FILE\*'),
    # --- 26/10: corrupted heap (tours 16-20: chunk 0x50 with swapped bk = use after free) ---
    Guard("corrupted heap: the guard-page allocator must wrap all of newlib's malloc",
          "CMake/oot3d/game.cmake",
          r'oot3d_switch_guard_alloc\.cpp.*?-Wl,--wrap=_malloc_r -Wl,--wrap=_memalign_r -Wl,--wrap=_free_r '
          r'-Wl,--wrap=_realloc_r\s+-Wl,--wrap=_calloc_r -Wl,--wrap=_malloc_usable_size_r',
          "oot3d_switch_guard_alloc_tests"),
    Guard("guard_alloc: real calloc/realloc read the chunk header; with a guard block it would be garbage",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'tBypass = true;\s+void\* const pointer = __real__calloc_r.*?'
          r'tBypass = true;\s+void\* const moved = __real__realloc_r'),
    Guard("guard_alloc: TLS only after it is enabled (libnx calls malloc before anything else)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(gEnabled\.load\(std::memory_order_relaxed\) && InRange\(gConfig, size\) && !tBypass\)'),
    Guard("guard_alloc: devkitA64's malloc() sends n >= 32 to _memalign_r(32, n); without it nothing gets a guard",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'void\* __wrap__memalign_r\(struct _reent\* reent, size_t alignment, size_t size\) \{\s+'
          r'(?:if \(void\* placed = Oot3dNativeGame::GuestWindow::TakePlacement\(size\)\) return placed;\s+)?'
          r'if \(gEnabled\.load\(std::memory_order_relaxed\) && alignment <= kAlignment',
          "oot3d_switch_guard_alloc_tests (TestLayout)"),
    Guard("guard_alloc: slack page after the object (header of newlib's next chunk)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.h",
          r'kBlockPages = 2U;'),
    Guard("guard_alloc: guard from boot = boot out of memory ('Not enough space'); only enabled in the session",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window.cpp",
          r'Oot3dNativeGame::GuardAlloc::Arm\(\);\s+// Hang watchdog'),
    Guard("guard_alloc page mode: 32768 closed pages exhausted the kernel (SDL audio/FS failed on the 1st frame)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(parsed\.GuardMode == Mode::Page && parsed\.QuarantineBlocks > kMaxPageQuarantine\)',
          "oot3d_switch_guard_alloc_tests (TestParse)"),
    Guard("guard_alloc canary: free read ptr-256 directly and hit an unmapped page (p6t, exception in __wrap__free_r)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'reinterpret_cast<const uint64_t\*>\(address\)\[-1\] != kCanaryTailMagic\) return nullptr;\s+'
          r'auto\* header = reinterpret_cast<CanaryHeader\*>\(address - sizeof\(CanaryHeader\)\);'),
    Guard("guard_alloc canary: filling the object with 0xFD on free hung the console (read after free); copy only",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'std::memcpy\(header->Snapshot, header \+ 1, header->Block\.Size\);',
          "oot3d_switch_guard_alloc_tests (TestCanary)", forbid=r'kCanaryFill'),
    Guard("guard_alloc: cap on live blocks (without it the heap runs out)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'if \(gLive\.load\(std::memory_order_relaxed\) >= gConfig\.MaxLive\) \{'),
    Guard("exception handler writes logs/hang.txt without allocating and returns (Atmosphère crash report still happens)",
          "tools/oot3d/native_game_runtime/oot3d_switch_guard_alloc.cpp",
          r'void __libnx_exception_handler\(ThreadExceptionDump\* context\) \{\s+HandleException\(context\);\s+\}'),
    # --- 04/11: first-launch import of the player's dump into game/ ---
    Guard("ROM import must run before the launch arguments read game/ (manifest)",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'if \(!Oot3dNativeGame::RunSwitchRomImport\(\)\)\s+return 0;.*?oot3d_native_process_manifest\.json'),
    Guard("ROM import: Horizon FS does not rename over an existing file (remove first)",
          "tools/oot3d/native_game_runtime/oot3d_rom_import.cpp",
          r'void Replace\(const fs::path& from, const fs::path& to\) \{\s+std::error_code error;\s+fs::remove\(to, error\);',
          "oot3d_rom_import_tests"),
    # --- 04/11: OTA updater and Exit game (release) ---
    # Horizon cannot rename over a file or touch an open one: the updater
    # moves the target aside first and swaps the files other than the NRO at
    # the next start, before resources.o2r is opened.
    Guard("OTA install: rename over an existing file fails on Horizon (move the target to .old first)",
          "tools/oot3d/native_game_runtime/oot3d_update_release.cpp",
          r'std::filesystem::rename\(target, backup, code\);.*?std::filesystem::rename\(staged, target, code\);',
          "oot3d_update_release_tests (TestInstall)"),
    Guard("OTA install: files swapped at the next start before anything opens them",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'Oot3dNativeGame::FinishPendingUpdate\("sdmc:/switch/oot3dre"\);.*?homebrewArguments\{'),
    Guard("release zip has no TopScreen mod payload: a missing atlas_overrides.o3tu must not stop the game",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'if \(!std::filesystem::is_regular_file\(homebrewArguments\[14\]\)\) \{\s+'
          r'homebrewArgv\.erase\(homebrewArgv\.begin\(\) \+ 13, homebrewArgv\.begin\(\) \+ 15\);'),
    Guard("Exit game / Close game (L3+R3) close the window like HOME > Close",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'if \(physicalInputFrame\.Exit \|\| devLabQuit \|\| Fast::Oot3d::ExitGameRequested\(\)\) \{'),
    # --- 04/11: crash on Exit game (hbl + 0x3810, 2001-0106) and right after the ROM import ---
    Guard("Exit game must leave through AM: returning to hbl crashed it (InvalidCurrentMemory), svcExitProcess still showed a crash",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'if \(Fast::Oot3d::ExitGameRequested\(\)\)\s+__nx_applet_exit_mode = 1;',
          forbid=r'svcExitProcess\('),
    Guard("start-up console screens (ROM import, HUD textures): stdout/stderr put back after consoleExit (first log line crashed)",
          "tools/oot3d/native_game_runtime/oot3d_switch_console_screen.h",
          r'consoleExit\(nullptr\);\s+devoptab_list\[STD_OUT\] = mStdout;\s+devoptab_list\[STD_ERR\] = mStderr;'),
    Guard("second start-up screen (HUD texture offer after the game data import) stayed black: libnx redirects stdout only on the first consoleInit",
          "tools/oot3d/native_game_runtime/oot3d_switch_console_screen.h",
          r'consoleInit\(nullptr\);.*?static const devoptab_t\* const console = devoptab_list\[STD_OUT\];\s+'
          r'devoptab_list\[STD_OUT\] = console;'),
    Guard("game data import hands over to the HUD texture offer on the same console (no screen of its own)",
          "tools/oot3d/native_game_runtime/oot3d_switch_rom_import.cpp",
          r'SwitchConsoleScreen& screen = StartupScreen\("Ocarina of Time 3D - game data import"\);',
          forbid=r'SwitchConsoleScreen screen\('),
    Guard("HUD texture offer reuses the start-up console the import opened",
          "tools/oot3d/native_game_runtime/oot3d_switch_topscreen_import.cpp",
          r'SwitchConsoleScreen& screen = StartupScreen\("Ocarina of Time 3D - HUD textures"\);',
          forbid=r'SwitchConsoleScreen screen\('),
    Guard("the start-up console is closed after the HUD step, before SDL/deko3d take the window",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'if \(!Oot3dNativeGame::RunSwitchTopScreenImport\(\)\)\s+return 0;\s+(//[^\n]*\n\s+)*'
          r'Oot3dNativeGame::CloseStartupScreen\(\);.*?FinishPendingUpdate'),
    # --- 04/11: TopScreen HUD textures downloaded and built on the console ---
    # The mod (CC BY-NC-ND) cannot ship with a release: the console downloads
    # it and builds config/atlas_overrides.o3tu. A damaged pack stops the game
    # at start-up ("cannot load TopScreen texture overrides").
    Guard("HUD texture offer runs after the ROM import and before the launch arguments look for atlas_overrides.o3tu",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'if \(!Oot3dNativeGame::RunSwitchRomImport\(\)\)\s+return 0;.*?'
          r'if \(!Oot3dNativeGame::RunSwitchTopScreenImport\(\)\)\s+return 0;.*?config/atlas_overrides\.o3tu'),
    Guard("HUD texture pack: read back before it replaces anything; Horizon does not rename over a file (remove first)",
          "tools/oot3d/native_game_runtime/oot3d_topscreen_import.cpp",
          r'the SD card did not keep .*?fs::remove\(path, error\); // Horizon cannot rename over an existing file\s+'
          r'fs::rename\(partial, path, error\);',
          "oot3d_topscreen_import_tests (TestWriteAndIdentify)"),
    Guard("HUD texture pack written last: the game only looks for the font pack beside an existing pack",
          "tools/oot3d/native_game_runtime/oot3d_topscreen_import.cpp",
          r'WriteFileVerified\(configDirectory / kFontPackName, outputs\.Fonts\);.*?'
          r'WriteFileVerified\(configDirectory / kUiAtlasHdName, outputs\.UiAtlasHd\);.*?'
          r'outputs\.HdTextures\[index\]\);\s+\}\s+'
          r'WriteFileVerified\(configDirectory / kPackName, outputs\.Pack\);\s+\}'),
    # --- 05/11: without the TopScreen pack (player chose "not now") ---
    Guard("no TopScreen pack: the Items hint/D-pad badges asked for menu_atlas and the frame threw (game closed, hbl crash)",
          "tools/oot3d/native_game_runtime/oot3d_native_ui_texture_provider.cpp",
          r'starts_with\("oot3d/topscreen/2\.1\.1/"\).*?pixels\.rgba8\.assign\(4U, 0U\);\s+\+\+mStats\.profile_asset_absent;\s+return true;',
          "oot3d_native_ui_texture_provider_tests (packlessProvider)"),
    Guard("a fatal error must close the game through AM (with the error dialog), not return to hbl (2001-0106)",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'catch \(const std::exception& ex\).*?ShowSwitchFatalError\(ex\.what\(\)\);.*?__nx_applet_exit_mode = 1;'),
    # --- 05/11: TopScreen HD UI atlas (3x) and texture packs on the HUD ---
    Guard("HD UI atlas: a missing or bad file logs and keeps the native atlas, never stops the game",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'HD UI atlas unused: cannot decode.*?HD UI atlas unused: %dx%d is not a whole multiple',
          forbid=r'throw std::runtime_error\([^;]*HD UI atlas'),
    Guard("HD UI atlas and HD images passed only beside an installed pack (--topscreen-ui-atlas-hd and "
          "--topscreen-hd-textures require the overrides)",
          "tools/oot3d/native_game_runtime/oot3d_native_game.cpp",
          r'homebrewArgv\.erase\(homebrewArgv\.begin\(\) \+ 13, homebrewArgv\.begin\(\) \+ 15\);\s+'
          r'\} else \{\s+if \(std::filesystem::is_regular_file\(\s+"sdmc:/switch/oot3dre/config/topscreen_ui_atlas_hd\.png"\)\)'
          r'.*?"--topscreen-hd-textures".*?\}\s+\}'),
    Guard("HUD custom textures (packs, the mod's HD images), asked for ahead or resolved, are named after the "
          "game's own texture, not this port's TopScreen CTXB override (the hearts and B label kept the old art)",
          "tools/oot3d/native_game_runtime/oot3d_native_ui_texture_provider.cpp",
          r'original = encoded;\s+switch \(mTextureOverrides->Apply\(encoded\)\).*?'
          r'ApplyCustomTexture\(encoding->native_pica_format, original\.empty\(\) \? encoded : original,\s+'
          r'pixels, true, false\);.*?'
          r'ApplyCustomTexture\(encoding->native_pica_format, original, pixels, false, true\);',
          "oot3d_native_ui_texture_provider_tests (identified by the game's own bytes, ahead too)"),
    Guard("HUD pack images: UI requests keep the 3x cap and their own cache (ReleaseDecoded of a world upload must not drop them)",
          "runtime/three_ds_recomp/src/fast/oot3d/azahar_texture_pack.cpp",
          r'DecodeOptions\{native, kUiTextureMaxScale, false\}',
          "oot3d_azahar_texture_pack_tests (CapsHudTexturesAtThreeTimesWithoutMipmaps)"),
    # --- 07/11: the TopScreen HUD showed the game's art for ~0.5 s, then the pack's ---
    # HUD decodes waited behind a scene's world PNGs on the single pack worker.
    Guard("HUD pack images decode on their own worker, never queued behind world images",
          "runtime/three_ds_recomp/src/fast/oot3d/azahar_texture_pack.cpp",
          r'\(userInterface \? mUiLoadJobs : mLoadJobs\)\.push_back\(std::move\(job\)\);.*?'
          r'mUiWorkAvailable\.wait\(lock, \[&\] \{ return mStopping \|\| !mUiLoadJobs\.empty\(\); \}\);',
          "oot3d_azahar_texture_pack_tests (HudImagesDoNotWaitBehindWorldImages)"),
    Guard("Switch: the HUD's pack images are remembered in config/texture_pack_hud and decoded at start-up",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'MakeTexturePackConfiguration\(mTexturePackSettings, kConfigDirectory\).*?'
          r'MakeTexturePackConfiguration\(settings, kConfigDirectory\)',
          "oot3d_azahar_texture_pack_tests (HudImagesSeenOnceAreDecodedAtTheNextStart)"),
    Guard("a HUD texture whose pack image decodes is held, not drawn with the old art (provider marks the stand-in)",
          "tools/oot3d/native_game_runtime/oot3d_native_ui_texture_provider.cpp",
          r'pixels\.provisional = true;\s+pixels\.native_stand_in = custom\.rgba8 == nullptr;',
          "oot3d_n64_ui_renderer_tests (TestHeldFirstDraws), oot3d_native_ui_texture_provider_tests"),
    # --- 07/11 (test.8): first start, empty HUD list: two HUD images first seen in play, held up to 263 ms ---
    Guard("the HUD's textures are asked for ahead of its first frame (title screen), one per frame, again per revision",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'uiLifecycleBridge\.HudTexturesAhead\(.*?n64UiRenderer\.RequestAhead\(ahead\) && complete',
          "oot3d_n64_ui_renderer_tests (TestRequestAhead), oot3d_native_ui_lifecycle_bridge_tests"),
    Guard("the UI renderer's provider (ocarina text runtime) passes the pack revision and ahead requests through",
          "tools/oot3d/native_game_runtime/oot3d_top_screen_ocarina_text_runtime.h",
          r'Revision\(\) const noexcept override \{ return mNativeTextures\.Revision\(\); \}.*?'
          r'mNativeTextures\.RequestAhead\(identity\);',
          "oot3d_top_screen_ocarina_text_runtime_tests"),
    Guard("an ahead HUD request queues its pack decode as one decoded ahead",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'\.UserInterface = true,\s+\.Ahead = request\.ahead,',
          "oot3d_azahar_texture_pack_tests (HudImagesAskedAheadAreReadyAtFirstSight), "
          "oot3d_native_ui_texture_provider_tests"),
    # --- 07/11 (test.9): pause pages showed the game's tab bar, then the pack's, on every page ---
    Guard("pause pages (PICA, Ui draws) resolve their pack images as HUD images, shared with the HUD's ahead requests",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'menu = PicaTextureIsMenuArt\(view, menuDraw, mMenuImages\);.*?\.UserInterface = menu,\s+\.Menu = menu,',
          "oot3d_azahar_texture_pack_tests (MenuTexturesShareTheHudImagesAndStayDecoded), "
          "oot3d_deko3d_backend_tests (TestMenuArt)"),
    Guard("a menu texture's pack image is not released after its upload (another guest address re-decoded it)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'if \(!menu\) \{\s+::Oot3d::Renderer::AzaharTexturePackRuntime::Instance\(\)\.ReleaseDecoded\(',
          "oot3d_azahar_texture_pack_tests (MenuTexturesShareTheHudImagesAndStayDecoded)"),
    Guard("a HUD/menu texture asked for at its draw goes before the images decoded ahead",
          "runtime/three_ds_recomp/src/fast/oot3d/azahar_texture_pack.cpp",
          r'if \(onSight\) \{\s+mUiLoadJobs\.push_front\(std::move\(job\)\);',
          "oot3d_azahar_texture_pack_tests (TexturesOnScreenGoBeforeHudImagesAskedAhead)"),
    # --- 07/11 (pop18, after test.10): file select, world textures and grass without pop-in
    # (SWITCH_PORT.md "File select, world textures and grass without pop-in").
    Guard("file select (relocated bottom screen, atmosphere layer) resolves its pack images as menu art",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'draw\.Composition\.Layer == Renderer3ds::PicaCompositionLayer::Ui \|\|\s+'
          r'\(mTexturePack && mMenuImages && !draw\.Textures\.empty\(\) && RelocatedMenuDraw\(draw\)\);',
          "gl_widescreen_menu_policy_tests (FileSelectPiecesAreRelocatedMenuDrawsInEitherDomain)"),
    Guard("a scene change reaches the pack runtime in frame order (scene lists, preload, hold)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'frameArea\.Scene != texturePackScene\) \{\s+texturePackScene = frameArea\.Scene;\s+'
          r'renderStream\.Submit\(\[scene = texturePackScene\] \{\s+'
          r'::Oot3d::Renderer::AzaharTexturePackRuntime::Instance\(\)\.NoteScene\(scene\);',
          "oot3d_azahar_texture_pack_tests (SceneImagesSeenOnceAreDecodedWhenTheSceneStarts)"),
    Guard("a scene change drops the scene before's queued world decodes",
          "runtime/three_ds_recomp/src/fast/oot3d/azahar_texture_pack.cpp",
          r'\+\+mSceneSerial;\s+mScene\.Dropped \+= DropEarlierSceneJobsLocked\(\);',
          "oot3d_azahar_texture_pack_tests (SceneChangesDropTheBacklogAndTheHoldWaitsForImagesOnScreen)"),
    Guard("the hold waits only for world textures drawn native last frame, within the scene's budget",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_pica_resources.cpp",
          r'pack\.WaitForImages\(mHoldHashes, std::chrono::microseconds\(mHoldLeftMicros\)\);',
          "oot3d_azahar_texture_pack_tests (SceneChangesDropTheBacklogAndTheHoldWaitsForImagesOnScreen)"),
    Guard("deko3d grass keeps placements on the SD card (config/grass_placement_cache)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_grass.cpp",
          r'if \(placementCache\) \{\s+state->Placements->SetCacheDirectory\(',
          "oot3d_grass_switch_tests (Oot3dGrassPlacementCache), oot3d_grass_async_placement_builder_tests"),
    Guard("HUD button labels draw over the item icons (a pack's bigger bomb hid the R of ZR)",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_hud.cpp",
          r'primitive\.layer = kTopScreenButtonLabelLayer;.*?'
          r'primitive\.layer = index < 4U \? kTopScreenButtonLabelLayer : 13U;',
          "oot3d_top_screen_hud_touch_tests (TestLabelsOverItemIcons)"),
    # --- 05/11: inventory taps one slot off (pause Items/Gear/Map, TopScreen) ---
    # The touch scaled the 400-wide top screen down to the 320-wide touch
    # screen, but TopScreen draws the page one pixel per pixel at X + 40 (the
    # widened Items grid at X + 63, its sixth column at X + 80): a tap on the
    # first Items column landed on the second.
    Guard("tapping an item in the pause inventory selected the block to its right (left side of the page)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_input.cpp",
          r'MapPresentationPointToTouch\(\s+TopScreenPresentedToPageX\(topScreen\.X, layout\), topScreen\.Y,',
          "oot3d_native_a32_input_tests (TestSwitchPageTouchLandsOnTheTappedElement)",
          forbid=r'TopScreen400x240'),
    Guard("inventory touch: the widened Items page maps with its own layout (grid X + 63, sixth column X + 80)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_input.cpp",
          r'ReadTopScreenWidePageState\(memory, &page\) &&\s+page\.ItemsActive\s+'
          r'\? NativeA32TouchPresentation::TopScreenWideItemsPage'),
    # --- 05/11: erasing a save file on the file select screen closed the game ---
    # The guest deletes "data:/save0N.bin" (worker 0x41E7B8) and sends any failed
    # result to its fatal-error handler. The host's DeleteFile made the save root
    # "sdmc:/switch/oot3dre/savedata" canonical: std::filesystem treats it as
    # relative and put the cwd in front, so the delete always failed on Horizon.
    Guard("erasing a save file on the file select screen closed the game (DeleteFile canonicalized the sdmc:/ save root)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_fs.cpp",
          r'kFsDeleteFileRequest\) \{.*?auto target = archive->Root;\s+bool missing = false;.*?'
          r'removed = std::filesystem::remove\(target, error\);',
          "oot3d_native_a32_ctr_fs_tests (TestSlotEraseWithRelativeSaveRoot)",
          forbid=r'std::filesystem::(weakly_canonical|canonical|absolute)\('),
    # --- hud18 (test.10): the Restoration HUD's layout was wrong; it is removed and a
    # saved hud_layout (normal or restoration) loads as the Normal HUD. ---
    Guard("hud18: L3+R3 > HUD offers no Restoration layout; a saved hud_layout is read and ignored",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_config.cpp",
          r'"schema", "hud_layout", "hud_scale"',
          "oot3d_top_screen_mod_profile_tests (saved hud_layout loads)",
          forbid=r'TopScreenHudLayout|ParseTopScreenHudLayout'),
    Guard("hud18: the settings panel has no layout radio buttons",
          "tools/oot3d/native_game_runtime/oot3d_top_screen_settings_panel.cpp",
          r'Checkbox\("Render Items hint"',
          forbid=r'Restoration HUD|HudLayout'),
    # --- hud18: the HUD said ZR/ZL over the items the physical R/L use (ZR/ZL do the
    # game's shield/Z-target). custom_ui01 has no R/L label: the second letter of its
    # ZR (rect 15) and ZL (rect 14) labels is drawn, centred where the label was. ---
    Guard("hud18: Item I/II lanes labelled ZR/ZL instead of the physical R/L",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_hud.cpp",
          r'result\.Quads\[0\] = \{alpha != 0\.0F,\s*\{372\.0F - 3\.875F, 31\.0F\},\s*'
          r'\{7\.75F, 16\.0F\}, \{54\.625F, 231\.0F\},.*?'
          r'\{6\.375F, 16\.0F\}, \{253\.0F, 227\.0F\},',
          "oot3d_top_screen_mod_profile_tests (R/L labels)"),
    # --- hud18: R (host ZR) lowered the ZR circle and the Y item; L only ZL; X/Y/A/B
    # never. The icons read the lane offsets by item region (Item I, X, Y, Item II),
    # so lane 2 (ZR) moved the Y icon. No lane moves on a press now. ---
    Guard("hud18: pressing R also moved the Y item (no HUD lane moves on a press)",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_mod_profile_hud.cpp",
          r'bool ReadTopScreenTouchDynamicState\(NativeA32Memory &memory,\s*'
          r'TopScreenTouchDynamicState \*state,',
          "oot3d_top_screen_mod_profile_tests (press handling)",
          forbid=r'VerticalOffsets|kNativeButtonOffsets'),
    # --- 11/07: Landing roll (N64), L3+R3 > Game > Gameplay (config/no_landing_roll) ---
    # OoT3D keeps the run-jump animation to the ground, so its run-jump-end check (0x004BC080)
    # pre-empts the game's own roll (0x004BC120). The hook must stay in the block-entry list and
    # divert only on the decision (generated code untouched).
    Guard("Landing roll: no roll after jumping off a ledge (run-jump end pre-empted the roll)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_projection_policy.cpp",
          r'pc == Oot3dNativeGame::kLandingRollHookPc && wholeAotExecutionActive &&.*?'
          r'DecideLandingRoll\(landing\) ==\s+Oot3dNativeGame::LandingRollChoice::Roll\).*?'
          r'Oot3dWholeAotExitAt\(Oot3dNativeGame::kLandingRollSetupPc, state\)',
          "oot3d_landing_roll_tests"),
    Guard("Landing roll: the hook pc is a block-entry hook",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'blockEntryHooks = std::vector<uint32_t>\{[^}]*Oot3dNativeGame::kLandingRollHookPc,',
          "oot3d_landing_roll_tests"),
    # --- 07/11: fixes from upstream coccofresco/TriAevum after beta.1c ---
    # dsp::DSP 0x14 (header 0x00140082) went unserved shortly before the end credits
    # (TRIAEVUM_HOST_REQUEST_UNSERVED detail=0x140082); upstream 657e3d0f (#104).
    Guard("process faulted before the end credits (DSP InvalidateDataCache 0x14 unserved, upstream #104)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_dsp.cpp",
          r'event\.Detail == kDspFlushDataCacheRequest \|\|\s+event\.Detail == kDspInvalidateDataCacheRequest',
          "oot3d_native_a32_ctr_host_tests"),
    # A guest thread's ExitThread while the others only waited made Run report
    # Terminated, and the window loop ended the game (leaving a hint-stone vision);
    # upstream 034405bd.
    Guard("game closed when one guest thread exited while the others waited (hint-stone vision)",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_process.cpp",
          r'result\.Kind = ResolveStoppedThreadRunKind\(\s+result\.Kind, AllThreadsTerminated\(\)\);.*?'
          r'result\.Kind = ResolveStoppedThreadRunKind\(\s+result\.Kind, AllThreadsTerminated\(\)\);',
          "oot3d_native_a32_process_tests (TestThreadExitWhileOthersWait)",
          forbid=r'if \(AllThreadsTerminated\(\)\) \{\s+result\.Kind = NativeA32ProcessRunKind::Terminated;'),
    # --- 07/11: tevskip, pass-through TEV stages compiled as copies in deko3d.pack ---
    # The binary must stay under the unmodified text's key (the console hashes the text it
    # generates); stage 0's "previous" source is another input; a `precise` copy stopped uam
    # folding the earlier clamps into .sat (+24 instructions on some texts).
    Guard("tevskip: pass-through rewrite keyed by the unmodified text, stage 0 left alone, copies not precise",
          "tools/oot3d/deko3d_shader_pack.py",
          r'stage >= 1 and rgb_ok and match\["expr"\] == "combiner_output\.rgb".*?'
          r'stage >= 1 and alpha_ok and match\["expr"\] == "combiner_output\.a".*?'
          r'source, \(colors, alphas, stages\) = skip_tev_passthrough\(text\).*?'
          r'compiled\[key\] = \(stage, source, flags\).*?entries\[key\] = \(stage, blob, flags\)',
          "tests/switch/test_tev_passthrough.py",
          forbid=r'shader_text_key\(\w+, source\)|precise vec3 color_output_\{stage\} = combiner_output\.rgb'),
    # From the fork author's a7424d3b: the pack read byte by byte delayed the boot.
    Guard("deko3d.pack read byte by byte delayed the boot (fork a7424d3b: one read)",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_shaders.cpp",
          r'std::ios::binary \| std::ios::ate\);.*?file\.read\(reinterpret_cast<char\*>\(bytes\.data\(\)\)',
          forbid=r'std::vector<uint8_t> bytes\(\(std::istreambuf_iterator'),
    Guard("tevskip: config/deko3d_pack picks the pack and deko3d.txt says which and whether it skips TEV stages",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_shaders.cpp",
          r'ShaderPackNameFromConfig\(line\).*?tev_passthrough=%s',
          "oot3d_deko3d_backend_tests (TestShaderPack)"),
    # --- 07/11: test.9 "the general flickers were still happening" (logs17) ---
    # A command list the GSP queue kicked after the next Gameplay_Draw had switched the frame to
    # its HUD phase came through as UI: its world models lost per-pixel lighting, grass and their
    # placement for one frame (ui_list in deko3d.txt). The tracker stamps spans with their
    # Gameplay_Draw generation; a list whose spans all predate the current one keeps its on-time
    # domain (config/late_lists_as_ui restores the old attribution).
    Guard("blink17: spans carry the Gameplay_Draw generation they were written in",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_composition.cpp",
          r'span\.WriteGeneration = mWriteGeneration;.*?'
          r'if \(pc == kOot3dGameplayDrawEntry\) \{\s+if \(\+\+mWriteGeneration == 0U\) \+\+mWriteGeneration;',
          test="oot3d_native_pica_composition_tests (TestWriteGeneration)"),
    Guard("blink17: a late list (every span from an earlier Gameplay_Draw, kicked in the HUD phase) keeps its "
          "on-time domain unless config/late_lists_as_ui",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_frontend.cpp",
          r'const bool lateList = IsLateCompositionCommandList\(\s*mCommandListCompositionDomain, compositionSpans, '
          r'mCurrentWriteGeneration\);.*?lateList && mLateCommandListsOnTime \? mOnTimeCompositionDomain.*?'
          r': listDomain == Oot3dPicaCompositionDomain::Ui.*?auto domain = listDomain;',
          test="oot3d_native_pica_frontend_tests"),
    Guard("blink17: the GSP submission gives the frontend the refresh domain and the write generation",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_gsp.cpp",
          r'SetCommandListTiming\(.*?PicaRefreshCompositionDomain.*?CurrentWriteGeneration\(\)'),
    Guard("blink17: config/late_lists_as_ui is the A/B switch of the late-list attribution",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'SetLateCommandListsOnTime\(\s*!std::filesystem::exists\(configDirectory / "late_lists_as_ui".*?'
          r'hostConfig\.PicaRefreshCompositionDomain = &picaRefreshCompositionDomain;'),
    Guard("blink17: the transient watch compares each frame kind with its own kind (at 60 FPS the real frame "
          "and its in-between frame lose the same draw) and names the first draw lost to its composition",
          "runtime/three_ds_recomp/src/fast/backends/gfx_deko3d_lighting_draw.cpp",
          r'auto& history = mHistory\[mOpen\.InBetween \? 1U : 0U\];.*?'
          r'composition && mOpen\.LostToComposition\(\) == 0U',
          test="oot3d_deko3d_inbetween_lighting_tests (TestLightingTransients)"),
    Guard("blink17: perf.txt counts late lists, UI-kept world draws, scene draws in no span and invalid spans",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_frame.cpp",
          r'" spans_invalid=".*?" late_lists=".*?" ui_world_draws=".*?" no_span_draws=".*?" owner_holes=".*?'
          r'"composition note: window_end="'),
    # --- 07/11: test.10 flicker (logs18, Temple of Time video 14:54:23 frame 102) ---
    # Every remaining lighting transient was one scene draw in no span (no_span=1): a draw a CMB
    # renderer writes inside its pass but outside its mesh packets lost per-pixel lighting for a
    # game frame (the room mesh: pillar lighter, windows brighter). The tracker records the whole
    # pass and the frontend gives such a draw its pass's composition (config/no_pass_span_fallback = old).
    Guard("flicker18: the tracker records each scene CMB pass (SubmitDrawHandle entry to its return) as a pass span",
          "tools/oot3d/native_game_runtime/oot3d_native_pica_composition.cpp",
          r'if \(mActivePassBeginValid && !mActiveUiScope && !mActiveAtmosphereScope &&.*?'
          r'span\.Attribution\.TemporalIdentity = \{\};.*?mPendingPassSpans\.push_back',
          test="oot3d_native_pica_composition_tests (TestPassSpans)"),
    Guard("flicker18: a scene draw in no span but inside a CMB pass takes the pass's composition",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_frontend.cpp",
          r'\+\+mCompositionResolveStats\.NoSpanDraws;.*?if \(passSpan != nullptr && mPassSpanFallback\) \{\s*'
          r'\+\+mCompositionResolveStats\.PassSpanDraws;\s*return passSpan;',
          test="oot3d_native_pica_frontend_tests"),
    Guard("flicker18: the GSP submission hands the list's pass spans to the frontend",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_ctr_host_gsp.cpp",
          r'TakeCommandListPassSpans\(.*?SetNextCommandListPassSpans\('),
    Guard("flicker18: config/no_pass_span_fallback is the A/B switch of the pass fallback",
          "tools/oot3d/native_game_runtime/oot3d_native_a32_window_setup.cpp",
          r'SetPassSpanFallback\(\s*!std::filesystem::exists\(configDirectory / "no_pass_span_fallback"'),
    Guard("flicker18: late_list notes have their own cap, so no_span notes still reach perf.txt",
          "tools/oot3d/native_pica_frontend/oot3d_native_pica_frontend.cpp",
          r'note\.Kind == Oot3dPicaCompositionNoteKind::LateList && mLateListNotesRecorded\+\+ >= 8U'),
    Guard("hudscale75: the Switch's default HUD scale is 0.75 with no config (the mod's screenshots are ~0.70)",
          "tools/oot3d/ui_topscreen/oot3d_top_screen_config.h",
          r'#ifdef __SWITCH__\s*inline constexpr float kTopScreenHudScaleDefault = 0\.75F;'),
    Guard("hudscale75: the staged Switch config ships HUD scale 0.75",
          "config/topscreen_ui.switch.json",
          r'"hud_scale": 0\.75,'),
]

# Native VFP overrides that cost more than the translation: they must stay
# disabled until someone measures again (guest FPSCR, 2026-10-18).
DISABLED_OVERRIDES = {"0x003727F0", "0x003325F0", "0x0036E168", "0x00372070", "0x002CDE90"}


def check_guard(guard: Guard) -> str | None:
    path = ROOT / guard.path
    if not path.exists():
        return f"file disappeared: {guard.path}"
    text = path.read_text(encoding="utf-8", errors="replace")
    if not re.search(guard.pattern, text, re.S):
        return f"fix not found in {guard.path}"
    if guard.forbid and re.search(guard.forbid, text, re.S):
        return f"forbidden pattern came back in {guard.path}"
    return None


def check_overrides() -> str | None:
    path = ROOT / "tools/oot3d/native_a32_runtime/guest_overrides.json"
    data = json.loads(path.read_text(encoding="utf-8"))
    entries = data if isinstance(data, list) else next(
        value for value in data.values() if isinstance(value, list))
    disabled = {entry["entry"] for entry in entries if entry.get("disabled")}
    missing = sorted(DISABLED_OVERRIDES - disabled)
    if missing:
        return "VFP overrides re-enabled without measurement: " + ", ".join(missing)
    return None


def main() -> int:
    if "--list" in sys.argv:
        for guard in GUARDS:
            print(f"- {guard.bug}\n    {guard.path}" + (f"\n    test: {guard.test}" if guard.test else ""))
        return 0
    failures = 0
    for guard in GUARDS:
        error = check_guard(guard)
        print(("FAIL " if error else "ok   ") + guard.bug + (f"\n     {error}" if error else ""))
        failures += error is not None
    error = check_overrides()
    print(("FAIL " if error else "ok   ") + "5 VFP overrides remain disabled"
          + (f"\n     {error}" if error else ""))
    failures += error is not None
    print(f"{len(GUARDS) + 1 - failures}/{len(GUARDS) + 1} guards ok")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
