#pragma once

// The per-frame grass instance stream of the deko3d backend (Switch): which
// tufts of the retained placements are drawn this frame, and the 32-byte
// instances the grass vertex shader reads (tools/oot3d/deko3d_grass). Pure
// CPU work, run on the grass worker thread, never on the game thread.
//
// Culling is per cluster: a cluster whose bounding sphere (grown by a margin
// for the one frame the result lags the camera) is outside the frustum or
// beyond the draw distance is skipped whole. Past FullDensityDistance the
// tufts thin with distance (kept when their stable visibility value is below
// (full / distance)^2, so about the same number per screen area), down to
// none at DrawDistance; they also shrink from FadeStart. The kept clusters
// are taken nearest first until the visible budget is spent, so a budget
// that bites shortens the reach instead of thinning the near grass.
//
// Nearer than FillDistance (at most FullDensityDistance, where every tuft is
// kept) the same tufts per m2 cover less of the screen the nearer they are
// (coverage ~ density x distance): each kept tuft on screen brings fill
// tufts around it so the grass around Link covers as much of the screen as
// the full-density band. config/grass_fill (GrassStreamSettings::FillFixed)
// chooses how: fixed (default), the same fill tufts at full size wherever
// the camera is within kGrassFixedFillNear, narrowing away past it; or
// test.9's, FillDistance / distance tufts in all (at most
// 1 + kGrassMaxFillPerTuft), each growing out of the ground over
// kGrassFillGrowth of its distance, a thinned tuft over kGrassThinGrowth.

#include "fast/oot3d/grass_switch_profile.h"
#include "fast/oot3d/grass_world_placement_cache.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Fast::Oot3d {

// Vertex stream of tools/oot3d/deko3d_grass/grass_vsh.glsl (per instance).
struct GrassStreamInstance {
    std::array<float, 4> PositionHeight{}; // world base, tuft height
    uint32_t PackedAxis = 0x00007fffU;     // snorm16 x2: width axis (x, z)
    uint32_t PackedNormal = 0x7fff0000U;   // octahedral snorm16 x2 ground normal
    uint32_t SurfaceColor = 0U;            // RGBA8 of the ground under the tuft
    // half float: blade half width (low 16 bits); unorm8: wind phase / 2pi
    // (bits 16-23); unorm8: distance fade (bits 24-31, 255 = full height).
    uint32_t WidthPhaseFade = 0U;
};
static_assert(sizeof(GrassStreamInstance) == 32U);

struct GrassStreamPlacement {
    std::shared_ptr<const GrassWorldPlacement> World;
    std::array<float, 16> PositionToClip{}; // world to the source draw's clip space, column-major
    std::array<float, 3> Eye{};
};

struct GrassStreamSettings {
    float FullDensityDistance = 1000.0F;
    float DrawDistance = 4000.0F;
    float FadeStart = 3300.0F;
    float SingleBladeDistance = 400.0F;
    // Fill tufts nearer than this (0: none), within FillRadius of their tuft.
    float FillDistance = 1000.0F;
    float FillRadius = 18.0F;
    uint32_t MaxInstances = 8000U;
    // Added to every cluster sphere: the camera of the frame that draws the
    // stream is one frame newer than the one it was culled with.
    float CullMargin = 64.0F;
    float CullMarginPerDistance = 0.06F;
    // config/grass_lod: the stream's kinds are the draws of grass_lod_vsh.glsl
    // (GrassStreamKind); off, test.9's two kinds of grass_vsh.glsl.
    bool Lod = false;
    // config/grass_fill: fixed (GrassFixedFillRank: nothing near the camera
    // changes as it moves; transitions narrow, far away); off, test.9's fill
    // and thinning edge, growing out of the ground.
    bool FillFixed = true;
};
[[nodiscard]] GrassStreamSettings GrassStreamSettingsFromProfile(const GrassSwitchProfile& profile) noexcept;

// The draw an instance goes to. config/grass_lod off: Tufts and Blades
// (single blades within SingleBladeDistance), both grass_vsh.glsl. On:
// Blades are the near draw (single blades and their turn into tufts, with
// the cull margin), Tufts the 5-blade far draw, Tufts3 and Tufts1 the 3-
// and 1-blade ones (GrassLodLevel).
enum class GrassStreamKind : uint8_t { Tufts, Blades, Tufts3, Tufts1 };
inline constexpr uint32_t kGrassStreamKinds = 4U;

// Instances [First, First + Count) of one placement, drawn as one kind.
struct GrassStreamRange {
    uint32_t Placement = 0U;
    GrassStreamKind Kind = GrassStreamKind::Tufts;
    uint32_t First = 0U;
    uint32_t Count = 0U;
};

