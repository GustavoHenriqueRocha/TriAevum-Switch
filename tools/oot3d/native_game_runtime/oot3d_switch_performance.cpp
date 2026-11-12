// L3+R3 > Video > Performance mode: the console side of
// oot3d_switch_performance.h (GPU and memory through apm, CPU High through
// clkrst / pcv, and the CpuClockKeeper driven by applet events, a follow-up
// thread and the perf.txt window).
//
// Sleep: SwitchWakerHD set CPU 1224 MHz through clkrst from the game and
// Erista consoles then did not wake from sleep (issue #33, commit 8d9a643e:
// "Putting the stock clocks back from an appletHook focus callback did not
// help: the program is suspended before the hook runs"; its clkrst sessions
// stayed open for the whole run). Hence:
// - Every clkrst / pcv access opens the service and one session, does one
//   read or write and closes both (as sys-clk and RetroArch do): nothing of
//   clkrst stays open while the game may be suspended.
// - With libnx's default focus handling (SuspendHomeSleep) AM suspends the
//   game for HOME and sleep without telling it. While CPU High is on (only
//   then) the game uses NoSuspend, as RetroArch does with its CPU clocks, so
//   HOME and sleep reach the applet hook as FocusState Background. The hook
//   puts the earlier CPU rate back first, then sets SuspendHomeSleep again,
//   which lets AM suspend the game as it always does; once the game runs
//   again (in focus) the follow-up thread sets NoSuspend and CPU High again.
// - Exit (atexit, ExitRequest) puts the earlier rate back too.
// CPU Standard (the default) touches none of this: no clkrst, no hook, the
// default focus handling.
#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <thread>

#include "oot3d_game_log.h"
#include "oot3d_hang_watchdog.h"
#include "oot3d_switch_clocks.h"
#include "oot3d_switch_performance.h"

