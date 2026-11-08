#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_memory_peek.h"
#include "oot3d_native_a32_second_core.h"
#include "oot3d_switch_guest_window.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Oot3dNativeGame {
namespace {

// Second core (oot3d_native_a32_second_core.h): every out-of-line path below
// that reads or changes the image's bytes or bookkeeping holds the kernel lock
// while it is active; nothing is taken otherwise.
using KernelGuard = SecondCore::KernelGuard;

// The global exclusive monitor: the reservation granule of each host core.
// Granules are 8-byte aligned, so an odd value is never one.
constexpr uint32_t kNoReservation = 1U;
std::mutex gExclusiveMutex;
std::array<uint32_t, SecondCore::kProcessors> gExclusiveGranule{
    kNoReservation, kNoReservation};

// Under gExclusiveMutex: an exclusive store to `granule` ends every core's
// reservation of it.
void ClearExclusiveGranule(uint32_t granule) noexcept {
    for (auto& reserved : gExclusiveGranule) {
        if (reserved == granule) reserved = kNoReservation;
    }
}

uint64_t SizeMask(uint8_t size) noexcept {
    return size >= 8U ? ~uint64_t{0} : (uint64_t{1} << (size * 8U)) - 1U;
}

// Host atomics on guest bytes, for an access the guest aligned to its size
// on a host address with the same alignment.
bool AtomicAccessible(uint32_t address, const uint8_t* bytes, uint8_t size) noexcept {
    return (size == 1U || size == 2U || size == 4U || size == 8U) &&
           (address & (size - 1U)) == 0U &&
           (reinterpret_cast<uintptr_t>(bytes) & (size - 1U)) == 0U;
}

uint64_t AtomicLoadGuest(const uint8_t* bytes, uint8_t size) noexcept {
    switch (size) {
    case 1: return __atomic_load_n(bytes, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n(reinterpret_cast<const uint16_t*>(bytes), __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n(reinterpret_cast<const uint32_t*>(bytes), __ATOMIC_SEQ_CST);
    default: return __atomic_load_n(reinterpret_cast<const uint64_t*>(bytes), __ATOMIC_SEQ_CST);
    }
}

template <typename T>
bool AtomicCompareExchangeAs(uint8_t* bytes, uint64_t expected, uint64_t desired) noexcept {
    T value = static_cast<T>(expected);
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(bytes), &value,
                                       static_cast<T>(desired), false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

bool AtomicCompareExchangeGuest(uint8_t* bytes, uint8_t size, uint64_t expected,
                                uint64_t desired) noexcept {
    switch (size) {
    case 1: return AtomicCompareExchangeAs<uint8_t>(bytes, expected, desired);
    case 2: return AtomicCompareExchangeAs<uint16_t>(bytes, expected, desired);
    case 4: return AtomicCompareExchangeAs<uint32_t>(bytes, expected, desired);
    default: return AtomicCompareExchangeAs<uint64_t>(bytes, expected, desired);
    }
}

uint64_t AtomicExchangeGuest(uint8_t* bytes, uint8_t size, uint32_t replacement) noexcept {
    return size == 1U
               ? __atomic_exchange_n(bytes, static_cast<uint8_t>(replacement), __ATOMIC_SEQ_CST)
               : __atomic_exchange_n(reinterpret_cast<uint32_t*>(bytes), replacement,
                                     __ATOMIC_SEQ_CST);
}

// One bit of a card seen and cleared in one step (atomic and-fetch), so a
// concurrent store's mark (the inline path's plain byte store) is never lost
// between the test and the clear, and the other bit is left alone. Same
// result as the test-and-clear when single-threaded.
bool TakeCardBit(GuestCard& card, GuestCardState bit) noexcept {
    const auto mask = static_cast<unsigned char>(bit);
    return (__atomic_fetch_and(reinterpret_cast<unsigned char*>(&card.State),
                               static_cast<unsigned char>(~mask), __ATOMIC_ACQ_REL) &
            mask) != 0U;
}

// config/vertex_snapshot_fast (oot3d_native_a32_memory_peek.h): a certificate
// reads the card first and takes the bit only when it is set. A clean card's
// and-fetch changes nothing but costs an exclusive load/store pair with
// acquire/release ordering on the A57 (no LSE) and makes the card's line
// exclusive to this core; a set bit is still taken by the same and-fetch.
bool gRangeCardPeek = false;

bool CardBitSeenClean(const GuestCard& card, GuestCardState bit) noexcept {
    return gRangeCardPeek &&
           (__atomic_load_n(reinterpret_cast<const unsigned char*>(&card.State), __ATOMIC_ACQUIRE) &
            static_cast<unsigned char>(bit)) == 0U;
}

// A store's mark from the lock-free exclusive/swap paths (another core may
// take the card's bits meanwhile).
void MarkCardAtomic(GuestCard& card) noexcept {
    __atomic_store_n(reinterpret_cast<unsigned char*>(&card.State),
                     static_cast<unsigned char>(GuestCardState::Dirty), __ATOMIC_RELEASE);
}

// Eight cards at once (WriteGeneration's scan, the clears): an access of this
// type may alias the GuestCard bytes.
using CardWord = uint64_t __attribute__((may_alias));
constexpr uint64_t CardWordOf(uint8_t bits) noexcept {
    return uint64_t{bits} * 0x0101010101010101ULL;
}

bool RangeFits(uint32_t baseAddress, size_t size) {
    return size != 0 &&
           static_cast<uint64_t>(baseAddress) + static_cast<uint64_t>(size) <=
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U;
}

bool RangesOverlap(uint32_t leftBase, size_t leftSize, uint32_t rightBase,
                   size_t rightSize) {
    const uint64_t leftEnd = static_cast<uint64_t>(leftBase) + leftSize;
    const uint64_t rightEnd = static_cast<uint64_t>(rightBase) + rightSize;
    return static_cast<uint64_t>(leftBase) < rightEnd &&
           static_cast<uint64_t>(rightBase) < leftEnd;
}

bool IsSupportedSize(uint8_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

void AddWriteRange(std::vector<std::pair<uint32_t, size_t>>& ranges,
                   uint32_t address, size_t size) {
    uint64_t begin = address, end = begin + size;
    auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, uint64_t value) {
            return uint64_t{range.first} + range.second < value;
        });
    auto last = first;
    while (last != ranges.end() && last->first <= end) {
        begin = std::min(begin, uint64_t{last->first});
        end = std::max(end, uint64_t{last->first} + last->second);
        ++last;
    }
    const auto position = ranges.erase(first, last);
    ranges.insert(position, {static_cast<uint32_t>(begin), static_cast<size_t>(end - begin)});
}

// ---- fastmem window (oot3d_switch_guest_window.h) ---------------------------
// The window's owner keeps its regions at window base + guest address (the
// allocator places them there). Every image whose regions are in the window
// keeps its own flag table, derived from its typed page entries
// (UpdatePageFlags/UpdatePageFlag): the inline paths read it instead of the
// page table. Correctness never depends on ownership: a flag allows an access
// only when the entry's own pointer is the page's window address, and
// placements never overlap, so the window bytes behind an allowed flag are
// always the image's own. An image with no region in the window (window off,
// a copy, a staged image) points at sNoFastPages and every access takes the
// slow path.

// Owner only: every region not yet at window base + address moves there (a
// copy). True when one moved; a region the window refuses stays where it is.
template <typename Regions>
bool HomeRegions(Regions& regions) {
    bool moved = false;
    for (auto& region : regions) {
        if (region.Bytes.empty() || GuestWindow::Placed(region.Bytes.data(), region.BaseAddress))
            continue;
        std::remove_reference_t<decltype(region.Bytes)> placed;
        if (!GuestWindow::Reserve(placed, region.BaseAddress, region.Bytes.size())) continue;
        placed.assign(region.Bytes.begin(), region.Bytes.end());
        region.Bytes.swap(placed);
        moved = true;
    }
    return moved;
}

// Owner only: `target` becomes a copy of `source` with every buffer in the
// window where it can be (the old buffers are released first).
template <typename Regions>
void CopyRegionsHome(Regions& target, const Regions& source) {
    target.clear();
    target.reserve(source.size());
    for (const auto& region : source) {
        auto& copy = target.emplace_back();
        copy.Name = region.Name;
        copy.BaseAddress = region.BaseAddress;
        GuestWindow::Reserve(copy.Bytes, region.BaseAddress, region.Bytes.size());
        copy.Bytes.assign(region.Bytes.begin(), region.Bytes.end());
        copy.Writable = region.Writable;
        copy.Executable = region.Executable;
    }
}

} // namespace

NativeA32Memory::NativeA32Memory(const NativeA32Memory& other)
    : mRegions((other.MaterializeDeferredWrites()
                    ? other.mRegions
                    : throw std::runtime_error("cannot copy deferred guest memory"))),
      mWriteGeneration(other.WriteGeneration()),
      // Certificates of the copy never equal ones the source handed out.
      mRangeGeneration(other.mRangeGeneration),
      mWriteTraceFingerprint(other.mWriteTraceFingerprint),
      mTraceWriteFingerprint(other.mTraceWriteFingerprint),
      mFastReadMismatchCount(other.mFastReadMismatchCount),
      mLastFastReadMismatchAddress(other.mLastFastReadMismatchAddress),
      mLastFastReadMismatchValue(other.mLastFastReadMismatchValue),
      mLastCheckedReadMismatchValue(other.mLastCheckedReadMismatchValue) {
    RebuildPageTable();
}

NativeA32Memory& NativeA32Memory::operator=(const NativeA32Memory& other) {
    if (this != &other) {
        // The old image's buffers and page groups are freed below.
        const SecondCore::WorldStop stop;
        if (!other.MaterializeDeferredWrites())
            throw std::runtime_error("cannot copy deferred guest memory");
        mDeferredWrites.clear();
        mDeferredPages.clear();
        mEscapedReadPointers.clear();
        mEscapedWritePointers.clear();
        // The replaced image's pending writes go with it: its generation is
        // the copy's from here on.
        ClearCards(static_cast<uint8_t>(GuestCardState::Dirty));
        // The window's owner gets its copy at window base + address again.
        if (GuestWindow::IsOwner(this)) {
            CopyRegionsHome(mRegions, other.mRegions);
        } else {
            mRegions = other.mRegions;
        }
        mWriteGeneration = other.WriteGeneration();
        mWriteTraceFingerprint = other.mWriteTraceFingerprint;
        mTraceWriteFingerprint = other.mTraceWriteFingerprint;
        mFastReadMismatchCount = other.mFastReadMismatchCount;
        mLastFastReadMismatchAddress = other.mLastFastReadMismatchAddress;
        mLastFastReadMismatchValue = other.mLastFastReadMismatchValue;
        mLastCheckedReadMismatchValue = other.mLastCheckedReadMismatchValue;
        // New certificates for every page (RebuildPageTable restamps).
        RebuildPageTable();
    }
    return *this;
}

NativeA32Memory::NativeA32Memory(NativeA32Memory&& other) noexcept {
    *this = std::move(other);
}

NativeA32Memory& NativeA32Memory::operator=(NativeA32Memory&& other) noexcept {
    if (this == &other) return *this;
    // The old image's buffers and page groups are freed below.
    const SecondCore::WorldStop stop;
    const bool versionsValid = mRangeVersionsValid && other.mRangeVersionsValid;
    // Folds the source's written cards into its generation: the cards that
    // move with the image carry no Written bit afterwards.
    mWriteGeneration = other.WriteGeneration();
    mRegions = std::move(other.mRegions);
    mPageRegions = std::move(other.mPageRegions);
    mFastPageGroups = std::move(other.mFastPageGroups);
    mCards = std::move(other.mCards);
    mOwnPageFlags = std::move(other.mOwnPageFlags);
    mPageFlags = other.mPageFlags;
    mWindowBase = other.mWindowBase;
    other.mPageFlags = sNoFastPages;
    other.mWindowBase = nullptr;
    mRangeVersionsValid = versionsValid;
    mWriteTraceFingerprint = other.mWriteTraceFingerprint;
    mTraceWriteFingerprint = other.mTraceWriteFingerprint;
    mFastReadMismatchCount = other.mFastReadMismatchCount;
    mLastFastReadMismatchAddress = other.mLastFastReadMismatchAddress;
    mLastFastReadMismatchValue = other.mLastFastReadMismatchValue;
    mLastCheckedReadMismatchValue = other.mLastCheckedReadMismatchValue;
    mPendingTracedPointerWrites = std::move(other.mPendingTracedPointerWrites);
    mWatchRanges = std::move(other.mWatchRanges);
    mTrappedPages = std::move(other.mTrappedPages);
    mWatchCounters = other.mWatchCounters;
    mWatchActive = other.mWatchActive;
    mWatchSuppressed = other.mWatchSuppressed;
    mWriteObserver = std::move(other.mWriteObserver);
    mObservedWriteRanges = std::move(other.mObservedWriteRanges);
    mObservedWritePages = std::move(other.mObservedWritePages);
    mEscapedWritePointers = std::move(other.mEscapedWritePointers);
    mEscapedReadPointers = std::move(other.mEscapedReadPointers);
    mDeferredWrites = std::move(other.mDeferredWrites);
    mDeferredPages = std::move(other.mDeferredPages);
    // Backing allocations, their fast pointers, flags and cards move
    // together. Only the destination's certificates change (one fresh value
    // on every page, the cards cleared); no page-table allocation. The
    // window's owner copies buffers that arrived from elsewhere (a staged or
    // restored image) to window base + address; their pointers change, so
    // its page table is rebuilt instead (which restamps too).
    if (GuestWindow::IsOwner(this) && HomeRegions(mRegions)) {
        RebuildPageTable();
    } else {
        RestampAllPages();
        UpdatePageFlags();
    }
    // A reused moved-from image must not revive its old certificates either.
    other.mRangeVersionsValid = false;
    return *this;
}

void NativeA32Memory::RestampAllPages() noexcept {
    const uint64_t stamp = NextRangeGeneration();
    for (auto& group : mFastPageGroups) {
        if (!group) continue;
        for (size_t page = 0; page < FastGroupPageCount; ++page) {
            group->Pages[page].WriteGeneration = stamp;
        }
    }
    ClearCards(static_cast<uint8_t>(GuestCardState::Dirty));
}

GuestPageFlag NativeA32Memory::sNoFastPages[NativeA32Memory::PageCount] = {};

uint8_t NativeA32Memory::DerivePageFlags(size_t page) const noexcept {
    const auto& group = mFastPageGroups[page / FastGroupPageCount];
    if (!group || mWindowBase == nullptr) return 0U;
    const FastPage& entry = group->Pages[page & FastGroupPageMask];
    const uint8_t* home = mWindowBase + (page << PageShift);
    return static_cast<uint8_t>((entry.Read == home ? kGuestPageLoadFast : 0U) |
                                (entry.Write == home ? kGuestPageStoreFast : 0U));
}

void NativeA32Memory::UpdatePageFlags() noexcept {
    uint8_t* const base = GuestWindow::Base();
    const bool inWindow =
        base != nullptr && std::any_of(mRegions.begin(), mRegions.end(), [](const Region& region) {
            return !region.Bytes.empty() &&
                   GuestWindow::Placed(region.Bytes.data(), region.BaseAddress);
        });
    if (inWindow && !mOwnPageFlags) {
        // Value-initialized: every page traps until derived below. Without the
        // memory the image just stays on the slow path.
        mOwnPageFlags.reset(new (std::nothrow) GuestPageFlag[PageCount]());
    }
    // Pointers are stored only when they change: a trace switch re-derives
    // under the kernel lock while the other core may be reading them.
    if (!inWindow || !mOwnPageFlags) {
        if (mPageFlags != sNoFastPages) mPageFlags = sNoFastPages;
        if (mWindowBase != nullptr) mWindowBase = nullptr;
        mOwnPageFlags.reset();
        return;
    }
    if (mWindowBase != base) mWindowBase = base;
    GuestPageFlag* flags = mOwnPageFlags.get();
    // Under the world stop or the kernel lock (trace switch): a page seen
    // cleared meanwhile only takes the slow path.
    std::memset(static_cast<void*>(flags), 0, PageCount);
    for (size_t groupIndex = 0; groupIndex < FastGroupCount; ++groupIndex) {
        if (!mFastPageGroups[groupIndex]) continue;
        for (size_t index = 0; index < FastGroupPageCount; ++index) {
            const size_t page = groupIndex * FastGroupPageCount + index;
            flags[page].Bits = static_cast<GuestPageFlags>(DerivePageFlags(page));
        }
    }
    if (mPageFlags != flags) mPageFlags = flags;
    if (GuestWindow::Verifying()) CheckPageFlags();
}

void NativeA32Memory::UpdatePageFlag(size_t page) const noexcept {
    // Not in the window: every page traps already.
    if (!mOwnPageFlags) return;
    mOwnPageFlags[page].Bits = static_cast<GuestPageFlags>(DerivePageFlags(page));
}

void NativeA32Memory::CheckPageFlags() const {
    if (!GuestWindow::Verifying()) return;
    uint64_t mismatches = 0;
    if (!mOwnPageFlags) {
        // An image outside the window must trap everywhere.
        mismatches += mPageFlags != sNoFastPages || mWindowBase != nullptr;
    } else {
        for (size_t page = 0; page < PageCount; ++page) {
            if (static_cast<uint8_t>(mOwnPageFlags[page].Bits) != DerivePageFlags(page))
                ++mismatches;
        }
        mismatches += mPageFlags != mOwnPageFlags.get();
    }
    GuestWindow::CountShadowCheck(mismatches);
}

// Store cards (oot3d_native_a32_memory.h). Reference, no code copied: the
// idea of marking each written 4 KiB page with a byte store instead of a
// global write counter, and its aliasing pitfall, come from sa2-nx's
// tools/store_cards.py and docs (https://github.com/ChanseyIsTheBest/sa2-nx,
// GPL-3.0); the two-bit cards, their union type and the generations are
// this port's.
void NativeA32Memory::EnsureCards() {
    if (!mCards) mCards.reset(new GuestCard[PageCount]());
}

void NativeA32Memory::ClearCards(uint8_t bits) noexcept {
    if (!mCards) return;
    auto* words = reinterpret_cast<CardWord*>(mCards.get());
    const uint64_t keep = ~CardWordOf(bits);
    for (const auto& region : mRegions) {
        if (region.Bytes.empty()) continue;
        const size_t first = (region.BaseAddress >> PageShift) / 8U;
        const size_t last =
            static_cast<size_t>((uint64_t{region.BaseAddress} + region.Bytes.size() - 1U) >>
                                PageShift) / 8U;
        for (size_t word = first; word <= last; ++word) words[word] &= keep;
    }
}

bool NativeA32Memory::TakeWrittenCards() const noexcept {
    if (!mCards) return false;
    auto* words = reinterpret_cast<CardWord*>(mCards.get());
    constexpr uint64_t written = CardWordOf(static_cast<uint8_t>(GuestCardState::Written));
    bool any = false;
    for (const auto& region : mRegions) {
        if (region.Bytes.empty()) continue;
        const size_t first = (region.BaseAddress >> PageShift) / 8U;
        const size_t last =
            static_cast<size_t>((uint64_t{region.BaseAddress} + region.Bytes.size() - 1U) >>
                                PageShift) / 8U;
        for (size_t word = first; word <= last; ++word) {
            // Read first: a word with no Written bit is not written to, so
            // the scan leaves the lines of untouched pages shared.
            if ((__atomic_load_n(&words[word], __ATOMIC_RELAXED) & written) == 0U) continue;
            if ((__atomic_fetch_and(&words[word], ~written, __ATOMIC_ACQ_REL) & written) != 0U)
                any = true;
        }
    }
    return any;
}

void NativeA32Memory::SetError(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool NativeA32Memory::MapRegion(const NativeA32MemoryRegionConfig& config,
                                std::string* error) {
    // A new region rebuilds the page table (RebuildPageTable).
    const SecondCore::WorldStop stop;
    if (config.Name.empty()) {
        SetError(error, "guest memory region has no name");
        return false;
    }
    if (!RangeFits(config.BaseAddress, config.Size)) {
        SetError(error, "guest memory region has an invalid address range");
        return false;
    }
    if (config.InitialBytes.size() > config.Size) {
        SetError(error, "guest memory initial image exceeds its region");
        return false;
    }
    if (mRegions.size() >= AmbiguousPage - 1U) {
        SetError(error, "guest memory has too many regions for its page table");
        return false;
    }
    for (const auto& region : mRegions) {
        if (RangesOverlap(config.BaseAddress, config.Size, region.BaseAddress,
                          region.Bytes.size())) {
            SetError(error, "guest memory regions overlap");
            return false;
        }
    }

    // fastmem window: the owner's buffers live at window base + address. An
    // image claims the window with its first region while no buffer lives
    // there (the process claims its own image at construction).
    const bool windowOwner =
        GuestWindow::Accepting() &&
        (GuestWindow::IsOwner(this) || (mRegions.empty() && GuestWindow::ClaimIfVacant(this)));
    Region region;
    region.Name = config.Name;
    region.BaseAddress = config.BaseAddress;
    if (windowOwner) GuestWindow::Reserve(region.Bytes, config.BaseAddress, config.Size);
    region.Bytes.assign(config.Size, 0);
    std::copy(config.InitialBytes.begin(), config.InitialBytes.end(),
              region.Bytes.begin());
    region.Writable = config.Writable;
    region.Executable = config.Executable;
    mRegions.push_back(std::move(region));
    std::sort(mRegions.begin(), mRegions.end(),
              [](const Region& left, const Region& right) {
                  return left.BaseAddress < right.BaseAddress;
              });
    RebuildPageTable();
    return true;
}

void NativeA32Memory::RebuildPageTable() {
    // Frees every page group: no other core may be in guest code, where it
    // holds page pointers.
    const SecondCore::WorldStop stop;
    mPageRegions.assign(PageCount, 0U);
    for (auto& group : mFastPageGroups) {
        group.reset();
    }
    // Every mapped page gets one fresh certificate, so writes not yet folded
    // into a stamp still change it; the cards' RangeWritten bits are cleared
    // with it (their Written bits stay for WriteGeneration).
    if (!mRegions.empty()) EnsureCards();
    ClearCards(static_cast<uint8_t>(GuestCardState::RangeWritten));
    const uint64_t stamp = NextRangeGeneration();
    for (size_t regionIndex = 0; regionIndex < mRegions.size();
         ++regionIndex) {
        auto& region = mRegions[regionIndex];
        const uint64_t lastAddress =
            static_cast<uint64_t>(region.BaseAddress) +
            region.Bytes.size() - 1U;
        const size_t firstPage = region.BaseAddress >> PageShift;
        const size_t lastPage = static_cast<size_t>(lastAddress >> PageShift);
        const uint16_t pageValue =
            static_cast<uint16_t>(regionIndex + 1U);
        for (size_t page = firstPage; page <= lastPage; ++page) {
            auto& entry = mPageRegions[page];
            if (entry == 0U) {
                entry = pageValue;
            } else if (entry != pageValue) {
                entry = AmbiguousPage;
            }
            auto& group = mFastPageGroups[page / FastGroupPageCount];
            if (!group) group = std::make_unique<FastPageGroup>();
            auto& fastPage = group->Pages[page & FastGroupPageMask];
            fastPage.WriteGeneration = stamp;

            const uint64_t pageAddress =
                static_cast<uint64_t>(page) << PageShift;
            const uint64_t regionEnd =
                static_cast<uint64_t>(region.BaseAddress) +
                region.Bytes.size();
            if (pageAddress >= region.BaseAddress &&
                pageAddress + PageSize <= regionEnd) {
                const size_t offset = static_cast<size_t>(
                    pageAddress - region.BaseAddress);
                // Watched pages keep no fast entry: typed accesses then take
                // the checked path, which reads and writes the same bytes.
                const bool trapped =
                    std::binary_search(mTrappedPages.begin(), mTrappedPages.end(), page) ||
                    std::binary_search(mDeferredPages.begin(), mDeferredPages.end(), page);
                if (trapped) {
                    continue;
                }
                AssignFastPointers(page, fastPage, region.Bytes.data() + offset,
                                   region.Writable);
            }
        }
    }
    UpdatePageFlags();
}

void NativeA32Memory::AssignFastPointers(size_t page, FastPage& entry,
                                         const uint8_t* bytes,
                                         bool writable) const noexcept {
    entry.Bytes = bytes;
    // While write tracing is enabled no typed access may complete inline:
    // reads must be cross-checked and writes recorded by the slow path.
    entry.Read = mTraceWriteFingerprint ? nullptr : bytes;
    entry.Write = !mTraceWriteFingerprint && writable &&
                          !std::binary_search(mObservedWritePages.begin(),
                                              mObservedWritePages.end(), page)
                      ? const_cast<uint8_t*>(bytes) : nullptr;
}

void NativeA32Memory::RefreshFastPointers() noexcept {
    for (size_t groupIndex = 0; groupIndex < FastGroupCount; ++groupIndex) {
        auto& group = mFastPageGroups[groupIndex];
        if (!group) continue;
        for (size_t index = 0; index < FastGroupPageCount; ++index) {
            auto& entry = group->Pages[index];
            if (entry.Bytes == nullptr) continue;
            const size_t page = groupIndex * FastGroupPageCount + index;
            const auto* region = FindRegion(
                static_cast<uint32_t>(page << PageShift), PageSize);
            AssignFastPointers(page, entry, entry.Bytes,
                               region != nullptr && region->Writable);
        }
    }
    UpdatePageFlags();
}

NativeA32Memory::Region* NativeA32Memory::FindRegion(uint32_t address,
                                                     size_t size) {
    return const_cast<Region*>(
        static_cast<const NativeA32Memory*>(this)->FindRegion(address, size));
}

const NativeA32Memory::Region* NativeA32Memory::FindRegion(
    uint32_t address, size_t size) const {
    if (!RangeFits(address, size)) {
        return nullptr;
    }
    const uint64_t end = static_cast<uint64_t>(address) + size;
    const auto contains = [&](size_t index) {
        const auto& region = mRegions[index];
        return address >= region.BaseAddress &&
               end <= static_cast<uint64_t>(region.BaseAddress) +
                          region.Bytes.size();
    };
    if (!mPageRegions.empty()) {
        const uint16_t pageEntry = mPageRegions[address >> PageShift];
        if (pageEntry != 0U && pageEntry != AmbiguousPage) {
            const size_t index = pageEntry - 1U;
            if (index < mRegions.size() && contains(index)) {
                return &mRegions[index];
            }
        }
    }
    auto found = std::upper_bound(
        mRegions.begin(), mRegions.end(), address,
        [](uint32_t candidate, const Region& region) {
            return candidate < region.BaseAddress;
        });
    if (found == mRegions.begin()) {
        return nullptr;
    }
    const size_t index =
        static_cast<size_t>(std::distance(mRegions.begin(), found - 1));
    if (contains(index)) {
        return &mRegions[index];
    }
    return nullptr;
}

bool NativeA32Memory::IsMapped(uint32_t address, size_t size) const {
    const KernelGuard kernel;
    return FindRegion(address, size) != nullptr;
}

bool NativeA32Memory::IsWritable(uint32_t address, size_t size) const {
    const KernelGuard kernel;
    const auto* region = FindRegion(address, size);
    return region != nullptr && region->Writable;
}

bool NativeA32Memory::IsExecutable(uint32_t address, size_t size) const {
    const KernelGuard kernel;
    const auto* region = FindRegion(address, size);
    return region != nullptr && region->Executable;
}

size_t NativeA32Memory::RegionCount() const {
    const KernelGuard kernel;
    return mRegions.size();
}

uint64_t NativeA32Memory::WriteGeneration() const {
    const KernelGuard kernel;
    // One step per observation that saw new writes: two observations differ
    // exactly when something was written between them, as with the per-write
    // count this replaces (savestates compare it for equality only). The
    // Written bits are taken atomically: a second core's store marks its card
    // concurrently.
    if (TakeWrittenCards()) {
        __atomic_add_fetch(&mWriteGeneration, 1U, __ATOMIC_RELAXED);
    }
    return __atomic_load_n(&mWriteGeneration, __ATOMIC_RELAXED);
}

std::optional<uint64_t>
NativeA32Memory::RangeWriteGeneration(uint32_t address,
                                      size_t size) const noexcept {
  const KernelGuard kernel;
  if (!mRangeVersionsValid || FindRegion(address, size) == nullptr ||
      HasEscapedWritePointer(address, size)) return std::nullopt;
  uint64_t generation = 0;
  uint64_t fresh = 0;
  const size_t last = (uint64_t{address} + size - 1U) >> PageShift;
  for (size_t page = address >> PageShift; page <= last; ++page) {
      FastPageGroup* group = mFastPageGroups[page / FastGroupPageCount].get();
      if (!group) return std::nullopt;
      auto& stamp = group->Pages[page & FastGroupPageMask].WriteGeneration;
      // Seen and cleared in one step (TakeCardBit): a mark a second core
      // sets meanwhile survives for the next certificate. A card read clean
      // first (the peek) is left as it is.
      if (!CardBitSeenClean(mCards[page], GuestCardState::RangeWritten) &&
          TakeCardBit(mCards[page], GuestCardState::RangeWritten)) {
          // Written since its last certificate: a value no earlier
          // certificate can equal (one per call is enough for the max).
          if (fresh == 0U) fresh = NextRangeGeneration();
          stamp = fresh;
      }
      if (!stamp) return std::nullopt;
      generation = std::max(generation, stamp);
  }
  if (!mRangeVersionsValid) return std::nullopt;
  return generation;
}

void SetRangeWriteGenerationCardPeek(bool enabled) noexcept {
    gRangeCardPeek = enabled;
}

void NativeA32Memory::EnableWriteTraceFingerprint(bool enabled) noexcept {
    const KernelGuard kernel;
    const bool changed = mTraceWriteFingerprint != enabled;
    mTraceWriteFingerprint = enabled;
    mWriteTraceFingerprint = 14695981039346656037ULL;
    mPendingTracedPointerWrites.clear();
    // The trace state lives in the typed page entries (see FastPage).
    if (changed) RefreshFastPointers();
}

uint64_t NativeA32Memory::WriteTraceFingerprint() noexcept {
    const KernelGuard kernel;
    FlushTracedPointerWrites();
    return mWriteTraceFingerprint;
}

uint64_t NativeA32Memory::FastReadMismatchCount() const noexcept {
    return mFastReadMismatchCount;
}

uint32_t NativeA32Memory::LastFastReadMismatchAddress() const noexcept {
    return mLastFastReadMismatchAddress;
}

uint64_t NativeA32Memory::LastFastReadMismatchValue() const noexcept {
    return mLastFastReadMismatchValue;
}

uint64_t NativeA32Memory::LastCheckedReadMismatchValue() const noexcept {
    return mLastCheckedReadMismatchValue;
}

void NativeA32Memory::RecordTracedWrite(
    uint32_t address, std::span<const uint8_t> bytes) noexcept {
    if (!mTraceWriteFingerprint) {
        return;
    }
    const auto append = [&](uint8_t byte) {
        mWriteTraceFingerprint ^= byte;
        mWriteTraceFingerprint *= 1099511628211ULL;
    };
    for (size_t index = 0; index < sizeof(address); ++index) {
        append(static_cast<uint8_t>(address >> (index * 8U)));
    }
    const uint64_t size = bytes.size();
    for (size_t index = 0; index < sizeof(size); ++index) {
        append(static_cast<uint8_t>(size >> (index * 8U)));
    }
    for (const uint8_t byte : bytes) {
        append(byte);
    }
}

void NativeA32Memory::FlushTracedPointerWrites() noexcept {
    for (const auto& [address, size] : mPendingTracedPointerWrites) {
        const auto* region = FindRegion(address, size);
        if (region == nullptr) {
            continue;
        }
        const size_t offset =
            static_cast<size_t>(address - region->BaseAddress);
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    mPendingTracedPointerWrites.clear();
}

uint64_t NativeA32Memory::ContentFingerprint() const {
    const KernelGuard kernel;
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot fingerprint deferred guest memory");
    uint64_t hash = 14695981039346656037ULL;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& region : mRegions) {
        append(region.Name.data(), region.Name.size());
        append(&region.BaseAddress, sizeof(region.BaseAddress));
        append(&region.Writable, sizeof(region.Writable));
        append(&region.Executable, sizeof(region.Executable));
        if (!region.Bytes.empty()) {
            append(region.Bytes.data(), region.Bytes.size());
        }
    }
    return hash;
}

uint64_t NativeA32Memory::StateFingerprint() const {
    const KernelGuard kernel;
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot fingerprint deferred guest memory");
    uint64_t hash = 14695981039346656037ULL;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    const uint64_t writeGeneration = WriteGeneration();
    append(&writeGeneration, sizeof(writeGeneration));
    for (const auto& region : mRegions) {
        append(region.Name.data(), region.Name.size());
        append(&region.BaseAddress, sizeof(region.BaseAddress));
        append(&region.Writable, sizeof(region.Writable));
        append(&region.Executable, sizeof(region.Executable));
        if (!region.Bytes.empty()) {
            append(region.Bytes.data(), region.Bytes.size());
        }
    }
    return hash;
}

nlohmann::json NativeA32Memory::CaptureState() const {
    const KernelGuard kernel;
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot capture deferred guest memory");
    CheckPageFlags();
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& region : mRegions) {
        regions.push_back({
            {"name", region.Name},
            {"base_address", region.BaseAddress},
            {"writable", region.Writable},
            {"executable", region.Executable},
            {"bytes", nlohmann::json::binary(region.Bytes)},
        });
    }
    return {
        {"format", "oot3d_native_a32_memory_state_v1"},
        {"write_generation", WriteGeneration()},
        {"regions", std::move(regions)},
    };
}

bool NativeA32Memory::RestoreState(const nlohmann::json& state,
                                   std::string* error) {
    const KernelGuard kernel;
    try {
        if (!state.is_object() ||
            state.value("format", std::string{}) !=
                "oot3d_native_a32_memory_state_v1" ||
            !state.contains("regions") || !state["regions"].is_array()) {
            SetError(error, "native A32 memory state format is invalid");
            return false;
        }
        NativeA32Memory staged;
        for (const auto& encoded : state["regions"]) {
            if (!encoded.is_object() || !encoded.contains("bytes") ||
                !encoded["bytes"].is_binary()) {
                SetError(error, "native A32 memory region state is invalid");
                return false;
            }
            const auto& bytes = encoded["bytes"].get_binary();
            NativeA32MemoryRegionConfig config;
            config.Name = encoded.at("name").get<std::string>();
            config.BaseAddress = encoded.at("base_address").get<uint32_t>();
            config.Size = bytes.size();
            config.Writable = encoded.at("writable").get<bool>();
            config.Executable = encoded.at("executable").get<bool>();
            config.InitialBytes = std::span<const uint8_t>(bytes.data(),
                                                           bytes.size());
            if (!staged.MapRegion(config, error)) {
                return false;
            }
        }
        // The architectural generation round-trips, so saved exclusive
        // tokens stay valid; certificates are fresh (move assignment).
        staged.mWriteGeneration =
            state.at("write_generation").get<uint64_t>();
        if (staged.mWriteGeneration == 0U) {
            SetError(error,
                     "native A32 memory write generation is invalid");
            return false;
        }
        staged.mTraceWriteFingerprint = mTraceWriteFingerprint;
        staged.PreserveAccessObserversFrom(*this);
        *this = std::move(staged);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("native A32 memory state decode failed: ") +
                     exception.what();
        }
        return false;
    }
}

void NativeA32Memory::SetRangeWatch(
    std::span<const std::pair<uint32_t, uint32_t>> ranges) {
    const KernelGuard kernel;
    mWatchRanges.assign(ranges.begin(), ranges.end());
    mWatchActive = !mWatchRanges.empty();
    std::vector<size_t> trapped;
    for (const auto& range : mWatchRanges) {
        if (range.second <= range.first) {
            continue;
        }
        const size_t firstPage = range.first >> PageShift;
        const size_t lastPage = static_cast<size_t>(
            (static_cast<uint64_t>(range.second) - 1U) >> PageShift);
        for (size_t page = firstPage; page <= lastPage; ++page) {
            trapped.push_back(page);
        }
    }
    std::sort(trapped.begin(), trapped.end());
    trapped.erase(std::unique(trapped.begin(), trapped.end()), trapped.end());
    // Trapping costs a page-table rebuild, so rebuild only when the trapped
    // page set changes: the same destination every frame costs nothing after
    // the first time.
    if (trapped != mTrappedPages) {
        mTrappedPages = std::move(trapped);
        RebuildPageTable();
    }
}

void NativeA32Memory::ClearRangeWatch() {
    const KernelGuard kernel;
    if (mWatchRanges.empty() && mTrappedPages.empty() && !mWatchActive) {
        return;
    }
    mWatchRanges.clear();
    mWatchActive = false;
    if (!mTrappedPages.empty()) {
        mTrappedPages.clear();
        RebuildPageTable();
    }
}


void NativeA32Memory::SetWriteObserver(WriteObserver observer) {
    const KernelGuard kernel;
    mWriteObserver = std::move(observer);
    if (!mWriteObserver && !mObservedWritePages.empty()) {
        mObservedWriteRanges.clear();
        mObservedWritePages.clear();
        RebuildPageTable();
    }
}

bool NativeA32Memory::HasEscapedWritePointer(uint32_t address, size_t size) const noexcept {
    const KernelGuard kernel;
    return std::any_of(mEscapedWritePointers.begin(), mEscapedWritePointers.end(),
        [&](const auto& range) { return RangesOverlap(address, size, range.first, range.second); });
}

bool NativeA32Memory::ObserveWrites(uint32_t address, size_t size) {
    const KernelGuard kernel;
    if (!mWriteObserver || !RangeFits(address, size) ||
        !IsMapped(address, size) || HasEscapedWritePointer(address, size)) return false;
    const auto covered = std::lower_bound(mObservedWriteRanges.begin(), mObservedWriteRanges.end(),
        address, [](const auto& range, uint32_t value) {
            return uint64_t{range.first} + range.second <= value;
        });
    if (covered != mObservedWriteRanges.end() && covered->first <= address &&
        uint64_t{covered->first} + covered->second >= uint64_t{address} + size) return true;
    AddWriteRange(mObservedWriteRanges, address, size);
    const size_t first = address >> PageShift;
    const size_t last = (uint64_t{address} + size - 1) >> PageShift;
    for (size_t page = first; page <= last; ++page) {
        const auto position = std::lower_bound(mObservedWritePages.begin(), mObservedWritePages.end(), page);
        if (position == mObservedWritePages.end() || *position != page)
            mObservedWritePages.insert(position, page);
        auto& group = mFastPageGroups[page / FastGroupPageCount];
        if (group) group->Pages[page & FastGroupPageMask].Write = nullptr;
        UpdatePageFlag(page);
    }
    CheckPageFlags();
    return true;
}

void NativeA32Memory::NotifyObservedWrite(uint32_t address, size_t size) {
    DiscardDeferredWrites(address, size);
    if (!mWriteObserver || mWatchSuppressed) return;
    const auto overlap = std::lower_bound(mObservedWriteRanges.begin(), mObservedWriteRanges.end(),
        address, [](const auto& range, uint32_t value) {
            return uint64_t{range.first} + range.second <= value;
        });
    if (overlap != mObservedWriteRanges.end() && overlap->first < uint64_t{address} + size)
        mWriteObserver(address, size);
}

void NativeA32Memory::PreserveAccessObserversFrom(const NativeA32Memory& memory) {
    const KernelGuard kernel;
    mWriteObserver = memory.mWriteObserver;
    mObservedWriteRanges = memory.mObservedWriteRanges;
    mObservedWritePages = memory.mObservedWritePages;
    mWatchRanges = memory.mWatchRanges;
    mTrappedPages = memory.mTrappedPages;
    mWatchCounters = memory.mWatchCounters;
    mWatchActive = memory.mWatchActive;
    // Escaped pointers referred to the replaced allocation, not this image.
    RebuildPageTable();
}

void NativeA32Memory::ReleaseDeferredPageTraps() const {
    if (!mDeferredWrites.empty()) return;
    for (const size_t page : mDeferredPages) {
        auto& group = mFastPageGroups[page / FastGroupPageCount];
        if (!group || std::binary_search(mTrappedPages.begin(), mTrappedPages.end(), page))
            continue;
        const uint32_t address = static_cast<uint32_t>(page << PageShift);
        const auto* region = FindRegion(address, PageSize);
        if (!region) continue;
        auto& entry = group->Pages[page & FastGroupPageMask];
        auto* bytes = region->Bytes.data() + (address - region->BaseAddress);
        AssignFastPointers(page, entry, bytes, region->Writable);
        UpdatePageFlag(page);
    }
    mDeferredPages.clear();
    CheckPageFlags();
}

void NativeA32Memory::DiscardDeferredWrites(uint32_t address, size_t size) const {
    if (mDeferredWrites.empty() || size == 0U) return;
    const uint64_t end = uint64_t{address} + size;
    for (size_t index = 0; index < mDeferredWrites.size();) {
        auto& write = mDeferredWrites[index];
        const uint64_t writeEnd = uint64_t{write.Address} + write.Size;
        if (writeEnd <= address || write.Address >= end) { ++index; continue; }
        if (write.Address < address && writeEnd > end) {
            DeferredWrite right{static_cast<uint32_t>(end),
                                static_cast<size_t>(writeEnd - end), write.Reader};
            write.Size = address - write.Address;
            mDeferredWrites.insert(mDeferredWrites.begin() + index + 1, std::move(right));
            index += 2;
        } else if (write.Address < address) {
            write.Size = address - write.Address;
            ++index;
        } else if (writeEnd > end) {
            write.Address = static_cast<uint32_t>(end);
            write.Size = static_cast<size_t>(writeEnd - end);
            ++index;
        } else {
            mDeferredWrites.erase(mDeferredWrites.begin() + index);
        }
    }
    ReleaseDeferredPageTraps();
}

bool NativeA32Memory::DeferWrites(
    std::span<const std::pair<uint32_t, size_t>> ranges, DeferredRead reader) {
    const KernelGuard kernel;
    if (!reader || ranges.empty() || mTraceWriteFingerprint) return false;
    for (const auto& [address, size] : ranges) {
        if (!RangeFits(address, size) || !IsWritable(address, size) ||
            HasEscapedWritePointer(address, size) ||
            std::any_of(mEscapedReadPointers.begin(), mEscapedReadPointers.end(),
                [&](const auto& escaped) {
                    return RangesOverlap(address, size, escaped.first, escaped.second);
                })) return false;
    }
    auto owner = std::make_shared<const DeferredRead>(std::move(reader));
    for (const auto& [address, size] : ranges) {
        DiscardDeferredWrites(address, size);
        mDeferredWrites.push_back({address, size, owner});
        const size_t last = (uint64_t{address} + size - 1U) >> PageShift;
        for (size_t page = address >> PageShift; page <= last; ++page) {
            const auto at = std::lower_bound(mDeferredPages.begin(), mDeferredPages.end(), page);
            if (at == mDeferredPages.end() || *at != page) mDeferredPages.insert(at, page);
            auto& group = mFastPageGroups[page / FastGroupPageCount];
            if (group) {
                group->Pages[page & FastGroupPageMask].Read = nullptr;
                group->Pages[page & FastGroupPageMask].Write = nullptr;
                group->Pages[page & FastGroupPageMask].Bytes = nullptr;
            }
            UpdatePageFlag(page);
        }
        // A logical device write counts as written now, not at readback.
        NoteWrite(address, size);
    }
    CheckPageFlags();
    return true;
}

bool NativeA32Memory::ResolveDeferredRead(uint32_t address, size_t size) const {
    if (mDeferredWrites.empty() || size == 0U) return true;
    const uint64_t end = uint64_t{address} + size;
    try {
        for (;;) {
            const auto found = std::find_if(mDeferredWrites.begin(), mDeferredWrites.end(),
                [&](const auto& write) {
                    return write.Address < end && uint64_t{write.Address} + write.Size > address;
                });
            if (found == mDeferredWrites.end()) return true;
            const uint32_t low = std::max(address, found->Address);
            const size_t count = static_cast<size_t>(
                std::min(end, uint64_t{found->Address} + found->Size) - low);
            const auto reader = found->Reader;
            auto* region = const_cast<NativeA32Memory*>(this)->FindRegion(low, count);
            if (!region || !(*reader)(low, std::span<uint8_t>(
                    region->Bytes.data() + (low - region->BaseAddress), count)))
                return false;
            DiscardDeferredWrites(low, count);
        }
    } catch (...) {
        // The typed AOT bus is noexcept: fail the memory access, never expose
        // stale bytes or let a renderer exception cross the title ABI.
        return false;
    }
}

bool NativeA32Memory::MaterializeDeferredWrites() const {
    const KernelGuard kernel;
    while (!mDeferredWrites.empty()) {
        const auto address = mDeferredWrites.front().Address;
        const auto size = mDeferredWrites.front().Size;
        if (!ResolveDeferredRead(address, size)) return false;
    }
    return true;
}

std::span<const uint8_t> NativeA32Memory::SynchronousReadView(
    uint32_t address, size_t size) const {
    const KernelGuard kernel;
    const auto* region = FindRegion(address, size);
    if (!region || !ResolveDeferredRead(address, size)) return {};
    return {region->Bytes.data() + (address - region->BaseAddress), size};
}

bool NativeA32Memory::ReadBytes(uint32_t address,
                                std::span<uint8_t> bytes) const {
    const KernelGuard kernel;
    if (bytes.empty()) {
        return true;
    }
    const auto* region = FindRegion(address, bytes.size());
    if (region == nullptr) {
        return false;
    }
    if (!ResolveDeferredRead(address, bytes.size())) return false;
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy_n(region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.size(), bytes.begin());
    if (!mWatchSuppressed && Watches(address, bytes.size())) [[unlikely]] {
        ++mWatchCounters.ByteReads;
    }
    return true;
}

const uint8_t* NativeA32Memory::GetReadPointer(uint32_t address,
                                               size_t size) const {
    const KernelGuard kernel;
    const auto* region = FindRegion(address, size);
    if (region == nullptr) {
        return nullptr;
    }
    if (!ResolveDeferredRead(address, size)) return nullptr;
    AddWriteRange(mEscapedReadPointers, address, size);
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        // Handing out a live pointer ends observability for this range: every
        // later read through it happens outside any hook we own.
        ++mWatchCounters.PointerReads;
    }
    return region->Bytes.data() +
           static_cast<size_t>(address - region->BaseAddress);
}

