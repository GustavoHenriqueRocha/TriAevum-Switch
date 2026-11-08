#include "oot3d_native_pica_submission.h"

#include "oot3d_native_a32_memory.h"
#include <algorithm>

namespace Oot3dNativeGame {

Oot3dPicaPhysicalMemoryView::Oot3dPicaPhysicalMemoryView(
    const NativeA32Memory& memory,
    std::vector<Oot3dPicaPhysicalMemoryRegion> regions)
    : Oot3dPicaPhysicalMemoryView(
          std::move(regions),
          [&memory](uint32_t address, size_t size) {
              return memory.SynchronousReadView(address, size);
          },
          [&memory](uint32_t address, size_t size) {
              return memory.RangeWriteGeneration(address, size);
          },
          [&memory](uint32_t address, size_t size) {
              return memory.IsMapped(address, size);
          }) {
    mDirectMemory = &memory;
}

Oot3dPicaPhysicalMemoryView::DirectView Oot3dPicaPhysicalMemoryView::ViewCertified(
    uint32_t physicalAddress, size_t size, std::span<const uint8_t>& bytes,
    uint32_t& guestAddress, std::optional<uint64_t>& generation) const {
    if (mDirectMemory == nullptr) return DirectView::Fallback;
    // The regions Translate and ForEachGuestAlias test: View takes the first
    // mapped one, the generation needs all of them.
    const uint64_t low = physicalAddress;
    const uint64_t high = low + size;
    if (size == 0U || high > uint64_t{1} << 32U) return DirectView::Unmapped;
    size_t covering = 0U;
    for (const auto& region : mRegions) {
        const uint64_t base = region.PhysicalBaseAddress;
        if (region.Size == 0U || base + region.Size > uint64_t{1} << 32U ||
            uint64_t{region.GuestBaseAddress} + region.Size > uint64_t{1} << 32U ||
            low < base || high > base + region.Size) {
            continue;
        }
        if (++covering > 1U) return DirectView::Fallback;
        guestAddress = region.GuestBaseAddress + static_cast<uint32_t>(low - base);
    }
    if (covering == 0U) return DirectView::Unmapped;
    // View: the mapping check is the view's own region lookup.
    bytes = mDirectMemory->SynchronousReadView(guestAddress, size);
    if (bytes.size() != size) return DirectView::Unmapped;
    // SnapshotWriteGeneration of a range with one alias (no observation).
    generation = mDirectMemory->RangeWriteGeneration(guestAddress, size);
    if (generation.has_value() && *generation == 0U) generation.reset();
    return DirectView::Viewed;
}

bool Oot3dPicaPhysicalMemoryView::GuestPageGenerations(
    uint32_t guestAddress, size_t size, std::vector<uint64_t>& generations) const {
    generations.clear();
    const uint64_t end = uint64_t{guestAddress} + size;
    for (uint64_t page = guestAddress; page < end;) {
        const uint64_t next = std::min(end, (page | 0xFFFU) + 1U);
        const auto generation = mDirectMemory->RangeWriteGeneration(
            static_cast<uint32_t>(page), static_cast<size_t>(next - page));
        if (!generation.has_value() || *generation == 0U) {
            generations.clear();
            return false;
        }
        generations.push_back(*generation);
        page = next;
    }
    return true;
}

bool Oot3dPicaPhysicalMemoryView::SynchronizeGuestWrite(
    NativeA32Memory& memory, uint32_t guestAddress, size_t size) const {
    const uint64_t end = uint64_t{guestAddress} + size;
    for (const auto& region : mRegions) {
        const uint64_t low = std::max<uint64_t>(guestAddress, region.GuestBaseAddress);
        const uint64_t high = std::min(end, uint64_t{region.GuestBaseAddress} + region.Size);
        if (low >= high) continue;
        const size_t count = static_cast<size_t>(high - low);
        const uint32_t physical = region.PhysicalBaseAddress +
                                  static_cast<uint32_t>(low - region.GuestBaseAddress);
        const auto source = memory.SynchronousReadView(static_cast<uint32_t>(low), count);
        if (source.size() != count) return false;
        NativeA32Memory::RangeWatchSuppression materialization(memory);
        if (!ForEachGuestAlias(physical, count, [&](uint32_t alias, size_t) {
                return alias == low || memory.WriteHostBytes(alias, source);
            })) return false;
    }
    return true;
}

} // namespace Oot3dNativeGame