struct GrassStreamStats {
    uint32_t ClustersTested = 0U;
    uint32_t ClustersVisible = 0U;
    uint64_t AnchorsTested = 0U;
    uint32_t Tufts = 0U;
    uint32_t Blades = 0U;
    // config/grass_lod on: the 3- and 1-blade far tufts (Tufts holds the 5-blade ones).
    uint32_t Tufts3 = 0U;
    uint32_t Tufts1 = 0U;
    // Of the instances, the fill tufts (the rest are the placement's own).
    uint32_t Fill = 0U;
    // Kept tufts of visible clusters outside the frustum (not drawn).
    uint32_t OffScreen = 0U;
    // Clusters the visible budget left out (the farthest ones).
    uint32_t ClustersOverBudget = 0U;
    // Anchors past FullDensityDistance left out by the distance thinning.
    uint64_t AnchorsThinned = 0U;
    [[nodiscard]] uint32_t Instances() const noexcept { return Tufts + Blades + Tufts3 + Tufts1; }
};

// The share of tufts kept at `distance` (1 up to FullDensityDistance).
[[nodiscard]] float GrassStreamRetention(const GrassStreamSettings& settings, float distance) noexcept;

// Fill: at most this many more tufts per kept tuft (at most 9x the
// placement's density, right in front of the camera).
inline constexpr uint32_t kGrassMaxFillPerTuft = 8U;
// config/grass_fill = 0 (test.9): a fill tuft c (1..kGrassMaxFillPerTuft)
// with stable value v starts to grow at FillDistance / (c + v) and is full
// grown kGrassFillGrowth nearer; a tuft the distance thinning keeps grows
// while the camera comes 10% nearer than the distance where it is first
// kept. Every other tuft is drawn at full height.
inline constexpr float kGrassFillGrowth = 0.25F;
inline constexpr float kGrassThinGrowth = 0.1F;
// The tufts drawn per placement tuft at `distance` (fill included), counting
// a fill tuft from half its growth: 1 at FillDistance, about FillDistance /
// (1.125 distance) nearer.
[[nodiscard]] float GrassStreamFillMultiplier(const GrassStreamSettings& settings, float distance) noexcept;

// ---- config/grass_fill fixed (SWITCH_PORT.md "Grass on deko3d: nothing grows near Link")
//
// test.9's fill tufts grew out of the ground as the camera came nearer:
// child c at FillDistance / (c + v), over a quarter of that distance, so
// children 2-4 grew right around Link (250-500 units from the camera) as he
// walked. Fixed: within kGrassFixedFillNear every tuft brings the same fill
// tufts, about GrassFixedFillCount - 1 of them (test.9's multiplier at
// kGrassLinkDistance, where Link stands from the field camera), whatever
// the camera does; past it the multiplier falls to test.9's (or 1) by
// GrassFixedFillFar, kGrassFixedFillPerRank units per fill tuft, each one
// narrowing away where it stands (its width, never its height).
inline constexpr float kGrassLinkDistance = 250.0F;
inline constexpr float kGrassFixedFillNear = 450.0F;
inline constexpr float kGrassFixedFillPerRank = 120.0F;
// The tufts drawn per placement tuft within kGrassFixedFillNear.
[[nodiscard]] float GrassFixedFillCount(const GrassStreamSettings& settings) noexcept;
// Where the multiplier has come down to test.9's.
[[nodiscard]] float GrassFixedFillFar(const GrassStreamSettings& settings) noexcept;
// The tufts drawn per placement tuft at `distance` (fill included): the
// count to kGrassFixedFillNear, then down to test.9's multiplier by
// GrassFixedFillFar; 1 from FullDensityDistance on.
[[nodiscard]] float GrassFixedFillMultiplier(const GrassStreamSettings& settings, float distance) noexcept;
// The fill rank reached at `distance`: fill tuft c with stable value v is
// drawn at width clamp(rank - (c - 1 + v), 0, 1), so the widths add up to
// the multiplier less 1 on average.
[[nodiscard]] float GrassFixedFillRank(const GrassStreamSettings& settings, float distance) noexcept;
[[nodiscard]] float GrassFixedFillWidth(float rank, uint32_t child, float value) noexcept;

// Packing helpers, exposed for the tests (GLSL unpackHalf2x16 / unpackUnorm).
[[nodiscard]] uint16_t PackGrassHalf(float value) noexcept;
[[nodiscard]] float UnpackGrassHalf(uint16_t value) noexcept;
// `widthScale` widens the tuft where distance thinning left fewer of them,
// so the ground stays about as covered (GrassStreamWidthScale).
[[nodiscard]] GrassStreamInstance PackGrassStreamInstance(const GrassWorldAnchor& anchor, float fade,
                                                          float widthScale = 1.0F) noexcept;
// Fill tuft `child` (1..kGrassMaxFillPerTuft) of `tuft`: a stable spot
// within `radius` on the tuft's ground plane, its own height, facing and
// phase, the tuft's ground colour and normal; `widthScale` narrows it
// (config/grass_fill fixed).
[[nodiscard]] GrassStreamInstance PackGrassFillInstance(const GrassWorldAnchor& tuft, uint32_t child, float fade,
                                                        float radius, float widthScale = 1.0F) noexcept;
