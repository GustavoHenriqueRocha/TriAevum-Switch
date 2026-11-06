#pragma once

#include "oot3d_native_a32_memory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Oot3dNativeGame {

class NativeA32Process;

struct NativeA32HostContext {
    NativeA32Process& Process;
    uint32_t ThreadId = 0;
};

enum class NativeA32HostAction : uint8_t {
    Resume,
    Wait,
    Terminate,
    Fault,
};

struct NativeA32HostResult {
    NativeA32HostAction Action = NativeA32HostAction::Fault;
    std::optional<uint32_t> ResumePc;
    uint32_t Detail = 0;
    std::string Error;
};

class NativeA32HostServices {
  public:
    virtual ~NativeA32HostServices() = default;
    virtual NativeA32HostResult HandleSvc(
        uint32_t immediate, oot3d::recomp::a32::GuestState& state,
        NativeA32Memory& memory, NativeA32HostContext& context) = 0;
    virtual NativeA32HostResult HandleFallback(
        oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
        const oot3d::recomp::a32::PackedOp& op,
        oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory);
    // Completes a finite wait that can be resolved atomically while an
    // InvokeFunctionWithState caller remains on the native C++ stack.
    // Synchronization waits requiring another guest thread must return false.
    virtual bool CompleteSynchronousWait(
        uint32_t immediate, const NativeA32HostResult& waitResult,
        oot3d::recomp::a32::GuestState& state,
        NativeA32Memory& memory, NativeA32HostContext& context,
        std::string* error = nullptr);
};

struct NativeA32PrimaryThreadConfig {
    uint32_t EntryAddress = 0;
    uint32_t StackBaseAddress = 0;
    size_t StackSize = 0;
    uint32_t TlsBaseAddress = 0;
    size_t TlsSize = 0;
    uint32_t ThreadPointer = 0;
    uint32_t Argument0 = 0;
    uint32_t InitialCpsr = 0;
    uint32_t InitialFpscr = 0;
    uint32_t Priority = 0;
};

struct NativeA32ThreadConfig {
    uint32_t EntryAddress = 0;
    uint32_t Argument = 0;
    uint32_t StackTopAddress = 0;
    uint32_t InitialCpsr = 0;
    uint32_t InitialFpscr = 0;
    uint32_t Priority = 0;
    // svcCreateThread's processor id (r4): -2 = the process default, -1 =
    // any core, 0..3 = that core. Processor 1 selects the second core's host
    // thread once EnableSecondCore() was called; every other thread runs on
    // the game thread's cooperative scheduler.
    int32_t ProcessorId = -2;
};

enum class NativeA32ThreadStatus : uint8_t {
    Empty,
    Ready,
    Running,
    Waiting,
    Terminated,
    Faulted,
};

enum class NativeA32ProcessRunKind : uint8_t {
    Yielded,
    Waiting,
    Terminated,
    Faulted,
};

struct NativeA32ProcessRunResult {
    NativeA32ProcessRunKind Kind = NativeA32ProcessRunKind::Faulted;
    oot3d::recomp::a32::ExecutionResult Exit;
    uint32_t HostTransitions = 0;
    std::string Error;
    // Faulted only: oot3d_native_guest_fault_report.h, for the game log.
    std::string GuestReport;
};

struct NativeA32ProcessTimingStats {
    uint64_t ProcessRunCalls = 0;
    uint64_t DispatchCalls = 0;
    uint64_t DispatchNanoseconds = 0;
    uint64_t SvcCalls = 0;
    uint64_t SvcNanoseconds = 0;
    std::array<uint64_t, 256> SvcCallsByImmediate{};
    std::array<uint64_t, 256> SvcNanosecondsByImmediate{};
    uint64_t FallbackCalls = 0;
    uint64_t FallbackNanoseconds = 0;
};

