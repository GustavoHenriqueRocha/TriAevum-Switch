// Second guest core (oot3d_native_a32_second_core.h, config/second_core,
// SWITCH_PORT.md "Second guest core"): the global exclusive monitor under two
// host threads, cross-core waits, signals, arbitration and timers through the
// CTR host services, the cooperative scheduler leaving second-core threads
// alone, faults, savestates, and the switch-absent path.
//
// Guest code is interpreted ARM (Registry blocks of one instruction each).
// Default: the quick run of every test. --stress: the long two-thread runs
// (millions of exclusive pairs and event round trips; built with
// -fsanitize=thread as oot3d_native_a32_second_core_tsan_tests).
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_second_core.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace Oot3dNativeGame;
using oot3d::recomp::a32::Condition;
using oot3d::recomp::a32::EncodeMetadata;
using oot3d::recomp::a32::ExclusiveStoreResult;
using oot3d::recomp::a32::GuestState;
using oot3d::recomp::a32::Opcode;
using oot3d::recomp::a32::PackedOp;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_a32_second_core_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

struct Sizes {
    uint32_t ExclusivePairs = 200'000U;  // per host thread, memory level
    uint32_t GuestIncrements = 20'000U;  // per guest thread, process level
    uint32_t PingPongRounds = 20'000U;
    uint32_t ArbitrationItems = 20'000U;
    uint32_t DspRefreshes = 10U;  // DSP frames on the second core: 3 each
};

// --- A few ARM words ---------------------------------------------------------

