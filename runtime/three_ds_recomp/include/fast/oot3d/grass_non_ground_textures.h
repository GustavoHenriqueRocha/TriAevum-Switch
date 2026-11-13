#pragma once

#include "fast/renderer3ds/pica_native_state.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_set>

namespace Fast::Oot3d {

// Textures an opaque draw has already been resolved over as no configured
// ground (neither a rule's image nor a twin's). Another draw over one is no
// grass source either, so PublishPicaGrassSource returns before the memo key:
// in the Market ~70 such draws a frame cost 3.4 ms of render with no grass
// drawn (grama-medida tour, 13/11). Valid for one rules version and one
// texture generation: a texture decoded later can make an earlier one a
// rule's twin (GrassTextureSourceCache::ResolveObservedHash).
class GrassNonGroundTextures {
  public:
    static constexpr size_t kMaxTextures = 4096;

    // The slot-0 texture's identity, as Resolve's BaseLevelContentHash reads
    // it, with what a rule matches besides the image (size, format). None for
    // textures Resolve decides on other grounds (copies, cube maps) or hashes
    // from its bytes.
    static std::optional<uint64_t> Key(std::span<const Renderer3ds::PicaTextureView> textures) {
        for (const auto& texture : textures) {
            if (texture.Slot != 0U) {
                continue;
            }
            if (!texture.NativeBaseLevelContentHashAvailable || texture.NativeType != 0U ||
                !texture.CopyReferences.empty()) {
                return std::nullopt;
            }
            uint64_t key = texture.NativeBaseLevelContentHash;
            key ^= (static_cast<uint64_t>(texture.Width) << 40U) ^ (static_cast<uint64_t>(texture.Height) << 20U) ^
                   texture.NativeFormat;
            key ^= key >> 33U;
            key *= 0xff51afd7ed558ccdULL;
            key ^= key >> 33U;
            return key;
        }
        return std::nullopt;
    }

    bool Known(uint64_t key, uint64_t rules, uint64_t generation) {
        Sync(rules, generation);
        return mKeys.contains(key);
    }

    void Remember(uint64_t key, uint64_t rules, uint64_t generation) {
        Sync(rules, generation);
        if (mKeys.size() >= kMaxTextures) {
            mKeys.clear();
        }
        mKeys.insert(key);
    }

    size_t Size() const {
        return mKeys.size();
    }

  private:
    void Sync(uint64_t rules, uint64_t generation) {
        if (rules != mRules || generation != mGeneration) {
            mKeys.clear();
            mRules = rules;
            mGeneration = generation;
        }
    }

    std::unordered_set<uint64_t> mKeys;
    uint64_t mRules = 0;
    uint64_t mGeneration = ~0ULL;
};

} // namespace Fast::Oot3d
