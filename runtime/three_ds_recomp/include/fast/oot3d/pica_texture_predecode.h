#pragma once

#include "oot3d/renderer/pica_texture_decode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fast::Oot3d {

// Mip levels the GL backend uploads for a PICA texture: halves while both
// sides stay above 8, capped by the sampler's max level.
inline uint32_t PicaTextureMipCount(uint32_t width, uint32_t height,
                                    uint8_t maxMipLevel) {
    if (width == 0U || height == 0U) {
        return 0U;
    }
    uint32_t levels = 1U;
    while (width > 8U && height > 8U) {
        ++levels;
        width >>= 1U;
        height >>= 1U;
    }
    return std::min(levels, static_cast<uint32_t>(maxMipLevel) + 1U);
}

// Bytes of one encoded (tiled) mip level.
inline std::optional<size_t> PicaTextureEncodedMipSize(
    uint8_t format, uint32_t width, uint32_t height) {
    static constexpr std::array<uint8_t, 14> kNibblesPerPixel{
        8U, 6U, 4U, 4U, 4U, 4U, 4U,
        2U, 2U, 2U, 1U, 1U, 1U, 2U};
    if (format >= kNibblesPerPixel.size() || width == 0U || height == 0U) {
        return std::nullopt;
    }
    const uint64_t tiledWidth = (std::max(8U, width) + 7U) & ~7ULL;
    const uint64_t tiledHeight = (std::max(8U, height) + 7U) & ~7ULL;
    const uint64_t bytes =
        (tiledWidth * tiledHeight * kNibblesPerPixel[format] + 1U) / 2U;
    if (bytes > std::numeric_limits<size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<size_t>(bytes);
}

// Identity of a decoded texture: same content, format and size decode to the
// same RGBA8 mips whatever the sampler state.
struct PicaPredecodeKey {
    uint64_t ContentHash = 0U;
    uint16_t Width = 0U;
    uint16_t Height = 0U;
    uint8_t Format = 0U;
    uint8_t MipLevels = 0U;

    bool operator==(const PicaPredecodeKey&) const = default;
};

struct PicaPredecodeKeyHash {
    size_t operator()(const PicaPredecodeKey& key) const noexcept {
        uint64_t value = key.ContentHash;
        value ^= (static_cast<uint64_t>(key.Width) << 48U) ^
                 (static_cast<uint64_t>(key.Height) << 32U) ^
                 (static_cast<uint64_t>(key.Format) << 8U) ^ key.MipLevels;
        value *= 0x9E3779B97F4A7C15ULL;
        return static_cast<size_t>(value ^ (value >> 32U));
    }
};

struct PicaPredecodedTexture {
    std::vector<std::vector<uint8_t>> Mips; // RGBA8, level 0 first
    size_t Bytes = 0U;
};

// Decodes every uploaded mip of one texture; nullptr when the bytes are
// truncated or the format is not decodable (the render thread then reports
// the error through its own decode).
inline std::shared_ptr<PicaPredecodedTexture> DecodePicaTextureMips(
    uint8_t format, uint16_t width, uint16_t height, uint8_t maxMipLevel,
    std::span<const uint8_t> nativeBytes) {
    const uint32_t levels = PicaTextureMipCount(width, height, maxMipLevel);
    if (levels == 0U) {
        return nullptr;
    }
    auto decoded = std::make_shared<PicaPredecodedTexture>();
    decoded->Mips.resize(levels);
    size_t offset = 0U;
    for (uint32_t level = 0U; level < levels; ++level) {
        const uint32_t mipWidth = std::max(1U, uint32_t{width} >> level);
        const uint32_t mipHeight = std::max(1U, uint32_t{height} >> level);
        const auto encoded = PicaTextureEncodedMipSize(format, mipWidth, mipHeight);
        if (!encoded || offset + *encoded > nativeBytes.size() ||
            !::Oot3d::Renderer::DecodePicaTextureRgba8(
                format, static_cast<uint16_t>(mipWidth),
                static_cast<uint16_t>(mipHeight),
                nativeBytes.subspan(offset, *encoded), decoded->Mips[level])) {
            return nullptr;
        }
        decoded->Bytes += decoded->Mips[level].size();
        offset += *encoded;
    }
    return decoded;
}

// Textures decoded ahead of time by the PICA plan worker (core 1) so the
// render thread only uploads them: entering a new area creates dozens of
// textures in one frame, and decoding them on the render thread was the main
// cause of the 30 FPS hitches on the console. Thread-safe; capped in bytes
// (oldest first out). Each entry is handed out once: the GL texture created
// from it is cached by the backend.
class PicaPredecodeCache {
  public:
    explicit PicaPredecodeCache(size_t budgetBytes = 24U << 20U)
        : mBudget(budgetBytes) {}

    // Worker: true when this content was never decoded nor uploaded yet, and
    // claims it (the next call for the same key returns false).
    bool Claim(const PicaPredecodeKey& key) {
        std::lock_guard lock(mMutex);
        return ClaimLocked(key);
    }

    void Put(const PicaPredecodeKey& key,
             std::shared_ptr<PicaPredecodedTexture> texture) {
        if (texture == nullptr || texture->Bytes > mBudget) {
            return;
        }
        std::lock_guard lock(mMutex);
        auto [it, inserted] = mEntries.try_emplace(key, std::move(texture));
        if (!inserted) {
            return;
        }
        mBytes += it->second->Bytes;
        // Drop keys already taken from the front so the queue stays bounded.
        while (!mOrder.empty() && !mEntries.contains(mOrder.front())) {
            mOrder.pop_front();
        }
        mOrder.push_back(key);
        while (mBytes > mBudget && !mOrder.empty()) {
            EraseLocked(mOrder.front());
            mOrder.pop_front();
            ++mEvicted;
        }
    }

    // Render thread, when it creates a texture: the decoded mips if the
    // worker got there first. Either way the content counts as seen, so the
    // worker will not decode it again.
    std::shared_ptr<PicaPredecodedTexture> Take(const PicaPredecodeKey& key) {
        std::lock_guard lock(mMutex);
        (void)ClaimLocked(key);
        auto it = mEntries.find(key);
        if (it == mEntries.end()) {
            ++mMisses;
            return nullptr;
        }
        auto texture = std::move(it->second);
        mBytes -= texture->Bytes;
        mEntries.erase(it);
        ++mHits;
        return texture;
    }

    struct Stats {
        uint64_t Hits = 0U, Misses = 0U, Evicted = 0U;
        size_t Bytes = 0U, Entries = 0U;
    };
    Stats Snapshot() const {
        std::lock_guard lock(mMutex);
        return {mHits, mMisses, mEvicted, mBytes, mEntries.size()};
    }

  private:
    // Remembered contents; forgotten wholesale past this (worst case one
    // extra decode of a texture still in use).
    static constexpr size_t kSeenLimit = 16384U;

    bool ClaimLocked(const PicaPredecodeKey& key) {
        if (mSeen.size() >= kSeenLimit) {
            mSeen.clear();
        }
        return mSeen.insert(key).second;
    }

    void EraseLocked(const PicaPredecodeKey& key) {
        auto it = mEntries.find(key);
        if (it != mEntries.end()) {
            mBytes -= it->second->Bytes;
            mEntries.erase(it);
        }
    }

    mutable std::mutex mMutex;
    size_t mBudget;
    size_t mBytes = 0U;
    std::unordered_map<PicaPredecodeKey, std::shared_ptr<PicaPredecodedTexture>,
                       PicaPredecodeKeyHash> mEntries;
    std::deque<PicaPredecodeKey> mOrder; // may hold keys already taken
    std::unordered_set<PicaPredecodeKey, PicaPredecodeKeyHash> mSeen;
    uint64_t mHits = 0U, mMisses = 0U, mEvicted = 0U;
};

// The one cache shared by the plan worker and the GL backend.
inline PicaPredecodeCache& SharedPicaPredecodeCache() {
    static PicaPredecodeCache cache;
    return cache;
}

} // namespace Fast::Oot3d