constexpr uint32_t Ldr(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE5900000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t Ldrex(uint32_t rt, uint32_t rn) {
    return 0xE1900F9FU | rn << 16U | rt << 12U;
}
constexpr uint32_t Strex(uint32_t rd, uint32_t rt, uint32_t rn) {
    return 0xE1800F90U | rn << 16U | rd << 12U | rt;
}
constexpr uint32_t AddImm(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE2800000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t SubImm(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE2400000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t SubsImm(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE2500000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t CmpImm(uint32_t rn, uint32_t imm) {
    return 0xE3500000U | rn << 16U | imm;
}
constexpr uint32_t MovImm(uint32_t rd, uint32_t imm) {
    return 0xE3A00000U | rd << 12U | imm;
}
constexpr uint32_t MvnZero(uint32_t rd) {  // rd = -1
    return 0xE3E00000U | rd << 12U;
}
constexpr uint32_t Str(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE5800000U | rn << 16U | rd << 12U | imm;
}

// One instruction per block, so every address is a branch target.
class Program {
  public:
    explicit Program(uint32_t base) : mBase(base) {}
    uint32_t Here() const { return mBase + static_cast<uint32_t>(mOps.size()) * 4U; }
    void Op(uint32_t raw) {
        mOps.push_back({raw, EncodeMetadata(Opcode::Core, Condition::Al)});
    }
    void Branch(Condition condition, uint32_t target) {
        const uint32_t offset = ((target - Here() - 8U) >> 2U) & 0x00FFFFFFU;
        const uint32_t raw =
            (static_cast<uint32_t>(condition) << 28U) | 0x0A000000U | offset;
        mOps.push_back({raw, EncodeMetadata(Opcode::Branch, condition)});
    }
    void Svc(uint32_t immediate) {
        mOps.push_back({0xEF000000U | immediate,
                        EncodeMetadata(Opcode::Svc, Condition::Al)});
    }
    // The registry keeps pointers into this program: it must outlive it, and
    // the program is complete once sealed.
    oot3d::recomp::a32::Registry Seal() {
        if (mBlocks.empty()) {
            for (size_t index = 0; index < mOps.size(); ++index) {
                mBlocks.push_back(
                    {mBase + static_cast<uint32_t>(index) * 4U, &mOps[index], 1U});
            }
            mShard = {mBase, Here() - 4U, mBlocks.data(),
                      static_cast<uint32_t>(mBlocks.size())};
        }
        return {&mShard, 1U, nullptr, 0U};
    }

  private:
    uint32_t mBase;
    std::vector<PackedOp> mOps;
    std::vector<oot3d::recomp::a32::Block> mBlocks;
    oot3d::recomp::a32::BlockShard mShard{};
};

// Programs live for the whole run: the interpreter caches blocks per
// registry address (thread-local), so a registry must never be reused.
std::deque<Program>& Programs() {
    static std::deque<Program> programs;
    return programs;
}

constexpr uint32_t kText = 0x00100000U;
constexpr uint32_t kData = 0x00200000U;
constexpr uint32_t kSecondStack = 0x00200F00U;
constexpr uint32_t kPrimaryStack = 0x00300000U;
constexpr uint32_t kTls = 0x00400000U;
constexpr uint32_t kBudget = 64U;

constexpr uint32_t kSvcExitThread = 0x09U;
constexpr uint32_t kSvcSleepThread = 0x0AU;
constexpr uint32_t kSvcCreateEvent = 0x17U;
constexpr uint32_t kSvcSignalEvent = 0x18U;
constexpr uint32_t kSvcCreateAddressArbiter = 0x21U;
constexpr uint32_t kSvcArbitrateAddress = 0x22U;
constexpr uint32_t kSvcWaitSynchronizationN = 0x25U;

NativeA32CtrHostConfig MakeConfig() {
    NativeA32CtrHostConfig config;
    config.ResourceLimitValues = {
        0x18, 0x04000000, 0x20, 0x20, 0x20,
        0x08, 0x08,       0x10, 0x02, 0,
    };
    config.ResourceCurrentValues[1] = 0x1000;
    config.ResourceCurrentValues[2] = 1;
    return config;
}

// A process on the CTR host services with one text and one data page; the
// primary thread runs `primaryEntry` with r9 = kData.
struct Guest {
    explicit Guest(oot3d::recomp::a32::Registry programRegistry)
        : registry(programRegistry), process(registry, host) {}

    void Boot(uint32_t primaryEntry, uint32_t primaryArgument = kData) {
        static const std::array<uint8_t, 0x1000> text{};
        std::string error;
        Require(process.MapRegion({"text", kText, text.size(), false, true, text}, &error) &&
                    process.MapRegion({"data", kData, 0x1000, true, false, {}}, &error) &&
                    process.CreatePrimaryThread({primaryEntry, kPrimaryStack, 0x1000, kTls,
                                                 0x1000, 0, primaryArgument, 0x10U,
                                                 0x03C00000U, 0x30U},
                                                &error),
                error);
        process.PrimaryThreadState().r[9] = kData;
    }

    // An SVC from the primary thread's context, outside guest code.
    GuestState Svc(uint32_t immediate, GuestState state) {
        NativeA32HostContext context{process, 0};
        const auto result = host.HandleSvc(immediate, state, process.Memory(), context);
        Require(result.Action == NativeA32HostAction::Resume && state.r[0] == 0U,
                "host SVC failed");
        return state;
    }

    uint32_t CreateEvent() {
        GuestState state{};
        state.r[1] = 0U;  // one-shot
        return Svc(kSvcCreateEvent, state).r[1];
    }

    uint32_t CreateArbiter() { return Svc(kSvcCreateAddressArbiter, GuestState{}).r[1]; }

    // svcCreateThread; the new thread runs `entry` with r0 = argument.
    uint32_t CreateThread(uint32_t entry, int32_t processor, uint32_t argument = kData) {
        GuestState state{};
        state.r[0] = 0x18U;
        state.r[1] = entry;
        state.r[2] = argument;
        state.r[3] = kSecondStack;
        state.r[4] = static_cast<uint32_t>(processor);
        Svc(0x08U, state);
        return static_cast<uint32_t>(process.ThreadCount() - 1U);
    }

    void Write32(uint32_t address, uint32_t value) {
        Require(process.Memory().Write32(address, value), "guest data write failed");
    }
    uint32_t Read32(uint32_t address) {
        uint32_t value = 0;
        Require(process.Memory().Read32(address, &value), "guest data read failed");
        return value;
    }

    oot3d::recomp::a32::Registry registry;
    NativeA32CtrHostServices host{MakeConfig()};
    NativeA32Process process;
};

// The game thread's side of a two-core run: the calling thread holds the
// kernel lock (as the window session's game thread does) and the second
// core's host thread runs RunSecondCore.
class TwoCores {
  public:
    explicit TwoCores(NativeA32Process& process, uint32_t budget = kBudget)
        : mProcess(process) {
        mProcess.EnableSecondCore();
        SecondCore::LockKernel();
        mRunner = std::thread([this, budget] { mProcess.RunSecondCore(budget); });
    }
    ~TwoCores() { Stop(); }

    void Stop() {
        if (!mRunner.joinable()) return;
        mProcess.RequestSecondCoreStop();
        {
            const SecondCore::KernelRelease release;
            mRunner.join();
        }
        mProcess.DisableSecondCore();
        SecondCore::UnlockKernel();
    }

    // Runs the cooperative scheduler until `done` (evaluated under the
    // kernel lock); sleeps on the kernel condition while it has nothing to run.
    void RunUntil(const std::function<bool()>& done,
                  std::chrono::seconds timeout = std::chrono::seconds(1800)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            const auto result = mProcess.Run(kBudget);
            Require(result.Kind != NativeA32ProcessRunKind::Faulted,
                    "unexpected fault: " + result.Error);
            if (result.Kind != NativeA32ProcessRunKind::Yielded) {
                SecondCore::WaitKernelFor(std::chrono::milliseconds(50), [&] {
                    return done() || Core0Ready();
                });
            }
            Require(std::chrono::steady_clock::now() < deadline,
                    "two-core run did not finish in time");
        }
    }

    bool Core0Ready() const {
        for (uint32_t id = 0; id < mProcess.ThreadCount(); ++id) {
            if (mProcess.ThreadHostCore(id) == 0U &&
                mProcess.ThreadStatus(id) == NativeA32ThreadStatus::Ready) {
                return true;
            }
        }
        return false;
    }

  private:
    NativeA32Process& mProcess;
    std::thread mRunner;
};

bool Terminated(const NativeA32Process& process, uint32_t id) {
    return process.ThreadStatus(id) == NativeA32ThreadStatus::Terminated;
}

// --- The global exclusive monitor, memory level ------------------------------

struct ActiveScope {
    ActiveScope() {
        SecondCore::ResetExclusiveMonitor();
        SecondCore::SetActive(true);
    }
    ~ActiveScope() {
        SecondCore::tProcessor = 0U;
        SecondCore::SetActive(false);
    }
};

uint64_t Load(NativeA32Memory& memory, uint8_t processor, uint32_t address,
              uint8_t size = 4U) {
    SecondCore::tProcessor = processor;
    uint64_t value = 0;
    uint64_t token = 0;
    uint32_t fault = 0;
    Require(memory.LoadExclusive(address, size, &value, &token, &fault) && token == value,
            "LDREX failed");
    return token;
}

ExclusiveStoreResult Store(NativeA32Memory& memory, uint8_t processor, uint32_t address,
                           uint64_t value, uint64_t token, uint8_t size = 4U) {
    SecondCore::tProcessor = processor;
    uint32_t fault = 0;
    return memory.StoreExclusive(address, size, value, token, &fault);
}

void TestMonitorSemantics() {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"data", 0x1000, 0x2000, true, false, {}}, &error), error);
    const ActiveScope active;
    constexpr uint32_t a = 0x1000U;
    Require(memory.Write32(a, 5U), "seed");

    // An undisturbed pair succeeds.
    uint64_t token = Load(memory, 0, a);
    Require(token == 5U && Store(memory, 0, a, 6U, token) == ExclusiveStoreResult::Success,
            "undisturbed STREX must succeed");
    uint32_t value = 0;
    Require(memory.Read32(a, &value) && value == 6U, "STREX value");

    // Another core's exclusive store to the granule ends the reservation.
    token = Load(memory, 0, a);
    const uint64_t other = Load(memory, 1, a);
    Require(Store(memory, 1, a, 7U, other) == ExclusiveStoreResult::Success,
            "core 1 STREX must succeed");
    Require(Store(memory, 0, a, 100U, token) == ExclusiveStoreResult::ReservationLost,
            "core 0 STREX after core 1's must fail");
    Require(memory.Read32(a, &value) && value == 7U, "failed STREX must not write");

    // ABA through exclusive stores: the value is back, the reservation is not.
    token = Load(memory, 0, a);
    uint64_t t1 = Load(memory, 1, a);
    Require(Store(memory, 1, a, 8U, t1) == ExclusiveStoreResult::Success, "aba 1");
    t1 = Load(memory, 1, a);
    Require(Store(memory, 1, a, 7U, t1) == ExclusiveStoreResult::Success, "aba 2");
    Require(Store(memory, 0, a, 9U, token) == ExclusiveStoreResult::ReservationLost,
            "ABA through exclusive stores must fail");

    // A plain store that changed the word fails the compare-and-swap.
    token = Load(memory, 0, a);
    Require(memory.Write32(a, 11U), "plain store");
    Require(Store(memory, 0, a, 12U, token) == ExclusiveStoreResult::ReservationLost,
            "STREX after a plain store must fail");

    // Another granule leaves the reservation alone; the same granule does not.
    token = Load(memory, 0, a);
    t1 = Load(memory, 1, a + 8U);
    Require(Store(memory, 1, a + 8U, 1U, t1) == ExclusiveStoreResult::Success, "b");
    Require(Store(memory, 0, a, 13U, token) == ExclusiveStoreResult::Success,
            "another granule must not end the reservation");
    token = Load(memory, 0, a);
    t1 = Load(memory, 1, a + 4U);
    Require(Store(memory, 1, a + 4U, 1U, t1) == ExclusiveStoreResult::Success, "a+4");
    Require(Store(memory, 0, a, 14U, token) == ExclusiveStoreResult::ReservationLost,
            "the same 8-byte granule ends the reservation");

    // A context switch (CLREX) ends it; a STREX ends it either way.
    token = Load(memory, 0, a);
    SecondCore::tProcessor = 0U;
    SecondCore::ClearExclusive();
    Require(Store(memory, 0, a, 15U, token) == ExclusiveStoreResult::ReservationLost,
            "STREX after ClearExclusive must fail");
    token = Load(memory, 0, a);
    Require(Store(memory, 0, a, 16U, token) == ExclusiveStoreResult::Success, "pair");
    Require(Store(memory, 0, a, 17U, token) == ExclusiveStoreResult::ReservationLost,
            "a second STREX without LDREX must fail");

    // Doubleword and byte pairs.
    constexpr uint32_t d = 0x1100U;
    Require(memory.Write64(d, 0x0123456789ABCDEFULL, nullptr), "seed 64");
    token = Load(memory, 1, d, 8U);
    Require(token == 0x0123456789ABCDEFULL &&
                Store(memory, 1, d, 0x1111222233334444ULL, token, 8U) ==
                    ExclusiveStoreResult::Success,
            "LDREXD/STREXD");
    uint64_t wide = 0;
    Require(memory.Read64(d, &wide, nullptr) && wide == 0x1111222233334444ULL, "STREXD value");
    token = Load(memory, 0, d + 3U, 1U);
    Require(Store(memory, 0, d + 3U, 0xAAU, token, 1U) == ExclusiveStoreResult::Success,
            "LDREXB/STREXB");

    // A successful STREX is a tracked write: the page's certificate changes.
    const auto before = memory.RangeWriteGeneration(a, 4U);
    token = Load(memory, 0, a);
    Require(Store(memory, 0, a, 18U, token) == ExclusiveStoreResult::Success, "certificate");
    Require(before.has_value() && memory.RangeWriteGeneration(a, 4U) != before,
            "STREX must mark the page written");

    // A page without a fast write entry (GPU-observed) takes the checked path:
    // same result, and the observer sees the write.
    uint32_t observed = 0;
    memory.SetWriteObserver([&](uint32_t address, size_t) { observed = address; });
    Require(memory.ObserveWrites(0x2000U, 0x10U), "observe");
    Require(memory.Write32(0x2000U, 1U), "seed observed");
    observed = 0;
    token = Load(memory, 1, 0x2000U);
    Require(Store(memory, 1, 0x2000U, 2U, token) == ExclusiveStoreResult::Success &&
                observed == 0x2000U,
            "checked-path STREX must succeed and notify the observer");
    token = Load(memory, 0, 0x2000U);
    t1 = Load(memory, 1, 0x2000U);
    Require(Store(memory, 1, 0x2000U, 3U, t1) == ExclusiveStoreResult::Success &&
                Store(memory, 0, 0x2000U, 4U, token) == ExclusiveStoreResult::ReservationLost,
            "checked-path reservations follow the same rules");
    memory.SetWriteObserver({});
}