// Host time each guest thread held the game thread inside Run(): its AOT
// dispatch, the host services its SVCs ran (inline, on the same thread) and
// the scheduler. One counter read at Run() entry, one per thread switch and
// one at exit, so the core-0 thread totals add up to RunTicks exactly. Always on.
// Threads on the second core (EnableSecondCore) are charged their host
// thread's time in guest code and in their SVCs, which SecondCoreTicks sums;
// SecondCoreLockTicks is the time it then waited for the kernel lock,
// SecondCoreIdleWorkTicks its idle work (the DSP frames) and
// SecondCoreCpuTicks the CPU time its host thread got (0 where the platform
// cannot tell: SecondCoreCpuKnown false). SecondCoreLockWaitNanoseconds is
// every wait of that host thread for the kernel lock (also the ones inside
// its SVCs and memory paths). Busy wall time minus lock waits minus CPU time
// is the time other host threads of its core preempted it.
// Ticks are the ARM system counter on the Switch, nanoseconds on the host.
// Threads with an id at or above the last slot share it ("other").
inline constexpr size_t kNativeA32GuestThreadSlots = 32U;
inline constexpr int32_t kNativeA32UnknownProcessor = INT32_MIN;
struct NativeA32GuestThreadProfile {
    std::array<uint64_t, kNativeA32GuestThreadSlots> Ticks{};
    // Requested processor per thread id, from CreateThread (not saved state:
    // kNativeA32UnknownProcessor for threads a restore brought in, unless the
    // savestate was made with the second core on).
    std::array<int32_t, kNativeA32GuestThreadSlots> ProcessorIds{};
    uint64_t RunTicks = 0;
    uint64_t Switches = 0;
    uint64_t SecondCoreTicks = 0;
    uint64_t SecondCoreLockTicks = 0;
    uint64_t SecondCoreIdleWorkTicks = 0;
    uint64_t SecondCoreIdleWorkUnits = 0;
    uint64_t SecondCoreCpuTicks = 0;
    bool SecondCoreCpuKnown = false;
    uint64_t SecondCoreLockWaitNanoseconds = 0;
};
uint64_t NativeA32ProfileTicksToNanoseconds(uint64_t ticks) noexcept;
// The perf.txt line for one window: "guest_threads: host_frames=<n>
// run_ms=<ms/frame> switches=<per frame> <id>:<entry>:<prio>:<proc>=<ms/frame> ...",
// threads that ran in the window only; with the second core on, then
// " core1_ms=<ms/frame> core1_lock_ms=<ms/frame>".
std::string FormatNativeA32GuestThreadsLine(
    const NativeA32Process& process,
    const NativeA32GuestThreadProfile& windowStart, uint64_t hostFrameCount,
    uint64_t windowHostFrames);
// The game-log line of one guest CreateThread.
std::string FormatNativeA32GuestThreadCreated(
    uint32_t id, uint32_t entryAddress, uint32_t priority, int32_t processorId,
    uint32_t stackTopAddress);
// The DSP side of one perf window with the second core on (window session).
struct NativeA32SecondCoreDspWindow {
    uint64_t Pumped = 0;       // delivered by the game thread's pump
    uint64_t Flushed = 0;      // delivered at a deferred point
    uint64_t Late = 0;         // ... after the idle wait timed out
    double WaitSeconds = 0.0;  // game thread waiting for the second core
};
// The perf.txt line "second_core: dsp_pumped= dsp_flushed= dsp_late=
// dsp_wait_ms= dsp_core1= dsp_core1_ms= core1_lock_wait_ms=
// core1_preempted_ms= core0_saved_ms=", per host frame of the window:
// dsp_core1 the frames the second core delivered (idle work) and their ms;
// core1_lock_wait_ms all its waits for the kernel lock; core1_preempted_ms
// its busy wall time (guest code, SVCs, DSP frames) minus those waits minus
// its host thread's CPU time ("na" where unknown); core0_saved_ms the CPU
// time it worked (busy wall time minus lock waits where unknown) minus
// dsp_wait_ms.
std::string FormatNativeA32SecondCoreLine(const NativeA32GuestThreadProfile& now,
                                          const NativeA32GuestThreadProfile& windowStart,
                                          const NativeA32SecondCoreDspWindow& dsp,
                                          uint64_t windowHostFrames);

// config/second_core: what the second core's host thread does while none of
// its guest threads is ready and no work an interrupt woke is pending
// (AuxiliaryWorkPending): the window session's DSP frames. Both run there
// with the kernel lock held. Pending is part of its wait predicate (whatever
// changes it notifies the kernel condition); Run does one unit of work and
// keeps the lock.
struct NativeA32SecondCoreIdleWork {
    std::function<bool()> Pending;
    std::function<void()> Run;
};

class NativeA32Process {
  public:
    NativeA32Process(const oot3d::recomp::a32::Registry& registry,
                     NativeA32HostServices& hostServices);

