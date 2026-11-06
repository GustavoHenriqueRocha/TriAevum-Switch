// fastmem: the guest window (see oot3d_switch_guest_window.h).
#include "oot3d_switch_guest_window.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
extern "C" {
// libnx (applet.c, weak, default 0): 1 ends the application through AM.
extern u32 __nx_applet_exit_mode;
}
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#define OOT3D_GUEST_WINDOW_HOST_MMAP 1
#endif

namespace Oot3dNativeGame::GuestWindow {
namespace {

#if defined(__SWITCH__)
constexpr const char* kConfigPath = "sdmc:/switch/oot3dre/config/fastmem_window";
// libnx's mutex (zero-initialised, so usable before any constructor): the
// allocator wrappers take it, as the guard allocator's quarantine ring does.
class Lock {
  public:
    void lock() noexcept { mutexLock(&mMutex); }
    void unlock() noexcept { mutexUnlock(&mMutex); }

  private:
    Mutex mMutex{};
};
uintptr_t ThreadToken() noexcept { return reinterpret_cast<uintptr_t>(armGetTls()); }
#elif defined(OOT3D_GUEST_WINDOW_HOST_MMAP)
using Lock = std::mutex;
thread_local char tThreadToken;
uintptr_t ThreadToken() noexcept { return reinterpret_cast<uintptr_t>(&tThreadToken); }
#else
// No window on this platform (bare-metal test runtimes, Windows): nothing is
// ever armed, the lock only guards the status.
class Lock {
  public:
    void lock() noexcept {
        while (mFlag.test_and_set(std::memory_order_acquire)) {
        }
    }
    void unlock() noexcept { mFlag.clear(std::memory_order_release); }

