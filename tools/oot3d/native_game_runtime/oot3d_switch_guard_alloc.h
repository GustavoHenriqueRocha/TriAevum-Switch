#pragma once

// Guard-page allocator (heap corruption diagnostics; Switch).
//
// The corrupted heap in tours 16 to 20 is always the same: the free 0x50-byte
// chunk (bin 10) at 0x27fe173a0 with its bk (byte 8 of the user block)
// replaced by 0x00ed5391'0000001a and the fd intact. An 8-byte write into a
// 57-to-72-byte object that had already been freed (use after free), and
// deterministic.
//
// Two modes (5th field of config/guard_alloc):
//
// "canary" (default): the block is the object with a 384-byte header before
// it (ordinary allocation, no kernel call). On free the header keeps a copy
// of the object's bytes (the object stays as it was) and the block enters a
// long FIFO quarantine (131072 blocks, ~60 MiB); when it leaves, the object is
// compared with the copy: if any byte changed, logs/hang.txt gets "USE AFTER
// FREE (canary)" with who allocated, who freed, which bytes changed (from/to).
// It does not give the PC of the write, but gives the object's owner. A double
// free becomes "DOUBLE FREE" in the same file. The 1st version (p6t) filled the
// object with 0xFD on free: a read after the free, which previously saw the old
// data and carried on, began reading garbage and the whole console hung (26/10).
// The copy keeps reads as they are without the guard and only flags writes;
// the canary requires max <= 128.
//
// "page": the mode below, with the exact PC, but each freed block closes a
// page and splits a kernel memory block. With 32768 in quarantine (26/10) the
// process's kernel ran out of those blocks and SDL audio (and FS) failed on
// the first frame; the page-mode quarantine is capped at kMaxPageQuarantine.
//
// Page mode: with config/guard_alloc on the SD, from the start of the game session (Arm, in
// HangWatch::Start; boot is excluded: with the guard active from the constructor,
// boot ran out of memory, "Not enough space", on 26/10), every malloc request
// with size in the configured range (default 57..72) gets its own pages:
//   [page 0: header ... object at the end][page 1: slack]
// On free page 0 loses access (svcSetMemoryPermission) and the block enters
// a FIFO quarantine; it only returns to the real malloc when it leaves. A
// write (or read) into a freed block becomes an exception at the exact PC; the
// exception handler writes to logs/hang.txt the PC, the fault address, the
// allocation and free stacks of the hit block and the thread state
// (oot3d_hang_watchdog). Page 1 exists because the header of newlib's next
// chunk sits 16 bytes before its aligned pointer: without it, it would land
// in page 0 (no access) of the previous block. There is no guard page for
// overflow: the corruption seen is use after free (fd intact).
//
// Guard-page idea: TheMoonPeople/Chronicle PR #205 (Switch port arenas with
// guard pages via svcSetMemoryPermission).
//
// Linked with -Wl,--wrap=_malloc_r (and _memalign_r, _free_r, _realloc_r,
// _calloc_r, _malloc_usable_size_r): in devkitA64's newlib each lives in its
// own object, so malloc/free/new/delete of the whole process go through
// here. devkitA64's malloc() sends every request of 32 bytes or more to
// _memalign_r(32, n), so the object is 32-aligned and _memalign_r
// (alignment up to 32) also gets a guard.
// Cost: 8 KiB per block in range and a kernel call on free; diagnostics
// only.
//
// config/guard_alloc: "min max quarantine live mode" (bytes, bytes, blocks,
// blocks, canary|page); empty = 57 72 131072 32768 canary. The quarantine
// holds freed blocks: the larger it is, the later a use after free is still
// caught (the game frees ~2900 blocks of 57-72 bytes per second). With `live`
// guarded blocks alive, subsequent requests go to normal malloc until
// one is freed.

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame::GuardAlloc {

inline constexpr size_t kPageSize = 0x1000U;
inline constexpr size_t kBlockPages = 2U;
inline constexpr size_t kBlockBytes = kBlockPages * kPageSize;
inline constexpr uint64_t kMagic = 0x434c414452415547ULL;  // "GUARDALC"
inline constexpr size_t kStackDepth = 12U;
// Object alignment: that of devkitA64's malloc() (_memalign_r(32, n)).
inline constexpr size_t kAlignment = 32U;

enum class Mode : uint8_t { Canary, Page };

// Page mode: freed blocks with the page closed at the same time (each one
// splits a kernel memory block; 32768 exhausted the process).
inline constexpr size_t kMaxPageQuarantine = 4096U;

struct Config {
    size_t MinSize = 57U;
    size_t MaxSize = 72U;
    size_t QuarantineBlocks = 131072U;
    size_t MaxLive = 32768U;
    Mode GuardMode = Mode::Canary;
};

// "min max quarantine live mode"; missing fields keep the default. False = invalid
// text (bad number, unknown mode, min > max, min 0 or max that does not
// fit in page 0). In page mode the quarantine is capped at
// kMaxPageQuarantine.
bool ParseConfig(const char* text, Config& config) noexcept;

// Header at the start of page 0 (read back by free and by the handler).
struct BlockHeader {
    uint64_t Magic;
    uint64_t Size;
    uint64_t Serial;
    uint64_t AllocTicks;
    uint64_t AllocStack[kStackDepth];
};

constexpr bool InRange(const Config& config, size_t size) noexcept {
    return size >= config.MinSize && size <= config.MaxSize &&
           size <= kPageSize - sizeof(BlockHeader);
}

// Object offset in page 0: as close to the end as 32-alignment
// allows (far from the header).
constexpr size_t ObjectOffset(size_t size) noexcept {
    return (kPageSize - size) & ~(kAlignment - 1U);
}

// Usable bytes (malloc_usable_size): up to the end of page 0.
constexpr size_t UsableSize(size_t size) noexcept {
    return kPageSize - ObjectOffset(size);
}

// Quarantined block: what the handler needs to tell who allocated and
// who freed the hit address.
struct QuarantineEntry {
    uintptr_t Page = 0;  // start of page 0 (= memalign pointer)
    uint64_t Size = 0;
    uint64_t Serial = 0;
    uint64_t AllocTicks = 0;
    uint64_t FreeTicks = 0;
    uint32_t FreeThread = 0;
    uint64_t AllocStack[kStackDepth] = {};
    uint64_t FreeStack[kStackDepth] = {};
};

// Fixed-capacity FIFO over caller memory (no allocation).
// Canary mode: header right before the object (ordinary allocation).
inline constexpr uint64_t kCanaryMagic = 0x5952414e41434c47ULL;       // "GLCANARY"
inline constexpr uint64_t kCanaryFreedMagic = 0x4545524659524143ULL;  // "CARYFREE"
inline constexpr uint64_t kCanaryTailMagic = 0x4c494154594e4143ULL;   // "CANYTAIL"
inline constexpr size_t kCanarySnapshotBytes = 128U;

struct CanaryHeader {
    BlockHeader Block;
    uint64_t FreeTicks;
    uint64_t FreeThread;
    uint64_t FreeStack[kStackDepth];
    uint8_t Snapshot[kCanarySnapshotBytes];  // the object at free time
    uint64_t Padding;
    // Right before the object (ptr - 8): in an ordinary block this holds newlib's
    // chunk size, always readable. Only with this mark does free read the rest of
    // the header at ptr - 384: reading there directly hit an unmapped page (p6t).
    uint64_t TailMagic;
};
static_assert(sizeof(CanaryHeader) % 32U == 0U, "object aligned to 32");

// Spans of the freed object that differ from the free-time copy: up to `capacity`
// [begin, end) pairs. Returns how many it found (and counts all in `changedBytes`).
struct ChangedRange {
    size_t Begin;
    size_t End;
};
inline size_t FindCanaryChanges(const uint8_t* object, const uint8_t* snapshot, size_t size,
                                ChangedRange* ranges, size_t capacity,
                                size_t& changedBytes) noexcept {
    size_t count = 0;
    changedBytes = 0;
    for (size_t index = 0; index < size; ++index) {
        if (object[index] == snapshot[index]) continue;
        ++changedBytes;
        if (count > 0U && ranges[count - 1U].End == index) {
            ranges[count - 1U].End = index + 1U;
        } else if (count < capacity) {
            ranges[count++] = {index, index + 1U};
        }
    }
    return count;
}

// Pointer FIFO (canary-mode quarantine: the rest lives in the header).
class PointerRing {
  public:
    PointerRing() noexcept = default;
    PointerRing(uintptr_t* storage, size_t capacity) noexcept
        : mEntries(storage), mCapacity(capacity) {}

    // Stores `value`; if it was full, returns the oldest in `evicted`.
    bool Push(uintptr_t value, uintptr_t& evicted) noexcept {
        if (mCapacity == 0U) {
            evicted = value;
            return true;
        }
        const bool full = mCount == mCapacity;
        if (full) {
            evicted = mEntries[mHead];
            mHead = (mHead + 1U) % mCapacity;
            --mCount;
        }
        mEntries[(mHead + mCount) % mCapacity] = value;
        ++mCount;
        return full;
    }

    [[nodiscard]] size_t Count() const noexcept { return mCount; }

  private:
    uintptr_t* mEntries = nullptr;
    size_t mCapacity = 0U;
    size_t mHead = 0U;
    size_t mCount = 0U;
};

class QuarantineRing {
  public:
    QuarantineRing() noexcept = default;
    QuarantineRing(QuarantineEntry* storage, size_t capacity) noexcept
        : mEntries(storage), mCapacity(capacity) {}

    // Stores `entry`; if it was full, returns the oldest in `evicted`.
    bool Push(const QuarantineEntry& entry, QuarantineEntry& evicted) noexcept {
        if (mCapacity == 0U) {
            evicted = entry;
            return true;
        }
        bool full = mCount == mCapacity;
        if (full) {
            evicted = mEntries[mHead];
            mHead = (mHead + 1U) % mCapacity;
            --mCount;
        }
        mEntries[(mHead + mCount) % mCapacity] = entry;
        ++mCount;
        return full;
    }

    // Block whose pages contain `address` (linear search; handler only).
    const QuarantineEntry* Find(uintptr_t address) const noexcept {
        for (size_t index = 0; index < mCount; ++index) {
            const QuarantineEntry& entry = mEntries[(mHead + index) % mCapacity];
            if (address >= entry.Page && address < entry.Page + kBlockBytes) return &entry;
        }
        return nullptr;
    }

    [[nodiscard]] size_t Count() const noexcept { return mCount; }

  private:
    QuarantineEntry* mEntries = nullptr;
    size_t mCapacity = 0U;
    size_t mHead = 0U;
    size_t mCount = 0U;
};

struct Stats {
    bool Enabled = false;
    uint64_t Guarded = 0;    // guarded blocks created
    uint64_t Live = 0;       // alive now
    uint64_t Quarantined = 0;
    uint64_t Fallbacks = 0;  // memalign or svc failed: went to normal malloc
    uint64_t Skipped = 0;    // live cap: went to normal malloc
    uint64_t CanaryHits = 0; // freed blocks that were written (canary mode)
    uint64_t DoubleFrees = 0;
    bool Canary = false;
};

// Reads config/guard_alloc (Switch; constructor, before the game). Does not enable.
void Start() noexcept;
// Enables, if the file existed (start of the game session, after boot).
void Arm() noexcept;
[[nodiscard]] Stats CurrentStats() noexcept;

} // namespace Oot3dNativeGame::GuardAlloc
