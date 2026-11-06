#pragma once

#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Fast::Oot3d {
struct GrassOverlaySet;

struct GrassPlacementView {
    bool Enabled = false;
    std::array<float, 16> WorldToClip{};
    std::array<float, 3> Eye{};
    float DrawDistance = 0.0F;
    float FullDensityDistance = 0.0F;
    float FarDensity = 0.0F;
    float GuardDistance = 0.0F;
};

struct GrassSourceVertex {
    std::array<float, 3> Position{};
    std::array<float, 3> Normal{0.0F, 1.0F, 0.0F};
    std::array<float, 2> Uv{};
    std::array<float, 3> SourceWeights{};
};

struct GrassSourceSurface {
    uint64_t GeometryId = 0;
    uint64_t ContentVersion = 0;
    uint64_t InstanceId = 0;
    uint64_t TextureHash = 0;
    uint8_t MapperSlot = 0;
    GrassTextureWrap MaterialWrapS = GrassTextureWrap::Repeat;
    GrassTextureWrap MaterialWrapT = GrassTextureWrap::Repeat;
    std::array<float, 16> ModelToWorld{};
    bool TransformBakedIntoVertices = true;
    std::span<const GrassSourceVertex> Vertices;
    std::span<const uint32_t> Indices;
    GrassPlacementView PlacementView;
    std::span<const std::array<uint32_t, 3>> TriangleSources;
    std::shared_ptr<const GrassOverlaySet> Overlays;
};

struct GrassAnchor {
    std::array<uint32_t, 2> SurfaceReference{};
    std::array<float, 3> LocalPosition{};
    std::array<float, 3> LocalNormal{};
    std::array<float, 2> Uv{};
    float BladeHeight = 0.3F;
    float BladeWidth = 0.03F;
    float Phase = 0.0F;
    std::array<float, 2> WidthAxis{1.0F, 0.0F};
    uint32_t StableId = 0U;
};

struct GrassAnchorCluster {
    uint32_t FirstAnchor = 0U;
    uint32_t AnchorCount = 0U;
    std::array<float, 3> LocalCenter{};
    float LocalRadius = 0.0F;
    float MaximumBladeHeight = 0.0F;
};

struct GrassPlacementSet {
    std::vector<GrassAnchor> Anchors;
    std::vector<GrassAnchorCluster> Clusters;
};

[[nodiscard]] GrassTextureWrap ResolveGrassTextureWrap(
    GrassWrapOverride overrideMode,
    GrassTextureWrap materialMode) noexcept;
[[nodiscard]] float WrapGrassTextureCoordinate(
    float coordinate, GrassTextureWrap wrap) noexcept;
// Converts one selected texture channel into placement probability. This is
// the canonical mask evaluation used by extraction and editor previews.
[[nodiscard]] float EvaluateGrassMaskLevel(
    float sample, const GrassPlacementRule& rule) noexcept;
// Whether a mask position (normalized, after wrap) is active: inside one of
// the rule's rectangles, or anywhere when it lists none.
[[nodiscard]] bool GrassMaskPositionActive(
    const GrassPlacementRule& rule, float u, float v) noexcept;
// The level at a mask position: zero outside the active rectangles.
[[nodiscard]] float EvaluateGrassMaskLevel(
    float sample, const GrassPlacementRule& rule, float u, float v) noexcept;

[[nodiscard]] uint64_t GrassPlacementRuleVersion(
    const GrassPlacementRule& rule,
    const GrassGenerationSettings& generation) noexcept;

[[nodiscard]] bool GrassPlacementViewNeedsRefresh(
    const GrassPlacementView& previous, const GrassPlacementView& current) noexcept;

// Candidates evaluated per allowed anchor when a budget bites: the mask,
// clustering and minimum spacing reject some, so the extractor samples up to
// this many times the budget and thins the survivors to it.
inline constexpr uint32_t kGrassCandidateHeadroom = 4U;

// Deterministic density thinning. When more anchors survive than `limit`,
// keeps the `limit` with the smallest rank key (a hash of StableId that is
// independent of position, of extraction order and of the LOD visibility
// value), in their original order. Every anchor has the same chance wherever
// it lies, so a budget that bites lowers the density evenly instead of
// leaving the last triangles of the index buffer bare.
void ThinGrassAnchorsToLimit(std::vector<GrassAnchor>& anchors, uint32_t limit);

class GrassSurfaceExtractor final {
  public:
    [[nodiscard]] static std::vector<GrassAnchor> Extract(
        const GrassSourceSurface& surface, const GrassPlacementRule& rule,
        const GrassGenerationSettings& generation,
        const GrassScalarMask& mask, uint32_t budget);
};

[[nodiscard]] GrassPlacementSet BuildGrassPlacementSet(
    std::vector<GrassAnchor> anchors, float clusterSize);

} // namespace Fast::Oot3d