  private:
    std::atomic_flag mFlag = ATOMIC_FLAG_INIT;
};
uintptr_t ThreadToken() noexcept { return 0U; }
#endif

enum class State : uint8_t { Unstarted, Off, On };

struct Placement {
    uintptr_t Begin = 0;
    size_t Size = 0;
};
// Region buffers of all live images together: 15-25 in the game, one more
// per TLS page of eight threads.
constexpr size_t kMaxPlacements = 4096;

Lock gLock;         // everything below that is not atomic
Lock gReserveLock;  // one armed placement at a time
std::atomic<State> gState{State::Unstarted};
std::atomic<bool> gAccepting{false};
std::atomic<bool> gHooked{false};
std::atomic<bool> gVerify{false};
std::atomic<uintptr_t> gBase{0};
std::atomic<const void*> gOwner{nullptr};
std::atomic<size_t> gLive{0};
std::atomic<uint64_t> gConflicts{0};
std::atomic<uint64_t> gMisses{0};
std::atomic<uint64_t> gPlaced{0};
std::atomic<uint64_t> gShadowChecks{0};
std::atomic<uint64_t> gShadowMismatches{0};
// Armed placement: written in the order thread, size, target (release);
// TakePlacement reads target first (acquire).
std::atomic<uintptr_t> gArmedTarget{0};
std::atomic<size_t> gArmedSize{0};
std::atomic<uintptr_t> gArmedThread{0};

bool gRequested = false;
char gReason[192] = "not started";
const char* gRoute = "";
uintptr_t gReservation = 0;  // start of the raw reservation
Placement gPlacements[kMaxPlacements];
size_t gPlacementCount = 0;
size_t gPlacementBytes = 0;
uint64_t gSlabMapped[kSlabCount / 64U] = {};
size_t gSlabs = 0;
size_t gFailSlabsAfter = SIZE_MAX;
#if defined(__SWITCH__)
Handle gProcess = INVALID_HANDLE;
VirtmemReservation* gVirtmem = nullptr;
#endif

void SetReason(const char* format, unsigned long long value = 0, unsigned long long value2 = 0) {
    std::snprintf(gReason, sizeof(gReason), format, value, value2);
}

bool SlabMapped(size_t index) noexcept {
    return (gSlabMapped[index / 64U] >> (index % 64U)) & 1U;
}

// ---- platform ---------------------------------------------------------------

bool ReadMode(Mode& mode) {
#if defined(__SWITCH__)
    std::FILE* file = std::fopen(kConfigPath, "rb");
    if (file == nullptr) {
        mode = Mode::Off;
        SetReason("config/fastmem_window absent");
        return false;
    }
    char text[64] = {};
    const size_t read = std::fread(text, 1, sizeof(text) - 1U, file);
    std::fclose(file);
    text[read] = '\0';
    mode = ParseMode(text);
    if (mode == Mode::Off) SetReason("config/fastmem_window says off");
    return mode != Mode::Off;
#else
    const char* value = std::getenv("TRIAEVUM_FASTMEM_WINDOW");
    if (value == nullptr) {
        mode = Mode::Off;
        SetReason("TRIAEVUM_FASTMEM_WINDOW unset");
        return false;
    }
    mode = ParseMode(value);
    if (mode == Mode::Off) SetReason("TRIAEVUM_FASTMEM_WINDOW says off");
    return mode != Mode::Off;
#endif
}

bool PlatformReserve() {
#if defined(__SWITCH__)
    gProcess = envGetOwnProcessHandle();
    if (gProcess == INVALID_HANDLE) {
        SetReason("no process handle (emulator or no loader)");
        return false;
    }
    virtmemLock();
    void* raw = virtmemFindAslr(kReservationSize, 0);
    gVirtmem = raw != nullptr ? virtmemAddReservation(raw, kReservationSize) : nullptr;
    virtmemUnlock();
    if (gVirtmem == nullptr) {
        SetReason("no free 4 GiB of address space");
        return false;
    }
    gReservation = reinterpret_cast<uintptr_t>(raw);
    gRoute = "";
    return true;
#elif defined(OOT3D_GUEST_WINDOW_HOST_MMAP)
    void* raw = mmap(nullptr, kReservationSize, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED) {
        SetReason("mmap of the 4 GiB reservation failed");
        return false;
    }
    gReservation = reinterpret_cast<uintptr_t>(raw);
    gRoute = "host";
    return true;
#else
    SetReason("no window on this platform");
    return false;
#endif
}

#if defined(OOT3D_GUEST_WINDOW_HOST_MMAP)
void PlatformReleaseReservation() {
    if (gReservation != 0) munmap(reinterpret_cast<void*>(gReservation), kReservationSize);
    gReservation = 0;
}
#endif

#if defined(__SWITCH__)
// Route 1: the heap pages lent straight to the window address (AliasCode),
// then made read-write (AliasCodeData).
Result MapDirect(uintptr_t va, uintptr_t source) {
    Result rc = svcMapProcessCodeMemory(gProcess, va, source, kSlabSize);
    if (R_FAILED(rc)) return rc;
    rc = svcSetProcessMemoryPermission(gProcess, va, kSlabSize, Perm_Rw);
    if (R_FAILED(rc)) svcUnmapProcessCodeMemory(gProcess, va, source, kSlabSize);
    return rc;
}

// Route 2 (libnx's JitType_CodeMemory): the pages lent to a shadow in the
// code region, which is then mapped read-write at the window address.
Result MapThroughShadow(uintptr_t va, uintptr_t source) {
    virtmemLock();
    void* shadow = virtmemFindCodeMemory(kSlabSize, 0);
    Result rc = shadow != nullptr
                    ? svcMapProcessCodeMemory(gProcess, reinterpret_cast<u64>(shadow), source,
                                              kSlabSize)
                    : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) return rc;
    rc = svcMapProcessMemory(reinterpret_cast<void*>(va), gProcess,
                             reinterpret_cast<u64>(shadow), kSlabSize);
    if (R_FAILED(rc)) {
        svcUnmapProcessCodeMemory(gProcess, reinterpret_cast<u64>(shadow), source, kSlabSize);
    }
    return rc;
}
#endif

// Under gLock. Never unmapped: the slabs stay for the process's lifetime.
bool PlatformMapSlab(size_t index) {
    const uintptr_t va = gBase.load(std::memory_order_relaxed) + index * kSlabSize;
    if (gSlabs >= gFailSlabsAfter) {
        SetReason("slab 0x%llx refused (test)", static_cast<unsigned long long>(index * kSlabSize));
        return false;
    }
#if defined(__SWITCH__)
    // 2 MiB aligned, so the kernel can use a block where the physical pages
    // line up too; whole pages of its own (newlib's headers sit outside).
    void* source = memalign(kSlabSize, kSlabSize);
    if (source == nullptr) {
        SetReason("no heap for slab 0x%llx", static_cast<unsigned long long>(index * kSlabSize));
        return false;
    }
    const uintptr_t from = reinterpret_cast<uintptr_t>(source);
    // The route of the first slab sticks: once the kernel refused the direct
    // mapping, every slab goes through a shadow.
    const bool tryDirect = std::strcmp(gRoute, "shadow") != 0;
    const Result direct = tryDirect ? MapDirect(va, from)
                                    : MAKERESULT(Module_Libnx, LibnxError_NotFound);
    if (R_SUCCEEDED(direct)) {
        if (*gRoute == '\0') gRoute = "direct";
    } else {
        const Result shadow = MapThroughShadow(va, from);
        if (R_FAILED(shadow)) {
            std::free(source);
            std::snprintf(gReason, sizeof(gReason),
                          "slab 0x%llx not mapped: direct rc=0x%x, shadow rc=0x%x",
                          static_cast<unsigned long long>(index * kSlabSize),
                          static_cast<unsigned>(direct), static_cast<unsigned>(shadow));
            return false;
        }
        if (*gRoute == '\0') gRoute = "shadow";
    }
    // The heap lent to the kernel cannot go back to hbloader: every exit
    // ends the application through AM (as oot3d_switch_code_pages.cpp does).
    __nx_applet_exit_mode = 1;
    return true;
#elif defined(OOT3D_GUEST_WINDOW_HOST_MMAP)
    void* mapped = mmap(reinterpret_cast<void*>(va), kSlabSize, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mapped != reinterpret_cast<void*>(va)) {
        SetReason("mmap of slab 0x%llx failed", static_cast<unsigned long long>(index * kSlabSize));
        return false;
    }
    return true;
#else
    static_cast<void>(va);
    return false;
#endif
}

// ---- state ------------------------------------------------------------------

// Under gLock.
void StartLocked(Mode mode) {
    gRequested = mode != Mode::Off;
    if (mode == Mode::Off) {
        gState.store(State::Off, std::memory_order_release);
        return;
    }
    if (!gHooked.load(std::memory_order_acquire)) {
        SetReason("allocator hook not linked");
        gState.store(State::Off, std::memory_order_release);
        return;
    }
    if (!PlatformReserve()) {
        gState.store(State::Off, std::memory_order_release);
        return;
    }
    // Before the base is published: no placement can be armed yet.
    gReason[0] = '\0';
    gVerify.store(mode == Mode::Verify, std::memory_order_relaxed);
    const uintptr_t base = (gReservation + kSlabSize - 1U) & ~(uintptr_t{kSlabSize} - 1U);
    gBase.store(base, std::memory_order_release);
    gAccepting.store(true, std::memory_order_release);
    gState.store(State::On, std::memory_order_release);
}

void Start() {
    const std::lock_guard<Lock> lock(gLock);
    if (gState.load(std::memory_order_acquire) != State::Unstarted) return;
    Mode mode = Mode::Off;
    ReadMode(mode);
    StartLocked(mode);
}

// Under gLock: a live buffer already covers part of the range.
bool Overlaps(uintptr_t begin, size_t size) noexcept {
    for (size_t index = 0; index < gPlacementCount; ++index) {
        const Placement& placement = gPlacements[index];
        if (begin < placement.Begin + placement.Size && placement.Begin < begin + size) return true;
    }
    return false;
}

// Under gLock: maps every slab of [address, address + size).
bool Commit(uint32_t address, size_t size) {
    const size_t first = address / kSlabSize;
    const size_t last = static_cast<size_t>((uint64_t{address} + size - 1U) / kSlabSize);
    for (size_t index = first; index <= last; ++index) {
        if (SlabMapped(index)) continue;
        if (!PlatformMapSlab(index)) {
            // Any refused mapping: no new placement from here on (regions
            // already placed stay valid memory).
            gAccepting.store(false, std::memory_order_release);
            return false;
        }
        gSlabMapped[index / 64U] |= uint64_t{1} << (index % 64U);
        ++gSlabs;
    }
    return true;
}

// Under gLock.
Placement* FindPlacement(uintptr_t begin) noexcept {
    for (size_t index = 0; index < gPlacementCount; ++index) {
        if (gPlacements[index].Begin == begin) return &gPlacements[index];
    }
    return nullptr;
}

void RemovePlacementLocked(Placement* placement) noexcept {
    gPlacementBytes -= placement->Size;
    *placement = gPlacements[--gPlacementCount];
    gLive.fetch_sub(1U, std::memory_order_relaxed);
}

} // namespace