// The stable value of fill tuft `child` (in [0, 1)): which fill tufts come
// first as the camera comes nearer.
[[nodiscard]] float GrassFillStableValue(uint32_t stableId, uint32_t child) noexcept;
// 1 / sqrt(retention), at most kGrassMaxWidthScale: the kept tufts cover the
// area of the thinned ones (their card is that much wider).
inline constexpr float kGrassMaxWidthScale = 3.0F;
[[nodiscard]] float GrassStreamWidthScale(float retention) noexcept;

// ---- config/grass_lod: levels of detail (SWITCH_PORT.md "Grass on deko3d: cost per tuft")
//
// A tuft's level coordinate runs from 0 (5 blades) through 1 (3 blades) to
// 2 (1 blade) as the projected width of a blade slot (2 x its half width,
// on a 1200x720 target) falls from kGrassLodPixels[0] to [3]; between the
// levels it turns from one into the next (grass_lod_blade). It stays 0 up
// to GrassLodMinimumDistance and reaches 2 at most 1.5x that far, so the
// near draw (single blades turning into tufts) never needs a level.
// grass_tuft_vsh.glsl computes it each frame (grass_lod_coordinate); the
// cull worker picks the draw from it at the tuft's distance less the cull
// margin (the camera may come nearer before the next cull), so a tuft is
// drawn with at least the blades it needs and changes draw only where both
// layouts are the same.
inline constexpr std::array<float, 4> kGrassLodPixels{2.4F, 1.8F, 1.2F, 0.9F};
struct GrassLodParameters {
    // Pixels per world unit at distance 1 on a 1200x720 target (GrassLodFocal).
    float Focal = 0.0F;
    float MinimumDistance = 800.0F;
    float RampInverse = 1.0F / 400.0F;
};
// 600 x the smaller of the clip x and y scales over the clip w scale of a
// world-to-clip matrix (column-major): a pixel width at 1200x720 (5:3, the
// long side is the one with the smaller scale), whichever way the PICA
// target is turned. 0 for a degenerate matrix (no levels).
[[nodiscard]] float GrassLodFocal(const std::array<float, 16>& positionToClip) noexcept;
// Where the levels may start: past the near draw and twice its cull margin.
[[nodiscard]] float GrassLodMinimumDistance(const GrassStreamSettings& settings) noexcept;
[[nodiscard]] GrassLodParameters GrassLodParametersFor(const GrassStreamSettings& settings,
                                                       const std::array<float, 16>& positionToClip) noexcept;
// grass_lod_coordinate of grass_lod_common.glsl, in [0, 2].
[[nodiscard]] float GrassLodCoordinate(const GrassLodParameters& lod, float distance, float halfWidth) noexcept;
// The far draw of a tuft whose coordinate is `coordinate`: the fewest blades whose
// layout still shows it (Tufts below 1, Tufts3 below 2, else Tufts1).
[[nodiscard]] GrassStreamKind GrassLodKind(float coordinate) noexcept;
// Blades per tuft of a draw (5, 3, 1) and the blades it turns into.
[[nodiscard]] uint32_t GrassLodBlades(GrassStreamKind kind) noexcept;
[[nodiscard]] uint32_t GrassLodTargetBlades(GrassStreamKind kind) noexcept;
// The near draw's single blades are curved, in this many segments
// (GRASS_NEAR_SEGMENTS of grass_lod_common.glsl); the far draws' tuft blades
// in this many (GRASS_FAR_SEGMENTS).
inline constexpr uint32_t kGrassNearBladeSegments = 3U;
inline constexpr uint32_t kGrassFarBladeSegments = 2U;
// Vertices of one blade of a draw: 6 x segments - 3 (a quad per segment, a
// triangle to the tip), kGrassNearBladeSegments near, kGrassFarBladeSegments far.
[[nodiscard]] uint32_t GrassLodVerticesPerBlade(GrassStreamKind kind) noexcept;

// Owns its scratch, so a frame allocates nothing once the sizes settled.
class GrassInstanceStreamBuilder {
  public:
    // Fills `output` (at most MaxInstances, at most output.size()) and the
    // ranges, grouped by placement then kind. Deterministic for equal input.
    GrassStreamStats Build(std::span<const GrassStreamPlacement> placements, const GrassStreamSettings& settings,
                           std::span<GrassStreamInstance> output, std::vector<GrassStreamRange>& ranges);

  private:
    struct Candidate {
        float Distance = 0.0F;
        uint32_t Placement = 0U;
        uint32_t Cluster = 0U;
    };
    struct Selected {
        uint32_t Anchor = 0U;
        uint32_t Bucket = 0U; // placement * kGrassStreamKinds + kind
        uint8_t Fade = 255U;
        uint8_t WidthScale = 64U; // x 1/64
        uint8_t Child = 0U;       // 0: the tuft itself, else its fill tuft
    };
    std::vector<Candidate> mCandidates;
    std::vector<Selected> mSelected;
    // config/grass_lod on: the instances, packed when selected (their
    // packed half width and place pick the level).
    std::vector<GrassStreamInstance> mPacked;
    std::vector<uint32_t> mBucketCounts;
};

} // namespace Fast::Oot3d
