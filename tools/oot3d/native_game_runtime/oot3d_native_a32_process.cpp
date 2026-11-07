#include "oot3d_native_a32_process.h"
#include "oot3d_cpu_phase_probe.h"
#include "oot3d_native_a32_second_core.h"

#include "oot3d_native_compiled_functions.h"
#include "oot3d_native_guest_fault_report.h"
#include "oot3d_switch_guest_window.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(__SWITCH__)
// The libnx headers carry C linkage only through <switch.h>.
extern "C" {
#include <switch/kernel/svc.h>
#include <switch/result.h>
}
#elif defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif

namespace Oot3dNativeGame {
namespace {

constexpr uint32_t kFallbackBoundaryDetail = 0x41333248U;
using TimingClock = std::chrono::steady_clock;

uint64_t ElapsedNanoseconds(TimingClock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            TimingClock::now() - start).count());
}

// The guest-thread profile's clock: one read per thread switch.
inline uint64_t ReadThreadProfileCounter() noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    uint64_t value;
    // armGetSystemTick(): Horizon blocks cntvct_el0 at EL0 (Data Abort).
    asm volatile("mrs %0, cntpct_el0" : "=r"(value));
    return value;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            TimingClock::now().time_since_epoch()).count());
#endif
}

// CPU time of the calling host thread, in the profile clock's unit (system
// ticks on the Switch, nanoseconds elsewhere); false where it is unknown.
bool ReadThreadCpuCounter(uint64_t* ticks) noexcept {
#if defined(__SWITCH__)
    // [13.0.0+] ThreadTickCount, before it the deprecated id; every core.
    return R_SUCCEEDED(svcGetInfo(ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE,
                                  TickCountInfo_Total)) ||
           R_SUCCEEDED(svcGetInfo(ticks, InfoType_ThreadTickCountDeprecated,
                                  CUR_THREAD_HANDLE, TickCountInfo_Total));
#elif defined(__unix__) || defined(__APPLE__)
    timespec value{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return false;
    *ticks = static_cast<uint64_t>(value.tv_sec) * 1'000'000'000ULL +
             static_cast<uint64_t>(value.tv_nsec);
    return true;
#else
    (void)ticks;
    return false;
#endif
}

constexpr size_t ThreadProfileSlot(uint32_t threadId) noexcept {
    return threadId < kNativeA32GuestThreadSlots
               ? threadId
               : kNativeA32GuestThreadSlots - 1U;
}

#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
oot3d::recomp::a32::ExecutionResult DispatchWholeAotProduct(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    uint32_t blockLimit,
    oot3d::recomp::a32::BlockEntryCallback blockEntry,
    void* blockEntryUser, const uint32_t* blockEntryPcs,
    size_t blockEntryPcCount, std::optional<uint32_t> stopPc,
    oot3d::recomp::a32::NativeBlockCallback productMod, void* productModUser) {
    using oot3d::recomp::a32::ExecutionResult;
    using oot3d::recomp::a32::ExitKind;
    using oot3d::recomp::a32::FallbackReason;

    uint32_t consumedTotal = 0U;
    while (consumedTotal < blockLimit) {
        if (consumedTotal != 0U && stopPc.has_value() &&
            state.r[15] == *stopPc) {
            return {ExitKind::Branch, *stopPc, FallbackReason::None, 0U};
        }

        ExecutionResult result{};
        uint32_t consumed = 0U;
        const uint32_t remaining = blockLimit - consumedTotal;
        const bool modHandled = productMod != nullptr && productMod(
            state.r[15], state, memory, &result, remaining, &consumed, productModUser);
        const bool handled = modHandled || ExecuteOot3dCompiledFunction(
            state.r[15], state, memory, &result, remaining, &consumed,
            blockEntry, blockEntryUser, blockEntryPcs, blockEntryPcCount,
            false, true, true, stopPc.value_or(0U));
        if (!handled) {
            return {
                ExitKind::MissingBlock,
                state.r[15],
                FallbackReason::MissingBlock,
                0x57414F54U,
            };
        }

        consumed = std::clamp(consumed, 1U, remaining);
        consumedTotal += consumed;
        if (result.kind != ExitKind::Branch &&
            result.kind != ExitKind::Fallthrough) {
            if (result.kind == ExitKind::BlockLimit) {
                result.detail = consumedTotal;
            }
            return result;
        }
        state.r[15] = result.pc;
    }

    return {
        ExitKind::BlockLimit,
        state.r[15],
        FallbackReason::None,
        consumedTotal,
    };
}
#endif

bool RangeFits(uint32_t baseAddress, size_t size) {
    return size != 0 &&
           static_cast<uint64_t>(baseAddress) + static_cast<uint64_t>(size) <=
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U;
}

struct SynchronousInvokeCallbackContext {
    uint32_t ReturnAddress = 0U;
    oot3d::recomp::a32::NativeFunctionCallback Delegate = nullptr;
    void* DelegateUser = nullptr;
};

bool StopSynchronousInvokeAtReturn(
    uint32_t pc, oot3d::recomp::a32::GuestState& state,
    oot3d::recomp::a32::MemoryBus& memory,
    oot3d::recomp::a32::ExecutionResult* result, void* user) {
    auto& context =
        *static_cast<SynchronousInvokeCallbackContext*>(user);
    if (pc == context.ReturnAddress) {
        if (result == nullptr) {
            return false;
        }
        *result = {
            oot3d::recomp::a32::ExitKind::MissingBlock,
            pc,
            oot3d::recomp::a32::FallbackReason::None,
            0U,
        };
        return true;
    }
    return context.Delegate != nullptr &&
           context.Delegate(
               pc, state, memory, result, context.DelegateUser);
}

void SetError(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

nlohmann::json EncodeGuestState(
    const oot3d::recomp::a32::GuestState& state) {
    return {
        {"r", state.r},
        {"cpsr", state.cpsr},
        {"fpscr", state.fpscr},
        {"thread_pointer", state.thread_pointer},
        {"vfp", state.vfp},
        {"exclusive_address", state.exclusive_address},
        {"exclusive_token", state.exclusive_token},
        {"exclusive_size", state.exclusive_size},
        {"exclusive_valid", state.exclusive_valid},
    };
}

bool DecodeGuestState(const nlohmann::json& encoded,
                      oot3d::recomp::a32::GuestState& state) {
    if (!encoded.is_object()) {
        return false;
    }
    state = {};
    state.r = encoded.at("r").get<decltype(state.r)>();
    state.cpsr = encoded.at("cpsr").get<uint32_t>();
    state.fpscr = encoded.at("fpscr").get<uint32_t>();
    state.thread_pointer = encoded.at("thread_pointer").get<uint32_t>();
    state.vfp = encoded.at("vfp").get<decltype(state.vfp)>();
    state.exclusive_address =
        encoded.at("exclusive_address").get<uint32_t>();
    state.exclusive_token =
        encoded.at("exclusive_token").get<uint64_t>();
    state.exclusive_size = encoded.at("exclusive_size").get<uint8_t>();
    state.exclusive_valid = encoded.at("exclusive_valid").get<bool>();
    return state.exclusive_size <= 8U;
}

template <typename Enum>
bool DecodeEnum(const nlohmann::json& encoded, Enum maximum, Enum& output) {
    const auto raw = encoded.get<uint32_t>();
    if (raw > static_cast<uint32_t>(maximum)) {
        return false;
    }
    output = static_cast<Enum>(raw);
    return true;
}

} // namespace

NativeA32HostResult NativeA32HostServices::HandleFallback(
    oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
    const oot3d::recomp::a32::PackedOp& op,
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory) {
    static_cast<void>(reason);
    static_cast<void>(pc);
    static_cast<void>(op);
    static_cast<void>(state);
    static_cast<void>(memory);
    return {NativeA32HostAction::Fault, std::nullopt, 0,
            "unhandled native A32 fallback"};
}

bool NativeA32HostServices::CompleteSynchronousWait(
    uint32_t immediate, const NativeA32HostResult& waitResult,
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    NativeA32HostContext& context, std::string* error) {
    static_cast<void>(immediate);
    static_cast<void>(waitResult);
    static_cast<void>(state);
    static_cast<void>(memory);
    static_cast<void>(context);
    static_cast<void>(error);
    return false;
}

NativeA32Process::NativeA32Process(
    const oot3d::recomp::a32::Registry& registry,
    NativeA32HostServices& hostServices)
    : mRegistry(registry), mHostServices(hostServices) {
    mThreadProfile.ProcessorIds.fill(kNativeA32UnknownProcessor);
    // fastmem window (config/fastmem_window): this image's regions, the
    // primary stack and TLS pages, the ones a savestate or CreatePrimaryThread
    // stages, all end up at window base + guest address (memory.cpp re-homes
    // them in its assignments). The newest process is the owner.
    if (GuestWindow::Accepting()) GuestWindow::Claim(&mMemory);
}

bool NativeA32Process::MapRegion(const NativeA32MemoryRegionConfig& config,
                                 std::string* error) {
    if (mPrimaryThreadStatus != NativeA32ThreadStatus::Empty) {
        SetError(error, "guest regions cannot change after thread creation");
        return false;
    }
    if (!mMemory.MapRegion(config, error)) {
        return false;
    }
    mRegionNames.push_back({config.Name, config.BaseAddress,
                            static_cast<uint32_t>(config.Size)});
    return true;
}

bool NativeA32Process::CreatePrimaryThread(
    const NativeA32PrimaryThreadConfig& config, std::string* error) {
    if (mPrimaryThreadStatus != NativeA32ThreadStatus::Empty) {
        SetError(error, "native A32 primary thread already exists");
        return false;
    }
    if (config.EntryAddress == 0 ||
        !mMemory.IsExecutable(config.EntryAddress, 1)) {
        SetError(error, "native A32 entrypoint is not executable");
        return false;
    }
    if (!RangeFits(config.StackBaseAddress, config.StackSize) ||
        !RangeFits(config.TlsBaseAddress, config.TlsSize)) {
        SetError(error, "native A32 stack or TLS range is invalid");
        return false;
    }

    const uint64_t stackEnd =
        static_cast<uint64_t>(config.StackBaseAddress) + config.StackSize;
    const uint64_t tlsEnd =
        static_cast<uint64_t>(config.TlsBaseAddress) + config.TlsSize;
    if (static_cast<uint64_t>(config.StackBaseAddress) < tlsEnd &&
        static_cast<uint64_t>(config.TlsBaseAddress) < stackEnd) {
        SetError(error, "native A32 stack and TLS ranges overlap");
        return false;
    }
    if (stackEnd > std::numeric_limits<uint32_t>::max()) {
        SetError(error, "native A32 stack top is not representable");
        return false;
    }

    NativeA32Memory stagedMemory = mMemory;
    std::string mapError;
    if (!stagedMemory.MapRegion({"primary_stack", config.StackBaseAddress,
                                 config.StackSize, true, false, {}},
                                &mapError) ||
        !stagedMemory.MapRegion({"primary_tls", config.TlsBaseAddress,
                                 config.TlsSize, true, false, {}},
                                &mapError)) {
        if (error != nullptr) {
            *error = mapError;
        }
        return false;
    }
    mMemory = std::move(stagedMemory);
    mRegionNames.push_back({"primary_stack", config.StackBaseAddress,
                            static_cast<uint32_t>(config.StackSize)});
    mRegionNames.push_back({"primary_tls", config.TlsBaseAddress,
                            static_cast<uint32_t>(config.TlsSize)});

    mPrimaryThreadState = {};
    mPrimaryThreadState.r[0] = config.Argument0;
    mPrimaryThreadState.r[13] =
        static_cast<uint32_t>(stackEnd) & ~7U;
    mPrimaryThreadState.r[15] = config.EntryAddress;
    mPrimaryThreadState.cpsr = config.InitialCpsr;
    mPrimaryThreadState.fpscr = config.InitialFpscr;
    mPrimaryThreadState.thread_pointer =
        config.ThreadPointer != 0 ? config.ThreadPointer
                                  : config.TlsBaseAddress;
    mPrimaryThreadPriority = config.Priority;
    mPrimaryThreadEntryAddress = config.EntryAddress;
    mPrimaryThreadArgument = config.Argument0;
    mTlsBaseAddress = config.TlsBaseAddress;
    mTlsSize = config.TlsSize;
    // The main thread runs on the exheader's ideal processor, which is what
    // processor id -2 selects for CreateThread.
    mThreadProfile.ProcessorIds[0] = -2;
    mNextTlsOffset = 0x200U;
    mPrimaryThreadStatus = NativeA32ThreadStatus::Ready;
    return true;
}

bool NativeA32Process::ResumePrimaryThread(
    std::optional<uint32_t> resumePc, std::string* error) {
    return ResumeThread(0, resumePc, error);
}

bool NativeA32Process::ResumeThread(
    uint32_t threadId, std::optional<uint32_t> resumePc,
    std::string* error) {
    auto* state = ThreadState(threadId);
    NativeA32ThreadStatus* status = nullptr;
    if (threadId == 0) {
        status = &mPrimaryThreadStatus;
    } else {
        const size_t index = static_cast<size_t>(threadId - 1U);
        if (index < mSecondaryThreads.size()) {
            status = &mSecondaryThreads[index].Status;
        }
    }
    if (state == nullptr || status == nullptr) {
        SetError(error, "native A32 thread does not exist");
        return false;
    }
    if (*status != NativeA32ThreadStatus::Waiting) {
        SetError(error, "native A32 thread is not waiting");
        return false;
    }
    if (resumePc.has_value()) {
        if (!mMemory.IsExecutable(*resumePc, 1)) {
            SetError(error, "native A32 resume PC is not executable");
            return false;
        }
        state->r[15] = *resumePc;
    }
    *status = NativeA32ThreadStatus::Ready;
    // A signal, arbitration, timer or interrupt from either core (kernel
    // lock held) wakes the second core's host thread, or a game thread that
    // waits on the kernel condition for one of its own threads.
    if (SecondCore::Active()) SecondCore::NotifyKernel();
    return true;
}

std::optional<uint32_t> NativeA32Process::CreateThread(
    const NativeA32ThreadConfig& config, std::string* error) {
    if (mPrimaryThreadStatus == NativeA32ThreadStatus::Empty) {
        SetError(error, "native A32 primary thread does not exist");
        return std::nullopt;
    }
    if (config.EntryAddress == 0 ||
        !mMemory.IsExecutable(config.EntryAddress, 1)) {
        SetError(error, "native A32 thread entrypoint is not executable");
        return std::nullopt;
    }
    if (config.StackTopAddress < 8U ||
        !mMemory.IsWritable(config.StackTopAddress - 8U, 8U)) {
        SetError(error, "native A32 thread stack top is not writable");
        return std::nullopt;
    }
    constexpr size_t kTlsEntrySize = 0x200U;
    if (mNextTlsOffset > mTlsSize ||
        kTlsEntrySize > mTlsSize - mNextTlsOffset) {
        // CTR allocates another page when its eight TLS slots are exhausted.
        // Keep old slots at stable addresses, including restored checkpoints.
        constexpr size_t kTlsPageSize = 0x1000U;
        const uint64_t nextPage = uint64_t(mTlsBaseAddress) + mTlsSize;
        if (mNextTlsOffset != mTlsSize || mTlsSize % kTlsPageSize != 0 ||
            nextPage + kTlsPageSize > uint64_t(UINT32_MAX) + 1 ||
            !mMemory.MapRegion({"thread_tls", static_cast<uint32_t>(nextPage),
                                kTlsPageSize, true, false, {}}, error)) {
            SetError(error, "native A32 process cannot allocate another TLS page");
            return std::nullopt;
        }
        mRegionNames.push_back({"thread_tls", static_cast<uint32_t>(nextPage),
                                static_cast<uint32_t>(kTlsPageSize)});
        mTlsSize += kTlsPageSize;
    }
    const uint32_t threadPointer =
        mTlsBaseAddress + static_cast<uint32_t>(mNextTlsOffset);
    if (!mMemory.Fill(threadPointer, kTlsEntrySize, 0)) {
        SetError(error, "native A32 TLS entry is not writable");
        return std::nullopt;
    }

    SecondaryThread thread;
    thread.Id = static_cast<uint32_t>(mSecondaryThreads.size()) + 1U;
    thread.Priority = config.Priority;
    thread.EntryAddress = config.EntryAddress;
    thread.Argument = config.Argument;
    thread.State.r[0] = config.Argument;
    thread.State.r[13] = config.StackTopAddress & ~7U;
    thread.State.r[15] = config.EntryAddress;
    thread.State.cpsr = config.InitialCpsr;
    thread.State.fpscr = config.InitialFpscr;
    thread.State.thread_pointer = threadPointer;
    thread.Status = NativeA32ThreadStatus::Ready;
    thread.ProcessorId = config.ProcessorId;
    thread.HostCore = HostCoreFor(config.ProcessorId);
    mNextTlsOffset += kTlsEntrySize;
    const uint32_t id = thread.Id;
    const bool secondCore = thread.HostCore != 0U;
    mSecondaryThreads.push_back(std::move(thread));
    mThreadProfile.ProcessorIds[ThreadProfileSlot(id)] = config.ProcessorId;
    if (secondCore) SecondCore::NotifyKernel();
    return id;
}

oot3d::recomp::a32::GuestState& NativeA32Process::CurrentThreadState() {
    if (mCurrentThreadId == 0) {
        return mPrimaryThreadState;
    }
    return mSecondaryThreads.at(mCurrentThreadId - 1U).State;
}

NativeA32ThreadStatus& NativeA32Process::CurrentThreadStatus() {
    if (mCurrentThreadId == 0) {
        return mPrimaryThreadStatus;
    }
    return mSecondaryThreads.at(mCurrentThreadId - 1U).Status;
}

// The cooperative scheduler's view: second-core threads (HostCore 1, only
// with the second core enabled) are never its business.
bool NativeA32Process::HasReadyThread() const {
    if (mPrimaryThreadStatus == NativeA32ThreadStatus::Ready) {
        return true;
    }
    for (const auto& thread : mSecondaryThreads) {
        if (thread.Status == NativeA32ThreadStatus::Ready &&
            thread.HostCore == 0U) {
            return true;
        }
    }
    return false;
}

bool NativeA32Process::OnlyBackgroundThreadsReady() const {
    if (mPrimaryThreadStatus != NativeA32ThreadStatus::Waiting) return false;
    for (const auto& thread : mSecondaryThreads) {
        if ((thread.Status == NativeA32ThreadStatus::Ready ||
             thread.Status == NativeA32ThreadStatus::Running) &&
            thread.HostCore == 0U &&
            thread.Priority <= mPrimaryThreadPriority) return false;
    }
    return true;
}

uint32_t NativeA32Process::DispatchBlockBudget(uint32_t requested) const {
    // Polling workers must not consume a foreground-sized quantum while all
    // foreground work waits on host events. Preserve their PC and resume them
    // after event delivery; this changes no guest clocks or thread priorities.
    constexpr uint32_t backgroundQuantum = 10'000U;
    return OnlyBackgroundThreadsReady() ? std::min(requested, backgroundQuantum)
                                       : requested;
}

bool NativeA32Process::SelectReadyThread() {
    if (!HasReadyThread()) {
        return false;
    }
    uint32_t selectedId = 0;
    uint32_t selectedPriority = std::numeric_limits<uint32_t>::max();
    if (mPrimaryThreadStatus == NativeA32ThreadStatus::Ready) {
        selectedPriority = mPrimaryThreadPriority;
    }
    for (const auto& thread : mSecondaryThreads) {
        if (thread.Status == NativeA32ThreadStatus::Ready &&
            thread.HostCore == 0U && thread.Priority < selectedPriority) {
            selectedId = thread.Id;
            selectedPriority = thread.Priority;
        }
    }
    if (mThreadProfileRunning && selectedId != mCurrentThreadId) {
        ChargeThreadProfile(mCurrentThreadId, ReadThreadProfileCounter());
        ++mThreadProfile.Switches;
    }
    // The kernel clears the exclusive monitor on a context switch (the global
    // monitor serves the single-core path too).
    if (selectedId != mCurrentThreadId) {
        SecondCore::ClearExclusive();
    }
    mCurrentThreadId = selectedId;
    CurrentThreadStatus() = NativeA32ThreadStatus::Running;
    return true;
}

bool NativeA32Process::AllThreadsTerminated() const {
    if (mPrimaryThreadStatus != NativeA32ThreadStatus::Terminated) {
        return false;
    }
    for (const auto& thread : mSecondaryThreads) {
        if (thread.Status != NativeA32ThreadStatus::Terminated) {
            return false;
        }
    }
    return true;
}

oot3d::recomp::a32::ExecutionResult NativeA32Process::DispatchFallback(
    oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
    const oot3d::recomp::a32::PackedOp& op,
    oot3d::recomp::a32::GuestState& state,
    oot3d::recomp::a32::MemoryBus& memory, void* user) {
    auto* process = static_cast<NativeA32Process*>(user);
    if (process == nullptr) {
        return {oot3d::recomp::a32::ExitKind::Fallback, pc, reason, op.raw};
    }
    return process->HandleDispatchFallback(
        process->mPendingFallback, reason, pc, op, state,
        static_cast<NativeA32Memory&>(memory));
}

oot3d::recomp::a32::ExecutionResult NativeA32Process::DispatchSecondCoreFallback(
    oot3d::recomp::a32::FallbackReason reason, uint32_t pc,
    const oot3d::recomp::a32::PackedOp& op,
    oot3d::recomp::a32::GuestState& state,
    oot3d::recomp::a32::MemoryBus& memory, void* user) {
    auto* process = static_cast<NativeA32Process*>(user);
    return process->HandleDispatchFallback(
        process->mSecondCorePendingFallback, reason, pc, op, state,
        static_cast<NativeA32Memory&>(memory));
}

oot3d::recomp::a32::ExecutionResult NativeA32Process::HandleDispatchFallback(
    PendingFallback& pending, oot3d::recomp::a32::FallbackReason reason,
    uint32_t pc, const oot3d::recomp::a32::PackedOp& op,
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& nativeMemory) {
    // Reached from inside a dispatch, where the kernel lock is released.
    const SecondCore::KernelGuard kernel;
    const auto hostStart = mTimingEnabled
                               ? TimingClock::now()
                               : TimingClock::time_point{};
    NativeA32HostResult hostResult = mHostServices.HandleFallback(
        reason, pc, op, state, nativeMemory);
    if (mTimingEnabled) {
        ++mTimingStats.FallbackCalls;
        mTimingStats.FallbackNanoseconds +=
            ElapsedNanoseconds(hostStart);
    }
    if (hostResult.Action == NativeA32HostAction::Resume) {
        if (reason == oot3d::recomp::a32::FallbackReason::MissingBlock &&
            !hostResult.ResumePc.has_value()) {
            hostResult.Action = NativeA32HostAction::Fault;
            hostResult.Error = "missing-block fallback supplied no resume PC";
        } else {
            const uint32_t resumePc =
                hostResult.ResumePc.value_or(pc + 4U);
            return {oot3d::recomp::a32::ExitKind::Fallthrough, resumePc,
                    oot3d::recomp::a32::FallbackReason::None,
                    hostResult.Detail};
        }
    }

    pending.Active = true;
    pending.Result = std::move(hostResult);
    pending.Exit = {
        oot3d::recomp::a32::ExitKind::Wait, pc, reason,
        kFallbackBoundaryDetail};
    return pending.Exit;
}

namespace {

const char* ThreadStatusName(NativeA32ThreadStatus status) {
    switch (status) {
    case NativeA32ThreadStatus::Empty: return "empty";
    case NativeA32ThreadStatus::Ready: return "ready";
    case NativeA32ThreadStatus::Running: return "running";
    case NativeA32ThreadStatus::Waiting: return "waiting";
    case NativeA32ThreadStatus::Terminated: return "terminated";
    case NativeA32ThreadStatus::Faulted: return "faulted";
    }
    return "?";
}

} // namespace

NativeA32ProcessRunResult NativeA32Process::Fault(
    const oot3d::recomp::a32::ExecutionResult& exit,
    uint32_t hostTransitions, std::string error) {
    return FaultThread(mCurrentThreadId, exit, hostTransitions, std::move(error));
}

NativeA32ProcessRunResult NativeA32Process::FaultThread(
    uint32_t threadId, const oot3d::recomp::a32::ExecutionResult& exit,
    uint32_t hostTransitions, std::string error) {
    // The other core's registers are read below: stop it at its next dispatch
    // boundary first (nothing while the second core is off).
    const SecondCore::WorldStop stop;
    if (threadId == 0U) {
        mPrimaryThreadStatus = NativeA32ThreadStatus::Faulted;
    } else if (threadId - 1U < mSecondaryThreads.size()) {
        mSecondaryThreads[threadId - 1U].Status = NativeA32ThreadStatus::Faulted;
    }
    NativeA32ProcessRunResult result{NativeA32ProcessRunKind::Faulted, exit,
                                     hostTransitions, std::move(error), {}};
    // Only here, on the way out: registers, backtrace and memory for the log.
    GuestFaultFacts facts;
    facts.ThreadId = threadId;
    facts.EntryAddress = ThreadEntryAddress(threadId);
    if (const auto* state = ThreadState(threadId)) facts.State = *state;
    facts.Exit = exit;
    for (uint32_t id = 0; id < ThreadCount(); ++id) {
        const auto* thread = ThreadState(id);
        if (id == threadId || thread == nullptr) continue;
        facts.OtherThreads.push_back({id, ThreadEntryAddress(id), thread->r[15],
                                      thread->r[14], thread->r[13],
                                      ThreadStatusName(ThreadStatus(id))});
    }
    for (const auto& name : mRegionNames) {
        facts.NamedRegions.push_back({name.Name, name.BaseAddress, name.Size});
    }
    result.GuestReport = FormatGuestFaultReport(mMemory, facts);
    return result;
}

NativeA32ProcessRunResult NativeA32Process::ApplyHostResult(
    NativeA32HostResult hostResult,
    const oot3d::recomp::a32::ExecutionResult& exit,
    uint32_t defaultResumePc, uint32_t hostTransitions) {
    const uint32_t resumePc = hostResult.ResumePc.value_or(defaultResumePc);
    auto& state = CurrentThreadState();
    auto& status = CurrentThreadStatus();
    switch (hostResult.Action) {
    case NativeA32HostAction::Resume:
        state.r[15] = resumePc;
        status = NativeA32ThreadStatus::Ready;
        return {NativeA32ProcessRunKind::Yielded, exit, hostTransitions, {}};
    case NativeA32HostAction::Wait:
        state.r[15] = resumePc;
        status = NativeA32ThreadStatus::Waiting;
        NoteCore0ThreadParked();
        return {NativeA32ProcessRunKind::Waiting, exit, hostTransitions, {}};
    case NativeA32HostAction::Terminate:
        state.r[15] = resumePc;
        status = NativeA32ThreadStatus::Terminated;
        NoteCore0ThreadParked();
        return {NativeA32ProcessRunKind::Terminated, exit, hostTransitions, {}};
    case NativeA32HostAction::Fault:
        return Fault(exit, hostTransitions,
                     hostResult.Error.empty()
                         ? "native A32 host service failed"
                         : std::move(hostResult.Error));
    }
    return Fault(exit, hostTransitions, "invalid native A32 host action");
}

NativeA32ProcessRunResult NativeA32Process::Run(
    uint32_t blockLimitPerDispatch, uint32_t maxHostTransitions) {
    CpuPhaseProbe::Scope cpuRun(CpuPhaseProbe::Phase::GuestScheduler);
    // The game thread already holds the kernel lock while the second core is
    // on (recursive); it is released only around the guest code below.
    const SecondCore::KernelGuard kernel;
    const bool concurrent = SecondCore::Active();
    if (concurrent && mSecondCoreFault.has_value()) {
        return *mSecondCoreFault;
    }
    if (mTimingEnabled) {
        ++mTimingStats.ProcessRunCalls;
    }
    if (mPrimaryThreadStatus == NativeA32ThreadStatus::Empty) {
        return Fault({}, 0, "native A32 primary thread does not exist");
    }
    if (mPrimaryThreadStatus == NativeA32ThreadStatus::Faulted) {
        return {NativeA32ProcessRunKind::Faulted, {}, 0,
                "native A32 primary thread is faulted"};
    }
    if (blockLimitPerDispatch == 0 || maxHostTransitions == 0) {
        return Fault({}, 0, "native A32 run limits must be nonzero");
    }
    if (AllThreadsTerminated()) {
        return {NativeA32ProcessRunKind::Terminated, {}, 0, {}};
    }
    if (!SelectReadyThread()) {
        return {NativeA32ProcessRunKind::Waiting, {}, 0, {}};
    }
    // Every return below charges the running thread up to here.
    BeginThreadProfile();
    struct ThreadProfileEnd {
        NativeA32Process& Process;
        ~ThreadProfileEnd() { Process.EndThreadProfile(); }
    } threadProfileEnd{*this};
    for (uint32_t transitions = 0; transitions < maxHostTransitions;
         ++transitions) {
        mPendingFallback = {};
        auto& state = CurrentThreadState();
        const uint32_t dispatchBudget = DispatchBlockBudget(blockLimitPerDispatch);
        const bool timed = mTimingEnabled;
        const auto dispatchStart = timed ? TimingClock::now()
                                         : TimingClock::time_point{};
        CpuPhaseProbe::Scope cpuDispatch(CpuPhaseProbe::Phase::Aot);
        if (concurrent) SecondCore::EnterGuest();
        oot3d::recomp::a32::ExecutionResult exit;
        {
            const SecondCore::KernelRelease release;
            exit = DispatchGuest(state, dispatchBudget, false);
        }
        if (concurrent) SecondCore::LeaveGuest();
        cpuDispatch.Stop();
        if (timed) {
            const uint64_t nanoseconds = ElapsedNanoseconds(dispatchStart);
            ++mTimingStats.DispatchCalls;
            mTimingStats.DispatchNanoseconds += nanoseconds;
        }

        if (mPendingFallback.Active) {
            const uint32_t defaultResumePc =
                mPendingFallback.Result.ResumePc.value_or(exit.pc);
            auto result = ApplyHostResult(std::move(mPendingFallback.Result),
                                          exit, defaultResumePc,
                                          transitions + 1U);
            if (result.Kind != NativeA32ProcessRunKind::Yielded) {
                if ((result.Kind == NativeA32ProcessRunKind::Waiting ||
                     result.Kind == NativeA32ProcessRunKind::Terminated) &&
                    SelectReadyThread()) {
                    continue;
                }
                result.Kind = ResolveStoppedThreadRunKind(
                    result.Kind, AllThreadsTerminated());
                return result;
            }
            SelectReadyThread();
            continue;
        }

        if (exit.kind == oot3d::recomp::a32::ExitKind::Svc) {
            NativeA32HostContext context{*this, mCurrentThreadId};
            const auto svcStart = timed ? TimingClock::now()
                                        : TimingClock::time_point{};
            CpuPhaseProbe::Scope cpuSvc(CpuPhaseProbe::Phase::Svc);
            auto hostResult =
                mHostServices.HandleSvc(exit.detail, state, mMemory, context);
            cpuSvc.Stop();
            if (mTimingEnabled) {
                const uint64_t nanoseconds = ElapsedNanoseconds(svcStart);
                ++mTimingStats.SvcCalls;
                mTimingStats.SvcNanoseconds += nanoseconds;
                if (exit.detail < mTimingStats.SvcCallsByImmediate.size()) {
                    ++mTimingStats.SvcCallsByImmediate[exit.detail];
                    mTimingStats.SvcNanosecondsByImmediate[exit.detail] +=
                        nanoseconds;
                }
            }
            auto result = ApplyHostResult(
                std::move(hostResult), exit, exit.pc + 4U,
                transitions + 1U);
            if (result.Kind != NativeA32ProcessRunKind::Yielded) {
                if ((result.Kind == NativeA32ProcessRunKind::Waiting ||
                     result.Kind == NativeA32ProcessRunKind::Terminated) &&
                    SelectReadyThread()) {
                    continue;
                }
                result.Kind = ResolveStoppedThreadRunKind(
                    result.Kind, AllThreadsTerminated());
                return result;
            }
            SelectReadyThread();
            continue;
        }
        if (exit.kind == oot3d::recomp::a32::ExitKind::BlockLimit) {
            CurrentThreadStatus() = NativeA32ThreadStatus::Ready;
            return {NativeA32ProcessRunKind::Yielded, exit, transitions, {}};
        }
        if (exit.kind == oot3d::recomp::a32::ExitKind::Wait) {
            CurrentThreadStatus() = NativeA32ThreadStatus::Waiting;
            NoteCore0ThreadParked();
            if (SelectReadyThread()) {
                continue;
            }
            return {NativeA32ProcessRunKind::Waiting, exit, transitions, {}};
        }
        return Fault(
            exit, transitions,
            "native A32 dispatch exited unexpectedly: kind=" +
                std::to_string(static_cast<uint32_t>(exit.kind)) +
                " pc=0x" + [&] {
                    char encoded[9]{};
                    std::snprintf(encoded, sizeof(encoded), "%08X", exit.pc);
                    return std::string(encoded);
                }() +
                " detail=" + std::to_string(exit.detail));
    }

    CurrentThreadStatus() = NativeA32ThreadStatus::Ready;
    return {NativeA32ProcessRunKind::Yielded,
            {oot3d::recomp::a32::ExitKind::BlockLimit,
             CurrentThreadState().r[15],
             oot3d::recomp::a32::FallbackReason::None,
             maxHostTransitions},
            maxHostTransitions, {}};
}

bool NativeA32Process::InvokeFunction(
    uint32_t entryAddress, std::span<const uint32_t> arguments,
    uint32_t returnAddress, std::string* error,
    uint32_t blockLimitPerDispatch, uint32_t maxHostTransitions) {
    return InvokeFunctionWithResult(
        entryAddress, arguments, returnAddress, nullptr, error,
        blockLimitPerDispatch, maxHostTransitions);
}

bool NativeA32Process::InvokeFunctionWithResult(
    uint32_t entryAddress, std::span<const uint32_t> arguments,
    uint32_t returnAddress, uint32_t* returnValue, std::string* error,
    uint32_t blockLimitPerDispatch, uint32_t maxHostTransitions) {
    if (arguments.size() > 4U) {
        SetError(error, "native A32 function accepts at most four arguments");
        return false;
    }
    auto state = mPrimaryThreadState;
    for (size_t index = 0; index < arguments.size(); ++index) {
        state.r[index] = arguments[index];
    }
    if (!InvokeFunctionWithState(
            entryAddress, state, returnAddress, error,
            blockLimitPerDispatch, maxHostTransitions)) {
        return false;
    }
    if (returnValue != nullptr) {
        *returnValue = state.r[0];
    }
    return true;
}

bool NativeA32Process::InvokeFunctionWithState(
    uint32_t entryAddress, oot3d::recomp::a32::GuestState& state,
    uint32_t returnAddress, std::string* error,
    uint32_t blockLimitPerDispatch, uint32_t maxHostTransitions) {
    CpuPhaseProbe::Scope cpuInvoke(CpuPhaseProbe::Phase::GuestScheduler);
    // Host-driven guest code (TopScreen routing and the like): it keeps the
    // kernel lock for its whole run, so the second core sees it as host work.
    const SecondCore::KernelGuard kernel;
    if (!mMemory.IsExecutable(entryAddress, 1U)) {
        SetError(error, "native A32 function entry is not executable");
        return false;
    }
    if (blockLimitPerDispatch == 0U || maxHostTransitions == 0U) {
        SetError(error, "native A32 function limits must be nonzero");
        return false;
    }

    state.r[14] = returnAddress;
    state.r[15] = entryAddress;

    SynchronousInvokeCallbackContext invokeCallback{
        returnAddress,
        mNativeFunctionCallback,
        mNativeFunctionUser,
    };
    auto invokeCallbackPcs = mNativeFunctionPcs;
    if (mNativeFunctionCallback == nullptr ||
        !mNativeFunctionPcs.empty()) {
        invokeCallbackPcs.push_back(returnAddress);
        std::sort(invokeCallbackPcs.begin(), invokeCallbackPcs.end());
        invokeCallbackPcs.erase(
            std::unique(
                invokeCallbackPcs.begin(), invokeCallbackPcs.end()),
            invokeCallbackPcs.end());
    }

    for (uint32_t transition = 0U; transition < maxHostTransitions;
         ++transition) {
        CpuPhaseProbe::Scope cpuDispatch(CpuPhaseProbe::Phase::Aot);
        const auto exit =
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
            DispatchWholeAotProduct(
                state, mMemory, blockLimitPerDispatch,
                mBlockEntryCallback, mBlockEntryUser,
                mBlockEntryFilterPcs.data(), mBlockEntryFilterPcs.size(),
                returnAddress, mNativeBlockCallback, mNativeBlockUser);
#else
            oot3d::recomp::a32::Dispatch(
            mRegistry, state.r[15], state, mMemory, nullptr, nullptr,
            blockLimitPerDispatch, mBlockEntryCallback, mBlockEntryUser,
            mBlockEntryFilterPcs.data(), mBlockEntryFilterPcs.size(),
            &StopSynchronousInvokeAtReturn, &invokeCallback,
            invokeCallbackPcs.data(), invokeCallbackPcs.size(),
            mNativeBlockCallback, mNativeBlockUser);
#endif
        cpuDispatch.Stop();
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
        if ((exit.kind == oot3d::recomp::a32::ExitKind::Branch ||
             exit.kind == oot3d::recomp::a32::ExitKind::Fallthrough) &&
            exit.pc == returnAddress) {
            return true;
        }
#else
        if (exit.kind == oot3d::recomp::a32::ExitKind::MissingBlock &&
            exit.pc == returnAddress) {
            return true;
        }
#endif
        if (exit.kind == oot3d::recomp::a32::ExitKind::Svc) {
            NativeA32HostContext context{*this, mCurrentThreadId};
            CpuPhaseProbe::Scope cpuSvc(CpuPhaseProbe::Phase::Svc);
            auto hostResult =
                mHostServices.HandleSvc(exit.detail, state, mMemory, context);
            cpuSvc.Stop();
            if (hostResult.Action == NativeA32HostAction::Resume) {
                state.r[15] = hostResult.ResumePc.value_or(exit.pc + 4U);
                continue;
            }
            if (hostResult.Action == NativeA32HostAction::Wait) {
                std::string completionError;
                if (mHostServices.CompleteSynchronousWait(
                        exit.detail, hostResult, state, mMemory, context,
                        &completionError)) {
                    state.r[15] =
                        hostResult.ResumePc.value_or(exit.pc + 4U);
                    continue;
                }
                if (hostResult.Error.empty() &&
                    !completionError.empty()) {
                    hostResult.Error = std::move(completionError);
                }
            }
            if (error != nullptr) {
                *error = "entry=" + std::to_string(entryAddress) +
                         " svc=" + std::to_string(exit.detail) +
                         " host_action=" + std::to_string(
                             static_cast<uint32_t>(hostResult.Action)) +
                         (hostResult.Error.empty()
                              ? std::string{}
                              : " host_error=" + hostResult.Error);
            }
            return false;
        }
        if (exit.kind == oot3d::recomp::a32::ExitKind::BlockLimit) {
            state.r[15] = exit.pc;
            continue;
        }
        if (error != nullptr) {
            *error = "entry=" + std::to_string(entryAddress) +
                     " exit_kind=" +
                     std::to_string(static_cast<uint32_t>(exit.kind)) +
                     " exit_pc=" + std::to_string(exit.pc) +
                     " detail=" + std::to_string(exit.detail);
        }
        return false;
    }
    SetError(error, "native A32 function exceeded host-transition limit");
    return false;
}

bool NativeA32Process::InvokeSvcWithState(
    uint32_t immediate, oot3d::recomp::a32::GuestState& state,
    std::string* error) {
    const SecondCore::KernelGuard kernel;
    NativeA32HostContext context{*this, mCurrentThreadId};
    const auto svcStart =
        mTimingEnabled ? TimingClock::now() : TimingClock::time_point{};
    auto hostResult =
        mHostServices.HandleSvc(immediate, state, mMemory, context);
    if (mTimingEnabled) {
        ++mTimingStats.SvcCalls;
        const uint64_t nanoseconds = ElapsedNanoseconds(svcStart);
        mTimingStats.SvcNanoseconds += nanoseconds;
        if (immediate < mTimingStats.SvcCallsByImmediate.size()) {
            ++mTimingStats.SvcCallsByImmediate[immediate];
            mTimingStats.SvcNanosecondsByImmediate[immediate] += nanoseconds;
        }
    }
    if (hostResult.Action != NativeA32HostAction::Resume) {
        if (error != nullptr) {
            *error = "svc=" + std::to_string(immediate) +
                     " host_action=" +
                     std::to_string(
                         static_cast<uint32_t>(hostResult.Action)) +
                     (hostResult.Error.empty()
                          ? std::string{}
                          : " host_error=" + hostResult.Error);
        }
        return false;
    }
    if (hostResult.ResumePc.has_value()) {
        state.r[15] = *hostResult.ResumePc;
    }
    return true;
}

void NativeA32Process::SetBlockEntryCallback(
    oot3d::recomp::a32::BlockEntryCallback callback, void* user,
    std::vector<uint32_t> filterPcs) {
    mBlockEntryCallback = callback;
    mBlockEntryUser = callback != nullptr ? user : nullptr;
    mBlockEntryFilterPcs = callback != nullptr ? std::move(filterPcs)
                                                : std::vector<uint32_t>{};
    std::sort(mBlockEntryFilterPcs.begin(), mBlockEntryFilterPcs.end());
    mBlockEntryFilterPcs.erase(
        std::unique(mBlockEntryFilterPcs.begin(),
                    mBlockEntryFilterPcs.end()),
        mBlockEntryFilterPcs.end());
}

void NativeA32Process::SetNativeFunctionCallback(
    oot3d::recomp::a32::NativeFunctionCallback callback, void* user,
    std::vector<uint32_t> functionPcs) {
    mNativeFunctionCallback = callback;
    mNativeFunctionUser = callback != nullptr ? user : nullptr;
    mNativeFunctionPcs = callback != nullptr ? std::move(functionPcs)
                                             : std::vector<uint32_t>{};
    std::sort(mNativeFunctionPcs.begin(), mNativeFunctionPcs.end());
    mNativeFunctionPcs.erase(
        std::unique(mNativeFunctionPcs.begin(), mNativeFunctionPcs.end()),
        mNativeFunctionPcs.end());
}

void NativeA32Process::SetNativeBlockCallback(
    oot3d::recomp::a32::NativeBlockCallback callback, void* user) {
    mNativeBlockCallback = callback;
    mNativeBlockUser = callback != nullptr ? user : nullptr;
}

void NativeA32Process::SetTimingEnabled(bool enabled) noexcept {
    mTimingEnabled = enabled;
    mTimingStats = {};
}

NativeA32ProcessTimingStats NativeA32Process::TimingStats() const noexcept {
    return mTimingStats;
}

void NativeA32Process::BeginThreadProfile() noexcept {
    const uint64_t now = ReadThreadProfileCounter();
    mThreadProfileRunStart = now;
    mThreadProfileMark = now;
    mThreadProfileRunning = true;
    // A different thread than the one the previous Run() left on.
    if (mCurrentThreadId != mThreadProfileLastThread) ++mThreadProfile.Switches;
}

void NativeA32Process::EndThreadProfile() noexcept {
    const uint64_t now = ReadThreadProfileCounter();
    ChargeThreadProfile(mCurrentThreadId, now);
    mThreadProfile.RunTicks += now - mThreadProfileRunStart;
    mThreadProfileLastThread = mCurrentThreadId;
    mThreadProfileRunning = false;
}

void NativeA32Process::ChargeThreadProfile(uint32_t threadId,
                                           uint64_t now) noexcept {
    mThreadProfile.Ticks[ThreadProfileSlot(threadId)] += now - mThreadProfileMark;
    mThreadProfileMark = now;
}

NativeA32GuestThreadProfile NativeA32Process::GuestThreadProfile() const noexcept {
    NativeA32GuestThreadProfile profile = mThreadProfile;
    for (size_t slot = 0; slot < kNativeA32GuestThreadSlots; ++slot) {
        profile.Ticks[slot] +=
            mSecondCoreThreadTicks[slot].load(std::memory_order_relaxed);
    }
    profile.SecondCoreTicks = mSecondCoreTicks.load(std::memory_order_relaxed);
    profile.SecondCoreLockTicks =
        mSecondCoreLockTicks.load(std::memory_order_relaxed);
    profile.SecondCoreIdleWorkTicks =
        mSecondCoreIdleWorkTicks.load(std::memory_order_relaxed);
    profile.SecondCoreIdleWorkUnits =
        mSecondCoreIdleWorkUnits.load(std::memory_order_relaxed);
    profile.SecondCoreCpuTicks = mSecondCoreCpuTicks.load(std::memory_order_relaxed);
    profile.SecondCoreCpuKnown = mSecondCoreCpuKnown.load(std::memory_order_relaxed);
    profile.SecondCoreLockWaitNanoseconds =
        SecondCore::gCore1LockWaitNanoseconds.load(std::memory_order_relaxed);
    return profile;
}

void NativeA32Process::ChargeSecondCore(uint32_t threadId, uint64_t busyTicks,
                                        uint64_t lockTicks) noexcept {
    mSecondCoreThreadTicks[ThreadProfileSlot(threadId)].fetch_add(
        busyTicks, std::memory_order_relaxed);
    mSecondCoreTicks.fetch_add(busyTicks, std::memory_order_relaxed);
    mSecondCoreLockTicks.fetch_add(lockTicks, std::memory_order_relaxed);
}

void NativeA32Process::EnableSecondCore() {
    mSecondCoreEnabled = true;
    mSecondCoreStopRequested = false;
    mSecondCoreFault.reset();
    for (auto& thread : mSecondaryThreads) {
        thread.HostCore = HostCoreFor(thread.ProcessorId);
    }
    SecondCore::ResetExclusiveMonitor();
    SecondCore::SetActive(true);
}

void NativeA32Process::DisableSecondCore() {
    SecondCore::SetActive(false);
    mSecondCoreEnabled = false;
    for (auto& thread : mSecondaryThreads) {
        thread.HostCore = 0U;
        if (thread.Status == NativeA32ThreadStatus::Running) {
            thread.Status = NativeA32ThreadStatus::Ready;
        }
    }
}

uint8_t NativeA32Process::ThreadHostCore(uint32_t threadId) const {
    const SecondCore::KernelGuard kernel;
    return threadId != 0U && threadId - 1U < mSecondaryThreads.size()
               ? mSecondaryThreads[threadId - 1U].HostCore
               : 0U;
}

bool NativeA32Process::SecondCoreBusy() const {
    for (const auto& thread : mSecondaryThreads) {
        if (thread.HostCore != 0U &&
            (thread.Status == NativeA32ThreadStatus::Ready ||
             thread.Status == NativeA32ThreadStatus::Running)) {
            return true;
        }
    }
    return false;
}

bool NativeA32Process::AuxiliaryWorkPending() const {
    for (const auto& thread : mSecondaryThreads) {
        const bool runnable = thread.Status == NativeA32ThreadStatus::Ready ||
                              thread.Status == NativeA32ThreadStatus::Running;
        if (runnable && (thread.HostCore != 0U ||
                         thread.Priority < mPrimaryThreadPriority)) {
            return true;
        }
    }
    return false;
}

bool NativeA32Process::WaitSecondCoreIdle(
    std::chrono::steady_clock::duration timeout) {
    if (!SecondCore::Active()) return true;
    return SecondCore::WaitKernelFor(timeout, [this] {
        return !mSecondCoreRunning || mSecondCoreFault.has_value() ||
               !SecondCoreBusy();
    });
}

void NativeA32Process::RequestSecondCoreStop() {
    const SecondCore::KernelGuard kernel;
    mSecondCoreStopRequested = true;
    SecondCore::NotifyKernel();
}

void NativeA32Process::SetSecondCoreIdleWork(NativeA32SecondCoreIdleWork work) {
    mSecondCoreIdleWork = std::move(work);
}

bool NativeA32Process::SecondCoreIdleWorkDue() const {
    return mSecondCoreIdleWork.Run && mSecondCoreIdleWork.Pending &&
           SecondCore::gStopRequests == 0U && !mSecondCoreFault.has_value() &&
           !AuxiliaryWorkPending() && mSecondCoreIdleWork.Pending();
}

void NativeA32Process::NoteCore0ThreadParked() noexcept {
    if (!SecondCore::Active() || !mSecondCoreIdleWork.Run || mCurrentThreadId == 0U ||
        mCurrentThreadId - 1U >= mSecondaryThreads.size() ||
        mSecondaryThreads[mCurrentThreadId - 1U].Priority >= mPrimaryThreadPriority) {
        return;
    }
    SecondCore::NotifyKernel();
}

NativeA32Process::SecondaryThread* NativeA32Process::SelectSecondCoreThread() {
    SecondaryThread* selected = nullptr;
    for (auto& thread : mSecondaryThreads) {
        if (thread.HostCore != 0U && thread.Status == NativeA32ThreadStatus::Ready &&
            (selected == nullptr || thread.Priority < selected->Priority)) {
            selected = &thread;
        }
    }
    return selected;
}

oot3d::recomp::a32::ExecutionResult NativeA32Process::DispatchGuest(
    oot3d::recomp::a32::GuestState& state, uint32_t blockLimit, bool secondCore) {
    // The second core runs plain guest code: the block-entry hooks, the
    // product mod and native function callbacks act on game-thread state
    // (projection policy, guest profiler, TopScreen routing) and stay on the
    // game thread.
#if defined(OOT3D_WHOLE_AOT_PRODUCT_MODE)
    if (secondCore) {
        return DispatchWholeAotProduct(state, mMemory, blockLimit, nullptr,
                                       nullptr, nullptr, 0U, std::nullopt,
                                       nullptr, nullptr);
    }
    return DispatchWholeAotProduct(
        state, mMemory, blockLimit,
        mBlockEntryCallback, mBlockEntryUser,
        mBlockEntryFilterPcs.data(), mBlockEntryFilterPcs.size(), std::nullopt,
        mNativeBlockCallback, mNativeBlockUser);
#else
    if (secondCore) {
        return oot3d::recomp::a32::Dispatch(
            mRegistry, state.r[15], state, mMemory,
            &NativeA32Process::DispatchSecondCoreFallback, this, blockLimit);
    }
    return oot3d::recomp::a32::Dispatch(
        mRegistry, state.r[15], state, mMemory,
        &NativeA32Process::DispatchFallback, this,
        blockLimit, mBlockEntryCallback, mBlockEntryUser,
        mBlockEntryFilterPcs.data(), mBlockEntryFilterPcs.size(),
        mNativeFunctionCallback, mNativeFunctionUser,
        mNativeFunctionPcs.data(), mNativeFunctionPcs.size(),
        mNativeBlockCallback, mNativeBlockUser);
#endif
}

void NativeA32Process::RunSecondCore(uint32_t blockLimitPerDispatch) {
    SecondCore::tProcessor = 1U;
    const SecondCore::KernelGuard kernel;
    mSecondCoreRunning = true;
    SecondCore::NotifyKernel();
    const uint32_t budget = std::max(blockLimitPerDispatch, 1U);
    uint32_t lastThread = 0U;
    uint64_t cpuStart = 0U;
    const bool cpuKnown = ReadThreadCpuCounter(&cpuStart);
    mSecondCoreCpuKnown.store(cpuKnown, std::memory_order_relaxed);
    for (;;) {
        SecondaryThread* thread = nullptr;
        bool idleWork = false;
        const auto ready = [&] {
            if (mSecondCoreStopRequested) return true;
            if (SecondCore::gStopRequests != 0U || mSecondCoreFault.has_value()) {
                return false;
            }
            thread = SelectSecondCoreThread();
            idleWork = thread == nullptr && SecondCoreIdleWorkDue();
            return thread != nullptr || idleWork;
        };
        if (!ready()) {
            // About to sleep: the CPU time of the busy stretch is complete.
            uint64_t cpu = 0U;
            if (cpuKnown && ReadThreadCpuCounter(&cpu)) {
                mSecondCoreCpuTicks.store(cpu - cpuStart, std::memory_order_relaxed);
            }
            SecondCore::WaitKernel(ready);
        }
        if (mSecondCoreStopRequested) break;
        if (idleWork) {
            // Every thread of this core is parked in a wait: the DSP frame
            // updates what they read without meeting them (the lock stays
            // held, so a world stop waits for the whole unit).
            const uint64_t idleStart = ReadThreadProfileCounter();
            try {
                mSecondCoreIdleWork.Run();
            } catch (const std::exception& exception) {
                mSecondCoreFault = NativeA32ProcessRunResult{
                    NativeA32ProcessRunKind::Faulted, {}, 0U,
                    std::string("second-core idle work: ") + exception.what(), {}};
            }
            mSecondCoreIdleWorkTicks.fetch_add(ReadThreadProfileCounter() - idleStart,
                                               std::memory_order_relaxed);
            mSecondCoreIdleWorkUnits.fetch_add(1U, std::memory_order_relaxed);
            continue;
        }
        if (thread->Id != lastThread) {
            // A context switch on this core clears its exclusive monitor.
            SecondCore::ClearExclusive();
            lastThread = thread->Id;
        }
        thread->Status = NativeA32ThreadStatus::Running;
        mSecondCorePendingFallback = {};
        ++SecondCore::gInGuest[1];
        const uint64_t start = ReadThreadProfileCounter();
        oot3d::recomp::a32::ExecutionResult exit{};
        uint64_t dispatched = start;
        std::string dispatchError;
        try {
            const SecondCore::KernelRelease release;
            exit = DispatchGuest(thread->State, budget, true);
            dispatched = ReadThreadProfileCounter();
        } catch (const std::exception& exception) {
            dispatchError = exception.what();
        }
        const uint64_t locked = ReadThreadProfileCounter();
        SecondCore::LeaveGuest();
        if (SecondCore::gStopRequests != 0U) {
            // The game thread stops the world (savestate, page-table
            // rebuild): the exit's SVC waits until it is over. A restore in
            // between replaced the thread records; the restored thread then
            // resumes from its saved state instead.
            const uint64_t generation = mThreadRecordGeneration;
            SecondCore::WaitKernel([this] {
                return SecondCore::gStopRequests == 0U || mSecondCoreStopRequested;
            });
            if (generation != mThreadRecordGeneration || mSecondCoreStopRequested) {
                continue;
            }
        }
        {
            // The thread record and registers stay ours until the exit is
            // complete, even if an SVC below waits for a world stop.
            const SecondCore::KernelSection section;
            try {
                if (!dispatchError.empty()) {
                    mSecondCoreFault = FaultThread(thread->Id, exit, 0U,
                                                   "second-core dispatch: " + dispatchError);
                } else {
                    CompleteSecondCoreDispatch(*thread, exit);
                }
            } catch (const std::exception& exception) {
                mSecondCoreFault = FaultThread(thread->Id, exit, 1U,
                                               std::string("second-core SVC: ") +
                                                   exception.what());
            }
        }
        const uint64_t end = ReadThreadProfileCounter();
        ChargeSecondCore(thread->Id, (dispatched - start) + (end - locked),
                         locked - dispatched);
        if (thread->Status != NativeA32ThreadStatus::Ready) {
            // Waiting, terminated or faulted: the second core may be idle.
            SecondCore::NotifyKernel();
        }
    }
    mSecondCoreRunning = false;
    SecondCore::NotifyKernel();
    SecondCore::tProcessor = 0U;
}

void NativeA32Process::CompleteSecondCoreDispatch(
    SecondaryThread& thread, const oot3d::recomp::a32::ExecutionResult& exit) {
    using oot3d::recomp::a32::ExitKind;
    if (mSecondCorePendingFallback.Active) {
        const uint32_t defaultResumePc =
            mSecondCorePendingFallback.Result.ResumePc.value_or(exit.pc);
        ApplySecondCoreHostResult(thread, std::move(mSecondCorePendingFallback.Result),
                                  exit, defaultResumePc);
        return;
    }
    switch (exit.kind) {
    case ExitKind::Svc: {
        NativeA32HostContext context{*this, thread.Id};
        auto hostResult =
            mHostServices.HandleSvc(exit.detail, thread.State, mMemory, context);
        ApplySecondCoreHostResult(thread, std::move(hostResult), exit,
                                  exit.pc + 4U);
        return;
    }
    case ExitKind::BlockLimit:
        thread.Status = NativeA32ThreadStatus::Ready;
        return;
    case ExitKind::Wait:
        thread.Status = NativeA32ThreadStatus::Waiting;
        return;
    default: {
        char encoded[96];
        std::snprintf(encoded, sizeof(encoded),
                      "native A32 second-core dispatch exited unexpectedly: "
                      "kind=%u pc=0x%08X detail=%u",
                      static_cast<unsigned>(exit.kind), static_cast<unsigned>(exit.pc),
                      static_cast<unsigned>(exit.detail));
        mSecondCoreFault = FaultThread(thread.Id, exit, 0U, encoded);
        return;
    }
    }
}

void NativeA32Process::ApplySecondCoreHostResult(
    SecondaryThread& thread, NativeA32HostResult hostResult,
    const oot3d::recomp::a32::ExecutionResult& exit, uint32_t defaultResumePc) {
    const uint32_t resumePc = hostResult.ResumePc.value_or(defaultResumePc);
    switch (hostResult.Action) {
    case NativeA32HostAction::Resume:
        thread.State.r[15] = resumePc;
        thread.Status = NativeA32ThreadStatus::Ready;
        return;
    case NativeA32HostAction::Wait:
        thread.State.r[15] = resumePc;
        thread.Status = NativeA32ThreadStatus::Waiting;
        return;
    case NativeA32HostAction::Terminate:
        thread.State.r[15] = resumePc;
        thread.Status = NativeA32ThreadStatus::Terminated;
        return;
    case NativeA32HostAction::Fault:
        break;
    }
    mSecondCoreFault = FaultThread(
        thread.Id, exit, 1U,
        hostResult.Action == NativeA32HostAction::Fault && !hostResult.Error.empty()
            ? std::move(hostResult.Error)
            : std::string("native A32 host service failed"));
}

uint64_t NativeA32ProfileTicksToNanoseconds(uint64_t ticks) noexcept {
#if defined(__SWITCH__) && defined(__aarch64__)
    constexpr uint64_t kFrequency = 19'200'000ULL; // Switch CNTFRQ
    return ticks / kFrequency * 1'000'000'000ULL +
           ticks % kFrequency * 1'000'000'000ULL / kFrequency;
#else
    return ticks;
#endif
}

std::string FormatNativeA32GuestThreadsLine(
    const NativeA32Process& process,
    const NativeA32GuestThreadProfile& windowStart, uint64_t hostFrameCount,
    uint64_t windowHostFrames) {
    const auto& now = process.GuestThreadProfile();
    const double frames =
        windowHostFrames != 0U ? static_cast<double>(windowHostFrames) : 1.0;
    const auto msPerFrame = [&](uint64_t ticks) {
        return static_cast<double>(NativeA32ProfileTicksToNanoseconds(ticks)) /
               1.0e6 / frames;
    };
    char buffer[128];
    std::string line = "guest_threads: host_frames=" + std::to_string(hostFrameCount);
    std::snprintf(buffer, sizeof(buffer), " run_ms=%.3f switches=%.1f",
                  msPerFrame(now.RunTicks - windowStart.RunTicks),
                  static_cast<double>(now.Switches - windowStart.Switches) / frames);
    line += buffer;
    for (size_t slot = 0; slot < kNativeA32GuestThreadSlots; ++slot) {
        const uint64_t ticks = now.Ticks[slot] - windowStart.Ticks[slot];
        if (ticks == 0U) continue;
        const double ms = msPerFrame(ticks);
        if (slot == kNativeA32GuestThreadSlots - 1U) {
            std::snprintf(buffer, sizeof(buffer), " other=%.3f", ms);
            line += buffer;
            continue;
        }
        const auto id = static_cast<uint32_t>(slot);
        char processor[16];
        if (now.ProcessorIds[slot] == kNativeA32UnknownProcessor) {
            std::snprintf(processor, sizeof(processor), "?");
        } else {
            std::snprintf(processor, sizeof(processor), "%d",
                          static_cast<int>(now.ProcessorIds[slot]));
        }
        std::snprintf(buffer, sizeof(buffer), " %u:0x%08X:0x%02X:%s=%.3f",
                      static_cast<unsigned>(id),
                      static_cast<unsigned>(process.ThreadEntryAddress(id).value_or(0U)),
                      static_cast<unsigned>(process.ThreadPriority(id).value_or(0U)),
                      processor, ms);
        line += buffer;
    }
    const uint64_t core1 = now.SecondCoreTicks - windowStart.SecondCoreTicks;
    if (core1 != 0U) {
        std::snprintf(buffer, sizeof(buffer), " core1_ms=%.3f core1_lock_ms=%.3f",
                      msPerFrame(core1),
                      msPerFrame(now.SecondCoreLockTicks -
                                 windowStart.SecondCoreLockTicks));
        line += buffer;
    }
    return line;
}

std::string FormatNativeA32SecondCoreLine(const NativeA32GuestThreadProfile& now,
                                          const NativeA32GuestThreadProfile& windowStart,
                                          const NativeA32SecondCoreDspWindow& dsp,
                                          uint64_t windowHostFrames) {
    const double frames =
        windowHostFrames != 0U ? static_cast<double>(windowHostFrames) : 1.0;
    const auto nsToMs = [&](uint64_t nanoseconds) {
        return static_cast<double>(nanoseconds) / 1.0e6 / frames;
    };
    const auto ns = [](uint64_t ticks) { return NativeA32ProfileTicksToNanoseconds(ticks); };
    const double waitMs = dsp.WaitSeconds * 1000.0 / frames;
    const uint64_t idleWork =
        ns(now.SecondCoreIdleWorkTicks - windowStart.SecondCoreIdleWorkTicks);
    // Wall time from each dispatch's start to the end of its exit, and of
    // the DSP frames.
    const uint64_t wall = ns(now.SecondCoreTicks - windowStart.SecondCoreTicks) +
                          ns(now.SecondCoreLockTicks - windowStart.SecondCoreLockTicks) +
                          idleWork;
    const uint64_t lockWait =
        now.SecondCoreLockWaitNanoseconds - windowStart.SecondCoreLockWaitNanoseconds;
    const uint64_t running = wall > lockWait ? wall - lockWait : 0U;
    // A window that starts before the second core's first CPU sample has
    // none to subtract.
    const bool cpuKnown = now.SecondCoreCpuKnown && windowStart.SecondCoreCpuKnown;
    const uint64_t cpu =
        cpuKnown ? ns(now.SecondCoreCpuTicks - windowStart.SecondCoreCpuTicks) : 0U;
    char preempted[24];
    if (cpuKnown) {
        std::snprintf(preempted, sizeof(preempted), "%.3f",
                      running > cpu ? nsToMs(running - cpu) : 0.0);
    } else {
        std::snprintf(preempted, sizeof(preempted), "na");
    }
    char line[384];
    std::snprintf(
        line, sizeof(line),
        "second_core: dsp_pumped=%.2f dsp_flushed=%.2f dsp_late=%llu dsp_wait_ms=%.3f "
        "dsp_core1=%.2f dsp_core1_ms=%.3f core1_lock_wait_ms=%.3f core1_preempted_ms=%s "
        "core0_saved_ms=%.3f",
        static_cast<double>(dsp.Pumped) / frames, static_cast<double>(dsp.Flushed) / frames,
        static_cast<unsigned long long>(dsp.Late), waitMs,
        static_cast<double>(now.SecondCoreIdleWorkUnits - windowStart.SecondCoreIdleWorkUnits) /
            frames,
        nsToMs(idleWork), nsToMs(lockWait), preempted,
        nsToMs(cpuKnown ? cpu : running) - waitMs);
    return line;
}

std::string FormatNativeA32GuestThreadCreated(
    uint32_t id, uint32_t entryAddress, uint32_t priority, int32_t processorId,
    uint32_t stackTopAddress) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer),
                  "guest_thread: id=%u entry=0x%08X prio=0x%02X processor=%d "
                  "stack_top=0x%08X",
                  static_cast<unsigned>(id), static_cast<unsigned>(entryAddress),
                  static_cast<unsigned>(priority), static_cast<int>(processorId),
                  static_cast<unsigned>(stackTopAddress));
    return buffer;
}

