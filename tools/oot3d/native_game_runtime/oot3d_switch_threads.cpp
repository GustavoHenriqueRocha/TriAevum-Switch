// Switch thread placement (linked with -Wl,--wrap=pthread_create).
//
// libnx creates every pthread on the process's default core (core 0) at
// priority 0x3B with a 128 KiB stack. Here every new pthread gets a 4 MiB
// stack (Mesa's GLSL compiler) and starts as a service thread of the active
// layout (oot3d_switch_thread_layout.h): by default priority 0x3B on cores
// 1|2, below everything that draws or plays sound, never on the game's core
// 0. The threads that matter (render worker, PICA plan worker, host audio)
// ask for their role when they start; SDL's audio thread is recognized by
// its TIME_CRITICAL request.
//
// Before (layout "legacy", kept for A/B) every pthread ran at the game's
// priority 0x2C on core 1 or 2, so spdlog's writer, Mesa's shader-cache
// maintenance (compaction), the resource pool and the loaders preempted the
// render worker (0x2D) whenever they had work: Horizon does not time-slice
// any priority but 0x3B.
// (Wrapping approach from centollOS/SwitchWaker switch/native/source/thread_wrap.c.)
#if defined(__SWITCH__)
#include <switch.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <sstream>
#include <string>

#include "oot3d_switch_threads.h"
#include "oot3d_hang_watchdog.h"

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* arg);
extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid);
extern "C" void* __aarch64_read_tp();

namespace Oot3dNativeGame {
namespace {

constexpr size_t kMinimumStackSize = 4U * 1024U * 1024U;
constexpr const char* kLayoutPath = "sdmc:/switch/oot3dre/config/thread_layout";

std::atomic<int> gNextCore{1};

// libnx code is built with -mtp=soft and finds TLS through __aarch64_read_tp,
// but devkitA64's libgcc (libgcov's indirect-call profiler, __thread data)
// reads tpidr_el0 directly, which Horizon leaves at 0: PGO-instrumented code
// faulted on its first indirect call. Mirror the libnx TLS pointer there on
// every thread.
void MirrorTlsPointer() {
    void* const tp = __aarch64_read_tp();
    asm volatile("msr tpidr_el0, %0" : : "r"(tp));
}

struct ThreadEntry {
    ThreadFunc Entry;
    void* Argument;
};

// Before any static constructor of instrumented code runs on the main thread.
__attribute__((constructor(101))) void MirrorMainThreadTlsPointer() {
    MirrorTlsPointer();
}

void RunThreadEntry(void* raw) {
    const ThreadEntry entry = *static_cast<ThreadEntry*>(raw);
    delete static_cast<ThreadEntry*>(raw);
    MirrorTlsPointer();
    entry.Entry(entry.Argument);
}

uint32_t ApplicationCoreMask() {
    uint64_t mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0) {
        mask = 0x7;
    }
    return static_cast<uint32_t>(mask) & 0x7U;
}

// ---- layout (read once) ----------------------------------------------------

SwitchThreadLayoutConfig ReadLayout() {
    char text[128] = {};
    if (FILE* file = std::fopen(kLayoutPath, "rb")) {
        const size_t read = std::fread(text, 1, sizeof(text) - 1, file);
        std::fclose(file);
        text[read] = '\0';
        return ParseSwitchThreadLayoutConfig(std::string_view(text, read));
    }
    return ParseSwitchThreadLayoutConfig({});
}

const SwitchThreadLayoutConfig& Layout() {
    // Function-local static: thread-safe, read on the first placement (the
    // main thread's, before any pthread exists in practice).
    static const SwitchThreadLayoutConfig config = ReadLayout();
    return config;
}

// ---- registry of live threads (perf summary) -----------------------------------

struct ThreadRecord {
    Handle ThreadHandle = INVALID_HANDLE;  // INVALID_HANDLE: free slot
    SwitchThreadRole Role = SwitchThreadRole::Service;
    uint64_t LastTicks = 0;   // summary reader only
    bool HasLastTicks = false;
    // Written only by the owning thread (load + store, no RMW); read by the
    // summary. Reset by the summary under the registry lock is not needed:
    // it keeps its own previous copy.
    std::array<std::atomic<uint32_t>, 3> CoreSeen{};
    std::array<uint32_t, 3> LastCoreSeen{};
};

std::mutex gRegistryMutex;
std::array<ThreadRecord, 48> gRecords;
uint64_t gLastSummaryTick = 0;
thread_local ThreadRecord* tRecord = nullptr;

void RegisterCurrentThread(SwitchThreadRole role) {
    const Handle self = threadGetCurHandle();
    std::lock_guard lock(gRegistryMutex);
    if (tRecord == nullptr) {
        for (auto& record : gRecords) {
            if (record.ThreadHandle == INVALID_HANDLE) {
                record.ThreadHandle = self;
                record.HasLastTicks = false;
                for (size_t core = 0; core < 3; ++core) {
                    record.CoreSeen[core].store(0, std::memory_order_relaxed);
                    record.LastCoreSeen[core] = 0;
                }
                tRecord = &record;
                break;
            }
        }
    }
    if (tRecord != nullptr) {
        tRecord->Role = role;
    }
    // Slot do watchdog de travamento (oot3d_hang_watchdog.h) para os papéis
    // que ele acompanha.
    switch (role) {
    case SwitchThreadRole::Game: HangWatch::Bind(HangWatch::Thread::Game, self); break;
    case SwitchThreadRole::Render: HangWatch::Bind(HangWatch::Thread::Render, self); break;
    case SwitchThreadRole::PicaPlan: HangWatch::Bind(HangWatch::Thread::PicaPlan, self); break;
    case SwitchThreadRole::HostAudio: HangWatch::Bind(HangWatch::Thread::HostAudio, self); break;
    case SwitchThreadRole::SdlAudio: HangWatch::Bind(HangWatch::Thread::SdlAudio, self); break;
    default: break;
    }
}

void UnregisterCurrentThread() {
    std::lock_guard lock(gRegistryMutex);
    if (tRecord != nullptr) {
        tRecord->ThreadHandle = INVALID_HANDLE;
        tRecord = nullptr;
    }
}

void ApplyPlacement(SwitchThreadRole role, int alternateCore) {
    const SwitchThreadPlacement placement =
        ResolveSwitchThreadPlacement(Layout().Layout, role, ApplicationCoreMask());
    const Handle self = threadGetCurHandle();
    svcSetThreadPriority(self, placement.Priority);
    if (placement.IdealCore == -2) {
        if (alternateCore >= 0) {
            svcSetThreadCoreMask(self, alternateCore, 1U << alternateCore);
        }
    } else if (placement.IdealCore >= 0 && placement.CoreMask != 0U) {
        svcSetThreadCoreMask(self, placement.IdealCore, placement.CoreMask);
    }
    RegisterCurrentThread(role);
}

struct Trampoline {
    void* (*Start)(void*);
    void* Argument;
    int AlternateCore;  // legacy layout only
};

void* RunPlaced(void* raw) {
    const Trampoline trampoline = *static_cast<Trampoline*>(raw);
    delete static_cast<Trampoline*>(raw);
    ApplyPlacement(SwitchThreadRole::Service, trampoline.AlternateCore);
    void* const result = trampoline.Start(trampoline.Argument);
    UnregisterCurrentThread();
    return result;
}

} // namespace