    bool MapRegion(const NativeA32MemoryRegionConfig& config,
                   std::string* error = nullptr);
    bool CreatePrimaryThread(const NativeA32PrimaryThreadConfig& config,
                             std::string* error = nullptr);
    bool ResumePrimaryThread(std::optional<uint32_t> resumePc = std::nullopt,
                             std::string* error = nullptr);
    bool ResumeThread(uint32_t threadId,
                      std::optional<uint32_t> resumePc = std::nullopt,
                      std::string* error = nullptr);
    std::optional<uint32_t> CreateThread(const NativeA32ThreadConfig& config,
                                         std::string* error = nullptr);
    NativeA32ProcessRunResult Run(uint32_t blockLimitPerDispatch = 1'000'000U,
                                  uint32_t maxHostTransitions = 1024U);
    // Executes an original guest routine synchronously against a copy of the
    // primary-thread registers. Guest-memory and host-service effects remain
    // visible, while scheduler ownership stays with Run(). Only finite waits
    // that the host can complete atomically are permitted.
    bool InvokeFunction(uint32_t entryAddress,
                        std::span<const uint32_t> arguments,
                        uint32_t returnAddress,
                        std::string* error = nullptr,
                        uint32_t blockLimitPerDispatch = 1'000'000U,
                        uint32_t maxHostTransitions = 1024U);
    bool InvokeFunctionWithResult(
        uint32_t entryAddress, std::span<const uint32_t> arguments,
        uint32_t returnAddress, uint32_t* returnValue,
        std::string* error = nullptr,
        uint32_t blockLimitPerDispatch = 1'000'000U,
        uint32_t maxHostTransitions = 1024U);
    // Executes a guest routine from a caller-prepared register image. This is
    // the ABI-complete path used by source-owner adapters for VFP and stack
    // arguments; scheduler-owned thread registers remain untouched.
    bool InvokeFunctionWithState(
        uint32_t entryAddress, oot3d::recomp::a32::GuestState& state,
        uint32_t returnAddress, std::string* error = nullptr,
        uint32_t blockLimitPerDispatch = 1'000'000U,
        uint32_t maxHostTransitions = 1024U);
    // Invokes one host SVC against a caller-owned register image without
    // mutating scheduler-owned thread registers.
    bool InvokeSvcWithState(
        uint32_t immediate, oot3d::recomp::a32::GuestState& state,
        std::string* error = nullptr);
    void SetBlockEntryCallback(
        oot3d::recomp::a32::BlockEntryCallback callback,
        void* user = nullptr,
        std::vector<uint32_t> filterPcs = {});
    void SetNativeFunctionCallback(
        oot3d::recomp::a32::NativeFunctionCallback callback,
        void* user = nullptr,
        std::vector<uint32_t> functionPcs = {});
    void SetNativeBlockCallback(
        oot3d::recomp::a32::NativeBlockCallback callback,
        void* user = nullptr);
    void SetTimingEnabled(bool enabled) noexcept;
    NativeA32ProcessTimingStats TimingStats() const noexcept;
    // A snapshot: the second core's per-thread time is folded in.
    NativeA32GuestThreadProfile GuestThreadProfile() const noexcept;

    // Second guest core (oot3d_native_a32_second_core.h, config/second_core).
    // From here on threads that asked for processor 1 (also ones that already
    // exist) belong to the second core: the cooperative scheduler never
    // selects them, RunSecondCore runs them. Activates the kernel lock; call
    // it before the second core's host thread starts, from the thread that
    // then holds the lock for the host work (the game thread).
    void EnableSecondCore();
    // After RunSecondCore returned: back to one core (the lock goes inert).
    void DisableSecondCore();
    [[nodiscard]] bool SecondCoreEnabled() const noexcept {
        return mSecondCoreEnabled;
    }
    // The second core's host thread: runs the ready second-core guest
    // threads (highest priority first, each dispatch at most
    // blockLimitPerDispatch blocks); while none is ready it does its idle
    // work when due (SetSecondCoreIdleWork) and otherwise waits on the kernel
    // lock's condition, until RequestSecondCoreStop(). A fault of one of
    // them or of the idle work is returned by the next Run() on the game
    // thread.
    void RunSecondCore(uint32_t blockLimitPerDispatch);
    // Kernel lock held (or the second core not running).
    void RequestSecondCoreStop();
    // Before the second core's host thread starts: what it does while none
    // of its threads is ready (the window session's DSP frames).
    void SetSecondCoreIdleWork(NativeA32SecondCoreIdleWork work);
    // Kernel lock held. A second-core thread is ready or running.
    [[nodiscard]] bool SecondCoreBusy() const;
    // Kernel lock held. Work an interrupt woke is still runnable: the second
    // core is busy, or a core-0 thread above the primary thread's priority is
    // ready or running (the next DSP frame goes out only once both are done).
    [[nodiscard]] bool AuxiliaryWorkPending() const;
    // Kernel lock held. Waits (releasing it) until the second core is idle,
    // stopped or faulted; false when it is still busy at the timeout.
    bool WaitSecondCoreIdle(std::chrono::steady_clock::duration timeout);
    // 1 for a second-core thread, else 0.
    [[nodiscard]] uint8_t ThreadHostCore(uint32_t threadId) const;