uint64_t NativeA32Process::StateFingerprint() const {
    // Registers and memory of a second core held at a dispatch boundary.
    const SecondCore::WorldStop stop;
    uint64_t hash = mMemory.StateFingerprint();
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    const auto appendGuestState = [&](
                                      const oot3d::recomp::a32::GuestState& state) {
        append(state.r.data(), state.r.size() * sizeof(state.r[0]));
        append(&state.cpsr, sizeof(state.cpsr));
        append(&state.fpscr, sizeof(state.fpscr));
        append(&state.thread_pointer, sizeof(state.thread_pointer));
        append(state.vfp.data(), state.vfp.size() * sizeof(state.vfp[0]));
        append(&state.exclusive_address, sizeof(state.exclusive_address));
        append(&state.exclusive_token, sizeof(state.exclusive_token));
        append(&state.exclusive_size, sizeof(state.exclusive_size));
        append(&state.exclusive_valid, sizeof(state.exclusive_valid));
    };
    appendGuestState(mPrimaryThreadState);
    append(&mPrimaryThreadStatus, sizeof(mPrimaryThreadStatus));
    append(&mPrimaryThreadPriority, sizeof(mPrimaryThreadPriority));
    append(&mPrimaryThreadEntryAddress, sizeof(mPrimaryThreadEntryAddress));
    append(&mPrimaryThreadArgument, sizeof(mPrimaryThreadArgument));
    append(&mCurrentThreadId, sizeof(mCurrentThreadId));
    append(&mTlsBaseAddress, sizeof(mTlsBaseAddress));
    append(&mTlsSize, sizeof(mTlsSize));
    append(&mNextTlsOffset, sizeof(mNextTlsOffset));
    for (const auto& thread : mSecondaryThreads) {
        append(&thread.Id, sizeof(thread.Id));
        append(&thread.Priority, sizeof(thread.Priority));
        append(&thread.EntryAddress, sizeof(thread.EntryAddress));
        append(&thread.Argument, sizeof(thread.Argument));
        appendGuestState(thread.State);
        append(&thread.Status, sizeof(thread.Status));
    }
    append(&mPendingFallback.Active, sizeof(mPendingFallback.Active));
    append(&mPendingFallback.Result.Action,
           sizeof(mPendingFallback.Result.Action));
    const bool hasResumePc = mPendingFallback.Result.ResumePc.has_value();
    append(&hasResumePc, sizeof(hasResumePc));
    if (hasResumePc) {
        const uint32_t resumePc = *mPendingFallback.Result.ResumePc;
        append(&resumePc, sizeof(resumePc));
    }
    append(&mPendingFallback.Result.Detail,
           sizeof(mPendingFallback.Result.Detail));
    append(mPendingFallback.Result.Error.data(),
           mPendingFallback.Result.Error.size());
    append(&mPendingFallback.Exit.kind, sizeof(mPendingFallback.Exit.kind));
    append(&mPendingFallback.Exit.pc, sizeof(mPendingFallback.Exit.pc));
    append(&mPendingFallback.Exit.fallback,
           sizeof(mPendingFallback.Exit.fallback));
    append(&mPendingFallback.Exit.detail,
           sizeof(mPendingFallback.Exit.detail));
    return hash;
}