// Switch absent (fastmem phase 2): the same global monitor as with the second
// core on. The token is the loaded value, a store to another word no longer
// fails the STREX (no memory-wide written mark on every store), one that
// changes the reserved word does, and so do a context switch (the scheduler
// clears the monitor), a SWP to the granule and a savestate load (reset).
void TestMonitorInactive() {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"data", 0x1000, 0x1000, true, false, {}}, &error), error);
    Require(!SecondCore::Active(), "monitor must start inactive");
    SecondCore::ResetExclusiveMonitor();
    uint64_t value = 0;
    uint64_t token = 0;
    uint32_t fault = 0;
    Require(memory.Write32(0x1000U, 7U) &&
                memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault) && value == 7U &&
                token == value,
            "inactive LDREX token is the loaded value");
    Require(memory.Write32(0x1800U, 1U), "unrelated write");
    Require(memory.StoreExclusive(0x1000U, 4U, 8U, token, &fault) == ExclusiveStoreResult::Success,
            "inactive STREX survives a write to another word");
    Require(memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault) &&
                memory.Write32(0x1000U, 9U) &&
                memory.StoreExclusive(0x1000U, 4U, 10U, token, &fault) ==
                    ExclusiveStoreResult::ReservationLost,
            "inactive STREX fails when the reserved word changed");
    Require(memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault), "LDREX");
    SecondCore::ClearExclusive();
    Require(memory.StoreExclusive(0x1000U, 4U, 10U, token, &fault) ==
                ExclusiveStoreResult::ReservationLost,
            "inactive STREX fails after a context switch");
    uint32_t previous = 0;
    Require(memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault) &&
                memory.AtomicSwap(0x1004U, 4U, 0U, &previous, &fault) &&
                memory.StoreExclusive(0x1000U, 4U, 10U, token, &fault) ==
                    ExclusiveStoreResult::ReservationLost,
            "inactive SWP to the granule clears the reservation");
    Require(memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault), "LDREX");
    SecondCore::ResetExclusiveMonitor();
    Require(memory.StoreExclusive(0x1000U, 4U, 10U, token, &fault) ==
                ExclusiveStoreResult::ReservationLost,
            "a savestate load (monitor reset) costs one STREX retry");
    Require(memory.LoadExclusive(0x1000U, 4U, &value, &token, &fault) &&
                memory.StoreExclusive(0x1000U, 4U, 11U, token, &fault) ==
                    ExclusiveStoreResult::Success &&
                memory.Read32(0x1000U, &previous) && previous == 11U,
            "the retry succeeds");
}

// Two host threads, one guest word: no lost update, no false success.
void TestMonitorTwoThreads(uint32_t pairs) {
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"data", 0x1000, 0x1000, true, false, {}}, &error), error);
    const ActiveScope active;
    constexpr uint32_t shared = 0x1000U;
    std::array<uint64_t, 2> successes{};
    std::array<uint64_t, 2> failures{};
    std::atomic<int> started{0};
    const auto worker = [&](uint8_t processor) {
        SecondCore::tProcessor = processor;
        started.fetch_add(1);
        while (started.load() < 2) {
        }
        // Each core also keeps a private counter in the shared word's
        // neighbour granule: its pairs must never fail for the other's sake.
        const uint32_t own = shared + 0x100U * (processor + 1U);
        for (uint32_t done = 0; done < pairs;) {
            uint64_t value = 0;
            uint64_t token = 0;
            uint32_t fault = 0;
            Require(memory.LoadExclusive(shared, 4U, &value, &token, &fault), "LDREX");
            const auto result =
                memory.StoreExclusive(shared, 4U, value + 1U, token, &fault);
            Require(result != ExclusiveStoreResult::MemoryFault, "STREX fault");
            if (result == ExclusiveStoreResult::Success) {
                ++successes[processor];
                ++done;
                Require(memory.LoadExclusive(own, 4U, &value, &token, &fault) &&
                            memory.StoreExclusive(own, 4U, value + 1U, token, &fault) ==
                                ExclusiveStoreResult::Success,
                        "an unrelated granule must not fail");
            } else {
                ++failures[processor];
            }
        }
        SecondCore::tProcessor = 0U;
    };
    std::thread core1(worker, uint8_t{1});
    worker(0);
    core1.join();
    uint32_t total = 0;
    uint32_t own0 = 0;
    uint32_t own1 = 0;
    Require(memory.Read32(shared, &total) && memory.Read32(shared + 0x100U, &own0) &&
                memory.Read32(shared + 0x200U, &own1),
            "read back");
    Require(total == 2U * pairs, "lost update: the counter must equal every successful STREX");
    Require(successes[0] + successes[1] == total, "false success");
    Require(own0 == pairs && own1 == pairs, "private counters");
    std::cout << "  monitor: " << 2U * pairs << " pairs, "
              << failures[0] + failures[1] << " retried\n";
}

