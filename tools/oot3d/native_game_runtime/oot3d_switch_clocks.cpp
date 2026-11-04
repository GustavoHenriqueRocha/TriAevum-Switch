// Current clocks for logs/perf.txt: see oot3d_switch_clocks.h.
//
// Why sys-clk's IPC and not clkrst/pcv: sys-clk already keeps the clocks
// (target and measured) in a context its overlay reads through the same
// GetCurrentContext command; reading from there opens no clkrst/pcv session in the app (the
// 2026-10-17 research says not to use clkrst in the app) and doesn't compete with
// sys-clk. Without sys-clk the MHz stay -1 and only the mode is recorded.
#if defined(__SWITCH__)
#include <switch.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "oot3d_switch_clocks.h"
#include "oot3d_hang_watchdog.h"

namespace Oot3dNativeGame {
namespace {

// SysClkContext layout (retronx-team/sys-clk,
// common/include/sysclk/clock_manager.h). The prefix (enabled, applicationId,
// profile, freqs) is the same from API 1 to 4; realFreqs exists since API 2.
// New fields were only appended at the end, so the API 4 struct
// covers the earlier ones (the rest stays zeroed and is not read).
struct SysClkContextV4 {
    uint8_t Enabled;
    uint64_t ApplicationId;
    uint32_t Profile;
    uint32_t Freqs[3];
    uint32_t RealFreqs[3];
    uint32_t OverrideFreqs[3];
    uint32_t Temps[3];
    int32_t Power[2];
    uint32_t RamLoad[2];
};

constexpr uint32_t kSysClkCmdGetApiVersion = 0;
constexpr uint32_t kSysClkCmdGetCurrentContext = 2;
constexpr uint32_t kAtmosphereHasService = 65100;

const char* ProfileName(uint32_t profile) {
    static constexpr const char* kNames[] = {"handheld", "handheld_charging",
                                             "handheld_charging_usb",
                                             "handheld_charging_official", "docked"};
    return profile < 5U ? kNames[profile] : "-1";
}

// smGetService of a service that doesn't exist keeps waiting for registration (sm
// defers the request): ask Atmosphère's sm first (extension command
// 65100, read-only). Without Atmosphère the command fails and sys-clk is taken as
// absent.
bool SysClkRegistered() {
    const SmServiceName name = smEncodeName("sys:clk");
    u8 has = 0;
    Result result;
    if (hosversionAtLeast(12, 0, 0)) {
        result = tipcDispatchInOut(smGetServiceSessionTipc(), kAtmosphereHasService, name, has);
    } else {
        result = serviceDispatchInOut(smGetServiceSession(), kAtmosphereHasService, name, has);
    }
    return R_SUCCEEDED(result) && has != 0;
}

struct SysClkClient {
    Service Session{};
    bool Open = false;
    bool Probed = false;
    uint32_t ApiVersion = 0;
    uint32_t WindowsUntilRetry = 0;

    void Close() {
        if (Open) {
            serviceClose(&Session);
        }
        Open = false;
    }