namespace Oot3dNativeGame {
namespace {

using Keeper = CpuClockKeeper;

uint64_t NowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

// One line for logs/oot3d_native_game.log (oot3d_game_log.h: kept until the
// logger exists, then queued for its thread; never an SD write here, so the
// HOME/sleep hook may log).
[[gnu::format(printf, 1, 2)]] void Log(const char* format, ...) {
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    WriteGameLog(line);
}

// ---- One clock read or write, its sessions closed afterwards ------------------

enum class ClockModule : uint8_t { Cpu, Gpu, Memory };

struct ClockAccess {
    uint32_t Hz = 0U;  // the rate read (after the write, for a write)
    Result Rc = 0;
    const char* FailedStep = "";
};

PcvModuleId ClkrstModule(ClockModule module) {
    switch (module) {
    case ClockModule::Gpu: return PcvModuleId_GPU;
    case ClockModule::Memory: return PcvModuleId_EMC;
    case ClockModule::Cpu: break;
    }
    return PcvModuleId_CpuBus;
}

PcvModule PcvModuleOf(ClockModule module) {
    switch (module) {
    case ClockModule::Gpu: return PcvModule_GPU;
    case ClockModule::Memory: return PcvModule_EMC;
    case ClockModule::Cpu: break;
    }
    return PcvModule_CpuBus;
}

// setHz = 0 reads. clkrst on 8.0.0+ (pcv's clock commands are gone there),
// pcv before.
ClockAccess AccessClock(ClockModule module, uint32_t setHz) {
    ClockAccess access;
    if (hosversionAtLeast(8, 0, 0)) {
        {
            HangWatch::ServiceCallScope call("clkrst:Initialize");
            access.Rc = clkrstInitialize();
        }
        if (R_FAILED(access.Rc)) {
            access.FailedStep = "clkrstInitialize";
            return access;
        }
        ClkrstSession session{};
        {
            HangWatch::ServiceCallScope call("clkrst:OpenSession");
            access.Rc = clkrstOpenSession(&session, ClkrstModule(module), 3);
        }
        if (R_FAILED(access.Rc)) {
            access.FailedStep = "clkrstOpenSession";
        } else {
            if (setHz != 0U) {
                HangWatch::ServiceCallScope call("clkrst:SetClockRate");
                access.Rc = clkrstSetClockRate(&session, setHz);
                if (R_FAILED(access.Rc)) access.FailedStep = "clkrstSetClockRate";
            }
            if (R_SUCCEEDED(access.Rc)) {
                HangWatch::ServiceCallScope call("clkrst:GetClockRate");
                u32 hz = 0U;
                access.Rc = clkrstGetClockRate(&session, &hz);
                if (R_FAILED(access.Rc)) access.FailedStep = "clkrstGetClockRate";
                else access.Hz = hz;
            }
            clkrstCloseSession(&session);
        }
        clkrstExit();
        return access;
    }
    {
        HangWatch::ServiceCallScope call("pcv:Initialize");
        access.Rc = pcvInitialize();
    }
    if (R_FAILED(access.Rc)) {
        access.FailedStep = "pcvInitialize";
        return access;
    }
    if (setHz != 0U) {
        HangWatch::ServiceCallScope call("pcv:SetClockRate");
        access.Rc = pcvSetClockRate(PcvModuleOf(module), setHz);
        if (R_FAILED(access.Rc)) access.FailedStep = "pcvSetClockRate";
    }
    if (R_SUCCEEDED(access.Rc)) {
        HangWatch::ServiceCallScope call("pcv:GetClockRate");
        u32 hz = 0U;
        access.Rc = pcvGetClockRate(PcvModuleOf(module), &hz);
        if (R_FAILED(access.Rc)) access.FailedStep = "pcvGetClockRate";
        else access.Hz = hz;
    }
    pcvExit();
    return access;
}

unsigned Mhz(uint32_t hz) {
    return static_cast<unsigned>((hz + 500000U) / 1000000U);
}

const char* OperationModeName() {
    return appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld";
}

// ---- GPU and memory: one official handheld configuration through apm --------

struct ApmState {
    bool SessionOpen = false;
    uint32_t Standard = 0U;  // HandheldStandardConfiguration of what was reported first
    bool Gpu = false;        // the last choice applied
    bool Memory = false;
    PerformanceStatus Last;  // its apm fields
};

// True when an apm request was made (HOS then applies that configuration's
// clocks, CPU 1020 MHz included). The session stays open for the run, so the
// request lasts as long as the game whether or not HOS keeps it after a
// session closes. What HOS applies is read back later on the follow-up thread
// (ApmReadback, TRIAEVUM_PERF_APM_EFFECTIVE): read here at once it was still
// the previous request's (test.15 logs23).
bool ApplyApm(ApmState& apm, bool gpu, bool memory, PerformanceStatus& status) {
    const bool unchanged = gpu == apm.Gpu && memory == apm.Memory;
    if (unchanged || (!gpu && !memory && !apm.SessionOpen)) {
        // Unchanged, or Standard and never changed: nothing to put back.
        status.ApmAttempted = apm.Last.ApmAttempted;
        status.ApmAvailable = apm.Last.ApmAvailable;
        status.ApmResult = apm.Last.ApmResult;
        status.ApmFailedStep = apm.Last.ApmFailedStep;
        status.ApmRequested = apm.Last.ApmRequested;
        status.ApmEffective = apm.Last.ApmEffective;
        return false;
    }
    apm.Gpu = gpu;
    apm.Memory = memory;
    status.ApmAttempted = true;
    if (!apm.SessionOpen) {
        Result result;
        {
            HangWatch::ServiceCallScope call("apm:Initialize");
            result = apmInitialize();
        }
        if (R_FAILED(result)) {
            status.ApmResult = result;
            status.ApmFailedStep = "apmInitialize";
        } else {
            apm.SessionOpen = true;
            u32 before = 0U;
            {
                HangWatch::ServiceCallScope call("apm:GetPerformanceConfiguration");
                if (R_FAILED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &before))) {
                    before = 0U;
                }
            }
            apm.Standard = HandheldStandardConfiguration(before);
        }
    }
    if (apm.SessionOpen) {
        status.ApmRequested = HandheldConfiguration(gpu, memory, apm.Standard);
        Result result;
        {
            HangWatch::ServiceCallScope call("apm:SetPerformanceConfiguration");
            result = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, status.ApmRequested);
        }
        if (R_FAILED(result)) {
            status.ApmResult = result;
            status.ApmFailedStep = "apmSetPerformanceConfiguration";
        } else {
            status.ApmAvailable = true;
        }
    }
    Log("TRIAEVUM_PERF_APM gpu_high=%d mem_high=%d available=%d rc=0x%X failed=%s requested=0x%08X "
        "standard=0x%08X op_mode=%s",
        gpu ? 1 : 0, memory ? 1 : 0, status.ApmAvailable ? 1 : 0, status.ApmResult,
        status.ApmFailedStep[0] != '\0' ? status.ApmFailedStep : "-", status.ApmRequested,
        apm.Standard, OperationModeName());
    apm.Last = status;
    return true;
}

