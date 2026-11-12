#pragma once

#include "recomp/a32_runtime.h"
#include "oot3d_guest_fp_environment.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Hot/cold attributes for the typed AOT memory paths. Only GCC gets the
// forced inline: its unit-growth limits otherwise leave nearly every
// generated access as an out-of-line call. Other compilers keep their own
// inlining decisions (measured separately on Windows).
#ifndef OOT3D_AOT_HOT
#if defined(__GNUC__) && !defined(__clang__)
#define OOT3D_AOT_HOT inline __attribute__((always_inline))
#define OOT3D_AOT_COLD __attribute__((noinline, cold))
#else
#define OOT3D_AOT_HOT inline
#define OOT3D_AOT_COLD
#endif
#endif

namespace Oot3dNativeGame {

// Typed guest accesses and type-based alias analysis (GCC).
//
// A guest store written as std::memcpy into uint8_t* is an access of the
// "any type" alias set: GCC must assume it may modify every object, so after
// each inlined guest store it reloads the guest registers it had in host
// registers (Oot3dAotArchitecturalState::R, the lazy flags), the block
// budget, the memory object pointer, the page-table group pointers and the
// generation counter. A guest load written as std::memcpy is a read of the
// same set, so every pending store to that state has to be completed before
// it and cannot be merged or dropped across it.
//
// The inline fast paths therefore access guest bytes through GuestCell, a
// packed union of enum types that exist only here:
//  - a member access to a union uses the alias set of the union, so loads and
//    stores of all four sizes share one alias set and stay ordered among
//    themselves (overlapping or unaligned STR/STRB/LDRH keep their order and
//    value forwarding);
//  - enum classes, unlike char/unsigned char/std::byte, do not alias
//    everything, and the union contains no character type, so its alias set
//    is disjoint from the host state (uint32_t/uint64_t/bool/pointer fields);
//  - every other path to guest bytes (slow paths, WriteBytes/ReadBytes,
//    spans, regions, native helpers) uses std::memcpy or character pointers,
//    which alias everything and so stay ordered with the cell accesses.
//
// Contract: in a function that inlines ReadFast/WriteFast/ReadWordsFast/
// WriteWordsFast, guest bytes may be accessed only through these helpers,
// std::memcpy, character types or std::byte, never through other typed
// lvalues (a uint32_t* or float* into guest memory, for example), which GCC
// would consider unrelated to the cell. Calls that are not inlined are
// unaffected. Other compilers, or OOT3D_GUEST_ACCESS_MEMCPY, use
// std::memcpy for every access.
#if defined(__GNUC__) && !defined(__clang__) && !defined(OOT3D_GUEST_ACCESS_MEMCPY)
#define OOT3D_GUEST_ACCESS_CELL 1
enum class GuestCell8 : uint8_t {};
enum class GuestCell16 : uint16_t {};
enum class GuestCell32 : uint32_t {};
enum class GuestCell64 : uint64_t {};
union __attribute__((packed)) GuestCell {
    GuestCell8 U8;
    GuestCell16 U16;
    GuestCell32 U32;
    GuestCell64 U64;
};
static_assert(sizeof(GuestCell) == 8U && alignof(GuestCell) == 1U);
#endif

// Stores `value` (little endian, any alignment) at `destination`: the same
// bytes as std::memcpy(destination, &value, sizeof(T)).
template <typename T>
OOT3D_AOT_HOT void StoreGuestValue(uint8_t* destination, T value) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    static_assert(std::endian::native == std::endian::little);
#if defined(OOT3D_GUEST_ACCESS_CELL)
    auto* cell = reinterpret_cast<GuestCell*>(destination);
    if constexpr (sizeof(T) == 1U) {
        cell->U8 = static_cast<GuestCell8>(value);
    } else if constexpr (sizeof(T) == 2U) {
        cell->U16 = static_cast<GuestCell16>(value);
    } else if constexpr (sizeof(T) == 4U) {
        cell->U32 = static_cast<GuestCell32>(value);
    } else {
        static_assert(sizeof(T) == 8U);
        cell->U64 = static_cast<GuestCell64>(value);
    }
#else
    std::memcpy(destination, &value, sizeof(T));
#endif
}