    // Opens the session once; if sys-clk isn't running (or goes down),
    // retries only every 60 windows (~5 min at 30 fps).
    bool Ensure() {
        if (Open) {
            return true;
        }
        if (Probed && WindowsUntilRetry != 0U) {
            --WindowsUntilRetry;
            return false;
        }
        Probed = true;
        WindowsUntilRetry = 60U;
        {
            // Hang watchdog: the last service call goes into hang.txt.
            HangWatch::ServiceCallScope call("sm:AtmosphereHasService(sys:clk)");
            if (!SysClkRegistered()) {
                return false;
            }
        }
        {
            HangWatch::ServiceCallScope call("sm:GetService(sys:clk)");
            if (R_FAILED(smGetService(&Session, "sys:clk"))) {
                return false;
            }
        }
        Open = true;
        u32 version = 0;
        Result versionResult;
        {
            HangWatch::ServiceCallScope call("sys:clk GetApiVersion");
            versionResult = serviceDispatchOut(&Session, kSysClkCmdGetApiVersion, version);
        }
        if (R_FAILED(versionResult) || version == 0U) {
            // API 0 had a different context: don't risk reading it.
            Close();
            return false;
        }
        ApiVersion = version;
        return true;
    }
};

SysClkClient gSysClk;

int Mhz(uint32_t hz) {
    return hz == 0U ? -1 : static_cast<int>((hz + 500000U) / 1000000U);
}

std::string ReadClocks() {
    int cpu = -1;
    int gpu = -1;
    int mem = -1;
    const char* source = "none";
    const char* profile = "-1";
    int enabled = -1;
    double ipcMicroseconds = -1.0;
    if (gSysClk.Ensure()) {
        SysClkContextV4 context{};
        const uint64_t start = armGetSystemTick();
        Result result;
        {
            HangWatch::ServiceCallScope call("sys:clk GetCurrentContext");
            result = serviceDispatchOut(&gSysClk.Session, kSysClkCmdGetCurrentContext, context);
        }
        ipcMicroseconds = static_cast<double>(armTicksToNs(armGetSystemTick() - start)) / 1000.0;
        if (R_SUCCEEDED(result)) {
            // realFreqs (measured by sys-clk) when present; otherwise the target.
            const bool real = gSysClk.ApiVersion >= 2U && context.RealFreqs[0] != 0U;
            const uint32_t* freqs = real ? context.RealFreqs : context.Freqs;
            cpu = Mhz(freqs[0]);
            gpu = Mhz(freqs[1]);
            mem = Mhz(freqs[2]);
            source = real ? "sysclk_real" : "sysclk";
            profile = ProfileName(context.Profile);
            enabled = context.Enabled != 0 ? 1 : 0;
        } else {
            gSysClk.Close(); // sys-clk exited: reopen later
        }
    }
    // Values kept by libnx from the applet messages: no IPC.
    const char* operation = "-1";
    switch (appletGetOperationMode()) {
    case AppletOperationMode_Handheld: operation = "handheld"; break;
    case AppletOperationMode_Console: operation = "docked"; break;
    default: break;
    }
    const char* performance = "-1";
    switch (appletGetPerformanceMode()) {
    case ApmPerformanceMode_Normal: performance = "normal"; break;
    case ApmPerformanceMode_Boost: performance = "boost"; break;
    default: break;
    }
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(0);
    text << "clocks: cpu_mhz=" << cpu << " gpu_mhz=" << gpu << " mem_mhz=" << mem
         << " clk_src=" << source << " op_mode=" << operation << " perf_mode=" << performance
         << " sysclk_profile=" << profile << " sysclk_enabled=" << enabled
         << " sysclk_api=" << (gSysClk.Open ? static_cast<int>(gSysClk.ApiVersion) : -1)
         << " clk_ipc_us=" << ipcMicroseconds;
    return text.str();
}

// sys-clk IPC on its own thread: p6w (26/10) sat 3.4 s in
// GetCurrentContext on the game thread (froze 4.4 s). The game only reads the last
// result and requests the next one; it never waits on sys-clk.
struct ClockReader {
    std::mutex Mutex;
    std::condition_variable Wake;
    std::string Last = "clocks: clk_src=none";
    bool Requested = false;
    bool Busy = false;
    std::thread Thread;

    void Loop() {
        std::unique_lock<std::mutex> lock(Mutex);
        for (;;) {
            Wake.wait(lock, [this] { return Requested; });
            Requested = false;
            Busy = true;
            lock.unlock();
            std::string text = ReadClocks();
            lock.lock();
            Last = std::move(text);
            Busy = false;
        }
    }
};

} // namespace

std::string SwitchClockPerfSummary() {
    static ClockReader* reader = [] {
        // Never destroyed (the thread lives until the process ends). No detach():
        // pthread_detach doesn't exist in devkitA64's newlib and the std::system_error
        // ("Function not implemented") crashed p6x on the first perf.
        auto* created = new ClockReader();
        created->Thread = std::thread([created] { created->Loop(); });
        return created;
    }();
    std::string text;
    bool busy = false;
    {
        const std::lock_guard<std::mutex> lock(reader->Mutex);
        text = reader->Last;
        busy = reader->Busy;
        reader->Requested = true;
    }
    reader->Wake.notify_one();
    // clk_busy=1: the previous read hasn't returned yet (sys-clk is slow).
    return text + (busy ? " clk_busy=1" : " clk_busy=0");
}

} // namespace Oot3dNativeGame
#endif