uint8_t* NativeA32Memory::GetWritePointer(uint32_t address, size_t size) {
    const KernelGuard kernel;
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return nullptr;
    }
    if (!ResolveDeferredRead(address, size)) return nullptr;
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        ++mWatchCounters.PointerWrites;
    }
    uint8_t* destination = region->Bytes.data() +
                           static_cast<size_t>(address - region->BaseAddress);
    if (mTraceWriteFingerprint) {
        mPendingTracedPointerWrites.emplace_back(address, size);
    }
    AddWriteRange(mEscapedWritePointers, address, size);
    NotifyObservedWrite(address, size);
    NoteWrite(address, size);
    return destination;
}

std::optional<uint32_t>
NativeA32Memory::GetGuestAddress(const void* pointer,
                                 size_t size) const noexcept {
    const KernelGuard kernel;
    if (pointer == nullptr || size == 0U) {
        return std::nullopt;
    }
    const uintptr_t candidate = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : mRegions) {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(region.Bytes.data());
        if (candidate < base) {
            continue;
        }
        const uintptr_t rawOffset = candidate - base;
        if (rawOffset > region.Bytes.size()) {
            continue;
        }
        const size_t offset = static_cast<size_t>(rawOffset);
        if (size > region.Bytes.size() - offset) {
            continue;
        }
        const uint64_t guestAddress =
            static_cast<uint64_t>(region.BaseAddress) + offset;
        if (guestAddress <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(guestAddress);
        }
    }
    return std::nullopt;
}

