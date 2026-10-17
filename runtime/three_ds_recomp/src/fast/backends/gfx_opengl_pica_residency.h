#pragma once

// GPU memory policy of the OpenGL PICA backend (gfx_opengl_pica.cpp), free of
// GL so it can be checked offline (tests/gfx_opengl_pica_residency_tests.cpp).
//
// - Texture residency: the decoded textures (and lighting LUTs) are cached by
//   content. Every content change (animated textures, guest-written images)
//   used to create a new GL texture that was never released, so a long session
//   ran the console out of memory: Mesa's nouveau then failed to map a buffer
//   and copied a draw's uniforms to NULL. Entries idle for MaxIdleFrames go,
//   and above BudgetBytes the least recently used ones (idle at least
//   MinIdleFrames) go first. Every draw carries the native bytes of its
//   textures, so an evicted texture is only decoded again.
// - Stream ring: per-draw uniforms, streamed vertices and indices go into one
//   large buffer per kind at increasing offsets instead of a glBufferData per
//   draw (which reallocated the buffer storage on every draw); the storage is
//   orphaned only when the ring wraps.

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace Fast::PicaGlResidency {

struct Policy {
    uint64_t BudgetBytes = 96ULL * 1024ULL * 1024ULL;
    // About 30 s at 60 Hz presentation.
    uint64_t MaxIdleFrames = 1800U;
    // Never evict what the current or the previous frame used.
    uint64_t MinIdleFrames = 2U;
};

// config/gl_texture_budget_mb (Switch) or TRIAEVUM_GL_TEXTURE_BUDGET_MB: a
// whole number of megabytes between 16 and 2048; anything else keeps the
// default.
[[nodiscard]] inline uint64_t ParseBudgetBytes(std::string_view text,
                                               uint64_t fallback) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1U);
    uint64_t megabytes = 0U;
    size_t digits = 0U;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9' &&
           digits < 5U) {
        megabytes = megabytes * 10U + static_cast<uint64_t>(text[digits] - '0');
        ++digits;
    }
    if (digits == 0U || megabytes < 16U || megabytes > 2048U) return fallback;
    return megabytes * 1024ULL * 1024ULL;
}

// Keys of `entries` (a map whose values have Bytes and LastUsedFrame) to
// release at `frame`, given their total `totalBytes`.
template <typename Map>
void CollectEvictions(const Map& entries, uint64_t frame, uint64_t totalBytes,
                      const Policy& policy,
                      std::vector<typename Map::key_type>& out) {
    out.clear();
    const auto idle = [&](uint64_t lastUsed) {
        return frame > lastUsed ? frame - lastUsed : 0U;
    };
    std::vector<std::pair<uint64_t, const typename Map::value_type*>> candidates;
    uint64_t remaining = totalBytes;
    for (const auto& entry : entries) {
        const uint64_t idleFrames = idle(entry.second.LastUsedFrame);
        if (idleFrames > policy.MaxIdleFrames) {
            out.push_back(entry.first);
            remaining -= std::min(remaining, entry.second.Bytes);
        } else if (idleFrames >= policy.MinIdleFrames) {
            candidates.emplace_back(entry.second.LastUsedFrame, &entry);
        }
    }
    if (remaining <= policy.BudgetBytes) return;
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });
    for (const auto& [lastUsed, entry] : candidates) {
        if (remaining <= policy.BudgetBytes) break;
        out.push_back(entry->first);
        remaining -= std::min(remaining, entry->second.Bytes);
    }
}

// Offsets of a stream ring of `Capacity` bytes. A draw first reserves the
// bytes of all its ranges in this ring (with their alignment slack), so they
// land in one generation of the storage: when they do not fit after the
// head, a new generation starts at offset 0 and the caller orphans the
// storage (the GPU keeps the previous one until it is done with it). Ranges
// of one generation never overlap. The generation only grows, so a range
// remembered with its generation is valid exactly while that generation is
// current.
class StreamRing {
  public:
    struct Allocation {
        uint64_t Offset = 0U;
        bool Valid = false;
    };

    explicit StreamRing(uint64_t capacity = 0U) noexcept
        : mCapacity(capacity) {}

    [[nodiscard]] uint64_t Capacity() const noexcept { return mCapacity; }
    [[nodiscard]] uint64_t Generation() const noexcept { return mGeneration; }
    [[nodiscard]] uint64_t Head() const noexcept { return mHead; }

    // Bytes to reserve for ranges of `size` with `alignment`.
    [[nodiscard]] static uint64_t Span(uint64_t size, uint64_t alignment) noexcept {
        return size + (alignment == 0U ? 0U : alignment - 1U);
    }

    // Makes room for `total` bytes (sum of Span). True: a new generation
    // started and the storage must be orphaned now. A total larger than the
    // ring cannot be reserved: Fits() is false and nothing changes.
    [[nodiscard]] bool Fits(uint64_t total) const noexcept {
        return total != 0U && total <= mCapacity;
    }
    bool Reserve(uint64_t total) noexcept {
        if (!Fits(total)) return false;
        if (mStale || mGeneration == 0U || mHead > mCapacity ||
            mCapacity - mHead < total) {
            ++mGeneration;
            mHead = 0U;
            mStale = false;
            return true;
        }
        return false;
    }

    // The next aligned range of the current generation; invalid when it
    // does not fit (the bytes were not reserved).
    [[nodiscard]] Allocation Allocate(uint64_t size, uint64_t alignment) noexcept {
        Allocation result;
        if (alignment == 0U) alignment = 1U;
        if (size == 0U || mStale || mGeneration == 0U) return result;
        const uint64_t offset = (mHead + alignment - 1U) / alignment * alignment;
        if (offset > mCapacity || mCapacity - offset < size) return result;
        mHead = offset + size;
        result.Offset = offset;
        result.Valid = true;
        return result;
    }

    // The storage was lost (a failed map): the next Reserve starts over.
    void Reset() noexcept { mStale = true; }

  private:
    uint64_t mCapacity = 0U;
    uint64_t mHead = 0U;
    uint64_t mGeneration = 0U;
    bool mStale = false;
};

} // namespace Fast::PicaGlResidency