    uint64_t StateFingerprint() const;
    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

    NativeA32Memory& Memory();
    const NativeA32Memory& Memory() const;
    oot3d::recomp::a32::GuestState& PrimaryThreadState();
    const oot3d::recomp::a32::GuestState& PrimaryThreadState() const;
    NativeA32ThreadStatus PrimaryThreadStatus() const;
    bool OnlyBackgroundThreadsReady() const;
    uint32_t DispatchBlockBudget(uint32_t requested) const;
    size_t ThreadCount() const;
    const oot3d::recomp::a32::GuestState* ThreadState(uint32_t threadId) const;
    oot3d::recomp::a32::GuestState* ThreadState(uint32_t threadId);
    NativeA32ThreadStatus ThreadStatus(uint32_t threadId) const;
    std::optional<uint32_t> ThreadPriority(uint32_t threadId) const;
    std::optional<uint32_t> ThreadEntryAddress(uint32_t threadId) const;
    std::optional<uint32_t> ThreadArgument(uint32_t threadId) const;

  private:
    struct PendingFallback {
        bool Active = false;
        NativeA32HostResult Result;
        oot3d::recomp::a32::ExecutionResult Exit;
    };

    static oot3d::recomp::a32::ExecutionResult DispatchFallback(
        oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
        const oot3d::recomp::a32::PackedOp& op,
        oot3d::recomp::a32::GuestState& state,
        oot3d::recomp::a32::MemoryBus& memory, void* user);
    static oot3d::recomp::a32::ExecutionResult DispatchSecondCoreFallback(
        oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
        const oot3d::recomp::a32::PackedOp& op,
        oot3d::recomp::a32::GuestState& state,
        oot3d::recomp::a32::MemoryBus& memory, void* user);
    oot3d::recomp::a32::ExecutionResult HandleDispatchFallback(
        PendingFallback& pending, oot3d::recomp::a32::FallbackReason reason,
        uint32_t pc, const oot3d::recomp::a32::PackedOp& op,
        oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory);
    NativeA32ProcessRunResult ApplyHostResult(
        NativeA32HostResult hostResult,
        const oot3d::recomp::a32::ExecutionResult& exit,
        uint32_t defaultResumePc, uint32_t hostTransitions);
    NativeA32ProcessRunResult Fault(
        const oot3d::recomp::a32::ExecutionResult& exit,
        uint32_t hostTransitions, std::string error);
    NativeA32ProcessRunResult FaultThread(
        uint32_t threadId, const oot3d::recomp::a32::ExecutionResult& exit,
        uint32_t hostTransitions, std::string error);
    oot3d::recomp::a32::GuestState& CurrentThreadState();
    NativeA32ThreadStatus& CurrentThreadStatus();
    bool SelectReadyThread();
    bool HasReadyThread() const;
    bool AllThreadsTerminated() const;
    void BeginThreadProfile() noexcept;
    void EndThreadProfile() noexcept;
    void ChargeThreadProfile(uint32_t threadId, uint64_t now) noexcept;

    struct SecondaryThread {
        uint32_t Id = 0;
        uint32_t Priority = 0;
        uint32_t EntryAddress = 0;
        uint32_t Argument = 0;
        oot3d::recomp::a32::GuestState State;
        NativeA32ThreadStatus Status = NativeA32ThreadStatus::Empty;
        // CreateThread's processor (kNativeA32UnknownProcessor if a restore
        // could not tell) and the host core it runs on: 1 = the second core.
        int32_t ProcessorId = kNativeA32UnknownProcessor;
        uint8_t HostCore = 0;
    };

