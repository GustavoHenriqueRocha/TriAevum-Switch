// Current clocks for logs/perf.txt and the overlay: see oot3d_switch_clocks.h.
//
// Why sys-clk's IPC and not clkrst/pcv: sys-clk already keeps the clocks
// (target and measured) in a context its overlay reads through the same
// GetCurrentContext command; reading from there opens no clkrst/pcv session in the app (the
// 2026-10-17 research says not to use clkrst in the app) and doesn't compete with
// sys-clk. Without sys-clk the MHz are the stock clocks of the performance
// configuration AM reports. The only write is the opt-in "GPU clock: High"
// (oot3d_switch_gpu_performance.h): an official configuration through the
// application's own apm session, as games do.
#if defined(__SWITCH__)
#include <switch.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "oot3d_switch_clocks.h"
#include "oot3d_switch_gpu_performance.h"
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
    uint64_t RetryTick = 0;

    void Close() {
        if (Open) {
            serviceClose(&Session);
        }
        Open = false;
    }

    // Opens the session once; if sys-clk isn't running (or goes down),
    // retries only once a minute (the overlay requests a read every half
    // second, so the retry is timed, not counted).
    bool Ensure() {
        if (Open) {
            return true;
        }
        const uint64_t now = armGetSystemTick();
        if (Probed && now < RetryTick) {
            return false;
        }
        Probed = true;
        RetryTick = now + armNsToTicks(60ULL * 1000000000ULL);
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

// HOS's active performance configuration (AM ICommonStateGetter 91, 7.0.0+;
// the same value apm:sys GetCurrentPerformanceConfiguration returns). Only
// read when sys-clk is absent: sys-clk sets the clocks without changing it.
SwitchClockReading ReadStockClocks() {
    u32 configuration = 0;
    Result result;
    {
        HangWatch::ServiceCallScope call("am:GetCurrentPerformanceConfiguration");
        result = appletGetCurrentPerformanceConfiguration(&configuration);
    }
    if (R_FAILED(result) || configuration == 0U) {
        return {};
    }
    return StockClockReading(configuration);
}

struct ClockSample {
    SwitchClockReading Reading;
    std::string Text = "clocks: clk_src=none";
};

ClockSample ReadClocks() {
    ClockSample sample;
    SwitchClockReading& reading = sample.Reading;
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
            reading.CpuMhz = Mhz(freqs[0]);
            reading.GpuMhz = Mhz(freqs[1]);
            reading.MemMhz = Mhz(freqs[2]);
            reading.Source = real ? SwitchClockSource::SysClkReal : SwitchClockSource::SysClkTarget;
            profile = ProfileName(context.Profile);
            enabled = context.Enabled != 0 ? 1 : 0;
        } else {
            gSysClk.Close(); // sys-clk exited: reopen later
        }
    }
    if (reading.Source == SwitchClockSource::None) {
        reading = ReadStockClocks();
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
    text << "clocks: cpu_mhz=" << reading.CpuMhz << " gpu_mhz=" << reading.GpuMhz
         << " mem_mhz=" << reading.MemMhz << " clk_src=" << SwitchClockSourceName(reading.Source);
    if (reading.Source == SwitchClockSource::Stock) {
        char configuration[24];
        std::snprintf(configuration, sizeof(configuration), " perf_cfg=0x%08X",
                      static_cast<unsigned>(reading.PerformanceConfiguration));
        text << configuration;
    }
    text << " op_mode=" << operation << " perf_mode=" << performance
         << " sysclk_profile=" << profile << " sysclk_enabled=" << enabled
         << " sysclk_api=" << (gSysClk.Open ? static_cast<int>(gSysClk.ApiVersion) : -1)
         << " clk_ipc_us=" << ipcMicroseconds;
    sample.Text = text.str();
    return sample;
}

// sys-clk IPC on its own thread: p6w (26/10) sat 3.4 s in
// GetCurrentContext on the game thread (froze 4.4 s). The game only reads the last
// result and requests the next one; it never waits on sys-clk.
struct ClockReader {
    std::mutex Mutex;
    std::condition_variable Wake;
    ClockSample Last;
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
            ClockSample sample = ReadClocks();
            lock.lock();
            Last = std::move(sample);
            Busy = false;
        }
    }
};

