#pragma once

// The second guest core (config/second_core, SWITCH_PORT.md "Second guest
// core"). Guest threads that asked svcCreateThread for processor 1 run on
// their own host thread (NativeA32Process::RunSecondCore), concurrently with
// the cooperative scheduler on the game thread (NativeA32Process::Run).
//
// Everything here is inert until SetActive(true): with the switch absent no
// lock is taken, no monitor is consulted and every path is the single-thread
// one.
//
// While active:
// - The kernel lock serializes the guest kernel and the host emulation: SVCs
//   and the scheduling of both cores, thread records, waits, handles, the
//   host services, the DSP HLE, savestates, and every out-of-line
//   NativeA32Memory path (slow paths, byte and pointer APIs, the page table
//   and its bookkeeping). The game thread holds it except while
//   NativeA32Process::Run executes guest code; the second core holds it for
//   its SVCs and scheduling. It is recursive per thread.
// - Guest code itself runs without it: plain loads and stores stay plain, and
//   LDREX/STREX go through the global exclusive monitor (memory.cpp), whose
//   lock is only ever taken after the kernel lock, never before it.
// - Guest code of each host core is bracketed by EnterGuest/LeaveGuest, so a
//   WorldStop (page-table rebuild, savestate) can wait until no other core is
//   inside guest code, where it may hold page pointers.

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace Oot3dNativeGame::SecondCore {

// Host cores that run guest code: 0 = the cooperative scheduler (the game
// thread, and every host-side caller), 1 = the second core's host thread.
inline constexpr size_t kProcessors = 2U;

inline std::atomic<bool> gActive{false};
inline std::mutex gKernelMutex;
// Every change a kernel waiter may wait for: a second-core thread became
// ready, a core left guest code, a world stop ended, the second core went
// idle, faulted or stopped. Few waiters (the game thread and the second
// core), so one condition and notify_all.
inline std::condition_variable gKernelChanged;
inline thread_local uint32_t tKernelDepth = 0U;
inline thread_local uint8_t tProcessor = 0U;
// Under the kernel lock.
inline std::array<uint32_t, kProcessors> gInGuest{};
inline std::array<uint32_t, kProcessors> gInKernel{};
inline uint32_t gStopRequests = 0U;
inline int gStopOwner = -1;
inline uint32_t gStopDepth = 0U;
inline uint32_t gKernelWaiters = 0U;

[[nodiscard]] inline bool Active() noexcept {
    return gActive.load(std::memory_order_acquire);
}

// Only while no other host thread runs guest code or host services (before
// the second core's host thread starts, after it was joined).
inline void SetActive(bool active) noexcept {
    gInGuest = {};
    gInKernel = {};
    gStopRequests = 0U;
    gStopOwner = -1;
    gStopDepth = 0U;
    gKernelWaiters = 0U;
    gActive.store(active, std::memory_order_release);
}

[[nodiscard]] inline bool KernelHeld() noexcept { return tKernelDepth != 0U; }
[[nodiscard]] inline uint8_t Processor() noexcept { return tProcessor; }

// Every wait of the second core's host thread for the kernel lock (after
// its dispatches and DSP frames, in its SVCs and memory paths), in
// nanoseconds: the perf line's core1_lock_wait_ms. The game thread's
// acquisitions are not timed.
inline std::atomic<uint64_t> gCore1LockWaitNanoseconds{0};

inline void AcquireKernelMutex() {
    if (tProcessor == 0U) {
        gKernelMutex.lock();
        return;
    }
    if (gKernelMutex.try_lock()) return;
    const auto start = std::chrono::steady_clock::now();
    gKernelMutex.lock();
    gCore1LockWaitNanoseconds.fetch_add(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - start)
                                  .count()),
        std::memory_order_relaxed);
}

inline void LockKernel() {
    if (tKernelDepth == 0U) AcquireKernelMutex();
    ++tKernelDepth;
}

inline void UnlockKernel() {
    if (--tKernelDepth == 0U) gKernelMutex.unlock();
}

// Holds the kernel lock for the scope while active (recursive).
class KernelGuard final {
  public:
    KernelGuard() : mLocked(Active()) {
        if (mLocked) LockKernel();
    }
    ~KernelGuard() {
        if (mLocked) UnlockKernel();
    }
    KernelGuard(const KernelGuard&) = delete;
    KernelGuard& operator=(const KernelGuard&) = delete;

  private:
    bool mLocked;
};

// Releases every level of the kernel lock the calling thread holds for the
// scope (guest code of the game thread, blocking host waits).
class KernelRelease final {
  public:
    KernelRelease() noexcept : mDepth(tKernelDepth) {
        if (mDepth != 0U) {
            tKernelDepth = 0U;
            gKernelMutex.unlock();
        }
    }
    ~KernelRelease() {
        if (mDepth != 0U) {
            AcquireKernelMutex();
            tKernelDepth = mDepth;
        }
    }
    KernelRelease(const KernelRelease&) = delete;
    KernelRelease& operator=(const KernelRelease&) = delete;

  private:
    uint32_t mDepth;
};

