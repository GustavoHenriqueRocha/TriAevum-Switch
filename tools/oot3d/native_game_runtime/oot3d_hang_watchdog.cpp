// Watchdog de travamento: ver oot3d_hang_watchdog.h.
#include "oot3d_hang_watchdog.h"

#include <chrono>

#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <thread>

#include "oot3d_switch_threads.h"
#endif

namespace Oot3dNativeGame::HangWatch {

// ---- relógio -----------------------------------------------------------------

uint64_t NowTicks() noexcept {
#if defined(__SWITCH__)
    return armGetSystemTick();
#else
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
#endif
}

uint64_t TicksToMicroseconds(uint64_t ticks) noexcept {
#if defined(__SWITCH__)
    return armTicksToNs(ticks) / 1000U;
#else
    return ticks / 1000U;
#endif
}

// ---- texto -------------------------------------------------------------------

Text& Text::Char(char value) noexcept {
    if (mSize + 1U < mCapacity) {
        mBuffer[mSize++] = value;
        mBuffer[mSize] = '\0';
    }
    return *this;
}

Text& Text::Str(const char* text) noexcept {
    if (text == nullptr) text = "(null)";
    while (*text != '\0' && mSize + 1U < mCapacity) {
        mBuffer[mSize++] = *text++;
    }
    if (mCapacity != 0U) mBuffer[mSize] = '\0';
    return *this;
}

Text& Text::Dec(uint64_t value) noexcept {
    char digits[24];
    size_t count = 0U;
    do {
        digits[count++] = static_cast<char>('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) Char(digits[--count]);
    return *this;
}

Text& Text::SignedDec(int64_t value) noexcept {
    if (value < 0) {
        Char('-');
        return Dec(static_cast<uint64_t>(-(value + 1)) + 1U);
    }
    return Dec(static_cast<uint64_t>(value));
}

Text& Text::Hex(uint64_t value) noexcept {
    static constexpr char kDigits[] = "0123456789abcdef";
    char digits[16];
    size_t count = 0U;
    do {
        digits[count++] = kDigits[value & 0xFU];
        value >>= 4U;
    } while (value != 0U);
    Str("0x");
    while (count != 0U) Char(digits[--count]);
    return *this;
}

// ---- política ----------------------------------------------------------------

StallTracker::Result StallTracker::Observe(const Observation& observation) noexcept {
    constexpr size_t kThreads = static_cast<size_t>(Thread::Count);
    Result result;
    const uint64_t now = observation.NowMilliseconds;
    const bool resumed = mStarted && now - mLastObservation > mPolicy.SuspendGapMilliseconds;
    if (!mStarted || !observation.Active || resumed || now < mLastObservation) {
        // Começo, fora de foco ou volta de uma suspensão: nada parado conta.
        for (size_t index = 0; index < kThreads; ++index) {
            mLastProgress[index] = observation.Progress[index];
            mLastChange[index] = now;
        }
        mStarted = true;
        mLastObservation = now;
        return result;
    }
    mLastObservation = now;
    uint64_t worst = 0U;
    for (size_t index = 0; index < kThreads; ++index) {
        if (observation.Progress[index] != mLastProgress[index] || !observation.Watched[index]) {
            mLastProgress[index] = observation.Progress[index];
            mLastChange[index] = now;
        }
        if (!observation.Watched[index]) continue;
        const uint64_t stalled = now - mLastChange[index];
        result.StalledFor[index] = stalled;
        if (stalled >= mPolicy.StallMilliseconds) {
            result.StalledMask |= 1U << index;
            if (stalled > worst) worst = stalled;
        }
    }
    if (mReported) {
        // Recuperou quando toda thread que estava parada no relatório andou.
        for (size_t index = 0; index < kThreads; ++index) {
            if ((mReportedMask & (1U << index)) != 0U &&
                observation.Progress[index] == mProgressAtReport[index]) {
                result.StalledMask = 0U;
                return result;
            }
        }
        result.Kind = Verdict::Recovered;
        result.Milliseconds = now - mStallBegan;
        result.StalledMask = 0U;
        mReported = false;
        mReportedMask = 0U;
        return result;
    }
    if (result.StalledMask != 0U) {
        result.Kind = Verdict::Hang;
        result.Milliseconds = worst;
        mReported = true;
        mReportedMask = result.StalledMask;
        mStallBegan = now - worst;
        for (size_t index = 0; index < kThreads; ++index) {
            mProgressAtReport[index] = observation.Progress[index];
        }
    }
    return result;
}

#if !defined(__SWITCH__)

void Start(ExtraReporter, void*) noexcept {}
void Stop() noexcept {}
bool Running() noexcept { return false; }
uint32_t ReportsWritten() noexcept { return 0U; }
void ReportEvent(const char*, const char*) noexcept {}

#else

namespace {

constexpr const char* kDisablePath = "sdmc:/switch/oot3dre/config/no_hang_watchdog";
constexpr const char* kHangPath = "/switch/oot3dre/logs/hang.txt";  // no sdmc
constexpr uint32_t kMaximumReports = 8U;
constexpr size_t kMaximumThreads = 48U;

std::atomic<bool> gStop{false};
std::atomic<bool> gRunning{false};
std::atomic<uint32_t> gReports{0};
std::thread gThread;
ExtraReporter gExtra = nullptr;
void* gExtraUser = nullptr;
FsFileSystem* gSdmc = nullptr;
uint64_t gStartTicks = 0;
uintptr_t gTextBegin = 0;
uintptr_t gTextEnd = 0;

// Um relatório por vez (watchdog ou ReportEvent), sem lock que bloqueie.
std::atomic<bool> gWriting{false};
char gReportBuffer[64 * 1024];

uint64_t UptimeMilliseconds() noexcept {
    return TicksToMicroseconds(NowTicks() - gStartTicks) / 1000U;
}

// ---- memória do próprio processo ----------------------------------------------

struct Region {
    uintptr_t Begin = 0;
    uintptr_t End = 0;
    bool Readable = false;
};

Region QueryRegion(uintptr_t address) noexcept {
    MemoryInfo info{};
    u32 page = 0;
    Region region;
    if (R_SUCCEEDED(svcQueryMemory(&info, &page, address))) {
        region.Begin = info.addr;
        region.End = info.addr + info.size;
        region.Readable = (info.perm & Perm_R) != 0 && info.type != MemType_Unmapped &&
                          (info.attr & MemAttr_IsDeviceMapped) == 0;
    }
    return region;
}

class MemoryReader {
  public:
    bool Read(uintptr_t address, uint64_t& value) noexcept {
        if ((address & 7U) != 0U) return false;
        if (!(address >= mRegion.Begin && address + 8U <= mRegion.End)) {
            mRegion = QueryRegion(address);
        }
        if (!mRegion.Readable || address + 8U > mRegion.End) return false;
        value = *reinterpret_cast<const volatile uint64_t*>(address);
        return true;
    }

  private:
    Region mRegion;
};

bool InText(uint64_t address) noexcept {
    return address >= gTextBegin && address < gTextEnd;
}

void AppendCode(Text& text, uint64_t address) {
    text.Hex(address);
    if (InText(address)) text.Str("(+").Hex(address - gTextBegin).Char(')');
}

// ---- estado de uma thread ------------------------------------------------------

void AppendThreadContext(Text& text, Handle handle) {
    const Handle self = threadGetCurHandle();
    if (handle == self) {
        text.Str("  ctx: (thread que grava o relatório)\n");
        return;
    }
    if (!envIsSyscallHinted(0x32) || !envIsSyscallHinted(0x33)) {
        text.Str("  ctx: svcSetThreadActivity/svcGetThreadContext3 não permitidos pelo NPDM\n");
        return;
    }
    Result result = svcSetThreadActivity(handle, ThreadActivity_Paused);
    if (R_FAILED(result)) {
        text.Str("  ctx: pausa falhou rc=").Hex(result).Char('\n');
        return;
    }
    ThreadContext context{};
    result = svcGetThreadContext3(&context, handle);
    // Lido com a thread parada; ela volta a rodar logo depois.
    uint64_t chain[16] = {};
    size_t chainCount = 0U;
    uint64_t scan[16] = {};
    size_t scanCount = 0U;
    if (R_SUCCEEDED(result)) {
        MemoryReader reader;
        uint64_t frame = context.fp;
        for (size_t depth = 0; depth < 16U; ++depth) {
            uint64_t next = 0;
            uint64_t ret = 0;
            if (!reader.Read(frame, next) || !reader.Read(frame + 8U, ret)) break;
            chain[chainCount++] = ret;
            if (next <= frame) break;
            frame = next;
        }
        for (uint64_t offset = 0; offset < 8192U && scanCount < 16U; offset += 8U) {
            uint64_t value = 0;
            if (!reader.Read(context.sp + offset, value)) break;
            if (InText(value)) scan[scanCount++] = value;
        }
    }
    svcSetThreadActivity(handle, ThreadActivity_Runnable);
    if (R_FAILED(result)) {
        text.Str("  ctx: svcGetThreadContext3 rc=").Hex(result).Char('\n');
        return;
    }
    text.Str("  pc=");
    AppendCode(text, context.pc.x);
    text.Str(" lr=");
    AppendCode(text, context.lr);
    text.Str(" sp=").Hex(context.sp).Str(" fp=").Hex(context.fp);
    text.Str("\n  x0=").Hex(context.cpu_gprs[0].x).Str(" x1=").Hex(context.cpu_gprs[1].x);
    text.Str(" x2=").Hex(context.cpu_gprs[2].x).Str(" x19=").Hex(context.cpu_gprs[19].x);
    text.Str("\n  fp_chain:");
    for (size_t index = 0; index < chainCount; ++index) {
        text.Char(' ');
        AppendCode(text, chain[index]);
    }
    text.Str("\n  stack_scan:");
    for (size_t index = 0; index < scanCount; ++index) {
        text.Char(' ');
        AppendCode(text, scan[index]);
    }
    text.Char('\n');
}

const char* SlotNameForHandle(Handle handle) noexcept {
    for (size_t index = 0; index < static_cast<size_t>(Thread::Count); ++index) {
        const Slot& slot = gSlots[index];
        if (slot.Bound.load(std::memory_order_acquire) != 0U &&
            slot.NativeHandle.load(std::memory_order_relaxed) == handle) {
            return kThreadNames[index];
        }
    }
    return nullptr;
}

void AppendThreads(Text& text, const StallTracker::Result* verdict) {
    for (size_t index = 0; index < static_cast<size_t>(Thread::Count); ++index) {
        const Slot& slot = gSlots[index];
        if (slot.Bound.load(std::memory_order_acquire) == 0U) continue;
        const auto handle = static_cast<Handle>(slot.NativeHandle.load(std::memory_order_relaxed));
        text.Str("thread=").Str(kThreadNames[index]).Str(" stage=")
            .Str(StageName(slot.CurrentStage.load(std::memory_order_relaxed)))
            .Str(" progress=").Dec(slot.Progress.load(std::memory_order_relaxed));
        if (verdict != nullptr) {
            text.Str(" parada_ms=").Dec(verdict->StalledFor[index]);
            if ((verdict->StalledMask & (1U << index)) != 0U) text.Str(" PARADA");
        }
        text.Str(" handle=").Hex(handle).Char('\n');
        AppendThreadContext(text, handle);
    }
    // As demais threads registradas (serviço: Mesa, spdlog, carregadores...):
    // quem segura um lock que o jogo espera aparece aqui.
    uint32_t handles[kMaximumThreads];
    SwitchThreadRole roles[kMaximumThreads];
    const size_t count = SnapshotSwitchThreads(handles, roles, kMaximumThreads);
    if (count == 0U) {
        text.Str("outras threads: registro ocupado ou vazio\n");
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        if (SlotNameForHandle(handles[index]) != nullptr) continue;
        text.Str("thread=").Str(SwitchThreadRoleName(roles[index])).Str(" handle=")
            .Hex(handles[index]).Char('\n');
        AppendThreadContext(text, handles[index]);
    }
}

void AppendCommon(Text& text) {
    const auto& service = gLastServiceCall;
    const char* name = service.Name.load(std::memory_order_relaxed);
    if (name != nullptr) {
        const uint64_t start = service.StartTicks.load(std::memory_order_relaxed);
        const uint64_t end = service.EndTicks.load(std::memory_order_relaxed);
        const uint64_t now = NowTicks();
        text.Str("ultima_chamada_servico=").Str(name)
            .Str(" chamadas=").Dec(service.Calls.load(std::memory_order_relaxed))
            .Str(" ha_ms=").Dec(TicksToMicroseconds(now - start) / 1000U);
        if (end == 0U || end < start) {
            text.Str(" EM_ANDAMENTO");
        } else {
            text.Str(" duracao_us=").Dec(TicksToMicroseconds(end - start));
        }
        text.Char('\n');
    } else {
        text.Str("ultima_chamada_servico=nenhuma\n");
    }
    if (gExtra != nullptr) {
        gExtra(gExtraUser, text);
        text.Char('\n');
    }
}

void AppendHeader(Text& text, const char* title) {
    text.Str("=== ").Str(title).Str(" ===\n");
    text.Str("uptime_ms=").Dec(UptimeMilliseconds()).Str(" relatorio=")
        .Dec(gReports.load(std::memory_order_relaxed) + 1U).Str(" nro_text=").Hex(gTextBegin)
        .Str("..").Hex(gTextEnd)
        .Str(" (addr2line -e oot3d_native_game.elf <offset entre parênteses>)\n");
}

bool AppendToFile(const char* data, size_t size) noexcept {
    if (gSdmc == nullptr || size == 0U) return false;
    char path[FS_MAX_PATH] = {};
    for (size_t index = 0; kHangPath[index] != '\0' && index + 1U < sizeof(path); ++index) {
        path[index] = kHangPath[index];
    }
    fsFsCreateFile(gSdmc, path, 0, 0);  // já existe: erro ignorado
    FsFile file;
    if (R_FAILED(fsFsOpenFile(gSdmc, path, FsOpenMode_Write | FsOpenMode_Append, &file))) {
        return false;
    }
    s64 offset = 0;
    fsFileGetSize(&file, &offset);
    const Result result = fsFileWrite(&file, offset, data, size, FsWriteOption_Flush);
    fsFileFlush(&file);
    fsFileClose(&file);
    return R_SUCCEEDED(result);
}

// Exclusão do buffer de relatório; espera até ~1 s pelo outro escritor.
bool AcquireWriter() noexcept {
    for (int attempt = 0; attempt < 100; ++attempt) {
        bool expected = false;
        if (gWriting.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
            return true;
        }
        svcSleepThread(10'000'000LL);
    }
    return false;
}

void ReleaseWriter() noexcept {
    gWriting.store(false, std::memory_order_release);
}

void WriteHang(const StallTracker::Result& verdict) {
    if (gReports.load(std::memory_order_relaxed) >= kMaximumReports || !AcquireWriter()) return;
    Text text(gReportBuffer, sizeof(gReportBuffer));
    AppendHeader(text, "TRAVAMENTO (hang watchdog)");
    text.Str("parada_max_ms=").Dec(verdict.Milliseconds).Char('\n');
    AppendCommon(text);
    AppendThreads(text, &verdict);
    text.Char('\n');
    if (AppendToFile(text.Data(), text.Size())) {
        gReports.store(gReports.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
    }
    ReleaseWriter();
}

void WriteRecovered(const StallTracker::Result& verdict) {
    if (!AcquireWriter()) return;
    char buffer[256];
    Text text(buffer, sizeof(buffer));
    text.Str("--- recuperou após ").Dec(verdict.Milliseconds).Str(" ms (uptime_ms=")
        .Dec(UptimeMilliseconds()).Str(")\n\n");
    AppendToFile(text.Data(), text.Size());
    ReleaseWriter();
}

bool InFocus() noexcept {
    return appletGetFocusState() == AppletFocusState_InFocus;
}

void WatchdogLoop() {
    StallTracker tracker;
    while (!gStop.load(std::memory_order_acquire)) {
        // ~1 s, em fatias, para o Stop não esperar.
        for (int slice = 0; slice < 10 && !gStop.load(std::memory_order_acquire); ++slice) {
            svcSleepThread(100'000'000LL);
        }
        if (gStop.load(std::memory_order_acquire)) break;
        StallTracker::Observation observation;
        observation.NowMilliseconds = UptimeMilliseconds();
        observation.Active = InFocus();
        for (size_t index = 0; index < static_cast<size_t>(Thread::Count); ++index) {
            const Slot& slot = gSlots[index];
            const bool bound = slot.Bound.load(std::memory_order_acquire) != 0U;
            // Vigiadas: o jogo e a render thread (as outras só aparecem no
            // relatório). A render thread ociosa (sem comando na fila) não
            // está travada: espera o jogo.
            const bool renderIdle =
                index == static_cast<size_t>(Thread::Render) &&
                slot.CurrentStage.load(std::memory_order_relaxed) ==
                    static_cast<uint8_t>(Stage::RenderIdle);
            observation.Watched[index] =
                bound && !renderIdle &&
                (index == static_cast<size_t>(Thread::Game) ||
                 index == static_cast<size_t>(Thread::Render));
            observation.Progress[index] = slot.Progress.load(std::memory_order_relaxed);
        }
        const auto verdict = tracker.Observe(observation);
        if (verdict.Kind == StallTracker::Verdict::Hang) {
            WriteHang(verdict);
        } else if (verdict.Kind == StallTracker::Verdict::Recovered) {
            WriteRecovered(verdict);
        }
    }
}

bool DisabledByConfig() noexcept {
    if (FILE* file = std::fopen(kDisablePath, "rb")) {
        std::fclose(file);
        return true;
    }
    return false;
}

void FindText() noexcept {
    const Region region = QueryRegion(reinterpret_cast<uintptr_t>(&FindText));
    gTextBegin = region.Begin;
    gTextEnd = region.End;
}

} // namespace

void Start(ExtraReporter extra, void* user) noexcept {
    if (gRunning.load(std::memory_order_acquire) || DisabledByConfig()) return;
    gSdmc = fsdevGetDeviceFileSystem("sdmc");
    if (gSdmc == nullptr) return;
    gStartTicks = NowTicks();
    FindText();
    gExtra = extra;
    gExtraUser = user;
    gStop.store(false, std::memory_order_release);
    try {
        // Nasce como thread de serviço (0x3B, núcleos 1|2: oot3d_switch_threads.cpp).
        gThread = std::thread(WatchdogLoop);
        gRunning.store(true, std::memory_order_release);
    } catch (...) {
    }
}

void Stop() noexcept {
    if (!gRunning.load(std::memory_order_acquire)) return;
    gStop.store(true, std::memory_order_release);
    if (gThread.joinable()) gThread.join();
    gRunning.store(false, std::memory_order_release);
    gExtra = nullptr;
    gExtraUser = nullptr;
}

bool Running() noexcept {
    return gRunning.load(std::memory_order_acquire);
}

uint32_t ReportsWritten() noexcept {
    return gReports.load(std::memory_order_relaxed);
}

void ReportEvent(const char* title, const char* details) noexcept {
    if (gSdmc == nullptr || gReports.load(std::memory_order_relaxed) >= kMaximumReports ||
        !AcquireWriter()) {
        return;
    }
    Text text(gReportBuffer, sizeof(gReportBuffer));
    AppendHeader(text, title);
    text.Str(details).Char('\n');
    AppendCommon(text);
    AppendThreads(text, nullptr);
    text.Char('\n');
    if (AppendToFile(text.Data(), text.Size())) {
        gReports.store(gReports.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
    }
    ReleaseWriter();
}

#endif

} // namespace Oot3dNativeGame::HangWatch
