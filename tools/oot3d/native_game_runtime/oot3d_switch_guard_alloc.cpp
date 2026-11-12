// Guard-page allocator: see oot3d_switch_guard_alloc.h.
#include "oot3d_switch_guard_alloc.h"

#include <cstdlib>
#include <cstring>

namespace Oot3dNativeGame::GuardAlloc {

namespace {

// Next unsigned number in `text` (skips spaces); false if there is no number
// or it is invalid. `text` advances.
bool NextNumber(const char*& text, size_t& value, bool& found) noexcept {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    found = false;
    if (*text == '\0' || (*text >= 'a' && *text <= 'z')) return true;  // end or the mode
    if (*text < '0' || *text > '9') return false;
    size_t parsed = 0;
    while (*text >= '0' && *text <= '9') {
        const size_t digit = static_cast<size_t>(*text - '0');
        if (parsed > (static_cast<size_t>(-1) - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        ++text;
    }
    value = parsed;
    found = true;
    return true;
}

} // namespace

bool ParseConfig(const char* text, Config& config) noexcept {
    Config parsed;
    if (text != nullptr) {
        size_t* const fields[] = {&parsed.MinSize, &parsed.MaxSize, &parsed.QuarantineBlocks,
                                  &parsed.MaxLive};
        for (size_t* field : fields) {
            bool found = false;
            if (!NextNumber(text, *field, found)) return false;
            if (!found) break;
        }
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
        const char* word = text;
        while (*text >= 'a' && *text <= 'z') ++text;
        const size_t length = static_cast<size_t>(text - word);
        if (length == 6U && std::strncmp(word, "canary", 6) == 0) {
            parsed.GuardMode = Mode::Canary;
        } else if (length == 4U && std::strncmp(word, "page", 4) == 0) {
            parsed.GuardMode = Mode::Page;
        } else if (length != 0U) {
            return false;
        }
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
        if (*text != '\0') return false;
    }
    if (parsed.GuardMode == Mode::Page && parsed.QuarantineBlocks > kMaxPageQuarantine) {
        parsed.QuarantineBlocks = kMaxPageQuarantine;
    }
    if (parsed.GuardMode == Mode::Canary && parsed.MaxSize > kCanarySnapshotBytes) return false;
    if (parsed.MinSize == 0U || parsed.MinSize > parsed.MaxSize ||
        !InRange(parsed, parsed.MaxSize)) {
        return false;
    }
    config = parsed;
    return true;
}

} // namespace Oot3dNativeGame::GuardAlloc

#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <reent.h>

#include "oot3d_hang_watchdog.h"
#include "oot3d_switch_guest_window.h"

extern "C" {
void* __real__malloc_r(struct _reent* reent, size_t size);
void __real__free_r(struct _reent* reent, void* pointer);
void* __real__realloc_r(struct _reent* reent, void* pointer, size_t size);
void* __real__calloc_r(struct _reent* reent, size_t count, size_t size);
size_t __real__malloc_usable_size_r(struct _reent* reent, void* pointer);
void* __real__memalign_r(struct _reent* reent, size_t alignment, size_t size);
extern char* fake_heap_start;
extern char* fake_heap_end;
}

namespace Oot3dNativeGame::GuardAlloc {
namespace {

constexpr const char* kConfigPath = "sdmc:/switch/oot3dre/config/guard_alloc";

Config gConfig;
std::atomic<bool> gConfigured{false};  // config/guard_alloc read
std::atomic<bool> gEnabled{false};
std::atomic<bool> gUsed{false};  // some guarded block already exists
std::atomic<uint64_t> gSerial{0};
std::atomic<uint64_t> gGuarded{0};
std::atomic<uint64_t> gLive{0};
std::atomic<uint64_t> gFallbacks{0};
std::atomic<uint64_t> gSkipped{0};
std::atomic<uint64_t> gCanaryHits{0};
std::atomic<uint64_t> gDoubleFrees{0};
std::atomic<uint32_t> gCanaryReports{0};
constexpr uint32_t kMaximumCanaryReports = 4U;
Mutex gRingMutex;
QuarantineRing gRing;        // page mode
PointerRing gCanaryRing;     // canary mode

// newlib calls that internally call the wrapped _malloc_r: the real
// calloc/realloc read the header of the chunk malloc returned, which
// in a guarded block is garbage. Inside them the wrapped malloc goes straight to the real one.
thread_local bool tBypass = false;

// Thread stack bounds (to follow frame pointers without leaving it).
thread_local uintptr_t tStackBegin = 0;
thread_local uintptr_t tStackEnd = 0;

[[gnu::noinline]] void CaptureStack(uint64_t (&out)[kStackDepth]) noexcept {
    uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (tStackEnd == 0U) {
        MemoryInfo info{};
        u32 page = 0;
        tStackEnd = 1U;  // nothing readable: do not follow
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, frame)) && (info.perm & Perm_R) != 0) {
            tStackBegin = info.addr;
            tStackEnd = info.addr + info.size;
        }
    }
    size_t count = 0;
    while (count < kStackDepth) {
        if ((frame & 7U) != 0U || frame < tStackBegin || frame + 16U > tStackEnd) break;
        const uintptr_t next = reinterpret_cast<const uintptr_t*>(frame)[0];
        out[count++] = reinterpret_cast<const uint64_t*>(frame)[1];
        if (next <= frame) break;
        frame = next;
    }
    for (; count < kStackDepth; ++count) out[count] = 0;
}

BlockHeader* HeaderOf(void* pointer) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t offset = address & (kPageSize - 1U);
    if ((offset & (kAlignment - 1U)) != 0U || offset < sizeof(BlockHeader)) return nullptr;
    auto* header = reinterpret_cast<BlockHeader*>(address - offset);
    if (header->Magic != kMagic || ObjectOffset(header->Size) != offset) return nullptr;
    return header;
}