// --- Process level ------------------------------------------------------------

// r0 = counter address; [r0+4] = iterations. Increments [r0] with LDREX/STREX
// that many times, then ExitThread.
uint32_t EmitCounter(Program& program) {
    const uint32_t entry = program.Here();
    program.Op(Ldr(3, 0, 4));
    const uint32_t loop = program.Here();
    program.Op(Ldrex(1, 0));
    program.Op(AddImm(1, 1, 1));
    program.Op(Strex(2, 1, 0));
    program.Op(CmpImm(2, 0));
    program.Branch(Condition::Ne, loop);
    program.Op(SubsImm(3, 3, 1));
    program.Branch(Condition::Ne, loop);
    program.Svc(kSvcExitThread);
    return entry;
}

// The cooperative scheduler never runs a second-core thread, and with the
// switch absent the same thread runs cooperatively as before.
void TestSchedulerExclusion() {
    Program& program = Programs().emplace_back(kText);
    const uint32_t counter = EmitCounter(program);
    // Primary: count its own word, exit.
    {
        Guest guest(program.Seal());
        guest.Boot(counter, kData);
        guest.Write32(kData + 4U, 10U);
        guest.Write32(kData + 0x100U + 4U, 10U);
        guest.process.EnableSecondCore();
        SecondCore::LockKernel();
        const uint32_t id = guest.CreateThread(counter, 1, kData + 0x100U);
        Require(guest.process.ThreadHostCore(id) == 1U, "processor 1 is a second-core thread");
        const uint32_t pc = guest.process.ThreadState(id)->r[15];
        for (int run = 0; run < 100; ++run) {
            const auto result = guest.process.Run(kBudget);
            Require(result.Kind != NativeA32ProcessRunKind::Faulted, result.Error);
            Require(guest.process.ThreadStatus(id) == NativeA32ThreadStatus::Ready &&
                        guest.process.ThreadState(id)->r[15] == pc,
                    "the cooperative scheduler selected a second-core thread");
        }
        Require(Terminated(guest.process, 0) && guest.Read32(kData) == 10U,
                "the primary thread must still run");
        Require(guest.Read32(kData + 0x100U) == 0U &&
                    guest.process.GuestThreadProfile().Ticks[id] == 0U,
                "a second-core thread must not have run on the game thread");
        guest.process.DisableSecondCore();
        SecondCore::UnlockKernel();
    }
    // Switch absent: processor 1 runs on the cooperative scheduler.
    {
        Guest guest(program.Seal());
        guest.Boot(counter, kData);
        guest.Write32(kData + 4U, 10U);
        guest.Write32(kData + 0x100U + 4U, 10U);
        const uint32_t id = guest.CreateThread(counter, 1, kData + 0x100U);
        Require(guest.process.ThreadHostCore(id) == 0U && !SecondCore::Active(),
                "without EnableSecondCore every thread is cooperative");
        for (int run = 0; run < 100 && !Terminated(guest.process, id); ++run) {
            Require(guest.process.Run(kBudget).Kind != NativeA32ProcessRunKind::Faulted,
                    "cooperative run faulted");
        }
        Require(Terminated(guest.process, id) && guest.Read32(kData + 0x100U) == 10U,
                "switch absent: the processor-1 thread runs cooperatively");
        const std::string line =
            FormatNativeA32GuestThreadsLine(guest.process, {}, 1U, 1U);
        Require(line.find("core1_ms") == std::string::npos,
                "switch absent: the guest_threads line is unchanged");
    }
}

// Both cores increment one guest word with LDREX/STREX.
void TestGuestCounter(uint32_t increments) {
    Program& program = Programs().emplace_back(kText);
    const uint32_t counter = EmitCounter(program);
    Guest guest(program.Seal());
    guest.Boot(counter, kData);
    guest.Write32(kData + 4U, increments);
    TwoCores cores(guest.process);
    // The second-core thread counts the same word ([kData], its own
    // iteration count at kData + 4 too).
    const uint32_t id = guest.CreateThread(counter, 1, kData);
    cores.RunUntil([&] { return Terminated(guest.process, 0) && Terminated(guest.process, id); });
    const auto profile = guest.process.GuestThreadProfile();
    cores.Stop();
    Require(guest.Read32(kData) == 2U * increments,
            "guest LDREX/STREX counter lost an update across cores");
    Require(profile.SecondCoreTicks != 0U && profile.Ticks[id] != 0U,
            "the second core's time is charged to its thread");
    std::cout << "  guest counter: " << 2U * increments << " increments\n";
}

// Event ping-pong: the game thread signals A and waits B, the second core
// waits A and signals B. r9 = kData: [0] = A, [4] = B, [8] = rounds.
void TestEventPingPong(uint32_t rounds) {
    Program& program = Programs().emplace_back(kText);
    const uint32_t ping = program.Here();
    program.Op(Ldr(8, 9, 8));
    const uint32_t pingLoop = program.Here();
    program.Op(Ldr(0, 9, 0));
    program.Svc(kSvcSignalEvent);
    program.Op(AddImm(1, 9, 4));
    program.Op(MovImm(2, 1));
    program.Op(MovImm(3, 0));
    program.Svc(kSvcWaitSynchronizationN);
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, pingLoop);
    program.Svc(kSvcExitThread);
    const uint32_t pong = program.Here();
    program.Op(AddImm(9, 0, 0));  // r9 = r0 = kData
    program.Op(Ldr(8, 9, 8));
    const uint32_t pongLoop = program.Here();
    program.Op(AddImm(1, 9, 0));
    program.Op(MovImm(2, 1));
    program.Op(MovImm(3, 0));
    program.Svc(kSvcWaitSynchronizationN);
    program.Op(Ldr(0, 9, 4));
    program.Svc(kSvcSignalEvent);
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, pongLoop);
    program.Svc(kSvcExitThread);

    Guest guest(program.Seal());
    guest.Boot(ping, kData);
    TwoCores cores(guest.process);
    guest.Write32(kData + 0U, guest.CreateEvent());
    guest.Write32(kData + 4U, guest.CreateEvent());
    guest.Write32(kData + 8U, rounds);
    const uint32_t id = guest.CreateThread(pong, 1, kData);
    const auto start = std::chrono::steady_clock::now();
    cores.RunUntil([&] { return Terminated(guest.process, 0) && Terminated(guest.process, id); });
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    Require(guest.process.ThreadState(0)->r[8] == 0U && guest.process.ThreadState(id)->r[8] == 0U,
            "every round trip completed on both cores");
    std::cout << "  event ping-pong: " << rounds << " round trips ("
              << seconds * 1.0e6 / rounds << " us each)\n";
}

