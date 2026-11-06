#pragma once

// fastmem: the guest window (SWITCH_PORT.md "fastmem window").
//
// A 4 GiB + 2 MiB address-space reservation, its base rounded up to 2 MiB, in
// which every guest region of one NativeA32Memory (the owner: the process's
// image) is backed at window_base + guest_address. Region::Bytes stays a
// std::vector, whose buffer the allocator hook places at the window address
// (Reserve). Phase 2: every image whose regions are here keeps a page-flag
// table (oot3d_native_a32_memory.h), and the inline typed paths of the
// shards load and store at window_base + address when a page's flag allows
// it.
//
// Not shard-visible: only oot3d_native_a32_memory.cpp, the process, the
// allocator wrappers and the session's log line include this header.
//
// Switch: on by default; config/fastmem_window with the text "off" (or "0")
// turns it off, "verify" also checks every image's flag table against its
// page entries after every change. Host: TRIAEVUM_FASTMEM_WINDOW (1 or
// verify), honoured only in binaries that link the host allocator hook
// (oot3d_switch_guest_window_host_alloc.cpp), off when unset. Off: no
// reservation, no hook fires, every region is allocated as before, and
// every guest access takes the slow path (page table).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame::GuestWindow {

inline constexpr size_t kSlabSize = size_t{2} << 20;
inline constexpr uint64_t kGuestSpan = uint64_t{1} << 32;
inline constexpr uint64_t kReservationSize = kGuestSpan + kSlabSize;
inline constexpr size_t kSlabCount = static_cast<size_t>(kGuestSpan / kSlabSize);

enum class Mode : uint8_t { Off, On, Verify };
// The config file's text or the environment variable: empty, "1" or "on" =
// On; "verify" = Verify; "0" or "off" = Off (leading/trailing space ignored).
Mode ParseMode(std::string_view text) noexcept;

struct Status {
    bool Requested = false;  // not turned off (Switch file, host variable)
    bool On = false;         // reserved and taking placements
    bool Verify = false;
    uintptr_t Base = 0;
    size_t Regions = 0;      // live placements (region buffers in the window)
    size_t RegionBytes = 0;  // their bytes
    size_t Slabs = 0;        // 2 MiB slabs mapped
    uint64_t Conflicts = 0;  // placements refused: range held by another buffer
    uint64_t Misses = 0;     // the allocator did not take a placement
    uint64_t Placed = 0;     // placements made since the start
    uint64_t ShadowChecks = 0;      // verify mode: flag-table checks
    uint64_t ShadowMismatches = 0;  // pages whose flag disagreed with its entry
    const char* Route = "";  // "direct", "shadow" (Switch) or "host"
    std::string Reason;      // why off, or why placements stopped
};
Status CurrentStatus();
// "fastmem_window: on base=0x… regions=N mapped_kb=… slabs=… region_kb=… route=…"
// or "fastmem_window: off (reason)".
std::string FormatStatus(const Status& status);

// True while new regions may be placed in the window. The first call reads
// the configuration and reserves the window.
bool Accepting();
// The memory image whose regions live in the window (compared, never
// dereferenced). The process claims its image; otherwise the first image
// that maps a region while no buffer lives in the window claims it.
void Claim(const void* memory) noexcept;
bool IsOwner(const void* memory) noexcept;
bool ClaimIfVacant(const void* memory) noexcept;
size_t LivePlacements() noexcept;
uint8_t* Base() noexcept;  // null while off

// Gives the empty vector `bytes` a capacity of exactly `size` bytes at
// Base() + address (slabs mapped as needed). False: nothing placed, `bytes`
// still empty with no buffer. Callers fill it with assign/resize (no
// reallocation within that capacity).
bool Reserve(std::vector<uint8_t>& bytes, uint32_t address, size_t size);
inline bool Placed(const uint8_t* data, uint32_t address) noexcept {
    const uint8_t* base = Base();
    return base != nullptr && data == base + address;
}

// Verify mode (the config file's text "verify").
bool Verifying() noexcept;
void CountShadowCheck(uint64_t mismatches) noexcept;

// Allocator side (Switch: oot3d_switch_guard_alloc.cpp's newlib wrappers;
// host: oot3d_switch_guest_window_host_alloc.cpp). TakePlacement returns the
// armed window address to the thread that armed it for exactly that size;
// Contains/Release/AllocationSize recognise the window buffers on free.
void MarkAllocatorHooked() noexcept;
void* TakePlacement(size_t size) noexcept;
bool Contains(const void* pointer) noexcept;
void ReleaseAllocation(void* pointer) noexcept;
size_t AllocationSize(const void* pointer) noexcept;

// Host tests only, with no live placement: restart in `mode` (host backend),
// optionally failing every slab mapping from the given one on.
bool ResetForTesting(Mode mode, size_t failSlabsAfter = SIZE_MAX);

} // namespace Oot3dNativeGame::GuestWindow