nlohmann::json NativeA32Process::CaptureState() const {
    // The second core is held at a dispatch boundary (nothing while it is off).
    const SecondCore::WorldStop stop;
    nlohmann::json secondaryThreads = nlohmann::json::array();
    for (const auto& thread : mSecondaryThreads) {
        nlohmann::json record{
            {"id", thread.Id},
            {"priority", thread.Priority},
            {"entry_address", thread.EntryAddress},
            {"argument", thread.Argument},
            {"state", EncodeGuestState(thread.State)},
            {"status", static_cast<uint32_t>(thread.Status)},
        };
        // With the second core on, the requested processor travels with the
        // thread, so a load places it on the same core (the record is
        // unchanged with the switch absent).
        if (mSecondCoreEnabled && thread.ProcessorId != kNativeA32UnknownProcessor) {
            record["processor_id"] = thread.ProcessorId;
        }
        secondaryThreads.push_back(std::move(record));
    }
    nlohmann::json pendingFallback{
        {"active", mPendingFallback.Active},
        {"action",
         static_cast<uint32_t>(mPendingFallback.Result.Action)},
        {"resume_pc", mPendingFallback.Result.ResumePc.has_value()
                          ? nlohmann::json(*mPendingFallback.Result.ResumePc)
                          : nlohmann::json(nullptr)},
        {"detail", mPendingFallback.Result.Detail},
        {"error", mPendingFallback.Result.Error},
        {"exit_kind",
         static_cast<uint32_t>(mPendingFallback.Exit.kind)},
        {"exit_pc", mPendingFallback.Exit.pc},
        {"exit_reason",
         static_cast<uint32_t>(mPendingFallback.Exit.fallback)},
        {"exit_detail", mPendingFallback.Exit.detail},
    };
    return {
        {"format", "oot3d_native_a32_process_state_v1"},
        {"memory", mMemory.CaptureState()},
        {"primary_thread_state", EncodeGuestState(mPrimaryThreadState)},
        {"primary_thread_status",
         static_cast<uint32_t>(mPrimaryThreadStatus)},
        {"primary_thread_priority", mPrimaryThreadPriority},
        {"primary_thread_entry_address", mPrimaryThreadEntryAddress},
        {"primary_thread_argument", mPrimaryThreadArgument},
        {"current_thread_id", mCurrentThreadId},
        {"tls_base_address", mTlsBaseAddress},
        {"tls_size", mTlsSize},
        {"next_tls_offset", mNextTlsOffset},
        {"secondary_threads", std::move(secondaryThreads)},
        {"pending_fallback", std::move(pendingFallback)},
    };
}