// Arbitration: the game thread produces (LDREX/STREX increment of W, then
// ArbitrateAddress signal), the second core consumes (ArbitrateAddress
// WaitIfLessThan 1, then LDREX/STREX decrement). r9 = kData: [8] = arbiter,
// [12] = items, [16] = W.
void TestArbitration(uint32_t items) {
    Program& program = Programs().emplace_back(kText);
    const uint32_t produce = program.Here();
    program.Op(Ldr(8, 9, 12));
    const uint32_t produceLoop = program.Here();
    program.Op(AddImm(1, 9, 16));
    const uint32_t increment = program.Here();
    program.Op(Ldrex(5, 1));
    program.Op(AddImm(5, 5, 1));
    program.Op(Strex(6, 5, 1));
    program.Op(CmpImm(6, 0));
    program.Branch(Condition::Ne, increment);
    program.Op(Ldr(0, 9, 8));
    program.Op(MovImm(2, 0));  // signal
    program.Op(MovImm(3, 1));  // one waiter
    program.Svc(kSvcArbitrateAddress);
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, produceLoop);
    program.Svc(kSvcExitThread);
    const uint32_t consume = program.Here();
    program.Op(AddImm(9, 0, 0));
    program.Op(Ldr(8, 9, 12));
    program.Op(MovImm(7, 0));
    const uint32_t consumeLoop = program.Here();
    program.Op(Ldr(0, 9, 8));
    program.Op(AddImm(1, 9, 16));
    program.Op(MovImm(2, 1));  // WaitIfLessThan
    program.Op(MovImm(3, 1));
    program.Svc(kSvcArbitrateAddress);
    const uint32_t decrement = program.Here();
    program.Op(AddImm(1, 9, 16));
    program.Op(Ldrex(5, 1));
    program.Op(CmpImm(5, 0));
    program.Branch(Condition::Eq, consumeLoop);
    program.Op(SubImm(5, 5, 1));
    program.Op(Strex(6, 5, 1));
    program.Op(CmpImm(6, 0));
    program.Branch(Condition::Ne, decrement);
    program.Op(AddImm(7, 7, 1));
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, consumeLoop);
    program.Svc(kSvcExitThread);

    Guest guest(program.Seal());
    guest.Boot(produce, kData);
    TwoCores cores(guest.process);
    guest.Write32(kData + 8U, guest.CreateArbiter());
    guest.Write32(kData + 12U, items);
    const uint32_t id = guest.CreateThread(consume, 1, kData);
    cores.RunUntil([&] { return Terminated(guest.process, 0) && Terminated(guest.process, id); });
    Require(guest.process.ThreadState(id)->r[7] == items && guest.Read32(kData + 16U) == 0U,
            "arbitration: every item produced on one core is consumed on the other");
    std::cout << "  arbitration: " << items << " items\n";
}

// DecrementAndWaitIfLessThan through the monitor (the kernel's own
// exclusive pair), then the wake-up from the other core's signal.
void TestArbitrationDecrement() {
    Program& program = Programs().emplace_back(kText);
    const uint32_t idle = program.Here();
    program.Branch(Condition::Al, idle);
    // r9 = kData: decrement-and-wait on [16] < 0, then ExitThread.
    const uint32_t waiter = program.Here();
    program.Op(AddImm(9, 0, 0));
    program.Op(Ldr(0, 9, 8));
    program.Op(AddImm(1, 9, 16));
    program.Op(MovImm(2, 2));  // DecrementAndWaitIfLessThan
    program.Op(MovImm(3, 0));
    program.Svc(kSvcArbitrateAddress);
    program.Svc(kSvcExitThread);
    Guest guest(program.Seal());
    guest.Boot(idle, kData);
    TwoCores cores(guest.process);
    guest.Write32(kData + 8U, guest.CreateArbiter());
    guest.Write32(kData + 16U, static_cast<uint32_t>(-1));
    const uint32_t id = guest.CreateThread(waiter, 1, kData);
    // The second core decrements (-1 -> -2) and parks.
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10), [&] {
                return guest.process.ThreadStatus(id) == NativeA32ThreadStatus::Waiting;
            }),
            "the decrement-and-wait did not park");
    Require(guest.Read32(kData + 16U) == static_cast<uint32_t>(-2),
            "DecrementAndWaitIfLessThan must decrement once");
    GuestState signal{};
    signal.r[0] = guest.Read32(kData + 8U);
    signal.r[1] = kData + 16U;
    signal.r[2] = 0U;
    signal.r[3] = 1U;
    guest.Svc(kSvcArbitrateAddress, signal);
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10),
                                      [&] { return Terminated(guest.process, id); }),
            "the signal from the game thread did not wake the second core");
}

// Timeout: a second-core SleepThread wakes when the game thread advances the
// system clock past it, not before.
void TestSleepTimeout() {
    Program& program = Programs().emplace_back(kText);
    const uint32_t idle = program.Here();
    program.Branch(Condition::Al, idle);
    const uint32_t sleeper = program.Here();
    program.Op(AddImm(9, 0, 0));
    program.Op(0xE3A00A01U);  // mov r0, #0x1000 (4096 ns)
    program.Op(MovImm(1, 0));
    program.Svc(kSvcSleepThread);
    program.Op(MovImm(5, 1));
    program.Op(0xE5895014U);  // str r5, [r9, #20]
    program.Svc(kSvcExitThread);
    Guest guest(program.Seal());
    guest.Boot(idle, kData);
    TwoCores cores(guest.process);
    const uint32_t id = guest.CreateThread(sleeper, 1, kData);
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10), [&] {
                return guest.process.ThreadStatus(id) == NativeA32ThreadStatus::Waiting;
            }),
            "the second-core sleep did not park");
    Require(guest.host.PendingSleepCount() == 1U, "one timer pending");
    guest.host.AdvanceSystemTicks(1U);
    Require(!SecondCore::WaitKernelFor(std::chrono::milliseconds(50),
                                       [&] { return Terminated(guest.process, id); }),
            "the sleep ended before its deadline");
    guest.host.AdvanceSystemTicks(1'000'000U);
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10),
                                      [&] { return Terminated(guest.process, id); }),
            "the timer from the game thread did not wake the second core");
    cores.Stop();
    Require(guest.Read32(kData + 20U) == 1U, "the woken thread ran on");
}

// A fault on the second core is the game thread's next Run() result, with
// the guest report.
void TestSecondCoreFault() {
    Program& program = Programs().emplace_back(kText);
    const uint32_t idle = program.Here();
    program.Branch(Condition::Al, idle);
    const uint32_t faulter = program.Here();
    program.Svc(0x7FU);  // unimplemented
    Guest guest(program.Seal());
    guest.Boot(idle, kData);
    TwoCores cores(guest.process);
    guest.CreateThread(faulter, 1, kData);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        const auto result = guest.process.Run(kBudget);
        if (result.Kind == NativeA32ProcessRunKind::Faulted) {
            Require(!result.GuestReport.empty() &&
                        result.Error.find("TRIAEVUM_HOST_REQUEST_UNSERVED") != std::string::npos,
                    "the second core's fault must carry its error and report");
            break;
        }
        Require(std::chrono::steady_clock::now() < deadline,
                "the second core's fault never reached Run()");
        const SecondCore::KernelRelease release;
        std::this_thread::yield();
    }
}

