// Clocks atuais para logs/perf.txt: ver oot3d_switch_clocks.h.
//
// Por que o IPC do sys-clk e não clkrst/pcv: o sys-clk já mantém os clocks
// (alvo e medidos) num contexto que o overlay dele lê pelo mesmo comando
// GetCurrentContext; ler dali não abre sessão de clkrst/pcv no app (a
// pesquisa de 2026-10-17 manda não usar clkrst no app) e não compete com o
// sys-clk. Sem sys-clk os MHz ficam -1 e só o modo é registrado.
#if defined(__SWITCH__)
#include <switch.h>

#include <cstdint>
#include <sstream>
#include <string>

#include "oot3d_switch_clocks.h"
#include "oot3d_hang_watchdog.h"

namespace Oot3dNativeGame {
namespace {

// Layout do SysClkContext (retronx-team/sys-clk,
// common/include/sysclk/clock_manager.h). O prefixo (enabled, applicationId,
// profile, freqs) é o mesmo da API 1 à 4; realFreqs existe desde a API 2.
// Campos novos só foram acrescentados no fim, então a estrutura da API 4
// cobre as anteriores (o que sobra fica zerado e não é lido).
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

// smGetService de um serviço que não existe fica esperando o registro (o sm
// adia o pedido): pergunta antes ao sm do Atmosphère (comando de extensão
// 65100, só leitura). Sem Atmosphère o comando falha e o sys-clk é dado como
// ausente.
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

    // Abre a sessão uma vez; se o sys-clk não estiver rodando (ou cair),
    // tenta de novo só a cada 60 janelas (~5 min a 30 fps).
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
            // Watchdog de travamento: a última chamada de serviço vai no hang.txt.
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
            // API 0 tinha outro contexto: não arrisca ler.
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

} // namespace

std::string SwitchClockPerfSummary() {
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
            // realFreqs (medido pelo sys-clk) quando houver; senão o alvo.
            const bool real = gSysClk.ApiVersion >= 2U && context.RealFreqs[0] != 0U;
            const uint32_t* freqs = real ? context.RealFreqs : context.Freqs;
            cpu = Mhz(freqs[0]);
            gpu = Mhz(freqs[1]);
            mem = Mhz(freqs[2]);
            source = real ? "sysclk_real" : "sysclk";
            profile = ProfileName(context.Profile);
            enabled = context.Enabled != 0 ? 1 : 0;
        } else {
            gSysClk.Close(); // sys-clk saiu: reabre mais tarde
        }
    }
    // Valores guardados pela libnx a partir das mensagens do applet: sem IPC.
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

} // namespace Oot3dNativeGame
#endif