bool NativeA32Process::RestoreState(const nlohmann::json& state,
                                    std::string* error) {
    // Thread records and the image are replaced: the second core must hold
    // no reference into them (nothing while it is off).
    const SecondCore::WorldStop stop;
    try {
        if (!state.is_object() ||
            state.value("format", std::string{}) !=
                "oot3d_native_a32_process_state_v1") {
            SetError(error, "native A32 process state format is invalid");
            return false;
        }
        NativeA32Memory stagedMemory;
        if (!stagedMemory.RestoreState(state.at("memory"), error)) {
            return false;
        }
        oot3d::recomp::a32::GuestState primaryState;
        if (!DecodeGuestState(state.at("primary_thread_state"),
                              primaryState)) {
            SetError(error, "native A32 primary thread state is invalid");
            return false;
        }
        NativeA32ThreadStatus primaryStatus;
        if (!DecodeEnum(state.at("primary_thread_status"),
                        NativeA32ThreadStatus::Faulted, primaryStatus)) {
            SetError(error, "native A32 primary thread status is invalid");
            return false;
        }
        std::deque<SecondaryThread> secondaryThreads;
        const auto& encodedThreads = state.at("secondary_threads");
        if (!encodedThreads.is_array()) {
            SetError(error, "native A32 secondary thread state is invalid");
            return false;
        }
        for (const auto& encoded : encodedThreads) {
            SecondaryThread thread;
            thread.Id = encoded.at("id").get<uint32_t>();
            thread.Priority = encoded.at("priority").get<uint32_t>();
            thread.EntryAddress =
                encoded.at("entry_address").get<uint32_t>();
            thread.Argument = encoded.at("argument").get<uint32_t>();
            if (thread.Id != secondaryThreads.size() + 1U ||
                !DecodeGuestState(encoded.at("state"), thread.State) ||
                !DecodeEnum(encoded.at("status"),
                            NativeA32ThreadStatus::Faulted,
                            thread.Status)) {
                SetError(error,
                         "native A32 secondary thread record is invalid");
                return false;
            }
            // Saved with the second core on, or else what this session's
            // CreateThread recorded for the same thread id.
            thread.ProcessorId =
                encoded.contains("processor_id")
                    ? encoded.at("processor_id").get<int32_t>()
                    : (thread.Id < kNativeA32GuestThreadSlots - 1U
                           ? mThreadProfile.ProcessorIds[thread.Id]
                           : kNativeA32UnknownProcessor);
            secondaryThreads.push_back(std::move(thread));
        }
        const uint32_t currentThreadId =
            state.at("current_thread_id").get<uint32_t>();
        if (currentThreadId > secondaryThreads.size()) {
            SetError(error, "native A32 current thread ID is invalid");
            return false;
        }
        const uint32_t tlsBaseAddress =
            state.at("tls_base_address").get<uint32_t>();
        const size_t tlsSize = state.at("tls_size").get<size_t>();
        const size_t nextTlsOffset =
            state.at("next_tls_offset").get<size_t>();
        bool tlsMapped = tlsSize == 0U || RangeFits(tlsBaseAddress, tlsSize);
        for (size_t offset = 0; tlsMapped && offset < tlsSize; offset += 0x200U)
            tlsMapped = stagedMemory.IsWritable(tlsBaseAddress + static_cast<uint32_t>(offset),
                                                std::min(size_t(0x200U), tlsSize - offset));
        if (nextTlsOffset > tlsSize || !tlsMapped) {
            SetError(error, "native A32 TLS state is invalid");
            return false;
        }

        PendingFallback pendingFallback;
        const auto& encodedFallback = state.at("pending_fallback");
        pendingFallback.Active = encodedFallback.at("active").get<bool>();
        if (!DecodeEnum(encodedFallback.at("action"),
                        NativeA32HostAction::Fault,
                        pendingFallback.Result.Action)) {
            SetError(error, "native A32 pending host action is invalid");
            return false;
        }
        if (!encodedFallback.at("resume_pc").is_null()) {
            pendingFallback.Result.ResumePc =
                encodedFallback.at("resume_pc").get<uint32_t>();
        }
        pendingFallback.Result.Detail =
            encodedFallback.at("detail").get<uint32_t>();
        pendingFallback.Result.Error =
            encodedFallback.at("error").get<std::string>();
        if (!DecodeEnum(encodedFallback.at("exit_kind"),
                        oot3d::recomp::a32::ExitKind::Unsupported,
                        pendingFallback.Exit.kind) ||
            !DecodeEnum(encodedFallback.at("exit_reason"),
                        oot3d::recomp::a32::FallbackReason::MissingBlock,
                        pendingFallback.Exit.fallback)) {
            SetError(error, "native A32 pending exit state is invalid");
            return false;
        }
        pendingFallback.Exit.pc =
            encodedFallback.at("exit_pc").get<uint32_t>();
        pendingFallback.Exit.detail =
            encodedFallback.at("exit_detail").get<uint32_t>();

        stagedMemory.PreserveAccessObserversFrom(mMemory);
        mMemory = std::move(stagedMemory);
        mPrimaryThreadState = primaryState;
        mPrimaryThreadStatus = primaryStatus;
        mPrimaryThreadPriority =
            state.at("primary_thread_priority").get<uint32_t>();
        mPrimaryThreadEntryAddress =
            state.at("primary_thread_entry_address").get<uint32_t>();
        mPrimaryThreadArgument =
            state.at("primary_thread_argument").get<uint32_t>();
        mCurrentThreadId = currentThreadId;
        mTlsBaseAddress = tlsBaseAddress;
        mTlsSize = tlsSize;
        mNextTlsOffset = nextTlsOffset;
        mSecondaryThreads = std::move(secondaryThreads);
        ++mThreadRecordGeneration;
        mPendingFallback = std::move(pendingFallback);
        mTimingStats = {};
        bool secondCoreReady = false;
        for (auto& thread : mSecondaryThreads) {
            if (thread.Id < kNativeA32GuestThreadSlots - 1U) {
                mThreadProfile.ProcessorIds[thread.Id] = thread.ProcessorId;
            }
            thread.HostCore = HostCoreFor(thread.ProcessorId);
            // A second-core thread saved inside an SVC restarts it.
            if (thread.HostCore != 0U &&
                thread.Status == NativeA32ThreadStatus::Running) {
                thread.Status = NativeA32ThreadStatus::Ready;
            }
            secondCoreReady |= thread.HostCore != 0U &&
                               thread.Status == NativeA32ThreadStatus::Ready;
        }
        // A restored thread's exclusive token (an old savestate's is a write
        // generation) never meets a live reservation: one STREX retry.
        SecondCore::ResetExclusiveMonitor();
        if (SecondCore::Active() && secondCoreReady) SecondCore::NotifyKernel();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("native A32 process state decode failed: ") +
                     exception.what();
        }
        return false;
    }
}