    [[nodiscard]] uint8_t HostCoreFor(int32_t processorId) const noexcept {
        return mSecondCoreEnabled && processorId == 1 ? 1U : 0U;
    }
    // Kernel lock held: the second core's next thread, or null.
    SecondaryThread* SelectSecondCoreThread();
    oot3d::recomp::a32::ExecutionResult DispatchGuest(
        oot3d::recomp::a32::GuestState& state, uint32_t blockLimit,
        bool secondCore);
    // Kernel lock held: the exit of one second-core dispatch.
    void CompleteSecondCoreDispatch(SecondaryThread& thread,
                                    const oot3d::recomp::a32::ExecutionResult& exit);
    void ApplySecondCoreHostResult(SecondaryThread& thread,
                                   NativeA32HostResult hostResult,
                                   const oot3d::recomp::a32::ExecutionResult& exit,
                                   uint32_t defaultResumePc);
    void ChargeSecondCore(uint32_t threadId, uint64_t busyTicks,
                          uint64_t lockTicks) noexcept;
    // Kernel lock held: the second core's idle work is due (its threads all
    // waiting, no stop or fault, no auxiliary work, the work pending).
    [[nodiscard]] bool SecondCoreIdleWorkDue() const;
    // Kernel lock held: the current core-0 thread stopped running; when it
    // was auxiliary work, the second core's idle work may be due now.
    void NoteCore0ThreadParked() noexcept;

    const oot3d::recomp::a32::Registry& mRegistry;
    NativeA32HostServices& mHostServices;
    NativeA32Memory mMemory;
    oot3d::recomp::a32::GuestState mPrimaryThreadState;
    NativeA32ThreadStatus mPrimaryThreadStatus = NativeA32ThreadStatus::Empty;
    uint32_t mPrimaryThreadPriority = 0;
    uint32_t mPrimaryThreadEntryAddress = 0;
    uint32_t mPrimaryThreadArgument = 0;
    uint32_t mCurrentThreadId = 0;
    uint32_t mTlsBaseAddress = 0;
    size_t mTlsSize = 0;
    size_t mNextTlsOffset = 0;
    std::deque<SecondaryThread> mSecondaryThreads;
    PendingFallback mPendingFallback;
    oot3d::recomp::a32::BlockEntryCallback mBlockEntryCallback = nullptr;
    void* mBlockEntryUser = nullptr;
    std::vector<uint32_t> mBlockEntryFilterPcs;
    oot3d::recomp::a32::NativeFunctionCallback mNativeFunctionCallback =
        nullptr;
    void* mNativeFunctionUser = nullptr;
    std::vector<uint32_t> mNativeFunctionPcs;
    oot3d::recomp::a32::NativeBlockCallback mNativeBlockCallback = nullptr;
    void* mNativeBlockUser = nullptr;
    bool mTimingEnabled = false;
    NativeA32ProcessTimingStats mTimingStats;
    NativeA32GuestThreadProfile mThreadProfile{};
    bool mThreadProfileRunning = false;
    uint64_t mThreadProfileMark = 0;
    uint64_t mThreadProfileRunStart = 0;
    uint32_t mThreadProfileLastThread = 0;
    // Second core. Flags and the fault under the kernel lock; the tick
    // counters are written by its host thread and read by the game thread.
    bool mSecondCoreEnabled = false;
    bool mSecondCoreRunning = false;
    bool mSecondCoreStopRequested = false;
    // Bumped by RestoreState (kernel lock held): the thread records were
    // replaced.
    uint64_t mThreadRecordGeneration = 0;
    std::optional<NativeA32ProcessRunResult> mSecondCoreFault;
    PendingFallback mSecondCorePendingFallback;
    std::array<std::atomic<uint64_t>, kNativeA32GuestThreadSlots>
        mSecondCoreThreadTicks{};
    std::atomic<uint64_t> mSecondCoreTicks{0};
    std::atomic<uint64_t> mSecondCoreLockTicks{0};
    NativeA32SecondCoreIdleWork mSecondCoreIdleWork;
    std::atomic<uint64_t> mSecondCoreIdleWorkTicks{0};
    std::atomic<uint64_t> mSecondCoreIdleWorkUnits{0};
    // CPU time of the second core's host thread since RunSecondCore began,
    // sampled each time it goes to sleep.
    std::atomic<uint64_t> mSecondCoreCpuTicks{0};
    std::atomic<bool> mSecondCoreCpuKnown{false};
    // Regions this process mapped (segments, stacks, TLS), named in the
    // guest fault report; host-service mappings are not listed.
    struct RegionName {
        std::string Name;
        uint32_t BaseAddress = 0;
        uint32_t Size = 0;
    };
    std::vector<RegionName> mRegionNames;
};

} // namespace Oot3dNativeGame
