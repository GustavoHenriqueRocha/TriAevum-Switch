#pragma once

#include "fast/oot3d/grass_placement_cache.h"
#include "fast/oot3d/grass_world_placement_cache.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <filesystem>
#include <memory>
#include <vector>

namespace Fast::Oot3d {

enum class GrassAsyncPlacementState : uint8_t {
    Invalid,
    Pending,
    Ready,
    Failed,
};

struct GrassAsyncPlacementRequest {
    GrassPlacementKey PlacementKey;
    uint64_t SourceContentVersion = 0U;
    uint64_t WorldIdentity = 0U;
    uint64_t FrameId = 0U;
    uint64_t InstanceId = 0U;
    uint64_t TextureHash = 0U;
    uint8_t MapperSlot = 0U;
    GrassTextureWrap MaterialWrapS = GrassTextureWrap::Repeat;
    GrassTextureWrap MaterialWrapT = GrassTextureWrap::Repeat;
    std::array<float, 16> ModelToWorld{};
    bool TransformBakedIntoVertices = true;
    std::shared_ptr<const std::vector<GrassSourceVertex>> Vertices;
    std::shared_ptr<const std::vector<uint32_t>> Indices;
    std::shared_ptr<const GrassScalarMask> Mask;
    GrassTextureColorSource ColorSource;
    // Native renderer already owns the source RGBA; channel/color conversion is
    // deferred until a disk miss, never required to load baked anchors.
    uint64_t LazyMaskTexture = 0, LazyColorTexture = 0;
    GrassPlacementRule Rule;
    GrassGenerationSettings Generation;
    uint32_t Budget = 0U;
    float ClusterSize = 1.0F;
    float NormalOffset = 0.0F;
    float HeightScale = 1.0F;
    float MidrangeCellExtent = 0.0F;
    bool MidrangeAdaptive = false;
    uint32_t MidrangeAdaptiveCapacity = kGrassDefaultAdaptiveClusterCapacity;
    GrassPlacementView PlacementView;
    bool ShadowGrid = false;
    std::shared_ptr<const GrassOverlaySet> Overlays;
};

struct GrassPlacementCompletion {
    std::shared_ptr<const GrassWorldPlacement> Placement;
    double BuildMilliseconds = 0.0;
};

struct GrassAsyncPlacementResult {
    GrassAsyncPlacementState State = GrassAsyncPlacementState::Invalid;
    std::shared_ptr<const GrassWorldPlacement> Placement;
    bool Queued = false;
    double BuildMilliseconds = 0.0;
    std::shared_future<GrassPlacementCompletion> Completion;
    void WaitUntilReady();
};

// Includes geometry, placement settings and transforms, but not camera/budget.
[[nodiscard]] uint64_t GrassPlacementSourceVersion(const GrassAsyncPlacementRequest& request) noexcept;

struct GrassAsyncPlacementStats {
    size_t ResidentBytes = 0U;
    uint64_t Hits = 0U;
    uint64_t Misses = 0U;
    uint64_t PendingHits = 0U;
    uint64_t Builds = 0U;
    uint64_t Failures = 0U;
    // DiskMisses: no file; DiskRejects: a damaged file; DiskOtherTransform /
    // DiskOtherGeometry: a file of the same ground made with another
    // transform (beyond GrassPlacementDiskMatch) / geometry identity (exact
    // matching), built again.
    uint64_t DiskHits = 0, DiskMisses = 0, DiskRejects = 0, DiskWrites = 0;
    uint64_t DiskOtherTransform = 0, DiskOtherGeometry = 0;
    // The nearest of those transforms (largest element difference).
    float DiskOtherTransformRotation = 0.0F, DiskOtherTransformTranslation = 0.0F;
    // Loads with reuse (config/grass_placement_reuse): made with another
    // transform (the largest offset) / under another geometry identity.
    uint64_t DiskReusedTransform = 0, DiskReusedGeometry = 0;
    float DiskReusedRotation = 0.0F, DiskReusedTranslation = 0.0F;
    size_t Entries = 0U;
    size_t Pending = 0U;
    double BuildMilliseconds = 0.0;     // every placement made, built or loaded
    double DiskLoadMilliseconds = 0.0;  // of those, the ones loaded from the disk cache
    double BuildMaxMilliseconds = 0.0;  // the longest build (not loaded)
};

// Owns immutable placement work until it can be published to the renderer.
// No scene-bridge or Vulkan object is accessed by the worker threads.
class GrassAsyncPlacementBuilder final {
  public:
    explicit GrassAsyncPlacementBuilder(size_t capacity = 64U, size_t workerCount = 2U,
                                       size_t byteCapacity = 512U * 1024U * 1024U);
    ~GrassAsyncPlacementBuilder();
    GrassAsyncPlacementBuilder(const GrassAsyncPlacementBuilder&) = delete;
    GrassAsyncPlacementBuilder& operator=(const GrassAsyncPlacementBuilder&) = delete;

    [[nodiscard]] GrassAsyncPlacementResult ResolveOrQueue(GrassAsyncPlacementRequest request);
    [[nodiscard]] GrassAsyncPlacementStats Stats() const;
    // Placements kept on disk (GrassPersistentPlacementCache): at most
    // `byteLimit` and `entryLimit` files; empty = none.
    void SetCacheDirectory(std::filesystem::path directory, uint64_t byteLimit = 1024ULL * 1024 * 1024,
                           size_t entryLimit = 128U);
    // config/grass_placement_reuse: a stored ground loads although its
    // transform is off by camera rounding or it was stored under another
    // geometry identity (GrassPlacementDiskMatch); off: exact.
    void SetDiskReuse(bool reuse);
    // Batch owner debounces edits. Loads/builds remain immediate; only current,
    // settled results may publish to disk. A new revision cancels queued writes.
    void SetPersistenceState(uint64_t revision, bool settled);
    // How long a built placement must stay in use before it is persisted.
    void SetDiskSettleDelay(std::chrono::steady_clock::duration delay);
    void Clear();
    void WaitForIdle();
    // Until no requested placement is still being built or loaded (writes
    // left aside), at most `timeout`; true when none is.
    [[nodiscard]] bool WaitForPending(std::chrono::microseconds timeout);

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Fast::Oot3d