Mode ParseMode(std::string_view text) noexcept {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text == "0" || text == "off") return Mode::Off;
    if (text == "verify") return Mode::Verify;
    return Mode::On;
}

Status CurrentStatus() {
    const std::lock_guard<Lock> lock(gLock);
    Status status;
    status.Requested = gRequested;
    status.On = gAccepting.load(std::memory_order_acquire);
    status.Verify = gVerify.load(std::memory_order_relaxed);
    status.Base = gBase.load(std::memory_order_relaxed);
    status.Regions = gPlacementCount;
    status.RegionBytes = gPlacementBytes;
    status.Slabs = gSlabs;
    status.Conflicts = gConflicts.load(std::memory_order_relaxed);
    status.Misses = gMisses.load(std::memory_order_relaxed);
    status.Placed = gPlaced.load(std::memory_order_relaxed);
    status.ShadowChecks = gShadowChecks.load(std::memory_order_relaxed);
    status.ShadowMismatches = gShadowMismatches.load(std::memory_order_relaxed);
    status.Route = gRoute;
    status.Reason = gReason;
    return status;
}

std::string FormatStatus(const Status& status) {
    char text[512];
    if (!status.On) {
        std::snprintf(text, sizeof(text), "fastmem_window: off (%s)",
                      status.Reason.empty() ? "not started" : status.Reason.c_str());
        std::string line = text;
        if (status.Regions != 0U) {
            std::snprintf(text, sizeof(text), " regions_in_window=%zu slabs=%zu", status.Regions,
                          status.Slabs);
            line += text;
        }
        return line;
    }
    std::snprintf(text, sizeof(text),
                  "fastmem_window: on base=0x%llx regions=%zu mapped_kb=%zu slabs=%zu "
                  "region_kb=%zu route=%s",
                  static_cast<unsigned long long>(status.Base), status.Regions,
                  status.Slabs * (kSlabSize / 1024U), status.Slabs, status.RegionBytes / 1024U,
                  status.Route);
    std::string line = text;
    if (status.Conflicts != 0U || status.Misses != 0U) {
        std::snprintf(text, sizeof(text), " conflicts=%llu misses=%llu",
                      static_cast<unsigned long long>(status.Conflicts),
                      static_cast<unsigned long long>(status.Misses));
        line += text;
    }
    if (status.Verify) {
        std::snprintf(text, sizeof(text), " verify checks=%llu mismatches=%llu",
                      static_cast<unsigned long long>(status.ShadowChecks),
                      static_cast<unsigned long long>(status.ShadowMismatches));
        line += text;
    }
    return line;
}