NativeA32Memory& NativeA32Process::Memory() {
    return mMemory;
}

const NativeA32Memory& NativeA32Process::Memory() const {
    return mMemory;
}

oot3d::recomp::a32::GuestState& NativeA32Process::PrimaryThreadState() {
    return mPrimaryThreadState;
}

const oot3d::recomp::a32::GuestState&
NativeA32Process::PrimaryThreadState() const {
    return mPrimaryThreadState;
}

NativeA32ThreadStatus NativeA32Process::PrimaryThreadStatus() const {
    return mPrimaryThreadStatus;
}

size_t NativeA32Process::ThreadCount() const {
    return mPrimaryThreadStatus == NativeA32ThreadStatus::Empty
               ? 0
               : 1U + mSecondaryThreads.size();
}

const oot3d::recomp::a32::GuestState* NativeA32Process::ThreadState(
    uint32_t threadId) const {
    if (threadId == 0) {
        return mPrimaryThreadStatus == NativeA32ThreadStatus::Empty
                   ? nullptr
                   : &mPrimaryThreadState;
    }
    const size_t index = threadId - 1U;
    return index < mSecondaryThreads.size()
               ? &mSecondaryThreads[index].State
               : nullptr;
}

oot3d::recomp::a32::GuestState* NativeA32Process::ThreadState(
    uint32_t threadId) {
    return const_cast<oot3d::recomp::a32::GuestState*>(
        std::as_const(*this).ThreadState(threadId));
}

