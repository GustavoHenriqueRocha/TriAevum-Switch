// One side of the sequence differential (run_store_sequence_diff.sh): compiles
// the functions of store_sequences.inc against the current memory (default) or against
// the base revision renamed to OldMem (-DSEQ_OLD). The optimizer sees the
// inlined accesses exactly as in the generated C++, with the guest registers
// as references to the architectural state.
#if defined(SEQ_OLD)
#include "old_memory.h"
namespace SeqMem = OldMem;
#define SEQ_SIDE OldSide
// The base's exclusives with its second core active: the global exclusive
// monitor, which the current memory uses with the second core on or off.
#if __has_include("old_second_core.h")
#include "old_second_core.h"
#define SEQ_MONITOR OldMem::SecondCore
#endif
#else
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_second_core.h"
namespace SeqMem = Oot3dNativeGame;
#define SEQ_SIDE NewSide
#define SEQ_MONITOR Oot3dNativeGame::SecondCore
#endif
#include "oot3d_aot_architectural_state.h"

#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace SEQ_SIDE {
namespace a32 = oot3d::recomp::a32;
using Memory = SeqMem::NativeA32Memory;
using Oot3dNativeGame::Oot3dAotArchitecturalState;
using Oot3dNativeGame::Oot3dAotFlagMask;
using Oot3dNativeGame::Oot3dAotSetSubtractFlags;

struct Hash {
    uint64_t h = 14695981039346656037ULL;
    void Add(uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFU;
            h *= 1099511628211ULL;
        }
    }
};

using Observations = Hash;

// Out-of-line observation mid-sequence, like a call out of the generated
// code: the generation and the page stamp must be exact.
__attribute__((noinline)) void Observe(Observations& observations, Memory& memory,
                                       uint32_t base) {
    observations.Add(memory.WriteGeneration());
    observations.Add(memory.RangeWriteGeneration(base & ~0xFFFU, 0x1000U).value_or(0xDEADU));
    uint8_t bytes[16] = {};
    observations.Add(memory.ReadBytes(base, bytes) ? 1U : 2U);
    for (uint8_t b : bytes) observations.Add(b);
}

constexpr uint32_t kSeqBases[] = {0x10000U, 0x10FF0U, 0x11FFAU, 0x12F00U, 0x13000U,
                                  0x13FF8U, 0x20000U, 0x1FFFCU, 0x21FF0U, 0x10800U,
                                  0x10804U, 0x10802U};
constexpr uint32_t kSeqBaseCount = sizeof(kSeqBases) / sizeof(kSeqBases[0]);

#define SEQ_FN(name)                                                                  \
    void name(Memory& mem, Oot3dAotArchitecturalState& state,                        \
              Observations& observations, uint32_t& faultPc, uint32_t& faultAddress, \
              uint64_t& exclusiveToken)
#define MEM mem
// A fault is recorded and the sequence continues (in generated code it exits
// the function); this way each sequence exercises every operation and partial
// LDM/STM faults enter the digest.
#define FAULT(pc, address)                         \
    do {                                           \
        faultPc = faultPc * 31U + (pc);            \
        faultAddress = faultAddress * 31U + (address); \
        observations.Add(mem.WriteGeneration());   \
    } while (0)
using SeqFn = void (*)(Memory&, Oot3dAotArchitecturalState&, Observations&, uint32_t&,
                       uint32_t&, uint64_t&);

// Straight-line access helpers: the new side uses the current memory's
// (StoreGuestValue/LoadGuestValue); the base side, std::memcpy.
#if defined(SEQ_OLD)
template <typename T>
inline void RawStore(uint8_t* destination, T value) {
    std::memcpy(destination, &value, sizeof(T));
}
template <typename T>
inline T RawLoad(const uint8_t* source) {
    T value;
    std::memcpy(&value, source, sizeof(T));
    return value;
}
#else
template <typename T>
inline void RawStore(uint8_t* destination, T value) {
    Oot3dNativeGame::StoreGuestValue<T>(destination, value);
}
template <typename T>
inline T RawLoad(const uint8_t* source) {
    return Oot3dNativeGame::LoadGuestValue<T>(source);
}
#endif
#define RAW_FN(name) void name(uint8_t* bytes, Oot3dAotArchitecturalState& state)
#define RAW_STORE(T, pointer, value) RawStore<T>((pointer), (value))
#define RAW_LOAD(T, pointer) RawLoad<T>((pointer))
using RawFn = void (*)(uint8_t*, Oot3dAotArchitecturalState&);

#include "store_sequences.inc"

