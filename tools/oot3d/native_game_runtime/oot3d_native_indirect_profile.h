#pragma once

// Indirect call profile: counts the whole-AOT indirect calls (BLX reg and
// computed ADD pc calls) per call site and target for the whole session.
// Two users, either or both:
//   - config/indirect_profile (off by default): once per logs/perf.txt window
//     an "indirect:" line with the window's calls and hottest pairs
//     (TakeLine, the difference from the previous window);
//   - the PGO generate build (oot3d_whole_aot_pgo.cpp) installs one before
//     guest code runs and writes the session's pairs (CensusText) beside each
//     .stream dump as oot3d_pgo_<N>.indirect.txt;
//     tools/oot3d/native_a32_runtime/whole_aot_indirect_census.py merges them
//     for a later round that guards the hottest targets with direct calls.
// Header-only; tests in oot3d_native_perf_diagnostics_tests.cpp.
//
// The generated dispatcher (ExecuteOot3dWholeAotIndirect, whole_aot_cpp.py)
// records through gOot3dIndirectProfile only while it is non-null, so the
// default build pays one load and one predicted branch per indirect call and
// guest state is never touched. A site is identified by the guest return
// address in LR at the call (BLX: the call pc + 4; computed calls: the resume
// pc that the call returns to).
//
// Record runs on every host thread that executes guest code (the game thread
// on core 0, the second-core thread, oot3d_native_a32_second_core.h) without
// a lock. The pair keys live in one table, claimed by compare-and-swap; the
// counts live in per-thread shards, each written by one thread only (a
// relaxed load and store, no read-modify-write and no shared cache line on
// the hot pairs), and summed by the readers. Threads beyond kThreadShards
// share one more shard updated with atomic adds. Readers (the perf window,
// the PGO dump thread) only load.
//
// Record is never inlined: the dispatcher keeps a call in its rarely taken
// branch instead of the hash and probe loop. That matters for PGO, whose
// generate build always records (the profile says the branch is taken)
// while the use build never does.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <new>
#include <string>
#include <vector>

#if defined(_MSC_VER) && !defined(__clang__)
#define OOT3D_INDIRECT_PROFILE_NOINLINE __declspec(noinline)
#else
#define OOT3D_INDIRECT_PROFILE_NOINLINE __attribute__((noinline))
#endif

namespace Oot3dNativeGame {

class Oot3dIndirectProfile final {
  public:
    // ~2,100 call sites; actor update/action pointers give hundreds of
    // targets at some. Power of two, open addressing; full = dropped.
    static constexpr size_t kSlots = 65536U;
    static constexpr size_t kTop = 16U;
    static constexpr size_t kThreadShards = 4U;

    struct Pair {
        uint32_t Return = 0;
        uint32_t Target = 0;
        uint64_t Count = 0;
    };

    Oot3dIndirectProfile()
        : mKeys(std::make_unique<std::atomic<uint64_t>[]>(kSlots)),
          mShared(std::make_unique<Shard>()),
          mLinePrevious(std::make_unique<uint64_t[]>(kSlots)) {
        for (size_t slot = 0; slot < kSlots; ++slot) mKeys[slot].store(kEmpty, std::memory_order_relaxed);
    }
    ~Oot3dIndirectProfile() {
        for (std::atomic<Shard*>& shard : mShards) delete shard.load(std::memory_order_acquire);
    }
    Oot3dIndirectProfile(const Oot3dIndirectProfile&) = delete;
    Oot3dIndirectProfile& operator=(const Oot3dIndirectProfile&) = delete;