// Savestates made with the second core on carry the requested processor;
// a restore places the thread on the second core again.
void TestSavestate() {
    Program& program = Programs().emplace_back(kText);
    const uint32_t idle = program.Here();
    program.Branch(Condition::Al, idle);
    Guest guest(program.Seal());
    guest.Boot(idle, kData);
    guest.process.EnableSecondCore();
    SecondCore::LockKernel();
    const uint32_t id = guest.CreateThread(idle, 1, kData);
    const auto saved = guest.process.CaptureState();
    Require(saved.at("secondary_threads").at(0).at("processor_id").get<int32_t>() == 1,
            "a second-core savestate records the processor");
    std::string error;
    Require(guest.process.RestoreState(saved, &error) &&
                guest.process.ThreadHostCore(id) == 1U,
            "a restore places the thread on the second core: " + error);
    guest.process.DisableSecondCore();
    SecondCore::UnlockKernel();
    // Switch absent: the record is as before (no processor field).
    Guest plain(program.Seal());
    plain.Boot(idle, kData);
    plain.CreateThread(idle, 1, kData);
    Require(!plain.process.CaptureState().at("secondary_threads").at(0).contains("processor_id"),
            "switch absent: thread records are unchanged");
}

// --- DSP frames on the second core (window session's idle work) --------------
//
// A stand-in for the game's sound thread (processor 1, prio 0x18) and its
// main thread. r9 = kData: [0] = DSP interrupt event, [4] = VBlank event,
// [8] = sound work per frame, [12] = main-thread work per refresh (a slow
// guest), [16] = frames the sound thread consumed, [20] = 1 while it works.
struct DspGuestProgram {
    uint32_t Sound = 0U;
    uint32_t Main = 0U;
};

// WaitSynchronizationN on the handle at r9 + offset, no timeout.
void EmitWait(Program& program, uint32_t offset) {
    program.Op(AddImm(1, 9, offset));
    program.Op(MovImm(2, 1));
    program.Op(MovImm(3, 0));
    program.Op(MvnZero(0));
    program.Op(MvnZero(4));
    program.Svc(kSvcWaitSynchronizationN);
}

DspGuestProgram EmitDspGuest(Program& program) {
    DspGuestProgram entries;
    entries.Sound = program.Here();
    program.Op(AddImm(9, 0, 0));
    const uint32_t soundLoop = program.Here();
    program.Op(MovImm(5, 0));
    program.Op(Str(5, 9, 20));
    EmitWait(program, 0);
    program.Op(MovImm(5, 1));
    program.Op(Str(5, 9, 20));
    program.Op(Ldr(8, 9, 8));
    const uint32_t soundSpin = program.Here();
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, soundSpin);
    program.Op(Ldr(5, 9, 16));
    program.Op(AddImm(5, 5, 1));
    program.Op(Str(5, 9, 16));
    program.Branch(Condition::Al, soundLoop);
    entries.Main = program.Here();
    program.Op(Ldr(8, 9, 12));
    const uint32_t mainSpin = program.Here();
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, mainSpin);
    EmitWait(program, 4);
    program.Branch(Condition::Al, entries.Main);
    return entries;
}

struct DspGuest {
    DspGuest() {
        Program& program = Programs().emplace_back(kText);
        entries = EmitDspGuest(program);
        guest = std::make_unique<Guest>(program.Seal());
        guest->Boot(entries.Main, kData);
    }
    // After EnableSecondCore when the sound thread is to run there.
    void Create(uint32_t soundWork, uint32_t mainWork) {
        guest->Write32(kData + 0U, guest->CreateEvent());
        guest->Write32(kData + 4U, guest->CreateEvent());
        guest->Write32(kData + 8U, soundWork);
        guest->Write32(kData + 12U, mainWork);
        sound = guest->CreateThread(entries.Sound, 1, kData);
    }
    void Signal(uint32_t offset) {
        GuestState state{};
        state.r[0] = guest->Read32(kData + offset);
        guest->Svc(kSvcSignalEvent, state);
    }
    bool SoundParked() {
        return guest->process.ThreadStatus(sound) == NativeA32ThreadStatus::Waiting &&
               guest->Read32(kData + 20U) == 0U;
    }

    DspGuestProgram entries;
    std::unique_ptr<Guest> guest;
    uint32_t sound = 0U;
};

// The DSP schedules dispatch as the game does: thousands of blocks between
// exits (SVCs), not the 64 of the lock stress tests above.
constexpr uint32_t kDspBudget = 4096U;

// Runs the game thread's cooperative scheduler until none of its threads is
// ready (the switch-off guest run, RunUntilGuestWait).
void RunCore0UntilWait(NativeA32Process& process) {
    for (;;) {
        const auto result = process.Run(kDspBudget);
        Require(result.Kind != NativeA32ProcessRunKind::Faulted, "unexpected fault: " + result.Error);
        if (result.Kind != NativeA32ProcessRunKind::Yielded) return;
    }
}

using Clock = std::chrono::steady_clock;

struct DspSchedule {
    // Per delivered frame: what it waited for besides the sound thread's
    // work: the time from its refresh's pending point minus the sound
    // thread's busy time since then (its own pace, which differs between
    // host cores, is not the schedule's).
    std::vector<double> WaitsMs;
    uint32_t Consumed = 0U;
    uint32_t Overlaps = 0U;
    uint64_t Core0RunsWhileSoundBusy = 0U;
    std::string Line;  // the run's per-refresh perf line
};

constexpr uint32_t kDspFramesPerRefresh = 3U;