// Runs Raw_<index> over a 32-byte guest buffer.
uint64_t RunRaw(uint32_t index, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<uint8_t> bytes(32);
    for (auto& b : bytes) b = uint8_t(rng());
    a32::GuestState guest{};
    for (int r = 0; r < 15; ++r) guest.r[r] = uint32_t(rng());
    Oot3dAotArchitecturalState state(guest);
    kRawFunctions[index](bytes.data() + (seed & 7U), state);
    state.Flush(0x7FFFU, true);
    Hash hash;
    for (int r = 0; r < 15; ++r) hash.Add(guest.r[r]);
    for (uint8_t b : bytes) hash.Add(b);
    return hash.h;
}

uint32_t SeqCount() { return sizeof(kSeqFunctions) / sizeof(kSeqFunctions[0]); }

// Runs whose image took the fastmem window (current side).
uint64_t gWindowRuns = 0;
uint64_t WindowRuns() { return gWindowRuns; }

// Runs function `index` with state and configuration derived from `seed` and
// returns the digest of everything observable.
uint64_t Run(uint32_t index, uint64_t seed) {
#if defined(SEQ_MONITOR)
#if defined(SEQ_OLD)
    static const bool active = [] {
        SEQ_MONITOR::SetActive(true);
        return true;
    }();
    static_cast<void>(active);
#endif
    SEQ_MONITOR::ResetExclusiveMonitor();
#endif
    std::mt19937_64 rng(seed);
    auto below = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
    Memory memory;
    std::vector<uint8_t> ram(0x3000), rom(0x1000), ram2(0x2000);
    for (auto& b : ram) b = uint8_t(rng());
    for (auto& b : rom) b = uint8_t(rng());
    for (auto& b : ram2) b = uint8_t(rng());
    if (!memory.MapRegion({"ram", 0x10000U, ram.size(), true, false, ram}) ||
        !memory.MapRegion({"rom", 0x13000U, rom.size(), false, true, rom}) ||
        !memory.MapRegion({"ram2", 0x20000U, ram2.size(), true, false, ram2})) {
        return 0;
    }
#if !defined(SEQ_OLD)
    if (memory.InGuestWindow()) ++gWindowRuns;
#endif
    Hash events;
    const uint32_t mode = below(12);
    if (mode == 0) {
        memory.EnableWriteTraceFingerprint(true);
    } else if (mode == 1) {
        memory.SetWriteObserver([&events](uint32_t address, size_t size) {
            events.Add(address);
            events.Add(size);
        });
        memory.ObserveWrites(0x10000U + below(3) * 0x1000U + below(0x1000), 1 + below(64));
    } else if (mode == 2) {
        const std::pair<uint32_t, uint32_t> range{0x10F00U + below(0x200), 0x11100U + below(0x200)};
        memory.SetRangeWatch(std::span(&range, 1));
    }
    // A few prior writes: non-trivial generations and stamps.
    for (uint32_t i = 0, n = below(8); i < n; ++i) {
        memory.WriteFast<uint32_t>(0x10000U + below(0x3000 / 4) * 4U, uint32_t(rng()));
    }
    a32::GuestState guest{};
    for (int r = 0; r < 8; ++r) {
        const uint32_t kind = below(3);
        guest.r[r] = kind == 0 ? uint32_t(rng()) : kind == 1 ? below(256)
                                                             : kSeqBases[below(kSeqBaseCount)];
    }
    for (int r = 8; r < 13; ++r) guest.r[r] = kSeqBases[below(kSeqBaseCount)] + below(24);
    guest.r[13] = 0x12000U;
    guest.r[14] = 0x00400000U;
    guest.cpsr = 0x10U | (uint32_t(rng()) & 0xF0000000U);
    Oot3dAotArchitecturalState state(guest);
    Observations observations;
    uint32_t faultPc = 0, faultAddress = 0;
    uint64_t token = 0;
    kSeqFunctions[index](memory, state, observations, faultPc, faultAddress, token);
    state.Flush(0x7FFFU, true);
    Hash hash;
    for (int r = 0; r < 15; ++r) hash.Add(guest.r[r]);
    hash.Add(guest.cpsr);
    hash.Add(faultPc);
    hash.Add(faultAddress);
    hash.Add(token);
    hash.Add(observations.h);
    hash.Add(events.h);
    hash.Add(memory.ContentFingerprint());
    hash.Add(memory.WriteGeneration());
    hash.Add(memory.WriteTraceFingerprint());
    hash.Add(memory.FastReadMismatchCount());
    for (uint32_t page = 0x10000U; page < 0x22000U; page += 0x1000U) {
        hash.Add(memory.RangeWriteGeneration(page, 0x1000U).value_or(0xDEADU));
    }
    const auto& watch = memory.WatchCounters();
    hash.Add(watch.TypedReads);
    hash.Add(watch.TypedWrites);
    hash.Add(watch.FirstWriteAddress);
    return hash.h;
}

} // namespace SEQ_SIDE