bool Accepting() {
    if (gState.load(std::memory_order_acquire) == State::Unstarted) [[unlikely]] Start();
    return gAccepting.load(std::memory_order_acquire);
}

void Claim(const void* memory) noexcept {
    gOwner.store(memory, std::memory_order_release);
}

bool IsOwner(const void* memory) noexcept {
    return memory != nullptr && gOwner.load(std::memory_order_acquire) == memory;
}

bool ClaimIfVacant(const void* memory) noexcept {
    if (gOwner.load(std::memory_order_acquire) == nullptr ||
        gLive.load(std::memory_order_acquire) == 0U) {
        Claim(memory);
    }
    return IsOwner(memory);
}

size_t LivePlacements() noexcept {
    return gLive.load(std::memory_order_acquire);
}

uint8_t* Base() noexcept {
    return reinterpret_cast<uint8_t*>(gBase.load(std::memory_order_acquire));
}

bool Reserve(std::vector<uint8_t>& bytes, uint32_t address, size_t size) {
    if (size == 0U || bytes.capacity() != 0U || uint64_t{address} + size > kGuestSpan ||
        !Accepting()) {
        return false;
    }
    const std::lock_guard<Lock> reserve(gReserveLock);
    uintptr_t target = 0;
    {
        const std::lock_guard<Lock> lock(gLock);
        if (!gAccepting.load(std::memory_order_acquire)) return false;
        target = gBase.load(std::memory_order_relaxed) + address;
        if (Overlaps(target, size) || gPlacementCount == kMaxPlacements) {
            gConflicts.fetch_add(1U, std::memory_order_relaxed);
            return false;
        }
        if (!Commit(address, size)) return false;
        gPlacements[gPlacementCount++] = {target, size};
        gPlaced.fetch_add(1U, std::memory_order_relaxed);
        gPlacementBytes += size;
        gLive.fetch_add(1U, std::memory_order_release);
    }
    gArmedThread.store(ThreadToken(), std::memory_order_relaxed);
    gArmedSize.store(size, std::memory_order_relaxed);
    gArmedTarget.store(target, std::memory_order_release);
    try {
        bytes.reserve(size);
    } catch (...) {
        gArmedTarget.store(0, std::memory_order_release);
        const std::lock_guard<Lock> lock(gLock);
        if (Placement* placement = FindPlacement(target)) RemovePlacementLocked(placement);
        throw;
    }
    const uintptr_t left = gArmedTarget.exchange(0, std::memory_order_acq_rel);
    if (reinterpret_cast<uintptr_t>(bytes.data()) == target) return true;
    // The allocation did not go through the hook (an allocator path the
    // wrappers do not see): it stays an ordinary heap buffer, released here.
    gMisses.fetch_add(1U, std::memory_order_relaxed);
    std::vector<uint8_t>().swap(bytes);
    if (left != 0U) {
        const std::lock_guard<Lock> lock(gLock);
        if (Placement* placement = FindPlacement(target)) RemovePlacementLocked(placement);
    }
    return false;
}