// Loads a T (little endian, any alignment) from `source`: the same value as
// std::memcpy(&value, source, sizeof(T)).
template <typename T>
OOT3D_AOT_HOT T LoadGuestValue(const uint8_t* source) noexcept {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    static_assert(std::endian::native == std::endian::little);
#if defined(OOT3D_GUEST_ACCESS_CELL)
    const auto* cell = reinterpret_cast<const GuestCell*>(source);
    if constexpr (sizeof(T) == 1U) {
        return static_cast<T>(cell->U8);
    } else if constexpr (sizeof(T) == 2U) {
        return static_cast<T>(cell->U16);
    } else if constexpr (sizeof(T) == 4U) {
        return static_cast<T>(cell->U32);
    } else {
        static_assert(sizeof(T) == 8U);
        return static_cast<T>(cell->U64);
    }
#else
    T value;
    std::memcpy(&value, source, sizeof(T));
    return value;
#endif
}

// Store cards. Every guest store marks the 4 KiB page it wrote: one byte per
// guest page in a flat table of the image, with two bits that the two kinds
// of write generation take independently (atomic and-fetch, so a racing
// store's mark is never lost): bit 0 by RangeWriteGeneration (the page's
// certificate), bit 1 by WriteGeneration (the architectural generation). A
// store makes the card Dirty only when it is not Dirty already, so after the
// first store the card's cache line is only read (shared by the two cores
// instead of bouncing between them). The card is a one-member union of an
// enum class, like GuestCell: an uint8_t store would be an "any type" access
// for GCC and force every guest register and pointer held in a host register
// to be reloaded after each guest store; this type has its own alias set, so
// the only cost of a card is its byte load (and the rare store).
enum class GuestCardState : uint8_t { Clean = 0, RangeWritten = 1, Written = 2, Dirty = 3 };
union GuestCard {
    GuestCardState State;
};
static_assert(sizeof(GuestCard) == 1U && alignof(GuestCard) == 1U);

// Page flags of the fastmem window (SWITCH_PORT.md "fastmem window"): one
// byte per 4 KiB guest page, in a flat table of the image. A load completes
// inline, at window base + address, only on a page with kGuestPageLoadFast,
// a store only on one with kGuestPageStoreFast; every other access (unmapped
// or read-only page, observed, deferred, watched or traced page, a page not
// in the window) takes the checked slow path. Zero traps everything: an image
// that is not in the window (window off, emulator, refused mapping, a copy)
// points at a shared table of zeros. Own alias set, like GuestCard.
inline constexpr uint8_t kGuestPageLoadFast = 0x01U;
inline constexpr uint8_t kGuestPageStoreFast = 0x02U;
enum class GuestPageFlags : uint8_t {};
union GuestPageFlag {
    GuestPageFlags Bits;
};
static_assert(sizeof(GuestPageFlag) == 1U && alignof(GuestPageFlag) == 1U);

struct NativeA32MemoryRegionConfig {
    std::string Name;
    uint32_t BaseAddress = 0;
    size_t Size = 0;
    bool Writable = false;
    bool Executable = false;
    std::span<const uint8_t> InitialBytes;
};

class NativeA32Memory final : public oot3d::recomp::a32::MemoryBus {
  public:
    NativeA32Memory() = default;
    NativeA32Memory(const NativeA32Memory& other);
    NativeA32Memory& operator=(const NativeA32Memory& other);
    NativeA32Memory(NativeA32Memory&& other) noexcept;
    NativeA32Memory& operator=(NativeA32Memory&& other) noexcept;

    bool MapRegion(const NativeA32MemoryRegionConfig& config,
                   std::string* error = nullptr);
    bool IsMapped(uint32_t address, size_t size) const;
    bool IsWritable(uint32_t address, size_t size) const;
    bool IsExecutable(uint32_t address, size_t size) const;
    size_t RegionCount() const;
    // Architectural write generation: changes between two calls exactly when
    // a tracked write happened in between (any store, service write, fill,
    // device publication or escaped write pointer); savestates carry it. It
    // counts the observations that saw new writes (bit 1 of a mapped page's
    // card), not the writes: a call scans the cards of the mapped pages.
    // Exclusives no longer use it (the global monitor's value token).
    uint64_t WriteGeneration() const;
    // Change certificate of a range: equal across two calls only if nothing
    // wrote the range in between; never 0; nullopt when it cannot be
    // certified (unmapped, escaped write pointer, invalidated versions).
    // Pages written since the last query get a fresh value here, larger than
    // every certificate handed out before.
    std::optional<uint64_t> RangeWriteGeneration(uint32_t address,
                                                 size_t size) const noexcept;
    void EnableWriteTraceFingerprint(bool enabled) noexcept;
    uint64_t WriteTraceFingerprint() noexcept;
    uint64_t FastReadMismatchCount() const noexcept;
    uint32_t LastFastReadMismatchAddress() const noexcept;
    uint64_t LastFastReadMismatchValue() const noexcept;
    uint64_t LastCheckedReadMismatchValue() const noexcept;