// ---- canary mode -------------------------------------------------------------

CanaryHeader* CanaryOf(void* pointer) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if ((address & (kAlignment - 1U)) != 0U ||
        address < reinterpret_cast<uintptr_t>(fake_heap_start) + sizeof(CanaryHeader) ||
        address >= reinterpret_cast<uintptr_t>(fake_heap_end)) {
        return nullptr;
    }
    // ptr - 8 belongs to the chunk itself (newlib size in an ordinary block): readable.
    if (reinterpret_cast<const uint64_t*>(address)[-1] != kCanaryTailMagic) return nullptr;
    auto* header = reinterpret_cast<CanaryHeader*>(address - sizeof(CanaryHeader));
    const uint64_t magic = header->Block.Magic;
    if ((magic != kCanaryMagic && magic != kCanaryFreedMagic) ||
        header->Block.Size < gConfig.MinSize || header->Block.Size > gConfig.MaxSize) {
        return nullptr;
    }
    return header;
}

void* CanaryAlloc(struct _reent* reent, size_t size) noexcept {
    // The malloc the real memalign does internally (256+ bytes) falls outside the range.
    void* const base = __real__memalign_r(reent, kAlignment, sizeof(CanaryHeader) + size);
    if (base == nullptr) {
        gFallbacks.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    auto* const header = static_cast<CanaryHeader*>(base);
    header->Block.Magic = kCanaryMagic;
    header->TailMagic = kCanaryTailMagic;
    header->Block.Size = size;
    header->Block.Serial = gSerial.fetch_add(1U, std::memory_order_relaxed) + 1U;
    header->Block.AllocTicks = armGetSystemTick();
    CaptureStack(header->Block.AllocStack);
    gUsed.store(true, std::memory_order_relaxed);
    gGuarded.fetch_add(1U, std::memory_order_relaxed);
    gLive.fetch_add(1U, std::memory_order_relaxed);
    return header + 1;
}

void AppendStack(HangWatch::Text& text, const char* label, const uint64_t (&stack)[kStackDepth]);

// One canary report at a time; a concurrent one is only counted.
std::atomic<bool> gCanaryWriting{false};
char gCanaryDetails[8 * 1024];

void AppendBlockLine(HangWatch::Text& text, const CanaryHeader& header) {
    const uint64_t now = armGetSystemTick();
    text.Str("block serial=").Dec(header.Block.Serial).Str(" size=").Dec(header.Block.Size)
        .Str(" object=").Hex(reinterpret_cast<uintptr_t>(&header + 1))
        .Str(" lifetime_ms=").Dec(armTicksToNs(header.FreeTicks - header.Block.AllocTicks) / 1000000U)
        .Str(" freed_ago_ms=").Dec(armTicksToNs(now - header.FreeTicks) / 1000000U)
        .Str(" free_thread=").Hex(header.FreeThread).Char('\n');
}

void ReportCanary(const char* title, const CanaryHeader& header, const ChangedRange* ranges,
                  size_t rangeCount, size_t changedBytes, bool withCurrentStack) {
    if (gCanaryReports.load(std::memory_order_relaxed) >= kMaximumCanaryReports) return;
    bool expected = false;
    if (!gCanaryWriting.compare_exchange_strong(expected, true, std::memory_order_acquire)) return;
    gCanaryReports.fetch_add(1U, std::memory_order_relaxed);
    HangWatch::Text text(gCanaryDetails, sizeof(gCanaryDetails));
    AppendBlockLine(text, header);
    if (rangeCount > 0U) {
        const auto* object = reinterpret_cast<const uint8_t*>(&header + 1);
        text.Str("changed: bytes=").Dec(changedBytes);
        for (size_t index = 0; index < rangeCount; ++index) {
            text.Str(" [").Dec(ranges[index].Begin).Char(',').Dec(ranges[index].End).Str("):");
            for (size_t byte = ranges[index].Begin;
                 byte < ranges[index].End && byte < ranges[index].Begin + 16U; ++byte) {
                text.Char(' ').Hex(header.Snapshot[byte]).Str("->").Hex(object[byte]);
            }
        }
        text.Char('\n');
    }
    AppendStack(text, "alloc_stack:", header.Block.AllocStack);
    AppendStack(text, "free_stack:", header.FreeStack);
    if (withCurrentStack) {
        uint64_t current[kStackDepth];
        CaptureStack(current);
        AppendStack(text, "free_again_stack:", current);
    }
    HangWatch::ReportEvent(title, text.Data());
    gCanaryWriting.store(false, std::memory_order_release);
}

// Leaves quarantine: checks the fill and returns it to the real malloc.
void CanaryRelease(struct _reent* reent, CanaryHeader* header) noexcept {
    ChangedRange ranges[8];
    size_t changedBytes = 0;
    const size_t rangeCount =
        FindCanaryChanges(reinterpret_cast<const uint8_t*>(header + 1), header->Snapshot,
                          header->Block.Size, ranges, 8U, changedBytes);
    if (changedBytes != 0U) {
        gCanaryHits.fetch_add(1U, std::memory_order_relaxed);
        ReportCanary("USE AFTER FREE (canary)", *header, ranges, rangeCount, changedBytes, false);
    }
    header->Block.Magic = 0;
    header->TailMagic = 0;
    __real__free_r(reent, header);
}

void CanaryFree(struct _reent* reent, CanaryHeader* header) noexcept {
    if (header->Block.Magic == kCanaryFreedMagic) {
        // Already in quarantine: the second free returns nothing.
        gDoubleFrees.fetch_add(1U, std::memory_order_relaxed);
        ReportCanary("DOUBLE FREE (canary)", *header, nullptr, 0U, 0U, true);
        return;
    }
    header->Block.Magic = kCanaryFreedMagic;
    header->FreeTicks = armGetSystemTick();
    header->FreeThread = threadGetCurHandle();
    CaptureStack(header->FreeStack);
    // Copy, not fill: reads after the free keep seeing the
    // old data (with 0xFD the whole console hung).
    std::memcpy(header->Snapshot, header + 1, header->Block.Size);
    gLive.fetch_sub(1U, std::memory_order_relaxed);
    uintptr_t evicted = 0;
    mutexLock(&gRingMutex);
    const bool full = gCanaryRing.Push(reinterpret_cast<uintptr_t>(header), evicted);
    mutexUnlock(&gRingMutex);
    // Outside the lock: the real free takes the malloc lock.
    if (full) CanaryRelease(reent, reinterpret_cast<CanaryHeader*>(evicted));
}

// ---- page mode ---------------------------------------------------------------

void* GuardedAlloc(struct _reent* reent, size_t size) noexcept {
    if (gLive.load(std::memory_order_relaxed) >= gConfig.MaxLive) {
        gSkipped.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    if (gConfig.GuardMode == Mode::Canary) return CanaryAlloc(reent, size);
    // The malloc the real memalign does internally (12 KiB+) falls outside the range.
    void* const base = __real__memalign_r(reent, kPageSize, kBlockBytes);
    if (base == nullptr) {
        gFallbacks.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    auto* const header = static_cast<BlockHeader*>(base);
    header->Magic = kMagic;
    header->Size = size;
    header->Serial = gSerial.fetch_add(1U, std::memory_order_relaxed) + 1U;
    header->AllocTicks = armGetSystemTick();
    CaptureStack(header->AllocStack);
    gUsed.store(true, std::memory_order_relaxed);
    gGuarded.fetch_add(1U, std::memory_order_relaxed);
    gLive.fetch_add(1U, std::memory_order_relaxed);
    return static_cast<u8*>(base) + ObjectOffset(size);
}

// Returns the pages to the real malloc.
void Release(struct _reent* reent, uintptr_t page) noexcept {
    svcSetMemoryPermission(reinterpret_cast<void*>(page), kPageSize, Perm_Rw);
    reinterpret_cast<BlockHeader*>(page)->Magic = 0;
    __real__free_r(reent, reinterpret_cast<void*>(page));
}

void GuardedFree(struct _reent* reent, BlockHeader* header) noexcept {
    QuarantineEntry entry;
    entry.Page = reinterpret_cast<uintptr_t>(header);
    entry.Size = header->Size;
    entry.Serial = header->Serial;
    entry.AllocTicks = header->AllocTicks;
    entry.FreeTicks = armGetSystemTick();
    entry.FreeThread = threadGetCurHandle();
    std::memcpy(entry.AllocStack, header->AllocStack, sizeof(entry.AllocStack));
    CaptureStack(entry.FreeStack);
    gLive.fetch_sub(1U, std::memory_order_relaxed);
    if (R_FAILED(svcSetMemoryPermission(header, kPageSize, Perm_None))) {
        // No free memory blocks in the kernel, for example.
        gFallbacks.fetch_add(1U, std::memory_order_relaxed);
        Release(reent, entry.Page);
        return;
    }
    QuarantineEntry evicted;
    mutexLock(&gRingMutex);
    const bool full = gRing.Push(entry, evicted);
    mutexUnlock(&gRingMutex);
    // Outside the lock: the real free takes the malloc lock.
    if (full) Release(reent, evicted.Page);
}

void AppendStack(HangWatch::Text& text, const char* label, const uint64_t (&stack)[kStackDepth]) {
    text.Str(label);
    for (uint64_t address : stack) {
        if (address == 0U) break;
        text.Char(' ');
        HangWatch::AppendCodeAddress(text, address);
    }
    text.Char('\n');
}

// Classifies the fault address against the guarded blocks. No lock: the
// faulting thread may hold it (free); the ring only grows up to capacity.
void AppendGuardVerdict(HangWatch::Text& text, uintptr_t address) {
    if (const QuarantineEntry* entry = gRing.Find(address)) {
        const uintptr_t object = entry->Page + ObjectOffset(entry->Size);
        text.Str("USE AFTER FREE: block serial=").Dec(entry->Serial).Str(" size=")
            .Dec(entry->Size).Str(" object=").Hex(object).Str(" offset=")
            .SignedDec(static_cast<int64_t>(address) - static_cast<int64_t>(object))
            .Str(" freed_ago_ms=")
            .Dec(armTicksToNs(armGetSystemTick() - entry->FreeTicks) / 1000000U)
            .Str(" lifetime_ms=").Dec(armTicksToNs(entry->FreeTicks - entry->AllocTicks) / 1000000U)
            .Str(" free_thread=").Hex(entry->FreeThread).Char('\n');
        AppendStack(text, "alloc_stack:", entry->AllocStack);
        AppendStack(text, "free_stack:", entry->FreeStack);
        return;
    }
    text.Str("outside guarded blocks\n");
}

char gFaultDetails[8 * 1024];

void HandleException(ThreadExceptionDump* context) {
    HangWatch::Text text(gFaultDetails, sizeof(gFaultDetails));
    text.Str("type=").Hex(context->error_desc).Str(" far=").Hex(context->far.x)
        .Str(" esr=").Hex(context->esr).Char('\n');
    if (gUsed.load(std::memory_order_relaxed) && gConfig.GuardMode == Mode::Page) {
        AppendGuardVerdict(text, context->far.x);
    }
    const Stats stats = CurrentStats();
    text.Str("guard_alloc=").Dec(stats.Enabled ? 1U : 0U).Str(" range=").Dec(gConfig.MinSize)
        .Char('-').Dec(gConfig.MaxSize).Str(" created=").Dec(stats.Guarded)
        .Str(" live=").Dec(stats.Live).Str(" quarantine=").Dec(stats.Quarantined)
        .Str(" fallbacks=").Dec(stats.Fallbacks).Str(" cap=").Dec(stats.Skipped);
    HangWatch::FaultFrame frame;
    frame.Pc = context->pc.x;
    frame.Lr = context->lr.x;
    frame.Sp = context->sp.x;
    frame.Fp = context->fp.x;
    frame.X0 = context->cpu_gprs[0].x;
    frame.X1 = context->cpu_gprs[1].x;
    frame.X2 = context->cpu_gprs[2].x;
    frame.X19 = context->cpu_gprs[19].x;
    HangWatch::ReportFault("EXCEPTION", text.Data(), frame);
}

// Before the game's constructors (libnx already mounted sdmc in __appInit) and
// after the code moved to 2 MB blocks (oot3d_switch_code_pages.cpp, 101).
[[gnu::constructor(102)]] void StartEarly() {
    Start();
    // The wrappers below place the fastmem window's region buffers.
    Oot3dNativeGame::GuestWindow::MarkAllocatorHooked();
}

} // namespace

void Start() noexcept {
    if (gConfigured.load(std::memory_order_acquire)) return;
    std::FILE* file = std::fopen(kConfigPath, "rb");
    if (file == nullptr) return;
    char text[128] = {};
    const size_t read = std::fread(text, 1, sizeof(text) - 1U, file);
    std::fclose(file);
    text[read] = '\0';
    Config config;
    if (!ParseConfig(text, config)) return;
    mutexInit(&gRingMutex);
    if (config.GuardMode == Mode::Canary) {
        auto* storage = static_cast<uintptr_t*>(
            __real__malloc_r(_REENT, sizeof(uintptr_t) * config.QuarantineBlocks));
        if (storage == nullptr && config.QuarantineBlocks != 0U) return;
        gCanaryRing = PointerRing(storage, config.QuarantineBlocks);
    } else {
        auto* storage = static_cast<QuarantineEntry*>(
            __real__malloc_r(_REENT, sizeof(QuarantineEntry) * config.QuarantineBlocks));
        if (storage == nullptr && config.QuarantineBlocks != 0U) return;
        for (size_t index = 0; index < config.QuarantineBlocks; ++index) {
            new (&storage[index]) QuarantineEntry();
        }
        gRing = QuarantineRing(storage, config.QuarantineBlocks);
    }
    gConfig = config;
    gConfigured.store(true, std::memory_order_release);
}

void Arm() noexcept {
    if (gConfigured.load(std::memory_order_acquire)) {
        gEnabled.store(true, std::memory_order_release);
    }
}

Stats CurrentStats() noexcept {
    Stats stats;
    stats.Enabled = gEnabled.load(std::memory_order_relaxed);
    stats.Guarded = gGuarded.load(std::memory_order_relaxed);
    stats.Live = gLive.load(std::memory_order_relaxed);
    stats.Canary = gConfig.GuardMode == Mode::Canary;
    stats.Quarantined = stats.Canary ? gCanaryRing.Count() : gRing.Count();
    stats.CanaryHits = gCanaryHits.load(std::memory_order_relaxed);
    stats.DoubleFrees = gDoubleFrees.load(std::memory_order_relaxed);
    stats.Fallbacks = gFallbacks.load(std::memory_order_relaxed);
    stats.Skipped = gSkipped.load(std::memory_order_relaxed);
    return stats;
}

} // namespace Oot3dNativeGame::GuardAlloc

using namespace Oot3dNativeGame::GuardAlloc;

extern "C" {

void* __wrap__malloc_r(struct _reent* reent, size_t size) {
    // fastmem window: the region buffer NativeA32Memory armed for this thread
    // and size (oot3d_switch_guest_window.h); one atomic load otherwise.
    if (void* placed = Oot3dNativeGame::GuestWindow::TakePlacement(size)) return placed;
    // gEnabled before TLS: libnx calls malloc before everything is up.
    if (gEnabled.load(std::memory_order_relaxed) && InRange(gConfig, size) && !tBypass) {
        if (void* pointer = GuardedAlloc(reent, size)) return pointer;
    }
    return __real__malloc_r(reent, size);
}

// devkitA64's malloc(): requests of 32 bytes or more arrive here with alignment 32.
void* __wrap__memalign_r(struct _reent* reent, size_t alignment, size_t size) {
    if (void* placed = Oot3dNativeGame::GuestWindow::TakePlacement(size)) return placed;
    if (gEnabled.load(std::memory_order_relaxed) && alignment <= kAlignment &&
        InRange(gConfig, size) && !tBypass) {
        if (void* pointer = GuardedAlloc(reent, size)) return pointer;
    }
    return __real__memalign_r(reent, alignment, size);
}

void __wrap__free_r(struct _reent* reent, void* pointer) {
    // A window buffer stays mapped; only its placement ends.
    if (Oot3dNativeGame::GuestWindow::Contains(pointer)) {
        Oot3dNativeGame::GuestWindow::ReleaseAllocation(pointer);
        return;
    }
    if (pointer != nullptr && gUsed.load(std::memory_order_relaxed)) {
        if (gConfig.GuardMode == Mode::Canary) {
            if (CanaryHeader* header = CanaryOf(pointer)) {
                CanaryFree(reent, header);
                return;
            }
        } else if (BlockHeader* header = HeaderOf(pointer)) {
            GuardedFree(reent, header);
            return;
        }
    }
    __real__free_r(reent, pointer);
}

void* __wrap__calloc_r(struct _reent* reent, size_t count, size_t size) {
    if (!gEnabled.load(std::memory_order_relaxed)) return __real__calloc_r(reent, count, size);
    size_t total = 0;
    if (!__builtin_mul_overflow(count, size, &total) && InRange(gConfig, total) && !tBypass) {
        if (void* pointer = GuardedAlloc(reent, total)) {
            std::memset(pointer, 0, total);
            return pointer;
        }
    }
    tBypass = true;
    void* const pointer = __real__calloc_r(reent, count, size);
    tBypass = false;
    return pointer;
}

void* __wrap__realloc_r(struct _reent* reent, void* pointer, size_t size) {
    if (pointer == nullptr) return __wrap__malloc_r(reent, size);
    if (Oot3dNativeGame::GuestWindow::Contains(pointer)) {
        // Never asked of a region buffer; moved to the heap if it is.
        const size_t oldSize = Oot3dNativeGame::GuestWindow::AllocationSize(pointer);
        void* const fresh = __real__malloc_r(reent, size);
        if (fresh == nullptr) return nullptr;
        std::memcpy(fresh, pointer, oldSize < size ? oldSize : size);
        Oot3dNativeGame::GuestWindow::ReleaseAllocation(pointer);
        return fresh;
    }
    if (!gUsed.load(std::memory_order_relaxed) && !gEnabled.load(std::memory_order_relaxed)) {
        return __real__realloc_r(reent, pointer, size);
    }
    const BlockHeader* header = nullptr;
    if (gUsed.load(std::memory_order_relaxed)) {
        if (gConfig.GuardMode == Mode::Canary) {
            if (const CanaryHeader* canary = CanaryOf(pointer)) header = &canary->Block;
        } else {
            header = HeaderOf(pointer);
        }
    }
    const bool guardNew = gEnabled.load(std::memory_order_relaxed) && InRange(gConfig, size);
    if (header == nullptr && !guardNew) {
        tBypass = true;
        void* const moved = __real__realloc_r(reent, pointer, size);
        tBypass = false;
        return moved;
    }
    const size_t oldSize =
        header != nullptr ? header->Size : __real__malloc_usable_size_r(reent, pointer);
    void* const fresh = __wrap__malloc_r(reent, size);
    if (fresh == nullptr) return nullptr;
    std::memcpy(fresh, pointer, oldSize < size ? oldSize : size);
    __wrap__free_r(reent, pointer);
    return fresh;
}

size_t __wrap__malloc_usable_size_r(struct _reent* reent, void* pointer) {
    if (Oot3dNativeGame::GuestWindow::Contains(pointer)) {
        return Oot3dNativeGame::GuestWindow::AllocationSize(pointer);
    }
    if (pointer != nullptr && gUsed.load(std::memory_order_relaxed)) {
        if (gConfig.GuardMode == Mode::Canary) {
            if (const CanaryHeader* header = CanaryOf(pointer)) return header->Block.Size;
        } else if (const BlockHeader* header = HeaderOf(pointer)) {
            return UsableSize(header->Size);
        }
    }
    return __real__malloc_usable_size_r(reent, pointer);
}

// libnx exception handler (always on: writes the report and returns,
// so the crash proceeds as before and Atmosphère still generates the crash report).
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* context) {
    HandleException(context);
}

} // extern "C"

#endif