// ---- CPU: the keeper, its follow-up thread and the applet hook --------------

constexpr size_t kFollowUps = std::size(Keeper::kFollowUpMs);
// The hook's SetFocusHandlingMode(SuspendHomeSleep) returning after this
// long means AM suspended the game inside it and it now runs again in focus;
// sooner, wait kRearmLateMs (a suspension that comes late happens first).
constexpr uint64_t kSuspendedMs = 200U;
constexpr uint64_t kRearmLateMs = 1000U;

struct CpuState {
    std::mutex Mutex;  // everything below and every clock access
    std::condition_variable Wake;
    Keeper Cpu;
    PerformanceChoice Requested;
    ApmState Apm;
    std::atomic<bool> Started{false};  // hook, thread and atexit installed (CPU High)
    bool ThreadStarted = false;        // the follow-up thread (CPU High or an apm readback)
    ApmReadback Readback;              // the last apm request, until HOS reports it
    AppletHookCookie Hook{};
    bool NoSuspend = false;  // focus handling switched to NoSuspend
    bool FocusModeRefused = false;
    uint64_t FollowUpStartMs = 0U;
    size_t FollowUpNext = kFollowUps;  // kFollowUps = none due
    const char* FollowUpWhy = "";
    uint64_t RearmAtMs = 0U;  // 0 = none
    bool FocusInPending = false;
    const char* HosEventPending = nullptr;
    Result LastResult = 0;
    const char* LastFailedStep = "";
    std::thread Thread;  // FollowUpLoop
    // A copy for the menu and perf.txt, which never wait for Mutex (held
    // during clock IPC): PublishedMutex is only held to copy it.
    std::mutex PublishedMutex;
    PerformanceStatus Published;
};

CpuState& State() {
    // Never destroyed: the thread and the atexit handler use it until the end.
    static CpuState* state = new CpuState();
    return *state;
}

void FillCpuStatus(const CpuState& state, PerformanceStatus& status) {
    status.CpuUnavailable = state.Cpu.Unavailable();
    status.CpuOverridden = state.Cpu.Overridden();
    status.CpuResult = state.LastResult;
    status.CpuFailedStep = state.LastFailedStep;
    status.CpuForeignHz = state.Cpu.ForeignHz();
    status.CpuReapplies = state.Cpu.Reapplies();
}

// Mutex held: refreshes the copy CurrentPerformanceStatus returns.
void Publish(CpuState& state) {
    PerformanceStatus status = state.Apm.Last;
    status.Requested = state.Requested;
    FillCpuStatus(state, status);
    const std::lock_guard<std::mutex> lock(state.PublishedMutex);
    state.Published = status;
}

const char* ActionName(Keeper::Action action) {
    switch (action) {
    case Keeper::Action::SetHigh: return "set_high";
    case Keeper::Action::Restore: return "restore";
    case Keeper::Action::GiveUp: return "override";
    case Keeper::Action::None: break;
    }
    return "none";
}