bool NativeA32Memory::WriteBytes(uint32_t address,
                                 std::span<const uint8_t> bytes) {
    const KernelGuard kernel;
    if (bytes.empty()) {
        return true;
    }
    auto* region = FindRegion(address, bytes.size());
    if (region == nullptr || !region->Writable) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy(bytes.begin(), bytes.end(),
              region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (Watches(address, bytes.size())) [[unlikely]] {
        // Counted apart from typed writes. The renderer suppresses itself
        // around its own materialization of these bytes: that traffic lands in
        // SuppressedByteWrites, which doubles as the control that the trap is
        // live in a run where no guest or service touched the range.
        if (mWatchSuppressed) {
            ++mWatchCounters.SuppressedByteWrites;
        } else {
            ++mWatchCounters.ByteWrites;
        }
    }
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(address, bytes);
    }
    NoteWrite(address, bytes.size());
    NotifyObservedWrite(address, bytes.size());
    return true;
}

bool NativeA32Memory::WriteHostBytes(
    uint32_t address, std::span<const uint8_t> bytes) {
    const KernelGuard kernel;
    if (bytes.empty()) {
        return true;
    }
    auto* region = FindRegion(address, bytes.size());
    if (region == nullptr) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy(bytes.begin(), bytes.end(),
              region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(address, bytes);
    }
    NoteWrite(address, bytes.size());
    if (!mWatchSuppressed && Watches(address, bytes.size())) [[unlikely]] {
        ++mWatchCounters.HostByteWrites;
    }
    NotifyObservedWrite(address, bytes.size());
    return true;
}