void PlaceSwitchThread(SwitchThreadRole role) {
    ApplyPlacement(role, -1);
}

void NoteSwitchThreadCore() {
    ThreadRecord* const record = tRecord;
    if (record == nullptr) {
        return;
    }
    const uint32_t core = svcGetCurrentProcessorNumber();
    if (core < 3U) {
        auto& counter = record->CoreSeen[core];
        counter.store(counter.load(std::memory_order_relaxed) + 1U,
                      std::memory_order_relaxed);
    }
}

size_t SnapshotSwitchThreads(uint32_t* handles, SwitchThreadRole* roles,
                             size_t capacity) noexcept {
    std::unique_lock lock(gRegistryMutex, std::try_to_lock);
    for (int attempt = 0; !lock.owns_lock() && attempt < 20; ++attempt) {
        svcSleepThread(5'000'000LL);
        lock.try_lock();
    }
    if (!lock.owns_lock()) {
        return 0U;
    }
    size_t count = 0U;
    for (const auto& record : gRecords) {
        if (record.ThreadHandle == INVALID_HANDLE || count == capacity) {
            continue;
        }
        handles[count] = record.ThreadHandle;
        roles[count] = record.Role;
        ++count;
    }
    return count;
}

SwitchThreadLayoutConfig ActiveSwitchThreadLayout() {
    return Layout();
}