// Mutex held. NoSuspend while CPU High is wanted and working, the default
// SuspendHomeSleep otherwise (applications only: an applet keeps its mode).
void SetFocusMode(CpuState& state, bool noSuspend) {
    if (state.NoSuspend == noSuspend || state.FocusModeRefused) return;
    if (appletGetAppletType() != AppletType_Application) {
        state.FocusModeRefused = true;
        Log("TRIAEVUM_PERF_CPU event=focus_mode result=skipped applet_type=%d "
            "(not an application: HOME and sleep keep the CPU rate until the game runs again)",
            static_cast<int>(appletGetAppletType()));
        return;
    }
    Result rc;
    {
        HangWatch::ServiceCallScope call("am:SetFocusHandlingMode");
        rc = appletSetFocusHandlingMode(noSuspend ? AppletFocusHandlingMode_NoSuspend
                                                  : AppletFocusHandlingMode_SuspendHomeSleep);
    }
    if (R_SUCCEEDED(rc)) state.NoSuspend = noSuspend;
    Log("TRIAEVUM_PERF_CPU event=focus_mode mode=%s rc=0x%X",
        noSuspend ? "no_suspend" : "suspend_home_sleep", rc);
}

// Mutex held. Carries out the keeper's step and logs it.
void Carry(CpuState& state, Keeper::Step step, const char* why, uint32_t hzBefore) {
    if (step.Do == Keeper::Action::None) return;
    if (step.Do == Keeper::Action::GiveUp) {
        Log("TRIAEVUM_PERF_CPU event=%s action=override foreign_mhz=%u reapply=%u "
            "(another tool keeps changing the CPU clock: left to it)",
            why, Mhz(step.Hz), static_cast<unsigned>(state.Cpu.Reapplies()));
        SetFocusMode(state, false);
        Publish(state);
        return;
    }
    const ClockAccess access = AccessClock(ClockModule::Cpu, step.Hz);
    const bool ok = R_SUCCEEDED(access.Rc);
    state.Cpu.Report(ok);
    if (!ok) {
        state.LastResult = access.Rc;
        state.LastFailedStep = access.FailedStep;
        if (state.Cpu.Unavailable()) SetFocusMode(state, false);
    }
    Log("TRIAEVUM_PERF_CPU event=%s action=%s mhz_before=%u requested_mhz=%u mhz_after=%u rc=0x%X "
        "failed=%s reapply=%u focus_mode=%s op_mode=%s",
        why, ActionName(step.Do), Mhz(hzBefore), Mhz(step.Hz), Mhz(access.Hz), access.Rc,
        ok ? "-" : access.FailedStep, static_cast<unsigned>(state.Cpu.Reapplies()),
        state.NoSuspend ? "no_suspend" : "default", OperationModeName());
    Publish(state);
}

// Mutex held. Reads the CPU rate (0 if that failed, recorded).
uint32_t ReadCpu(CpuState& state) {
    const ClockAccess access = AccessClock(ClockModule::Cpu, 0U);
    if (R_FAILED(access.Rc)) {
        state.LastResult = access.Rc;
        state.LastFailedStep = access.FailedStep;
        Publish(state);
        return 0U;
    }
    return access.Hz;
}

// CPU High wanted and still the game's to keep.
bool Keeping(const CpuState& state) {
    return state.Cpu.Wanted() && !state.Cpu.Overridden() && !state.Cpu.Unavailable();
}

void StartFollowUps(CpuState& state, uint64_t now, const char* why) {
    if (!Keeping(state)) return;
    state.FollowUpStartMs = now;
    state.FollowUpNext = 0U;
    state.FollowUpWhy = why;
}

// Mutex held: in focus again (the hook's InFocus, or the re-arm after a
// suspension).
void FocusIn(CpuState& state, const char* why) {
    if (!state.Cpu.Wanted()) return;
    const uint64_t now = NowMs();
    const uint32_t hz = Keeping(state) ? ReadCpu(state) : 0U;
    Carry(state, state.Cpu.Focus(true, hz, now), why, hz);
    if (Keeping(state)) SetFocusMode(state, true);
    StartFollowUps(state, now, why);
}