ClockReader& Reader() {
    static ClockReader* reader = [] {
        // Never destroyed (the thread lives until the process ends). No detach():
        // pthread_detach doesn't exist in devkitA64's newlib and the std::system_error
        // ("Function not implemented") crashed p6x on the first perf.
        auto* created = new ClockReader();
        created->Thread = std::thread([created] { created->Loop(); });
        return created;
    }();
    return *reader;
}

} // namespace

SwitchClockReading SwitchClockLatest() {
    ClockReader& reader = Reader();
    SwitchClockReading reading;
    {
        const std::lock_guard<std::mutex> lock(reader.Mutex);
        reading = reader.Last.Reading;
        reader.Requested = true;
    }
    reader.Wake.notify_one();
    return reading;
}

std::string SwitchClockPerfSummary() {
    ClockReader& reader = Reader();
    std::string text;
    bool busy = false;
    {
        const std::lock_guard<std::mutex> lock(reader.Mutex);
        text = reader.Last.Text;
        busy = reader.Busy;
        reader.Requested = true;
    }
    reader.Wake.notify_one();
    // clk_busy=1: the previous read hasn't returned yet (sys-clk is slow).
    return text + (busy ? " clk_busy=1" : " clk_busy=0");
}

// L3+R3 > Video > GPU clock. apm (not clkrst/pcv): the configuration HOS
// applies to this application. The session stays open for the run, so the
// request lasts as long as the game whether or not HOS keeps it after a
// session closes.
GpuPerformanceStatus ApplyGpuPerformance(bool high) {
    static std::mutex mutex;
    static bool sessionOpen = false;
    static uint32_t standard = 0U;
    const std::lock_guard<std::mutex> lock(mutex);
    GpuPerformanceStatus status;
    if (!high && !sessionOpen) {
        return status;  // Standard and never changed: nothing to put back.
    }
    status.Attempted = true;
    if (!sessionOpen) {
        Result result;
        {
            HangWatch::ServiceCallScope call("apm:Initialize");
            result = apmInitialize();
        }
        if (R_FAILED(result)) {
            status.Result = result;
            status.FailedStep = "apmInitialize";
        } else {
            sessionOpen = true;
            u32 before = 0U;
            {
                HangWatch::ServiceCallScope call("apm:GetPerformanceConfiguration");
                if (R_FAILED(apmGetPerformanceConfiguration(ApmPerformanceMode_Normal, &before))) {
                    before = 0U;
                }
            }
            standard = GpuStandardConfiguration(before);
        }
    }
    if (sessionOpen) {
        status.Requested = high ? kGpuHighHandheldConfiguration : standard;
        Result result;
        {
            HangWatch::ServiceCallScope call("apm:SetPerformanceConfiguration");
            result = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, status.Requested);
        }
        if (R_FAILED(result)) {
            status.Result = result;
            status.FailedStep = "apmSetPerformanceConfiguration";
        } else {
            status.Available = true;
        }
        u32 effective = 0U;
        HangWatch::ServiceCallScope call("am:GetCurrentPerformanceConfiguration");
        if (R_SUCCEEDED(appletGetCurrentPerformanceConfiguration(&effective))) {
            status.Effective = effective;
        }
    }
    const SwitchClockReading clocks = StockClockReading(status.Effective);
    std::fprintf(stderr,
                 "TRIAEVUM_GPU_PERF high=%d available=%d rc=0x%X failed=%s requested=0x%08X "
                 "standard=0x%08X effective=0x%08X gpu_mhz=%d mem_mhz=%d op_mode=%s\n",
                 high ? 1 : 0, status.Available ? 1 : 0, status.Result,
                 status.FailedStep[0] != '\0' ? status.FailedStep : "-", status.Requested, standard,
                 status.Effective, clocks.GpuMhz, clocks.MemMhz,
                 appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
    return status;
}

} // namespace Oot3dNativeGame
#endif
