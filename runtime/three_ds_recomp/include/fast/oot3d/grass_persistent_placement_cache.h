#pragma once
#include "fast/oot3d/grass_async_placement_builder.h"
#include <array>
#include <filesystem>

namespace Fast::Oot3d {
// Bump whenever extraction, world packing, clustering or overlay semantics change.
// 2: the geometry identity and the exact model-to-world moved from the file
// name into the header (GrassPlacementDiskMatch).
inline constexpr uint32_t kGrassPlacementDiskVersion = 2;
struct GrassPlacementDiskKey {
    uint64_t Content = 0, Settings = 0;
    bool operator==(const GrassPlacementDiskKey&) const = default;
};
// The file name: the ground's payload (vertices, indices, texture, mask,
// colour, overlays) and every bake setting, with the model-to-world rounded
// (rotation to 1/64, translation to 16 units). The geometry identity (guest
// addresses) and the exact model-to-world are in the file (GrassPlacementDiskMatch).
[[nodiscard]] GrassPlacementDiskKey GrassPlacementPersistentKey(const GrassAsyncPlacementRequest& request);

// What a stored ground must match beyond its file name. The model-to-world of
// a scene's ground comes from the camera of its first drawn frame
// (inverse light-binding world-to-view x view x model, in float), so every
// camera rounds it differently: exact matching (the PC) loads a ground only
// when that first camera repeats. Reuse (config/grass_placement_reuse, the
// Switch) also loads a ground whose transform is off by at most
// kGrassPlacementReuseRotation per rotation element and
// kGrassPlacementReuseTranslation units per translation element, and one
// stored under another geometry identity (the same ground loaded at other
// guest addresses: the same vertices, indices and texture; its tufts are
// those of that earlier build, the identity seeds them). The loaded
// placement keeps its own ModelToWorld, which the draw uses.
struct GrassPlacementDiskMatch {
    uint64_t GeometryId = 0;
    std::array<float, 16> ModelToWorld{}; // row-major; translation in 3, 7, 11
    bool Reuse = false;
};
inline constexpr float kGrassPlacementReuseRotation = 1.0e-4F;
inline constexpr float kGrassPlacementReuseTranslation = 0.5F;
[[nodiscard]] GrassPlacementDiskMatch GrassPlacementDiskMatchFor(const GrassAsyncPlacementRequest& request,
                                                                 bool reuse);
// The largest difference of two model-to-world matrices: rotation/scale
// elements and translation elements (the bottom row must be equal, else both
// are infinite).
struct GrassPlacementTransformOffset {
    float Rotation = 0.0F;
    float Translation = 0.0F;
};
[[nodiscard]] GrassPlacementTransformOffset GrassPlacementTransformDifference(const std::array<float, 16>& a,
                                                                             const std::array<float, 16>& b) noexcept;

enum class GrassPlacementDiskOutcome : uint8_t {
    Loaded,
    Missing,        // no file under the name
    Damaged,        // a file, unreadable or failing its checks
    OtherTransform, // a file of the same ground made with a transform beyond the match
    OtherGeometry,  // a file of the same ground under another geometry identity (exact matching)
};
struct GrassPlacementDiskResult {
    GrassPlacementDiskOutcome Outcome = GrassPlacementDiskOutcome::Missing;
    // Loaded with reuse: stored under another geometry identity / the stored
    // transform's offset from the requested one (also set for OtherTransform).
    bool OtherGeometry = false;
    GrassPlacementTransformOffset Offset;
};

// Whole final placement, including baked RGB, visibility hierarchy and shadow
// support. No texture, mask, vertex pointer or process-local identity is stored.
class GrassPersistentPlacementCache {
  public:
    explicit GrassPersistentPlacementCache(std::filesystem::path directory = {},
                                           uint64_t byteLimit = 1024ULL * 1024 * 1024, size_t entryLimit = 128)
        : mDirectory(std::move(directory)), mByteLimit(byteLimit), mEntryLimit(entryLimit) {
    }
    [[nodiscard]] std::shared_ptr<GrassWorldPlacement> Load(GrassPlacementDiskKey key,
                                                            const GrassPlacementDiskMatch& match,
                                                            GrassPlacementDiskResult* result = nullptr) const;
    // `built`: the geometry identity and model-to-world the placement was made with.
    [[nodiscard]] bool Store(GrassPlacementDiskKey key, const GrassWorldPlacement& placement,
                             const GrassPlacementDiskMatch& built) const;
    [[nodiscard]] std::filesystem::path Path(GrassPlacementDiskKey key) const;

  private:
    std::filesystem::path mDirectory;
    uint64_t mByteLimit;
    size_t mEntryLimit;
};
} // namespace Fast::Oot3d