// Mutex held: one read of the configuration in effect for the pending apm
// request; logs once it equals the request or kSettleMs passed.
void PollApmReadback(CpuState& state, uint64_t now) {
    u32 effective = 0U;
    {
        HangWatch::ServiceCallScope call("am:GetCurrentPerformanceConfiguration");
        if (R_FAILED(appletGetCurrentPerformanceConfiguration(&effective))) effective = 0U;
    }
    const ApmReadback::Outcome outcome = state.Readback.Poll(effective, now);
    if (outcome == ApmReadback::Outcome::Pending) return;
    const bool matched = outcome == ApmReadback::Outcome::Matched;
    const uint32_t reported = state.Readback.LastEffective();
    const SwitchClockReading clocks = StockClockReading(reported);
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    Log("TRIAEVUM_PERF_APM_EFFECTIVE requested=0x%08X effective=0x%08X match=%d after_ms=%llu gpu_mhz=%d "
        "mem_mhz=%d op_mode=%s%s",
        state.Readback.Requested(), reported, matched ? 1 : 0,
        static_cast<unsigned long long>(state.Readback.ElapsedMs(now)), clocks.GpuMhz, clocks.MemMhz,
        docked ? "docked" : "handheld",
        matched ? ""
        : docked ? " (docked: HOS runs the docked configuration; the request applies in handheld)"
                 : " (HOS still reports another configuration after 1 s)");
    if (state.Apm.Last.ApmRequested == state.Readback.Requested()) state.Apm.Last.ApmEffective = reported;
    Publish(state);
}

void FollowUpLoop(CpuState& state) {
    std::unique_lock<std::mutex> lock(state.Mutex);
    for (;;) {
        uint64_t due = UINT64_MAX;
        if (state.FollowUpNext < kFollowUps)
            due = state.FollowUpStartMs + Keeper::kFollowUpMs[state.FollowUpNext];
        if (state.RearmAtMs != 0U && state.RearmAtMs < due) due = state.RearmAtMs;
        if (state.Readback.Active() && state.Readback.NextPollMs() < due) due = state.Readback.NextPollMs();
        if (!state.FocusInPending && state.HosEventPending == nullptr) {
            const uint64_t now = NowMs();
            if (due == UINT64_MAX) {
                state.Wake.wait(lock);
            } else if (due > now) {
                state.Wake.wait_for(lock, std::chrono::milliseconds(due - now));
            }
        }
        const uint64_t now = NowMs();
        if (state.RearmAtMs != 0U && now >= state.RearmAtMs) {
            state.RearmAtMs = 0U;
            state.FocusInPending = false;
            FocusIn(state, "resume");
        }
        if (state.FocusInPending) {
            state.FocusInPending = false;
            FocusIn(state, "focus");
        }
        if (state.HosEventPending != nullptr) {
            const char* why = state.HosEventPending;
            state.HosEventPending = nullptr;
            state.Cpu.HosEvent(now);
            StartFollowUps(state, now, why);
        }
        if (state.FollowUpNext < kFollowUps &&
            now >= state.FollowUpStartMs + Keeper::kFollowUpMs[state.FollowUpNext]) {
            ++state.FollowUpNext;
            if (Keeping(state) && state.Cpu.InFocus()) {
                const uint32_t hz = ReadCpu(state);
                Carry(state, state.Cpu.Check(hz, NowMs()), state.FollowUpWhy, hz);
            }
        }
        if (state.Readback.Active() && now >= state.Readback.NextPollMs()) PollApmReadback(state, now);
    }
}