    OOT3D_INDIRECT_PROFILE_NOINLINE void Record(uint32_t returnPc, uint32_t target) noexcept {
        ThreadShard& mine = tThreadShard;
        if (mine.Generation != mGeneration) [[unlikely]] Register(mine);
        Shard& shard = *mine.Owner;
        const bool shared = mine.Owner == mShared.get();
        Bump(shard.Calls, shared);
        const uint64_t key = (uint64_t{returnPc} << 32U) | target;
        if (key == kEmpty) {  // 0xFFFFFFFF>0xFFFFFFFF marks a free slot
            Bump(shard.Dropped, shared);
            return;
        }
        size_t slot = SlotOf(key);
        for (size_t probe = 0; probe < kMaxProbes; ++probe) {
            std::atomic<uint64_t>& cell = mKeys[slot];
            uint64_t seen = cell.load(std::memory_order_acquire);
            if (seen == kEmpty &&
                cell.compare_exchange_strong(seen, key, std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
                mUsed.fetch_add(1U, std::memory_order_relaxed);
                seen = key;
            }
            if (seen == key) {
                Bump(shard.Counts[slot], shared);
                return;
            }
            slot = (slot + 1U) & (kSlots - 1U);
        }
        Bump(shard.Dropped, shared);
    }

    // Session totals so far (every thread).
    [[nodiscard]] uint64_t Calls() const noexcept { return Total(&Shard::Calls); }
    [[nodiscard]] size_t Pairs() const noexcept { return mUsed.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t Dropped() const noexcept { return Total(&Shard::Dropped); }
    // Host threads that have recorded (each got a shard, or the shared one).
    [[nodiscard]] uint32_t Threads() const noexcept { return mThreads.load(std::memory_order_relaxed); }

    // Every pair counted so far, most calls first (ties by return then
    // target, so the order is stable).
    [[nodiscard]] std::vector<Pair> Snapshot() const {
        const Shards shards = LoadShards();
        std::vector<Pair> pairs;
        pairs.reserve(Pairs());
        for (size_t slot = 0; slot < kSlots; ++slot) {
            const uint64_t key = mKeys[slot].load(std::memory_order_acquire);
            if (key == kEmpty) continue;
            const uint64_t count = SlotCount(shards, slot);
            if (count != 0U) pairs.push_back(MakePair(key, count));
        }
        std::sort(pairs.begin(), pairs.end(), Before);
        return pairs;
    }

    // The PGO census file (oot3d_pgo_<N>.indirect.txt): three '#' header
    // lines, then "return target count" per pair (hex, hex, decimal), most
    // calls first. pairs= is the number of rows.
    [[nodiscard]] std::string CensusText() const {
        const uint64_t calls = Calls();
        const uint64_t dropped = Dropped();
        const std::vector<Pair> pairs = Snapshot();
        std::string text;
        text.reserve(160U + pairs.size() * 28U);
        char line[128];
        text += "# oot3d indirect census v1\n";
        std::snprintf(line, sizeof(line), "# calls=%llu pairs=%zu dropped=%llu threads=%u\n",
                      static_cast<unsigned long long>(calls), pairs.size(),
                      static_cast<unsigned long long>(dropped), static_cast<unsigned>(Threads()));
        text += line;
        text += "# return target count\n";
        for (const Pair& pair : pairs) {
            std::snprintf(line, sizeof(line), "%08X %08X %llu\n", static_cast<unsigned>(pair.Return),
                          static_cast<unsigned>(pair.Target),
                          static_cast<unsigned long long>(pair.Count));
            text += line;
        }
        return text;
    }

    // "indirect: calls=N pairs=K dropped=D top=ret>target:count,..." for the
    // window since the previous call (the calls, the distinct pairs called,
    // the calls that found no slot and the 16 hottest pairs; guest addresses
    // in hex). One caller at a time (the perf window).
    std::string TakeLine() {
        const Shards shards = LoadShards();
        const uint64_t calls = Total(&Shard::Calls);
        const uint64_t dropped = Total(&Shard::Dropped);
        std::array<Pair, kTop> top{};
        size_t count = 0U;
        size_t pairs = 0U;
        for (size_t slot = 0; slot < kSlots; ++slot) {
            const uint64_t key = mKeys[slot].load(std::memory_order_acquire);
            if (key == kEmpty) continue;
            const uint64_t total = SlotCount(shards, slot);
            const uint64_t window = total - mLinePrevious[slot];
            mLinePrevious[slot] = total;
            if (window == 0U) continue;
            ++pairs;
            const Pair pair = MakePair(key, window);
            if (count < kTop) {
                top[count++] = pair;
                std::push_heap(top.begin(), top.begin() + count, Before);
            } else if (Before(pair, top.front())) {
                std::pop_heap(top.begin(), top.end(), Before);
                top.back() = pair;
                std::push_heap(top.begin(), top.end(), Before);
            }
        }
        std::sort_heap(top.begin(), top.begin() + count, Before);
        std::string line;
        line.reserve(64U + count * 32U);
        char text[96];
        std::snprintf(text, sizeof(text), "indirect: calls=%llu pairs=%zu dropped=%llu top=",
                      static_cast<unsigned long long>(calls - mLineCalls), pairs,
                      static_cast<unsigned long long>(dropped - mLineDropped));
        line += text;
        for (size_t index = 0; index < count; ++index) {
            std::snprintf(text, sizeof(text), "%s%08X>%08X:%llu", index != 0U ? "," : "",
                          static_cast<unsigned>(top[index].Return),
                          static_cast<unsigned>(top[index].Target),
                          static_cast<unsigned long long>(top[index].Count));
            line += text;
        }
        if (count == 0U) line += '-';
        line += '\n';
        mLineCalls = calls;
        mLineDropped = dropped;
        return line;
    }

  private:
    static constexpr uint64_t kEmpty = ~uint64_t{0};
    static constexpr size_t kMaxProbes = 64U;

    struct Shard {
        std::array<std::atomic<uint64_t>, kSlots> Counts{};
        std::atomic<uint64_t> Calls{0U};
        std::atomic<uint64_t> Dropped{0U};
    };
    using Shards = std::array<const Shard*, kThreadShards + 1U>;

    // Which shard this thread writes, for the profile with that generation
    // (a profile created later at the same address gets a new one). Thread
    // storage starts zeroed: generation 0 is never a profile's.
    struct ThreadShard {
        uint64_t Generation;
        Shard* Owner;
    };
    static inline thread_local ThreadShard tThreadShard;
    static inline std::atomic<uint64_t> sNextGeneration{1U};

    static size_t SlotOf(uint64_t key) noexcept {
        return static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 48U) & (kSlots - 1U);
    }
    static Pair MakePair(uint64_t key, uint64_t count) noexcept {
        return {static_cast<uint32_t>(key >> 32U), static_cast<uint32_t>(key), count};
    }
    static bool Before(const Pair& a, const Pair& b) noexcept {
        if (a.Count != b.Count) return a.Count > b.Count;
        if (a.Return != b.Return) return a.Return < b.Return;
        return a.Target < b.Target;
    }
    // Single writer: a plain increment of an atomic; the shared shard adds.
    static void Bump(std::atomic<uint64_t>& counter, bool shared) noexcept {
        if (shared) {
            counter.fetch_add(1U, std::memory_order_relaxed);
        } else {
            counter.store(counter.load(std::memory_order_relaxed) + 1U, std::memory_order_relaxed);
        }
    }

    // First Record of a thread: its own shard while there are free ones (or
    // the shared one if the allocation fails).
    void Register(ThreadShard& mine) noexcept {
        const uint32_t index = mThreads.fetch_add(1U, std::memory_order_relaxed);
        Shard* owner = mShared.get();
        if (index < kThreadShards) {
            if (Shard* shard = new (std::nothrow) Shard()) {
                mShards[index].store(shard, std::memory_order_release);
                owner = shard;
            }
        }
        mine = {mGeneration, owner};
    }

    Shards LoadShards() const noexcept {
        Shards shards{};
        for (size_t index = 0; index < kThreadShards; ++index)
            shards[index] = mShards[index].load(std::memory_order_acquire);
        shards[kThreadShards] = mShared.get();
        return shards;
    }
    static uint64_t SlotCount(const Shards& shards, size_t slot) noexcept {
        uint64_t count = 0U;
        for (const Shard* shard : shards) {
            if (shard != nullptr) count += shard->Counts[slot].load(std::memory_order_relaxed);
        }
        return count;
    }
    uint64_t Total(std::atomic<uint64_t> Shard::*counter) const noexcept {
        uint64_t total = 0U;
        for (const Shard* shard : LoadShards()) {
            if (shard != nullptr) total += (shard->*counter).load(std::memory_order_relaxed);
        }
        return total;
    }

    const uint64_t mGeneration = sNextGeneration.fetch_add(1U, std::memory_order_relaxed);
    std::unique_ptr<std::atomic<uint64_t>[]> mKeys;  // kEmpty or the pair, never changes once set
    std::array<std::atomic<Shard*>, kThreadShards> mShards{};
    std::unique_ptr<Shard> mShared;
    std::atomic<size_t> mUsed{0U};
    std::atomic<uint32_t> mThreads{0U};
    // TakeLine's previous window (its caller only).
    std::unique_ptr<uint64_t[]> mLinePrevious;
    uint64_t mLineCalls = 0U;
    uint64_t mLineDropped = 0U;
};

// Set before guest code runs (the PGO generate build's start, or the window
// setup with config/indirect_profile) and read by the generated dispatcher on
// every guest thread; threads started later see it through their creation.
inline Oot3dIndirectProfile* gOot3dIndirectProfile = nullptr;

// Publishes a profile to the dispatcher for its lifetime, or shares the one
// already published (the PGO generate build's session profile): the perf
// window's line is a difference between windows either way.
class Oot3dIndirectProfileInstall final {
  public:
    Oot3dIndirectProfileInstall() {
        if (gOot3dIndirectProfile == nullptr) {
            mOwned = std::make_unique<Oot3dIndirectProfile>();
            gOot3dIndirectProfile = mOwned.get();
        }
        mProfile = gOot3dIndirectProfile;
    }
    ~Oot3dIndirectProfileInstall() {
        if (mOwned != nullptr && gOot3dIndirectProfile == mOwned.get()) gOot3dIndirectProfile = nullptr;
    }
    Oot3dIndirectProfileInstall(const Oot3dIndirectProfileInstall&) = delete;
    Oot3dIndirectProfileInstall& operator=(const Oot3dIndirectProfileInstall&) = delete;

    [[nodiscard]] Oot3dIndirectProfile& Profile() noexcept { return *mProfile; }
    // True when it reuses a profile installed before it.
    [[nodiscard]] bool Shared() const noexcept { return mOwned == nullptr; }

  private:
    std::unique_ptr<Oot3dIndirectProfile> mOwned;
    Oot3dIndirectProfile* mProfile = nullptr;
};

} // namespace Oot3dNativeGame