bool Verifying() noexcept {
    return gVerify.load(std::memory_order_relaxed);
}

void CountShadowCheck(uint64_t mismatches) noexcept {
    gShadowChecks.fetch_add(1U, std::memory_order_relaxed);
    if (mismatches != 0U) gShadowMismatches.fetch_add(mismatches, std::memory_order_relaxed);
}

void MarkAllocatorHooked() noexcept {
    gHooked.store(true, std::memory_order_release);
}

void* TakePlacement(size_t size) noexcept {
    // The armed target is read first: libnx and newlib allocate before
    // thread-local storage exists, and nothing is armed then.
    if (gArmedTarget.load(std::memory_order_acquire) == 0U) [[likely]] return nullptr;
    if (gArmedSize.load(std::memory_order_relaxed) != size ||
        gArmedThread.load(std::memory_order_relaxed) != ThreadToken()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(gArmedTarget.exchange(0, std::memory_order_acq_rel));
}

bool Contains(const void* pointer) noexcept {
    const uintptr_t base = gBase.load(std::memory_order_acquire);
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    return base != 0U && address >= base && address - base < kGuestSpan;
}

void ReleaseAllocation(void* pointer) noexcept {
    const std::lock_guard<Lock> lock(gLock);
    if (Placement* placement = FindPlacement(reinterpret_cast<uintptr_t>(pointer))) {
        RemovePlacementLocked(placement);
    }
}

size_t AllocationSize(const void* pointer) noexcept {
    const std::lock_guard<Lock> lock(gLock);
    const Placement* placement = FindPlacement(reinterpret_cast<uintptr_t>(pointer));
    return placement != nullptr ? placement->Size : 0U;
}

bool ResetForTesting(Mode mode, size_t failSlabsAfter) {
#if defined(OOT3D_GUEST_WINDOW_HOST_MMAP)
    const std::lock_guard<Lock> lock(gLock);
    if (gPlacementCount != 0U) return false;
    gAccepting.store(false, std::memory_order_release);
    gBase.store(0, std::memory_order_release);
    PlatformReleaseReservation();
    gOwner.store(nullptr, std::memory_order_release);
    std::memset(gSlabMapped, 0, sizeof(gSlabMapped));
    gSlabs = 0;
    gFailSlabsAfter = failSlabsAfter;
    gConflicts = gMisses = gPlaced = gShadowChecks = gShadowMismatches = 0;
    gRoute = "";
    SetReason(mode == Mode::Off ? "off for this test" : "");
    StartLocked(mode);
    return mode == Mode::Off || gState.load(std::memory_order_acquire) == State::On;
#else
    static_cast<void>(mode);
    static_cast<void>(failSlabsAfter);
    return false;
#endif
}

} // namespace Oot3dNativeGame::GuestWindow