NativeA32ThreadStatus NativeA32Process::ThreadStatus(uint32_t threadId) const {
    if (threadId == 0) {
        return mPrimaryThreadStatus;
    }
    const size_t index = threadId - 1U;
    return index < mSecondaryThreads.size()
               ? mSecondaryThreads[index].Status
               : NativeA32ThreadStatus::Empty;
}

std::optional<uint32_t> NativeA32Process::ThreadPriority(
    uint32_t threadId) const {
    if (threadId == 0) {
        return mPrimaryThreadStatus == NativeA32ThreadStatus::Empty
                   ? std::nullopt
                   : std::optional<uint32_t>(mPrimaryThreadPriority);
    }
    const size_t index = static_cast<size_t>(threadId - 1U);
    return index < mSecondaryThreads.size()
               ? std::optional<uint32_t>(mSecondaryThreads[index].Priority)
               : std::nullopt;
}

std::optional<uint32_t> NativeA32Process::ThreadEntryAddress(
    uint32_t threadId) const {
    if (threadId == 0) {
        return mPrimaryThreadStatus == NativeA32ThreadStatus::Empty
                   ? std::nullopt
                   : std::optional<uint32_t>(mPrimaryThreadEntryAddress);
    }
    const size_t index = static_cast<size_t>(threadId - 1U);
    return index < mSecondaryThreads.size()
               ? std::optional<uint32_t>(mSecondaryThreads[index].EntryAddress)
               : std::nullopt;
}

std::optional<uint32_t> NativeA32Process::ThreadArgument(
    uint32_t threadId) const {
    if (threadId == 0) {
        return mPrimaryThreadStatus == NativeA32ThreadStatus::Empty
                   ? std::nullopt
                   : std::optional<uint32_t>(mPrimaryThreadArgument);
    }
    const size_t index = static_cast<size_t>(threadId - 1U);
    return index < mSecondaryThreads.size()
               ? std::optional<uint32_t>(mSecondaryThreads[index].Argument)
               : std::nullopt;
}

} // namespace Oot3dNativeGame