    // GPU ownership is invalidated by writes, including same-value writes.
    // ObserveWrites traps only write fast-path entries for registered ranges.
    using WriteObserver = std::function<void(uint32_t, size_t)>;
    // 5: the inline paths read a page-flag table and access the guest window
    // (window base + address); stores mark two-bit cards in a flat table,
    // with no memory-wide written mark.
    static constexpr uint32_t WriteObservationAbiVersion() noexcept { return 5U; }
    void SetWriteObserver(WriteObserver observer);
    bool ObserveWrites(uint32_t address, size_t size);
    bool HasEscapedWritePointer(uint32_t address, size_t size) const noexcept;
    void PreserveAccessObserversFrom(const NativeA32Memory& memory);

    // A completed device write is logically visible immediately. Backing RAM
    // is populated only when a CPU reader or a state snapshot needs its bytes.
    // The reader owns the immutable producer until its last range is consumed
    // or overwritten. All ranges are validated before publishing any write.
    using DeferredRead = std::function<bool(uint32_t, std::span<uint8_t>)>;
    bool DeferWrites(std::span<const std::pair<uint32_t, size_t>> ranges,
                     DeferredRead reader);
    bool MaterializeDeferredWrites() const;
    // Synchronous, read-only view: callers must finish consuming it before
    // executing guest code, device work, or another memory operation.
    std::span<const uint8_t> SynchronousReadView(uint32_t address, size_t size) const;
    bool AllowsUnobservedAccess() const noexcept {
        return !mTraceWriteFingerprint && !mWatchActive &&
               mObservedWriteRanges.empty() && mDeferredWrites.empty();
    }

    // Diagnostic guest-access watch for a small number of address ranges
    // (whole-AOT raw TextureCopy destinations, for example). Watched pages lose
    // their fast entry, so typed accesses issued by the title module - compiled
    // against this header, including the precompiled module - take the checked
    // path, where they are counted. Reads and writes return and store exactly
    // the same bytes, so enabling the watch never changes behaviour; with no
    // range set the fast path is untouched and the branches below are a single
    // predictable bool test.
    struct RangeWatchCounters {
        uint64_t TypedReads = 0;   // ReadFast/Read8..64 fallback
        uint64_t TypedWrites = 0;  // WriteFast/Write8..64 fallback
        uint64_t ByteReads = 0;    // ReadBytes
        uint64_t PointerReads = 0; // GetReadPointer: a live pointer, invisible
                                   // after it is handed out
        uint64_t ByteWrites = 0;   // WriteBytes from a guest or a service
        // The renderer's own materialization of the same bytes, kept apart so
        // a run where nothing else touches the range still proves the trap is
        // live (one writeback span per written row).
        uint64_t SuppressedByteWrites = 0;
        // Host-service write paths that do not go through the guest bus.
        uint64_t HostByteWrites = 0;
        uint64_t Fills = 0;
        uint64_t PointerWrites = 0; // GetWritePointer: same, for writes
        uint32_t FirstReadAddress = 0;
        uint8_t FirstReadSize = 0;
        uint32_t FirstWriteAddress = 0;
        uint8_t FirstWriteSize = 0;
        bool FirstReadRecorded = false;
        bool FirstWriteRecorded = false;
    };
    // Ranges are half-open [begin, end). Setting them rebuilds the page table
    // only when the trapped pages change, so the same destination seen once per
    // frame costs nothing after the first time.
    void SetRangeWatch(std::span<const std::pair<uint32_t, uint32_t>> ranges);
    void ClearRangeWatch();
    [[nodiscard]] bool RangeWatchActive() const noexcept {
        return mWatchActive;
    }
    [[nodiscard]] const RangeWatchCounters& WatchCounters() const noexcept {
        return mWatchCounters;
    }
    [[nodiscard]] size_t WatchedRangeCount() const noexcept {
        return mWatchRanges.size();
    }
    // The renderer materializes a raw copy into the same bytes it is watching,
    // through the byte API; suppression keeps that self-traffic out of the
    // guest-access counts without changing what is written.
    void SetRangeWatchSuppressed(bool suppressed) noexcept {
        mWatchSuppressed = suppressed;
    }
    uint64_t ContentFingerprint() const;
    uint64_t StateFingerprint() const;
    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

    bool ReadBytes(uint32_t address, std::span<uint8_t> bytes) const;
    const uint8_t* GetReadPointer(uint32_t address, size_t size = 1) const;
    uint8_t* GetWritePointer(uint32_t address, size_t size = 1);
    std::optional<uint32_t>
    GetGuestAddress(const void* pointer, size_t size = 1) const noexcept;
    bool WriteBytes(uint32_t address, std::span<const uint8_t> bytes);
    bool Fill(uint32_t address, size_t size, uint8_t value);