uint8_t* NativeA32Memory::BeginHostUpdate(uint32_t address, size_t size) {
    const KernelGuard kernel;
    if (size == 0) {
        return nullptr;
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return nullptr;
    }
    // ReadBytes of the range.
    if (!ResolveDeferredRead(address, size)) return nullptr;
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        ++mWatchCounters.ByteReads;
    }
    return region->Bytes.data() +
           static_cast<size_t>(address - region->BaseAddress);
}

bool NativeA32Memory::EndHostUpdate(uint32_t address, size_t size) {
    const KernelGuard kernel;
    if (size == 0) {
        return true;
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return false;
    }
    // WriteBytes of the range's current bytes, without the copy.
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    if (Watches(address, size)) [[unlikely]] {
        if (mWatchSuppressed) {
            ++mWatchCounters.SuppressedByteWrites;
        } else {
            ++mWatchCounters.ByteWrites;
        }
    }
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(address, std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    NoteWrite(address, size);
    NotifyObservedWrite(address, size);
    return true;
}

bool NativeA32Memory::Fill(uint32_t address, size_t size, uint8_t value) {
    const KernelGuard kernel;
    if (size == 0) {
        return true;
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::fill_n(region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                size, value);
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    NoteWrite(address, size);
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        ++mWatchCounters.Fills;
    }
    NotifyObservedWrite(address, size);
    return true;
}

bool NativeA32Memory::ReadSized(uint32_t address, uint8_t size,
                                uint64_t* value,
                                uint32_t* faultAddress) const {
    const KernelGuard kernel;
    if (value == nullptr || !IsSupportedSize(size)) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    const auto* region = FindRegion(address, size);
    if (region == nullptr) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    if (!ResolveDeferredRead(address, size)) {
        if (faultAddress) *faultAddress = address;
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    uint64_t decoded = 0;
    for (uint8_t index = 0; index < size; ++index) {
        decoded |= static_cast<uint64_t>(region->Bytes[offset + index])
                   << (index * 8U);
    }
    *value = decoded;
    if (Watches(address, size)) [[unlikely]] {
        NoteWatchedRead(address, size);
    }
    return true;
}

bool NativeA32Memory::WriteSized(uint32_t address, uint8_t size,
                                 uint64_t value, uint32_t* faultAddress) {
    const KernelGuard kernel;
    if (!IsSupportedSize(size)) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    switch (size) {
    case 1: std::memcpy(region->Bytes.data() + offset, &value, 1); break;
    case 2: std::memcpy(region->Bytes.data() + offset, &value, 2); break;
    case 4: std::memcpy(region->Bytes.data() + offset, &value, 4); break;
    case 8: std::memcpy(region->Bytes.data() + offset, &value, 8); break;
    }
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    NoteWrite(address, size);
    if (Watches(address, size)) [[unlikely]] {
        NoteWatchedWrite(address, size);
    }
    NotifyObservedWrite(address, size);
    return true;
}

bool NativeA32Memory::Read8(uint32_t address, uint8_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read16(uint32_t address, uint16_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read32(uint32_t address, uint32_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read64(uint32_t address, uint64_t* value,
                             uint32_t* faultAddress) {
    return ReadFast(address, value, faultAddress);
}

bool NativeA32Memory::Write8(uint32_t address, uint8_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write16(uint32_t address, uint16_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write32(uint32_t address, uint32_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write64(uint32_t address, uint64_t value,
                             uint32_t* faultAddress) {
    return WriteFast(address, value, faultAddress);
}

// Exclusives go through the global exclusive monitor
// (oot3d_native_a32_second_core.h) with the second core on or off: the token
// is the loaded value, the reservation is the core's granule, and the store
// is a host compare-and-swap on the guest word. A plain store to the word
// between LDREX and STREX fails it only if it changed the value (as with the
// second core since it exists); a guest context switch, CLREX, another
// STREX or SWP to the granule and a savestate load drop the reservation. No
// guest store has to mark a memory-wide written byte for it. A page without
// a fast entry (watched, observed, deferred, traced) takes the same steps
// through the checked paths under the kernel lock, which is always taken
// before the monitor's.
bool NativeA32Memory::LoadExclusive(uint32_t address, uint8_t size,
                                    uint64_t* value, uint64_t* token,
                                    uint32_t* faultAddress) {
    if (token == nullptr || value == nullptr) {
        if (faultAddress != nullptr) *faultAddress = address;
        return false;
    }
    const uint32_t granule = address & SecondCore::kExclusiveGranuleMask;
    const FastPage* page = FastPageFor(address, size);
    const uint8_t* bytes = page != nullptr && page->Read != nullptr
                               ? page->Read + (address & PageMask)
                               : nullptr;
    if (bytes != nullptr && AtomicAccessible(address, bytes, size)) {
        const std::lock_guard monitor(gExclusiveMutex);
        *value = AtomicLoadGuest(bytes, size);
        gExclusiveGranule[SecondCore::Processor()] = granule;
    } else {
        const KernelGuard kernel;
        const std::lock_guard monitor(gExclusiveMutex);
        if (!ReadSized(address, size, value, faultAddress)) {
            return false;
        }
        gExclusiveGranule[SecondCore::Processor()] = granule;
    }
    *token = *value;
    return true;
}

oot3d::recomp::a32::ExclusiveStoreResult NativeA32Memory::StoreExclusive(
    uint32_t address, uint8_t size, uint64_t value, uint64_t token,
    uint32_t* faultAddress) {
    using oot3d::recomp::a32::ExclusiveStoreResult;
    const uint32_t granule = address & SecondCore::kExclusiveGranuleMask;
    const uint8_t processor = SecondCore::Processor();
    FastPageGroup* group = mFastPageGroups[size_t{address} >> FastGroupShift].get();
    const size_t pageIndex = (address >> PageShift) & FastGroupPageMask;
    uint8_t* bytes = group != nullptr && group->Pages[pageIndex].Write != nullptr
                         ? group->Pages[pageIndex].Write + (address & PageMask)
                         : nullptr;
    if (bytes != nullptr && AtomicAccessible(address, bytes, size)) {
        const std::lock_guard monitor(gExclusiveMutex);
        if (gExclusiveGranule[processor] != granule) {
            gExclusiveGranule[processor] = kNoReservation;
            return ExclusiveStoreResult::ReservationLost;
        }
        ClearExclusiveGranule(granule);
        if (!AtomicCompareExchangeGuest(bytes, size, token, value)) {
            return ExclusiveStoreResult::ReservationLost;
        }
        // WriteFast's bookkeeping, as an atomic byte store.
        MarkCardAtomic(mCards[address >> PageShift]);
        return ExclusiveStoreResult::Success;
    }
    const KernelGuard kernel;
    const std::lock_guard monitor(gExclusiveMutex);
    if (gExclusiveGranule[processor] != granule) {
        gExclusiveGranule[processor] = kNoReservation;
        return ExclusiveStoreResult::ReservationLost;
    }
    ClearExclusiveGranule(granule);
    uint64_t current = 0;
    if (!ReadSized(address, size, &current, faultAddress)) {
        return ExclusiveStoreResult::MemoryFault;
    }
    if (current != (token & SizeMask(size))) {
        return ExclusiveStoreResult::ReservationLost;
    }
    if (!WriteSized(address, size, value, faultAddress)) {
        return ExclusiveStoreResult::MemoryFault;
    }
    return ExclusiveStoreResult::Success;
}

bool NativeA32Memory::AtomicSwap(uint32_t address, uint8_t size,
                                 uint32_t replacement, uint32_t* previous,
                                 uint32_t* faultAddress) {
    if (previous != nullptr && (size == 1U || size == 4U)) {
        // SWP/SWPB: one atomic exchange, an exclusive store for the monitor.
        const uint32_t granule = address & SecondCore::kExclusiveGranuleMask;
        FastPageGroup* group = mFastPageGroups[size_t{address} >> FastGroupShift].get();
        const size_t pageIndex = (address >> PageShift) & FastGroupPageMask;
        uint8_t* bytes = group != nullptr && group->Pages[pageIndex].Write != nullptr
                             ? group->Pages[pageIndex].Write + (address & PageMask)
                             : nullptr;
        if (bytes != nullptr && AtomicAccessible(address, bytes, size)) {
            const std::lock_guard monitor(gExclusiveMutex);
            ClearExclusiveGranule(granule);
            *previous = static_cast<uint32_t>(AtomicExchangeGuest(bytes, size, replacement));
            MarkCardAtomic(mCards[address >> PageShift]);
            return true;
        }
        const KernelGuard kernel;
        const std::lock_guard monitor(gExclusiveMutex);
        ClearExclusiveGranule(granule);
        uint64_t oldValue = 0;
        if (!ReadSized(address, size, &oldValue, faultAddress) ||
            !WriteSized(address, size, replacement, faultAddress)) {
            return false;
        }
        *previous = static_cast<uint32_t>(oldValue);
        return true;
    }
    uint64_t oldValue = 0;
    if (previous == nullptr || !ReadSized(address, size, &oldValue, faultAddress) ||
        !WriteSized(address, size, replacement, faultAddress)) {
        return false;
    }
    *previous = static_cast<uint32_t>(oldValue);
    return true;
}

namespace SecondCore {

void ClearExclusive() noexcept {
    const std::lock_guard monitor(gExclusiveMutex);
    gExclusiveGranule[Processor()] = kNoReservation;
}

void ResetExclusiveMonitor() noexcept {
    const std::lock_guard monitor(gExclusiveMutex);
    gExclusiveGranule.fill(kNoReservation);
}

} // namespace SecondCore

} // namespace Oot3dNativeGame