// The window session's refresh, R times: the refresh's DSP frames become
// pending, VBlank wakes the main thread (a slow guest: mainWork), then host
// work (render, presentation) of hostWork.
// - Switch off (the session's immediate path, the earliest of its
//   schedules; config/audio_deferred delivers later still): each frame is
//   mixed, its interrupt signaled and the guest run until it waits, then
//   VBlank and the main thread's run, then the host work.
// - Switch on: the frames are added and the kernel condition notified; the
//   second core delivers them as its idle work while the game thread runs
//   the main thread and does its host work.
// Switch off, idle work is installed too and must never run.
DspSchedule RunDspSchedule(bool secondCore, uint32_t refreshes, uint32_t soundWork,
                           uint32_t mainWork, std::chrono::milliseconds hostWork) {
    DspGuest dsp;
    NativeA32Process& process = dsp.guest->process;
    DspSchedule schedule;
    uint32_t pending = 0U;
    uint32_t delivered = 0U;
    Clock::time_point refreshStart{};
    uint64_t refreshSoundTicks = 0U;
    uint32_t refreshPending = 0U;  // frames of the refresh still to deliver
    // Kernel lock held (or the switch off).
    const auto soundTicks = [&] {
        return NativeA32ProfileTicksToNanoseconds(
            process.GuestThreadProfile().Ticks[dsp.sound]);
    };
    const auto recordWait = [&](bool leftOver) {
        const double sinceRefresh =
            std::chrono::duration<double, std::milli>(Clock::now() - refreshStart).count();
        const double soundMs = static_cast<double>(soundTicks() - refreshSoundTicks) / 1.0e6;
        // A frame of an earlier refresh waited for a whole refresh.
        schedule.WaitsMs.push_back(sinceRefresh - soundMs + (leftOver ? 1.0e6 : 0.0));
    };
    process.SetSecondCoreIdleWork(
        {[&] { return pending != 0U; },
         [&] {
             Require(secondCore, "switch absent: the idle work ran");
             Require(SecondCore::Processor() == 1U && SecondCore::KernelHeld(),
                     "idle work on the second core with the kernel lock");
             if (!dsp.SoundParked()) ++schedule.Overlaps;
             recordWait(pending > refreshPending);
             --pending;
             if (refreshPending != 0U) --refreshPending;
             ++delivered;
             dsp.Signal(0);
         }});
    std::unique_ptr<TwoCores> cores;
    if (secondCore) cores = std::make_unique<TwoCores>(process, kDspBudget);
    NativeA32GuestThreadProfile profileStart = process.GuestThreadProfile();
    profileStart.SecondCoreCpuKnown = true;  // its CPU time counts from 0
    dsp.Create(soundWork, mainWork);
    // The main thread runs its first frame and waits for VBlank.
    RunCore0UntilWait(process);
    for (uint32_t refresh = 0; refresh < refreshes; ++refresh) {
        refreshStart = Clock::now();
        refreshSoundTicks = soundTicks();
        if (!secondCore) {
            for (uint32_t frame = 0; frame < kDspFramesPerRefresh; ++frame) {
                recordWait(false);
                ++delivered;
                dsp.Signal(0);
                RunCore0UntilWait(process);
            }
            dsp.Signal(4);
            RunCore0UntilWait(process);
            std::this_thread::sleep_for(hostWork);
            continue;
        }
        // The game thread never waits for the second core: it adds the
        // frames and goes on.
        refreshPending = kDspFramesPerRefresh;
        pending += kDspFramesPerRefresh;
        SecondCore::NotifyKernel();
        dsp.Signal(4);
        for (;;) {
            // Thread records are read under the kernel lock (guest memory
            // would race with the second core's plain stores).
            const auto soundStatus = process.ThreadStatus(dsp.sound);
            if (soundStatus == NativeA32ThreadStatus::Ready ||
                soundStatus == NativeA32ThreadStatus::Running) {
                ++schedule.Core0RunsWhileSoundBusy;
            }
            const auto result = process.Run(kDspBudget);
            Require(result.Kind != NativeA32ProcessRunKind::Faulted,
                    "unexpected fault: " + result.Error);
            if (result.Kind != NativeA32ProcessRunKind::Yielded) break;
        }
        const SecondCore::KernelRelease release;
        std::this_thread::sleep_for(hostWork);
    }
    if (secondCore) {
        Require(SecondCore::WaitKernelFor(std::chrono::seconds(600),
                                          [&] { return pending == 0U && dsp.SoundParked(); }),
                "the second core did not deliver every frame");
        cores->Stop();
        // One "host frame" per refresh.
        schedule.Line = FormatNativeA32GuestThreadsLine(process, {}, refreshes, refreshes) +
                        "\n    " +
                        FormatNativeA32SecondCoreLine(process.GuestThreadProfile(),
                                                      profileStart, {}, refreshes);
    } else {
        RunCore0UntilWait(process);
        schedule.Line = FormatNativeA32GuestThreadsLine(process, {}, refreshes, refreshes);
    }
    schedule.Consumed = dsp.guest->Read32(kData + 16U);
    Require(delivered == refreshes * kDspFramesPerRefresh, "every frame delivered");
    return schedule;
}

// DSP frames on the second core: each waits (never drops) until the sound
// thread is parked, never overlaps it, and the game thread runs guest code
// meanwhile instead of waiting; their cadence never lags the switch-off
// schedule (a frame waits for nothing but the sound thread's work), with a
// slow guest and host work in between; switch absent, the idle work never
// runs and the schedule is the cooperative one.
void TestDspOnSecondCore(uint32_t refreshes) {
    constexpr uint32_t kSoundWork = 20'000U;   // per frame
    constexpr uint32_t kMainWork = 400'000U;   // per refresh: a slow guest
    constexpr auto kHostWork = std::chrono::milliseconds(10);
    const auto off = RunDspSchedule(false, refreshes, kSoundWork, kMainWork, kHostWork);
    Require(!SecondCore::Active(), "switch absent: the kernel lock stays inert");
    Require(off.Consumed == refreshes * kDspFramesPerRefresh,
            "switch off: the sound thread consumed every frame");
    const auto on = RunDspSchedule(true, refreshes, kSoundWork, kMainWork, kHostWork);
    Require(on.Overlaps == 0U, "a DSP frame met a running sound thread");
    Require(on.Consumed == refreshes * kDspFramesPerRefresh,
            "a DSP frame went out before the sound thread parked (frames merged)");
    Require(on.Core0RunsWhileSoundBusy != 0U,
            "the game thread did not run guest code while the second core worked");
    double worstLag = 0.0;
    std::string waits;
    for (size_t frame = 0; frame < on.WaitsMs.size(); ++frame) {
        worstLag = std::max(worstLag, on.WaitsMs[frame] - off.WaitsMs[frame]);
        waits += " " + std::to_string(off.WaitsMs[frame]) + "/" +
                 std::to_string(on.WaitsMs[frame]);
    }
    // One cross-thread wake-up for the refresh's first frame (0.1-0.5 ms in
    // a WSL2 container, more under TSan) and scheduling noise; a frame held
    // behind the main thread or the host work would wait 30 ms or more.
    if (worstLag > 5.0) {
        Fail("DSP frames lag the switch-off schedule (ms waited besides the sound thread's "
             "work, off/on):" + waits + "\n    " + off.Line + "\n    " + on.Line);
    }
    std::cout << "  DSP on the second core: " << on.WaitsMs.size()
              << " frames, worst lag vs switch off " << worstLag << " ms\n    " << off.Line
              << "\n    " << on.Line << '\n';
}

// A failure of the idle work (a DSP mix error) is the game thread's next
// Run() result; the second core then stops doing it.
void TestIdleWorkFault() {
    DspGuest dsp;
    NativeA32Process& process = dsp.guest->process;
    uint32_t pending = 1U;
    uint32_t runs = 0U;
    process.SetSecondCoreIdleWork({[&] { return pending != 0U; },
                                   [&] {
                                       ++runs;
                                       throw std::runtime_error("mix failed");
                                   }});
    TwoCores cores(process);
    dsp.Create(1U, 1U);
    SecondCore::NotifyKernel();
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    for (;;) {
        const auto result = process.Run(kBudget);
        if (result.Kind == NativeA32ProcessRunKind::Faulted) {
            Require(result.Error.find("second-core idle work: mix failed") != std::string::npos,
                    "the idle work's error must reach Run(): " + result.Error);
            break;
        }
        Require(Clock::now() < deadline, "the idle work's fault never reached Run()");
        const SecondCore::KernelRelease release;
        std::this_thread::yield();
    }
    Require(runs == 1U, "the idle work ran again after its fault");
}