// Game thread, inside appletMainLoop (SDL's event pump). Only reached for
// HOME and sleep while NoSuspend is set.
void EnterBackground(CpuState& state) {
    {
        const std::lock_guard<std::mutex> lock(state.Mutex);
        if (!state.NoSuspend) return;
        Carry(state, state.Cpu.Focus(false, 0U, NowMs()), "background", 0U);
        state.FollowUpNext = kFollowUps;
        state.RearmAtMs = 0U;
        state.FocusInPending = false;
    }
    // AM suspends the game inside this call (in the background with
    // SuspendHomeSleep) and resumes it when it is in focus again.
    const uint64_t start = NowMs();
    Result rc;
    {
        HangWatch::ServiceCallScope call("am:SetFocusHandlingMode(suspend)");
        rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleep);
    }
    const uint64_t now = NowMs();
    const std::lock_guard<std::mutex> lock(state.Mutex);
    if (R_SUCCEEDED(rc)) state.NoSuspend = false;
    Log("TRIAEVUM_PERF_CPU event=background focus_mode=suspend_home_sleep rc=0x%X waited_ms=%llu",
        rc, static_cast<unsigned long long>(now - start));
    // Running again after a long call: AM suspended the game in it and it is
    // in focus now. A quick return: AM suspends later, re-arm after that.
    state.RearmAtMs = now - start >= kSuspendedMs ? now : now + kRearmLateMs;
    state.Wake.notify_one();
}

void RestoreForExit(const char* why) {
    CpuState& state = State();
    const std::lock_guard<std::mutex> lock(state.Mutex);
    Carry(state, state.Cpu.Exit(), why, 0U);
}

// The safety net (another exit path, or a second call: then nothing to do).
// The game log may be gone by now: RestorePerformanceForExit logs it.
void RestoreAtExit() {
    RestoreForExit("atexit");
}

void OnAppletHook(AppletHookType hook, void* param) {
    CpuState& state = *static_cast<CpuState*>(param);
    const char* event = nullptr;
    switch (hook) {
    case AppletHookType_OnFocusState:
        switch (appletGetFocusState()) {
        case AppletFocusState_Background:
            EnterBackground(state);
            return;
        case AppletFocusState_InFocus: {
            const std::lock_guard<std::mutex> lock(state.Mutex);
            state.FocusInPending = true;
            state.Wake.notify_one();
            return;
        }
        default:
            return;  // OutOfFocus: a library applet over the game, which keeps running
        }
    case AppletHookType_OnExitRequest:
        RestoreForExit("exit_request");
        return;
    case AppletHookType_OnResume: event = "wake"; break;
    case AppletHookType_OnOperationMode: event = "dock_change"; break;
    case AppletHookType_OnPerformanceMode: event = "perf_mode_change"; break;
    default: return;
    }
    const std::lock_guard<std::mutex> lock(state.Mutex);
    state.HosEventPending = event;
    state.Wake.notify_one();
}

// Game thread (the Video panel), apply mutex held: the follow-up thread, for
// CPU High or an apm readback. A pthread: oot3d_switch_threads.cpp makes it a
// service thread (0x3B, cores 1-2). Never joined (no detach(): it throws on
// Switch, p6x).
void EnsureFollowUpThread(CpuState& state) {
    if (state.ThreadStarted) return;
    state.ThreadStarted = true;
    state.Thread = std::thread([&state] { FollowUpLoop(state); });
}

// Game thread (the Video panel), apply mutex held.
void StartCpuKeeper(CpuState& state) {
    if (state.Started.load(std::memory_order_relaxed)) return;
    appletHook(&state.Hook, OnAppletHook, &state);
    EnsureFollowUpThread(state);
    std::atexit(RestoreAtExit);
    state.Started.store(true, std::memory_order_release);
}

} // namespace

