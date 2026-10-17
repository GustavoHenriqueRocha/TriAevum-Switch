// Switch thread placement (linked with -Wl,--wrap=pthread_create).
//
// libnx creates every pthread on the process's default core (core 0) at
// priority 0x3B, below the main thread's 0x2C, with a 128 KiB stack. The
// render worker, audio and the shader warm-up thread would then share the
// game's core and run only while it waits; Horizon does not time-slice equal
// priorities either. Here the main (game) thread keeps core 0 to itself and
// every other thread alternates between cores 1 and 2 at the main thread's
// priority, with a stack large enough for Mesa's GLSL compiler.
// (Approach taken from centollOS/SwitchWaker switch/native/source/thread_wrap.c.)
#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#include <cstdint>
#include <pthread.h>

extern "C" int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* arg);
extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid);
extern "C" void* __aarch64_read_tp();

namespace {

constexpr int kThreadPriority = 0x2C;
constexpr size_t kMinimumStackSize = 4U * 1024U * 1024U;

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

struct Trampoline {
    void* (*Start)(void*);
    void* Argument;
    int Core;
};

void* RunPlaced(void* raw) {
    const Trampoline trampoline = *static_cast<Trampoline*>(raw);
    delete static_cast<Trampoline*>(raw);
    const Handle self = threadGetCurHandle();
    svcSetThreadPriority(self, kThreadPriority);
    if (trampoline.Core >= 0) {
        svcSetThreadCoreMask(self, trampoline.Core, 1U << trampoline.Core);
    }
    return trampoline.Start(trampoline.Argument);
}

} // namespace

extern "C" int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                                     void* (*start)(void*), void* arg) {
    pthread_attr_t placed;
    pthread_attr_init(&placed);
    size_t stackSize = 0;
    if (attr != nullptr) {
        pthread_attr_getstacksize(attr, &stackSize);
    }
    pthread_attr_setstacksize(&placed, stackSize > kMinimumStackSize ? stackSize : kMinimumStackSize);

    const uint32_t cores = ApplicationCoreMask() & ~1U; // core 0 belongs to the game thread
    int core = -1;
    if (cores != 0U) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const int candidate = 1 + (gNextCore.fetch_add(1) & 1);
            if ((cores & (1U << candidate)) != 0U) {
                core = candidate;
                break;
            }
        }
    }
    auto* trampoline = new Trampoline{start, arg, core};
    const int result = __real_pthread_create(thread, &placed, RunPlaced, trampoline);
    pthread_attr_destroy(&placed);
    if (result != 0) {
        delete trampoline;
    }
    return result;
}

extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem,
                                      size_t stack_sz, int prio, int cpuid) {
    auto* wrapped = new ThreadEntry{entry, arg};
    const Result result = __real_threadCreate(t, RunThreadEntry, wrapped, stack_mem, stack_sz, prio, cpuid);
    if (R_FAILED(result)) {
        delete wrapped;
    }
    return result;
}

// Called once from main: the game thread runs alone on core 0.
void PlaceSwitchMainThread() {
    MirrorTlsPointer();
    const Handle self = threadGetCurHandle();
    svcSetThreadPriority(self, kThreadPriority);
    if ((ApplicationCoreMask() & 1U) != 0U) {
        svcSetThreadCoreMask(self, 0, 1U);
    }
}
#endif