std::string SwitchThreadPerfSummary() {
    struct RoleTotals {
        double Percent = 0.0;
        uint32_t Threads = 0;
        uint32_t Measured = 0;
        std::array<uint64_t, 3> Cores{};
    };
    std::array<RoleTotals, static_cast<size_t>(SwitchThreadRole::Count)> totals{};
    const uint64_t now = armGetSystemTick();
    {
        std::lock_guard lock(gRegistryMutex);
        const uint64_t window = gLastSummaryTick != 0 ? now - gLastSummaryTick : 0;
        gLastSummaryTick = now;
        for (auto& record : gRecords) {
            if (record.ThreadHandle == INVALID_HANDLE) {
                continue;
            }
            auto& role = totals[static_cast<size_t>(record.Role)];
            ++role.Threads;
            uint64_t ticks = 0;
            Result result = svcGetInfo(&ticks, InfoType_ThreadTickCount, record.ThreadHandle,
                                       UINT64_MAX);
            if (R_FAILED(result)) {
                result = svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated,
                                    record.ThreadHandle, UINT64_MAX);
            }
            if (R_SUCCEEDED(result)) {
                if (record.HasLastTicks && window != 0 && ticks >= record.LastTicks) {
                    role.Percent += static_cast<double>(ticks - record.LastTicks) * 100.0 /
                                    static_cast<double>(window);
                    ++role.Measured;
                }
                record.LastTicks = ticks;
                record.HasLastTicks = true;
            }
            for (size_t core = 0; core < 3; ++core) {
                const uint32_t seen = record.CoreSeen[core].load(std::memory_order_relaxed);
                role.Cores[core] += seen - record.LastCoreSeen[core];
                record.LastCoreSeen[core] = seen;
            }
        }
    }
    const auto& layout = Layout();
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(1);
    text << "threads: layout=" << SwitchThreadLayoutName(layout.Layout)
         << (layout.Valid ? "" : "(invalid)") << " pacer_spin_us=" << layout.PacerSpinMicroseconds;
    // Per role: CPU % of one core over the window (sum over its threads),
    // "/nN" when several threads share the role, "@c0:c1:c2" commands or
    // batches seen on each core (threads that note it).
    for (size_t index = 0; index < totals.size(); ++index) {
        const auto& role = totals[index];
        if (role.Threads == 0) {
            continue;
        }
        text << ' ' << SwitchThreadRoleName(static_cast<SwitchThreadRole>(index)) << "_cpu=";
        if (role.Measured != 0) {
            text << role.Percent;
        } else {
            text << "na";
        }
        if (role.Threads > 1) {
            text << "/n" << role.Threads;
        }
        if (role.Cores[0] + role.Cores[1] + role.Cores[2] != 0) {
            text << '@' << role.Cores[0] << ':' << role.Cores[1] << ':' << role.Cores[2];
        }
    }
    return text.str();
}

} // namespace Oot3dNativeGame

using Oot3dNativeGame::ApplicationCoreMask;

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* arg) {
    pthread_attr_t placed;
    pthread_attr_init(&placed);
    size_t stackSize = 0;
    if (attr != nullptr) {
        pthread_attr_getstacksize(attr, &stackSize);
    }
    pthread_attr_setstacksize(&placed, stackSize > Oot3dNativeGame::kMinimumStackSize
                                           ? stackSize
                                           : Oot3dNativeGame::kMinimumStackSize);

    // Legacy layout: alternate cores 1/2 by creation order (core 0 belongs to
    // the game thread). The other layouts ignore it.
    const uint32_t cores = ApplicationCoreMask() & ~1U;
    int core = -1;
    if (cores != 0U) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const int candidate = 1 + (Oot3dNativeGame::gNextCore.fetch_add(1) & 1);
            if ((cores & (1U << candidate)) != 0U) {
                core = candidate;
                break;
            }
        }
    }
    auto* trampoline = new Oot3dNativeGame::Trampoline{start, arg, core};
    const int result =
        __real_pthread_create(thread, &placed, Oot3dNativeGame::RunPlaced, trampoline);
    pthread_attr_destroy(&placed);
    if (result != 0) {
        delete trampoline;
    }
    return result;
}

extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid) {
    auto* wrapped = new Oot3dNativeGame::ThreadEntry{entry, arg};
    const Result result = __real_threadCreate(t, Oot3dNativeGame::RunThreadEntry, wrapped,
                                              stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(result)) {
        delete wrapped;
    }
    return result;
}

// SDL's audio thread asks for SDL_THREAD_PRIORITY_TIME_CRITICAL when it
// starts (SDL_RunAudio). devkitPro's SDL 2.28 maps every priority except HIGH
// to 0x3B (SDL_SYS_SetThreadPriority: HIGH -> 0x2B, anything else -> 0x3B),
// which would leave the thread that refills the audio renderer every 5 ms
// below the render worker and starve the voice (constant crackle). It gets
// the SdlAudio role: 0x2B, ahead of the game's threads (it only copies a
// buffer per audren frame), and core 1 with the host audio thread instead of
// wherever creation order put it (legacy: priority only).
extern "C" int __real_SDL_SetThreadPriority(int priority);
extern "C" int __wrap_SDL_SetThreadPriority(int priority) {
    constexpr int kSdlThreadPriorityTimeCritical = 3;
    if (priority == kSdlThreadPriorityTimeCritical) {
        Oot3dNativeGame::PlaceSwitchThread(Oot3dNativeGame::SwitchThreadRole::SdlAudio);
        return 0;
    }
    return __real_SDL_SetThreadPriority(priority);
}

// Called once from main: the game thread runs alone on core 0.
void PlaceSwitchMainThread() {
    Oot3dNativeGame::MirrorTlsPointer();
    Oot3dNativeGame::PlaceSwitchThread(Oot3dNativeGame::SwitchThreadRole::Game);
}
#endif