    // Service-owned shared memory remains host-writable even when the guest
    // maps it read-only.
    bool WriteHostBytes(uint32_t address, std::span<const uint8_t> bytes);
    template <typename T>
    bool WriteHost(uint32_t address, T value) {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8);
        static_assert(std::endian::native == std::endian::little);
        return WriteHostBytes(
            address,
            std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(&value), sizeof(value)));
    }

    // In-place host update of a writable guest range (the DSP HLE's shared
    // regions): BeginHostUpdate checks and accounts like ReadBytes of the range
    // and returns its bytes; EndHostUpdate accounts like WriteBytes of the
    // range's current bytes, without the copy. Read the range, modify it,
    // write it back whole has the same observable effect, minus two copies.
    // EndHostUpdate must keep WriteBytes' bookkeeping exactly.
    uint8_t* BeginHostUpdate(uint32_t address, size_t size);
    bool EndHostUpdate(uint32_t address, size_t size);

    // AOT code uses these non-virtual typed paths (fastmem window, phase 2).
    // An access that lies wholly inside one 4 KiB page whose flag allows it
    // (kGuestPageLoadFast / kGuestPageStoreFast, PageAllows) is a direct load
    // or store at window base + address: one flag byte, the page-crossing test
    // of 2/4/8-byte accesses and the access itself, with no page-table walk.
    // A store then marks its page's card (MarkCard).
    //
    // Hot/cold split: everything else (null value, page boundary, a flag that
    // traps: unmapped, read-only for stores, observed, deferred, watched,
    // traced, or an image that is not in the window, whose flag table is all
    // zeros) goes to ReadFastSlow/WriteFastSlow, the complete original bodies
    // (page table, then the checked path), so values, faults, observers,
    // deferred readback, watches, traces and generations are unchanged.
    //
    // Lean entries: write-trace fingerprinting is encoded in the page table
    // (every typed Read/Write entry null while it is enabled; FastPage::Bytes
    // keeps the page bytes for the slow path and the span helpers), and the
    // page flags are derived from those entries, so a traced access traps
    // with no extra test and still reaches the exact traced body.
    template <typename T>
    OOT3D_AOT_HOT bool ReadFast(uint32_t address, T* value,
                                uint32_t* faultAddress = nullptr) const noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8);
        static_assert(std::endian::native == std::endian::little);
        if (value != nullptr && (address & PageMask) <= PageSize - sizeof(T) &&
            PageAllows(address, kGuestPageLoadFast)) [[likely]] {
            *value = LoadGuestValue<T>(mWindowBase + address);
            return true;
        }
        return ReadFastSlow<T>(address, value, faultAddress);
    }

    template <typename T>
    OOT3D_AOT_HOT bool WriteFast(uint32_t address, T value,
                                 uint32_t* faultAddress = nullptr) noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8);
        static_assert(std::endian::native == std::endian::little);
        if ((address & PageMask) <= PageSize - sizeof(T) &&
            PageAllows(address, kGuestPageStoreFast)) [[likely]] {
            StoreGuestValue<T>(mWindowBase + address, value);
            MarkCard(address >> PageShift);
            return true;
        }
        return WriteFastSlow<T>(address, value, faultAddress);
    }

    // Multi-word guest transfers (LDM/STM, VLDM/VSTM). True only when every
    // word lies on one page whose flag allows the access; the effect is then
    // exactly that of `count` consecutive ReadFast<uint32_t>/
    // WriteFast<uint32_t> calls at ascending addresses: the same bytes and the
    // page's card set. False means nothing was read or written; the caller
    // then uses the per-word path, which keeps the exact fault order and
    // partial-write behavior.
    OOT3D_AOT_HOT bool ReadWordsFast(uint32_t address, uint32_t* values,
                                     uint32_t count) const noexcept {
        const size_t bytes = size_t{count} * 4U;
        if (bytes > PageSize || (address & PageMask) > PageSize - bytes ||
            !PageAllows(address, kGuestPageLoadFast)) [[unlikely]] {
            return false;
        }
        // Word loads (see LoadGuestValue); `count` is a literal at every
        // generated call site.
        const uint8_t* source = mWindowBase + address;
        for (uint32_t index = 0; index < count; ++index) {
            values[index] = LoadGuestValue<uint32_t>(source + size_t{index} * 4U);
        }
        return true;
    }

    OOT3D_AOT_HOT bool WriteWordsFast(uint32_t address, const uint32_t* values,
                                      uint32_t count) noexcept {
        const size_t bytes = size_t{count} * 4U;
        if (count == 0U || bytes > PageSize || (address & PageMask) > PageSize - bytes ||
            !PageAllows(address, kGuestPageStoreFast)) [[unlikely]] {
            return false;
        }
        // Word stores (see StoreGuestValue); `count` is a literal at every
        // generated call site.
        uint8_t* destination = mWindowBase + address;
        for (uint32_t index = 0; index < count; ++index) {
            StoreGuestValue<uint32_t>(destination + size_t{index} * 4U, values[index]);
        }
        MarkCard(address >> PageShift);
        return true;
    }

    // Complete original typed-access bodies, reached only when the inline
    // fast path declines.
    template <typename T>
    OOT3D_AOT_COLD bool ReadFastSlow(uint32_t address, T* value,
                                     uint32_t* faultAddress) const noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8);
        static_assert(std::endian::native == std::endian::little);
        if (value == nullptr) {
            return false;
        }
        const size_t group = address >> FastGroupShift;
        const size_t page = (address >> PageShift) & FastGroupPageMask;
        const size_t offset = address & PageMask;
        if (offset <= PageSize - sizeof(T) &&
            mFastPageGroups[group] != nullptr) {
            // Bytes, not Read: a traced page keeps its bytes here and the
            // fast/checked cross-check below still runs exactly as before.
            const uint8_t* source = mFastPageGroups[group]->Pages[page].Bytes;
            if (source != nullptr) {
                std::memcpy(value, source + offset, sizeof(T));
                if (mTraceWriteFingerprint) [[unlikely]] {
                    uint64_t checked = 0;
                    if (ReadSized(address, static_cast<uint8_t>(sizeof(T)),
                                  &checked, nullptr) &&
                        *value != static_cast<T>(checked)) {
                        ++mFastReadMismatchCount;
                        mLastFastReadMismatchAddress = address;
                        mLastFastReadMismatchValue = *value;
                        mLastCheckedReadMismatchValue = checked;
                    }
                }
                return true;
            }
        }
        // Device and service handlers run under the host FPCR; the guest's
        // FPCR and accumulated FPSR flags are restored afterwards.
        const Oot3dNativeGame::Oot3dGuestFpSlowPathScope hostFp;
        uint64_t decoded = 0;
        if (!ReadSized(address, static_cast<uint8_t>(sizeof(T)), &decoded,
                       faultAddress)) {
            return false;
        }
        *value = static_cast<T>(decoded);
        return true;
    }

    template <typename T>
    OOT3D_AOT_COLD bool WriteFastSlow(uint32_t address, T value,
                                      uint32_t* faultAddress) noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 ||
                      sizeof(T) == 8);
        static_assert(std::endian::native == std::endian::little);
        const size_t group = address >> FastGroupShift;
        const size_t page = (address >> PageShift) & FastGroupPageMask;
        const size_t offset = address & PageMask;
        if (offset <= PageSize - sizeof(T) &&
            mFastPageGroups[group] != nullptr) {
            // Null on every page while write tracing is enabled: a traced
            // write then takes WriteSized, which records the same trace bytes
            // and marks the page identically.
            uint8_t* destination = mFastPageGroups[group]->Pages[page].Write;
            if (destination != nullptr) {
                std::memcpy(destination + offset, &value, sizeof(T));
                MarkCard(address >> PageShift);
                return true;
            }
        }
        const Oot3dNativeGame::Oot3dGuestFpSlowPathScope hostFp;
        return WriteSized(address, static_cast<uint8_t>(sizeof(T)), value,
                          faultAddress);
    }

    bool RangeWatched(uint32_t address, size_t size) const noexcept {
        return Watches(address, size);
    }

    // Exact single-page equivalents of IsMapped/IsWritable, ReadBytes and
    // WriteBytes for host-native title helpers. A fast page lies wholly in one
    // region and is neither watched nor deferred; a fast write page is also
    // writable and unobserved. On such a page the checked calls reduce to a
    // copy plus, for writes, the page's card. False means the span needs
    // the checked path, and then nothing was read or written.
    bool SpanOnFastReadPage(uint32_t address, size_t size) const noexcept {
        return FastPageFor(address, size) != nullptr &&
               FastPageFor(address, size)->Bytes != nullptr;
    }
    bool SpanOnFastWritePage(uint32_t address, size_t size) const noexcept {
        return !mTraceWriteFingerprint && FastPageFor(address, size) != nullptr &&
               FastPageFor(address, size)->Write != nullptr;
    }
    bool ReadBytesFast(uint32_t address, std::span<uint8_t> bytes) const noexcept {
        const FastPage* page = FastPageFor(address, bytes.size());
        if (page == nullptr || page->Bytes == nullptr) {
            return false;
        }
        std::memcpy(bytes.data(), page->Bytes + (address & PageMask), bytes.size());
        return true;
    }
    bool WriteBytesFast(uint32_t address, std::span<const uint8_t> bytes) noexcept {
        const size_t offset = address & PageMask;
        if (mTraceWriteFingerprint || bytes.empty() || bytes.size() > PageSize - offset) {
            return false;
        }
        FastPageGroup* group = mFastPageGroups[address >> FastGroupShift].get();
        const size_t page = (address >> PageShift) & FastGroupPageMask;
        if (group == nullptr || group->Pages[page].Write == nullptr) {
            return false;
        }
        std::memcpy(group->Pages[page].Write + offset, bytes.data(), bytes.size());
        MarkCard(address >> PageShift);
        return true;
    }

    // Write bookkeeping of [address, address + size) (size > 0, mapped): the
    // card of every page it covers, so a store crossing a page boundary marks
    // both pages. Every tracked write path calls it; a host-native helper that
    // writes through a fast-page pointer (guest overrides) calls it for the
    // bytes it wrote.
    void NoteWrite(uint32_t address, size_t size) noexcept {
        const size_t last = (uint64_t{address} + size - 1U) >> PageShift;
        for (size_t page = address >> PageShift; page <= last; ++page) MarkCard(page);
    }

    // fastmem window diagnostics (tests, verify mode): whether this image's
    // regions are in the window (its own flag table, else all zeros), the
    // flag byte and the card of the page of `address`.
    [[nodiscard]] bool InGuestWindow() const noexcept { return mWindowBase != nullptr; }
    [[nodiscard]] uint8_t PageFlagsAt(uint32_t address) const noexcept {
        return static_cast<uint8_t>(mPageFlags[address >> PageShift].Bits);
    }
    [[nodiscard]] uint8_t CardAt(uint32_t address) const noexcept {
        return mCards ? static_cast<uint8_t>(mCards[address >> PageShift].State) : 0U;
    }

    bool Read8(uint32_t address, uint8_t* value) override;
    bool Read16(uint32_t address, uint16_t* value) override;
    bool Read32(uint32_t address, uint32_t* value) override;
    bool Read64(uint32_t address, uint64_t* value,
                uint32_t* faultAddress) override;
    bool Write8(uint32_t address, uint8_t value) override;
    bool Write16(uint32_t address, uint16_t value) override;
    bool Write32(uint32_t address, uint32_t value) override;
    bool Write64(uint32_t address, uint64_t value,
                 uint32_t* faultAddress) override;
    bool LoadExclusive(uint32_t address, uint8_t size, uint64_t* value,
                       uint64_t* token, uint32_t* faultAddress) override;
    oot3d::recomp::a32::ExclusiveStoreResult StoreExclusive(
        uint32_t address, uint8_t size, uint64_t value, uint64_t token,
        uint32_t* faultAddress) override;
    bool AtomicSwap(uint32_t address, uint8_t size, uint32_t replacement,
                    uint32_t* previous, uint32_t* faultAddress) override;

  private:
    // A call-free AOT region may retain views until its next observable exit.
    // This does not change the memory object layout or the title-module ABI.
    friend class NativeA32UnobservedMemory;
    struct Region {
        std::string Name;
        uint32_t BaseAddress = 0;
        std::vector<uint8_t> Bytes;
        bool Writable = false;
        bool Executable = false;
    };

    // Read/Write are the typed fast-path entries; both are null while write
    // tracing is enabled. Bytes is the page's backing store whenever the page
    // is directly addressable (mapped wholly by one region, not watched, not
    // deferred), whatever the trace state: it is what Read used to be for the
    // checked slow path and the span helpers. WriteGeneration is the page's
    // change certificate as of the last RangeWriteGeneration that covered it
    // (pages written since have bit 0 of their card set). 32 bytes, so an
    // entry index is a single shift.
    struct FastPage {
        const uint8_t* Read = nullptr;
        uint8_t* Write = nullptr;
        uint64_t WriteGeneration = 0;
        const uint8_t* Bytes = nullptr;
    };
    static_assert(sizeof(void*) != 8U || sizeof(FastPage) == 32U);
    static constexpr uint32_t PageShift = 12U;
    static constexpr size_t PageSize = size_t{1} << PageShift;
    static constexpr size_t PageMask = PageSize - 1U;
    static constexpr size_t PageCount = size_t{1} << (32U - PageShift);
    static constexpr uint32_t FastGroupShift = 20U;
    static constexpr size_t FastGroupCount =
        size_t{1} << (32U - FastGroupShift);
    static constexpr size_t FastGroupPageCount =
        size_t{1} << (FastGroupShift - PageShift);
    static constexpr size_t FastGroupPageMask = FastGroupPageCount - 1U;
    static constexpr uint16_t AmbiguousPage = 0xFFFFU;
    // One 1 MiB group of the two-level page table (the slow paths, the span
    // helpers and the exclusives; the inline paths use the flag table).
    struct FastPageGroup {
        FastPage Pages[FastGroupPageCount];
    };

    Region* FindRegion(uint32_t address, size_t size);
    const Region* FindRegion(uint32_t address, size_t size) const;
    // Null for empty spans and spans that leave their first page.
    const FastPage* FastPageFor(uint32_t address, size_t size) const noexcept {
        const size_t offset = address & PageMask;
        if (size == 0U || size > PageSize - offset) {
            return nullptr;
        }
        const auto& group = mFastPageGroups[address >> FastGroupShift];
        return group ? &group->Pages[(address >> PageShift) & FastGroupPageMask] : nullptr;
    }
    void RebuildPageTable();
    // Typed entries of one directly addressable page (bytes non-null) under
    // the current trace and observation state.
    void AssignFastPointers(size_t page, FastPage& entry, const uint8_t* bytes,
                            bool writable) const noexcept;
    // Re-derives Read/Write of every directly addressable page after the
    // trace state changed. Generations and stamps are not touched.
    void RefreshFastPointers() noexcept;
    // fastmem window. UpdatePageFlags: residency (a region buffer at window
    // base + address) and the whole flag table, after the regions or many
    // entries changed; an image not in the window points at sNoFastPages.
    // UpdatePageFlag: one page after its entry changed. A flag allows an
    // access only when the entry's pointer is the page's window address, so
    // a trapped entry always traps inline. CheckPageFlags: verify mode.
    void UpdatePageFlags() noexcept;
    void UpdatePageFlag(size_t page) const noexcept;
    [[nodiscard]] uint8_t DerivePageFlags(size_t page) const noexcept;
    void CheckPageFlags() const;
    // The card table exists once the image has a region.
    void EnsureCards();
    // Clears `bits` of the cards of every mapped page (no core in guest
    // code: rebuild, move, copy).
    void ClearCards(uint8_t bits) noexcept;
    // Takes bit 1 (Written) of every mapped page's card (atomic and-fetch):
    // true if any was set.
    [[nodiscard]] bool TakeWrittenCards() const noexcept;
    [[nodiscard]] bool Watches(uint32_t address, size_t size) const noexcept {
        if (!mWatchActive) {
            return false;
        }
        const uint64_t end = static_cast<uint64_t>(address) + size;
        for (const auto& range : mWatchRanges) {
            if (static_cast<uint64_t>(address) < range.second &&
                end > range.first) {
                return true;
            }
        }
        return false;
    }
    // Both raise only when a watch is active, and both are reached only after
    // the range was actually accessed. Counting here, in the checked path,
    // covers every compiled variant: a trapped access from the precompiled
    // title module lands here, not in the inline fast path above.
    void NoteWatchedRead(uint32_t address, uint8_t size) const noexcept {
        ++mWatchCounters.TypedReads;
        if (!mWatchCounters.FirstReadRecorded) {
            mWatchCounters.FirstReadRecorded = true;
            mWatchCounters.FirstReadAddress = address;
            mWatchCounters.FirstReadSize = size;
        }
    }
    void NoteWatchedWrite(uint32_t address, uint8_t size) const noexcept {
        ++mWatchCounters.TypedWrites;
        if (!mWatchCounters.FirstWriteRecorded) {
            mWatchCounters.FirstWriteRecorded = true;
            mWatchCounters.FirstWriteAddress = address;
            mWatchCounters.FirstWriteSize = size;
        }
    }
    bool ReadSized(uint32_t address, uint8_t size, uint64_t* value,
                   uint32_t* faultAddress) const;
    bool WriteSized(uint32_t address, uint8_t size, uint64_t value,
                    uint32_t* faultAddress);
    void RecordTracedWrite(uint32_t address,
                           std::span<const uint8_t> bytes) noexcept;
    void FlushTracedPointerWrites() noexcept;
    static void SetError(std::string* error, std::string_view message);
    void NotifyObservedWrite(uint32_t address, size_t size);
    OOT3D_AOT_HOT bool PageAllows(uint32_t address, uint8_t access) const noexcept {
        return (static_cast<uint8_t>(mPageFlags[address >> PageShift].Bits) & access) != 0U;
    }
    // A store's bookkeeping: its page's card becomes Dirty (both bits), only
    // if it is not Dirty already (see GuestCard). Plain byte accesses: the
    // generations take the bits with atomic and-fetch, so a mark that races a
    // take is either taken or survives.
    OOT3D_AOT_HOT void MarkCard(size_t page) noexcept {
        GuestCard& card = mCards[page];
        if (card.State != GuestCardState::Dirty) [[unlikely]] {
            card.State = GuestCardState::Dirty;
        }
    }
    // A fresh certificate, larger than every one handed out before.
    uint64_t NextRangeGeneration() const noexcept {
        if (++mRangeGeneration == 0U) [[unlikely]] mRangeVersionsValid = false;
        return mRangeGeneration;
    }
    // Restamps every page with one fresh certificate and clears every card:
    // the image was replaced (its generation folded by the assignment).
    void RestampAllPages() noexcept;

    // The inline paths' state, first so it sits at small immediate offsets
    // from the memory object: the page-flag table (own, or the shared zeros),
    // the window base (null while not in the window) and the card table.
    const GuestPageFlag* mPageFlags = sNoFastPages;
    uint8_t* mWindowBase = nullptr;
    std::unique_ptr<GuestCard[]> mCards;
    std::unique_ptr<GuestPageFlag[]> mOwnPageFlags;
    static GuestPageFlag sNoFastPages[PageCount];

    mutable std::vector<Region> mRegions;
    std::vector<uint16_t> mPageRegions;
    mutable bool mRangeVersionsValid = true;
    // Architectural write generation (savestates).
    mutable uint64_t mWriteGeneration = 1;
    // Certificate sequence (RangeWriteGeneration). Not serialized; replacing
    // an image or rebuilding its page table restamps every page with a fresh
    // value, independently of the architectural generation.
    mutable uint64_t mRangeGeneration = 1;
    std::array<std::unique_ptr<FastPageGroup>, FastGroupCount> mFastPageGroups;
    uint64_t mWriteTraceFingerprint = 14695981039346656037ULL;
    bool mTraceWriteFingerprint = false;
    mutable uint64_t mFastReadMismatchCount = 0;
    mutable uint32_t mLastFastReadMismatchAddress = 0;
    mutable uint64_t mLastFastReadMismatchValue = 0;
    mutable uint64_t mLastCheckedReadMismatchValue = 0;
    std::vector<std::pair<uint32_t, size_t>> mPendingTracedPointerWrites;
    // Diagnostic watch state. Empty and inactive in normal runs.
    std::vector<std::pair<uint32_t, uint32_t>> mWatchRanges;
  public:
    // Scoped suppression for the renderer's own materialization: it must not be
    // possible to leave the watch suppressed by returning early.
    class RangeWatchSuppression {
      public:
        explicit RangeWatchSuppression(NativeA32Memory& memory) noexcept
            : mMemory(&memory), mPrevious(memory.mWatchSuppressed) {
            memory.mWatchSuppressed = true;
        }
        ~RangeWatchSuppression() { mMemory->mWatchSuppressed = mPrevious; }
        RangeWatchSuppression(const RangeWatchSuppression&) = delete;
        RangeWatchSuppression& operator=(const RangeWatchSuppression&) = delete;

      private:
        NativeA32Memory* mMemory = nullptr;
        bool mPrevious = false;
    };

  private:
    std::vector<size_t> mTrappedPages;
    mutable RangeWatchCounters mWatchCounters;
    bool mWatchActive = false;
    bool mWatchSuppressed = false;
    WriteObserver mWriteObserver;
    std::vector<std::pair<uint32_t, size_t>> mObservedWriteRanges;
    std::vector<size_t> mObservedWritePages;
    // Mutable pointers have no release contract. Their ranges cannot become
    // GPU-owned again until the memory image is replaced.
    std::vector<std::pair<uint32_t, size_t>> mEscapedWritePointers;
    mutable std::vector<std::pair<uint32_t, size_t>> mEscapedReadPointers;
    struct DeferredWrite {
        uint32_t Address = 0;
        size_t Size = 0;
        std::shared_ptr<const DeferredRead> Reader;
    };
    mutable std::vector<DeferredWrite> mDeferredWrites;
    mutable std::vector<size_t> mDeferredPages;
    bool ResolveDeferredRead(uint32_t address, size_t size) const;
    void DiscardDeferredWrites(uint32_t address, size_t size) const;
    void ReleaseDeferredPageTraps() const;
};

} // namespace Oot3dNativeGame