// A blocking host wait of the game thread at the frame loop's own level
// (holding only its base level of the kernel lock: not inside an SVC, a
// memory path or a world stop) releases the lock for the scope, so the
// second core keeps its SVCs while the game thread waits for the renderer;
// so does host work of the frame loop that touches no kernel, thread or
// host-service state (frame start, UI submission, presentation hand-off),
// whose guest memory accesses take the lock themselves. Anywhere else it
// does nothing.
class HostWaitRelease final {
  public:
    HostWaitRelease() noexcept
        : mReleased(tProcessor == 0U && tKernelDepth == 1U && Active()) {
        if (mReleased) {
            tKernelDepth = 0U;
            gKernelMutex.unlock();
        }
    }
    ~HostWaitRelease() {
        if (mReleased) {
            gKernelMutex.lock();
            tKernelDepth = 1U;
        }
    }
    HostWaitRelease(const HostWaitRelease&) = delete;
    HostWaitRelease& operator=(const HostWaitRelease&) = delete;

  private:
    bool mReleased;
};

// Kernel lock held: wakes the kernel waiters (none: no system call).
inline void NotifyKernel() noexcept {
    if (gKernelWaiters != 0U) gKernelChanged.notify_all();
}

// Waits for `ready`; the caller holds the kernel lock at any depth, which is
// released while blocked. `ready` runs with the lock held and the depth kept,
// so it may call code that takes the lock (recursively).
template <typename Predicate>
void WaitKernel(Predicate ready) {
    if (ready()) return;
    std::unique_lock<std::mutex> lock(gKernelMutex, std::adopt_lock);
    ++gKernelWaiters;
    gKernelChanged.wait(lock, ready);
    --gKernelWaiters;
    lock.release();
}

// As WaitKernel, bounded; false when `ready` still fails at the deadline.
template <typename Predicate>
bool WaitKernelFor(std::chrono::steady_clock::duration timeout, Predicate ready) {
    if (ready()) return true;
    std::unique_lock<std::mutex> lock(gKernelMutex, std::adopt_lock);
    ++gKernelWaiters;
    const bool result = gKernelChanged.wait_for(lock, timeout, ready);
    --gKernelWaiters;
    lock.release();
    return result;
}

// Kernel lock held.
[[nodiscard]] inline bool OtherCoresInGuest() noexcept {
    for (size_t processor = 0; processor < kProcessors; ++processor) {
        if (processor != tProcessor && gInGuest[processor] != 0U) return true;
    }
    return false;
}

[[nodiscard]] inline bool OtherCoresInKernel() noexcept {
    for (size_t processor = 0; processor < kProcessors; ++processor) {
        if (processor != tProcessor && gInKernel[processor] != 0U) return true;
    }
    return false;
}

// Kernel lock held. The calling core is about to run guest code: waits out a
// world stop first.
inline void EnterGuest() {
    WaitKernel([] { return gStopRequests == 0U; });
    ++gInGuest[tProcessor];
}

// Kernel lock held. The calling core left guest code.
inline void LeaveGuest() {
    --gInGuest[tProcessor];
    NotifyKernel();
}

// Kernel lock held. The second core's SVC handling: it may release the lock
// inside (its own world stop), so a world stop of the game thread waits until
// it is over and never replaces the thread record or registers it works on.
class KernelSection final {
  public:
    KernelSection() noexcept { ++gInKernel[tProcessor]; }
    ~KernelSection() {
        --gInKernel[tProcessor];
        NotifyKernel();
    }
    KernelSection(const KernelSection&) = delete;
    KernelSection& operator=(const KernelSection&) = delete;
};

// For its scope no other host core runs guest code (each one stops at its
// next dispatch boundary, holding no page pointer) or handles an SVC (the
// second core does not start one while a stop is requested): page-table
// rebuilds, savestates. One core at a time owns the stop; holds the kernel
// lock; nests on the owner.
class WorldStop final {
  public:
    WorldStop() : mActive(Active()) {
        if (!mActive) return;
        LockKernel();
        WaitKernel([] {
            return (gStopOwner < 0 || gStopOwner == static_cast<int>(tProcessor)) &&
                   !OtherCoresInKernel();
        });
        gStopOwner = static_cast<int>(tProcessor);
        ++gStopDepth;
        ++gStopRequests;
        WaitKernel([] { return !OtherCoresInGuest(); });
    }
    ~WorldStop() {
        if (!mActive) return;
        --gStopRequests;
        if (--gStopDepth == 0U) gStopOwner = -1;
        NotifyKernel();
        UnlockKernel();
    }
    WorldStop(const WorldStop&) = delete;
    WorldStop& operator=(const WorldStop&) = delete;

  private:
    bool mActive;
};

// Global exclusive monitor (oot3d_native_a32_memory.cpp), as dynarmic's
// ExclusiveMonitor: one reservation granule per host core. While active,
// LDREX marks the granule and returns the loaded value as the token; STREX
// succeeds only while the core's mark survives (another core's exclusive
// store to the granule clears it) and its compare-and-swap still finds the
// loaded value.
inline constexpr uint32_t kExclusiveGranuleMask = ~7U;
// Drops the calling core's reservation (a guest context switch on that core).
void ClearExclusive() noexcept;
// Drops every reservation (activation, savestate load, tests).
void ResetExclusiveMonitor() noexcept;

} // namespace Oot3dNativeGame::SecondCore