// The second_core: perf line (host units: nanoseconds).
void TestSecondCoreLine() {
    NativeA32GuestThreadProfile start;
    start.SecondCoreCpuKnown = true;
    NativeA32GuestThreadProfile now = start;
    now.SecondCoreTicks = 8'000'000U;          // 8 ms busy (guest + SVCs)
    now.SecondCoreLockTicks = 1'000'000U;      // 1 ms after dispatches
    now.SecondCoreIdleWorkTicks = 1'000'000U;  // 1 ms of DSP frames
    now.SecondCoreIdleWorkUnits = 7U;
    now.SecondCoreLockWaitNanoseconds = 2'000'000U;
    now.SecondCoreCpuTicks = 5'000'000U;
    const std::string line =
        FormatNativeA32SecondCoreLine(now, start, {0U, 0U, 0U, 0.0005}, 1U);
    Require(line ==
                "second_core: dsp_pumped=0.00 dsp_flushed=0.00 dsp_late=0 dsp_wait_ms=0.500 "
                "dsp_core1=7.00 dsp_core1_ms=1.000 core1_lock_wait_ms=2.000 "
                "core1_preempted_ms=3.000 core0_saved_ms=4.500 core1_wakes=0.0 "
                "core1_empty_wakes=0.0",
            "second_core line: " + line);
    now.SecondCoreIdleWakes = 30U;
    now.SecondCoreEmptyWakes = 25U;
    Require(FormatNativeA32SecondCoreLine(now, start, {}, 10U)
                .ends_with(" core1_wakes=3.0 core1_empty_wakes=2.5"),
            "second_core line: idle wakes per frame");
    now.SecondCoreCpuKnown = false;
    Require(FormatNativeA32SecondCoreLine(now, start, {}, 1U).find("core1_preempted_ms=na") !=
                std::string::npos,
            "unknown CPU time: preemption na");
}

// config/kernel_wake: the game thread's SVCs (each a guest exit and a kernel
// lock retake) must not wake the idle second core, whose thread waits on an
// event; without it each exit notifies the kernel waiters. Either way the
// second core's thread wakes for the signal and the guest results match.
void TestQuietWakes(bool quiet) {
    constexpr uint32_t kCalls = 2000U;
    constexpr uint32_t kSvcGetSystemTick = 0x28U;
    Program& program = Programs().emplace_back(kText);
    const uint32_t spinner = program.Here();  // r8 = calls
    program.Svc(kSvcGetSystemTick);
    program.Op(SubsImm(8, 8, 1));
    program.Branch(Condition::Ne, spinner);
    program.Svc(kSvcExitThread);
    const uint32_t parked = program.Here();
    program.Op(AddImm(9, 0, 0));
    EmitWait(program, 8);  // the event at kData + 8
    program.Op(MovImm(5, 1));
    program.Op(Str(5, 9, 12));
    program.Svc(kSvcExitThread);
    Guest guest(program.Seal());
    guest.Boot(spinner, kData);
    guest.process.PrimaryThreadState().r[8] = kCalls;
    const uint32_t event = guest.CreateEvent();
    guest.Write32(kData + 8U, event);
    TwoCores cores(guest.process);
    const uint32_t id = guest.CreateThread(parked, 1, kData);
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10), [&] {
                return guest.process.ThreadStatus(id) == NativeA32ThreadStatus::Waiting;
            }),
            "the second-core thread did not park on its event");
    const auto before = guest.process.GuestThreadProfile();
    cores.RunUntil([&] { return Terminated(guest.process, 0); });
    const auto after = guest.process.GuestThreadProfile();
    const uint64_t wakes = after.SecondCoreIdleWakes - before.SecondCoreIdleWakes;
    Require(after.SvcCalls[0] - before.SvcCalls[0] == kCalls + 1U,
            "the game thread's SVCs were not counted");
    // The game thread's time per SVC round trip (guest exit, lock retake,
    // SVC, scheduler, re-entry) and its share retaking the kernel lock.
    const double perCallUs =
        static_cast<double>(NativeA32ProfileTicksToNanoseconds(after.Ticks[0] - before.Ticks[0])) /
        1000.0 / kCalls;
    const double lockUs = static_cast<double>(NativeA32ProfileTicksToNanoseconds(
                              after.LockTicks[0] - before.LockTicks[0])) /
                          1000.0 / kCalls;
    std::cout << "    kernel_wake " << (quiet ? "on" : "off") << ": " << wakes
              << " idle wakes of the second core over " << kCalls
              << " game-thread SVCs; game thread " << perCallUs << " us per SVC round trip, "
              << lockUs << " us of it retaking the kernel lock\n";
    if (quiet) {
        Require(wakes <= 2U, "an idle second core woke at the game thread's SVCs");
    }
    GuestState signal{};
    signal.r[0] = event;
    guest.Svc(kSvcSignalEvent, signal);
    Require(SecondCore::WaitKernelFor(std::chrono::seconds(10),
                                      [&] { return Terminated(guest.process, id); }),
            "the signal did not wake the second core");
    cores.Stop();
    Require(guest.Read32(kData + 12U) == 1U, "the woken thread ran on");
}

} // namespace

int main(int argc, char** argv) {
    Sizes sizes;
    if (argc == 2 && std::string_view(argv[1]) == "--stress") {
        sizes.ExclusivePairs = 2'000'000U;
        sizes.GuestIncrements = 1'000'000U;
        // Each round trip is two cross-thread wake-ups (0.2-0.5 ms in a
        // WSL2 container, more under TSan): 200k of each in minutes.
        sizes.PingPongRounds = 200'000U;
        sizes.ArbitrationItems = 200'000U;
        sizes.DspRefreshes = 100U;
    }
    TestMonitorInactive();
    TestMonitorSemantics();
    TestMonitorTwoThreads(sizes.ExclusivePairs);
    // Every two-core test with config/kernel_wake off and on: the same guest
    // results, fewer wakes.
    for (const bool quiet : {false, true}) {
        SecondCore::gQuietWakes = quiet;
        std::cout << "  kernel_wake " << (quiet ? "on" : "off") << '\n';
        TestSchedulerExclusion();
        TestGuestCounter(sizes.GuestIncrements);
        TestEventPingPong(sizes.PingPongRounds);
        TestArbitration(sizes.ArbitrationItems);
        TestArbitrationDecrement();
        TestSleepTimeout();
        TestSecondCoreFault();
        TestSavestate();
        TestIdleWorkFault();
        TestDspOnSecondCore(sizes.DspRefreshes);
        TestQuietWakes(quiet);
    }
    SecondCore::gQuietWakes = false;
    TestSecondCoreLine();
    std::cout << "oot3d_native_a32_second_core_tests: ok\n";
    return 0;
}