PerformanceStatus ApplyPerformance(PerformanceChoice choice) {
    static std::mutex applyMutex;
    const std::lock_guard<std::mutex> applyLock(applyMutex);
    CpuState& state = State();
    if (choice.Cpu) StartCpuKeeper(state);
    const std::lock_guard<std::mutex> lock(state.Mutex);
    PerformanceStatus status;
    status.Requested = choice;
    state.Requested = choice;
    const bool apmChanged = ApplyApm(state.Apm, choice.Gpu, choice.Memory, status);
    if (apmChanged && status.ApmAvailable) {
        // Read back on the follow-up thread what HOS applies (never here). A
        // request still unsettled is logged as superseded with HOS's last report.
        if (state.Readback.Active())
            Log("TRIAEVUM_PERF_APM_EFFECTIVE requested=0x%08X effective=0x%08X match=0 superseded=1",
                state.Readback.Requested(), state.Readback.LastEffective());
        state.Readback.Start(status.ApmRequested, NowMs());
        EnsureFollowUpThread(state);
        state.Wake.notify_one();
    }
    if (state.Started.load(std::memory_order_relaxed)) {
        const uint64_t now = NowMs();
        if (choice.Cpu != state.Cpu.Wanted()) {
            if (choice.Cpu) {
                const ClockAccess read = AccessClock(ClockModule::Cpu, 0U);
                if (R_FAILED(read.Rc)) {
                    state.LastResult = read.Rc;
                    state.LastFailedStep = read.FailedStep;
                    state.Cpu.Choose(true, 0U, now);
                    state.Cpu.MarkUnavailable();
                    Log("TRIAEVUM_PERF_CPU event=choose_high action=none rc=0x%X failed=%s "
                        "(CPU High unavailable this run)",
                        read.Rc, read.FailedStep);
                } else {
                    Carry(state, state.Cpu.Choose(true, read.Hz, now), "choose_high", read.Hz);
                    Log("TRIAEVUM_PERF_CPU event=choose_high prior_mhz=%u",
                        Mhz(state.Cpu.PriorHz()));
                    if (!state.Cpu.Unavailable()) SetFocusMode(state, true);
                }
            } else {
                Carry(state, state.Cpu.Choose(false, 0U, now), "choose_standard", 0U);
                SetFocusMode(state, false);
                state.FollowUpNext = kFollowUps;
                state.RearmAtMs = 0U;
            }
        }
        if (state.Cpu.Wanted()) {
            // Our apm change, like HOS's, makes HOS set CPU 1020 again (with
            // a delay): read back at the follow-up times.
            if (apmChanged) state.Cpu.HosEvent(now);
            StartFollowUps(state, now, apmChanged ? "apm_change" : "choose_high");
            state.Wake.notify_one();
        }
    }
    FillCpuStatus(state, status);
    Publish(state);
    return status;
}

PerformanceStatus CurrentPerformanceStatus() {
    CpuState& state = State();
    const std::lock_guard<std::mutex> lock(state.PublishedMutex);
    return state.Published;
}

void RestorePerformanceForExit() {
    if (State().Started.load(std::memory_order_acquire)) RestoreForExit("exit");
}

PerformanceClockSample SamplePerformanceClocks(bool perfWindow) {
    PerformanceClockSample sample;
    CpuState& state = State();
    if (!state.Started.load(std::memory_order_acquire)) return sample;
    const std::lock_guard<std::mutex> lock(state.Mutex);
    if (!state.Cpu.Wanted() || state.Cpu.Unavailable()) return sample;
    const ClockAccess cpu = AccessClock(ClockModule::Cpu, 0U);
    const ClockAccess gpu = AccessClock(ClockModule::Gpu, 0U);
    const ClockAccess memory = AccessClock(ClockModule::Memory, 0U);
    sample.Read = R_SUCCEEDED(cpu.Rc) && R_SUCCEEDED(gpu.Rc) && R_SUCCEEDED(memory.Rc);
    sample.CpuHz = cpu.Hz;
    sample.GpuHz = gpu.Hz;
    sample.MemHz = memory.Hz;
    if (R_FAILED(cpu.Rc)) {
        state.LastResult = cpu.Rc;
        state.LastFailedStep = cpu.FailedStep;
    } else if (perfWindow) {
        // The cheap check: once per perf.txt window.
        Carry(state, state.Cpu.Check(cpu.Hz, NowMs()), "perf_window", cpu.Hz);
    }
    Publish(state);
    return sample;
}

} // namespace Oot3dNativeGame
#endif
