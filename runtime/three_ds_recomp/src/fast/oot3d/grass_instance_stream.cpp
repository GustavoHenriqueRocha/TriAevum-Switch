#include "fast/oot3d/grass_instance_stream.h"

#include "fast/oot3d/grass_anchor_codec.h"
#include "fast/oot3d/grass_visibility.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace Fast::Oot3d {

GrassStreamSettings GrassStreamSettingsFromProfile(const GrassSwitchProfile& profile) noexcept {
    GrassStreamSettings settings;
    settings.FullDensityDistance = std::min(profile.FullDensityDistance, profile.DrawDistance);
    settings.DrawDistance = profile.DrawDistance;
    settings.FadeStart = std::min(profile.FadeStart, profile.DrawDistance);
    settings.SingleBladeDistance = profile.SingleBladeDistance;
    settings.FillDistance = std::min(profile.FillDistance, profile.DrawDistance);
    settings.FillRadius = profile.FillRadius;
    settings.MaxInstances = profile.MaxVisibleInstances;
    return settings;
}

float GrassStreamRetention(const GrassStreamSettings& settings, float distance) noexcept {
    if (!(distance <= settings.DrawDistance)) return 0.0F;
    const float full = std::max(settings.FullDensityDistance, 1.0F);
    if (distance <= full) return 1.0F;
    const float ratio = full / distance;
    return ratio * ratio;
}

float GrassStreamFillMultiplier(const GrassStreamSettings& settings, float distance) noexcept {
    const float reach = std::min(settings.FillDistance, std::max(settings.FullDensityDistance, 1.0F));
    const float multiplier = reach / ((1.0F + 0.5F * kGrassFillGrowth) * std::max(distance, 1.0F));
    return std::clamp(multiplier, 1.0F, 1.0F + static_cast<float>(kGrassMaxFillPerTuft));
}

float GrassFixedFillCount(const GrassStreamSettings& settings) noexcept {
    return GrassStreamFillMultiplier(settings, kGrassLinkDistance);
}

float GrassFixedFillFar(const GrassStreamSettings& settings) noexcept {
    return kGrassFixedFillNear + kGrassFixedFillPerRank * (GrassFixedFillCount(settings) - 1.0F);
}

float GrassFixedFillMultiplier(const GrassStreamSettings& settings, float distance) noexcept {
    // Fill only where every tuft is kept: a fill tuft of a tuft the thinning
    // drops would vanish with it at once.
    if (!(distance < settings.FullDensityDistance)) return 1.0F;
    const float count = GrassFixedFillCount(settings);
    if (!(distance > kGrassFixedFillNear)) return count;
    const float far = GrassFixedFillFar(settings);
    const float along = far > kGrassFixedFillNear ? std::min((distance - kGrassFixedFillNear) / (far - kGrassFixedFillNear), 1.0F)
                                                  : 1.0F;
    return std::max(GrassStreamFillMultiplier(settings, distance), count + (1.0F - count) * along);
}

float GrassFixedFillRank(const GrassStreamSettings& settings, float distance) noexcept {
    // Widths clamp(rank - u, 0, 1) over ranks u spread evenly add up to
    // rank^2 / 2 below 1, rank - 1/2 up to the last fill tuft's rank, then
    // to all of them: the multiplier less 1.
    const float fill = GrassFixedFillMultiplier(settings, distance) - 1.0F;
    constexpr float kAll = static_cast<float>(kGrassMaxFillPerTuft);
    if (!(fill > 0.0F)) return 0.0F;
    if (fill < 0.5F) return std::sqrt(2.0F * fill);
    if (fill <= kAll - 0.5F) return fill + 0.5F;
    return kAll + 1.0F - std::sqrt(2.0F * std::max(kAll - fill, 0.0F));
}

float GrassFixedFillWidth(float rank, uint32_t child, float value) noexcept {
    return std::clamp(rank - (static_cast<float>(child) - 1.0F + value), 0.0F, 1.0F);
}

namespace {

uint32_t MixGrassBits(uint32_t value) noexcept {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

float GrassUnit(uint32_t value) noexcept {
    return static_cast<float>(MixGrassBits(value) >> 8U) / static_cast<float>(1U << 24U);
}

// 0 at `appear` (the distance the tuft is first drawn at), 1 once the camera
// is `growth` of it nearer: the tuft's height grows with it (grass_vsh.glsl).
float GrassGrowth(float appear, float distance, float growth) noexcept {
    if (!(distance > 0.0F)) return 1.0F;
    return std::clamp((appear / distance - 1.0F) / growth, 0.0F, 1.0F);
}

} // namespace

float GrassFillStableValue(uint32_t stableId, uint32_t child) noexcept {
    return GrassUnit(stableId ^ (child * 0x9e3779b9U));
}

uint16_t PackGrassHalf(float value) noexcept {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16U) & 0x8000U;
    const int32_t exponent = static_cast<int32_t>((bits >> 23U) & 0xffU) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffU;
    if (((bits >> 23U) & 0xffU) == 0xffU) return static_cast<uint16_t>(sign | 0x7c00U | (mantissa ? 0x200U : 0U));
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00U);
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000U;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        if ((mantissa >> (shift - 1U)) & 1U) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(exponent) << 10U) | (mantissa >> 13U);
    if (mantissa & 0x1000U) ++half; // round half up; carries into the exponent correctly
    return static_cast<uint16_t>(sign | half);
}

float UnpackGrassHalf(uint16_t value) noexcept {
    const uint32_t sign = (value & 0x8000U) << 16U;
    const uint32_t exponent = (value >> 10U) & 0x1fU;
    const uint32_t mantissa = value & 0x3ffU;
    if (exponent == 0U) {
        const float magnitude = std::ldexp(static_cast<float>(mantissa), -24);
        return sign ? -magnitude : magnitude;
    }
    if (exponent == 31U) return std::bit_cast<float>(sign | 0x7f800000U | (mantissa << 13U));
    return std::bit_cast<float>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

float GrassStreamWidthScale(float retention) noexcept {
    if (!(retention > 0.0F)) return kGrassMaxWidthScale;
    return std::clamp(1.0F / std::sqrt(retention), 1.0F, kGrassMaxWidthScale);
}

GrassStreamInstance PackGrassStreamInstance(const GrassWorldAnchor& anchor, float fade, float widthScale) noexcept {
    GrassStreamInstance instance;
    instance.PositionHeight = anchor.BaseHeight;
    instance.PackedAxis = anchor.PackedWidthAxis;
    instance.PackedNormal = anchor.PackedWorldNormal;
    instance.SurfaceColor = anchor.SurfaceColor;
    constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
    float phase = std::fmod(anchor.HalfWidthPhase[1], kTau);
    if (phase < 0.0F) phase += kTau;
    const auto unorm8 = [](float value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    instance.WidthPhaseFade = PackGrassHalf(anchor.HalfWidthPhase[0] * widthScale) | (unorm8(phase / kTau) << 16U) |
                              (unorm8(fade) << 24U);
    return instance;
}

GrassStreamInstance PackGrassFillInstance(const GrassWorldAnchor& tuft, uint32_t child, float fade, float radius,
                                          float widthScale) noexcept {
    constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
    constexpr float kGoldenAngle = 2.39996323F;
    const uint32_t seed = MixGrassBits(tuft.StableId ^ (child * 0x85ebca6bU));
    const auto unit = [&](uint32_t salt) { return GrassUnit(seed + salt); };
    // A sunflower spiral around the tuft from a start of its own: the fill
    // tufts of one tuft spread evenly over its disc, the first ones nearest.
    const float angle = GrassUnit(tuft.StableId) * kTau + static_cast<float>(child) * kGoldenAngle;
    const float spread =
        radius * std::sqrt((static_cast<float>(child) - 0.5F * unit(1U)) / static_cast<float>(kGrassMaxFillPerTuft));
    const float ox = std::cos(angle) * spread, oz = std::sin(angle) * spread;
    // On the tuft's ground plane.
    const auto normal = UnpackGrassNormal(tuft.PackedWorldNormal);
    const float oy = -(normal[0] * ox + normal[2] * oz) / std::max(normal[1], 0.3F);
    GrassWorldAnchor fill = tuft;
    fill.BaseHeight = {tuft.BaseHeight[0] + ox, tuft.BaseHeight[1] + oy, tuft.BaseHeight[2] + oz,
                       tuft.BaseHeight[3] * (0.75F + 0.5F * unit(2U))};
    const float facing = unit(3U) * kTau;
    fill.PackedWidthAxis = PackGrassDirection({std::cos(facing), std::sin(facing)});
    fill.HalfWidthPhase = {tuft.HalfWidthPhase[0] * (0.85F + 0.3F * unit(4U)), tuft.HalfWidthPhase[1] + unit(5U) * kTau};
    return PackGrassStreamInstance(fill, fade, widthScale);
}

float GrassLodFocal(const std::array<float, 16>& m) noexcept {
    // Row r of a column-major matrix: m[r], m[4 + r], m[8 + r].
    const auto row = [&](int r) { return std::sqrt(m[r] * m[r] + m[4 + r] * m[4 + r] + m[8 + r] * m[8 + r]); };
    const float w = row(3);
    const float focal = 600.0F * std::min(row(0), row(1)) / w;
    return std::isfinite(focal) && focal > 0.0F ? focal : 0.0F;
}

float GrassLodMinimumDistance(const GrassStreamSettings& settings) noexcept {
    const float nearEnd = 1.5F * settings.SingleBladeDistance;
    return nearEnd + 2.0F * (settings.CullMargin + settings.CullMarginPerDistance * nearEnd);
}

GrassLodParameters GrassLodParametersFor(const GrassStreamSettings& settings,
                                         const std::array<float, 16>& positionToClip) noexcept {
    GrassLodParameters lod;
    lod.Focal = GrassLodFocal(positionToClip);
    lod.MinimumDistance = GrassLodMinimumDistance(settings);
    lod.RampInverse = 2.0F / lod.MinimumDistance;
    return lod;
}

float GrassLodCoordinate(const GrassLodParameters& lod, float distance, float halfWidth) noexcept {
    const auto& px = kGrassLodPixels;
    const float slot = 2.0F * halfWidth * lod.Focal / std::max(distance, 1.0F);
    const float toThree = std::clamp((px[0] - slot) / (px[0] - px[1]), 0.0F, 1.0F);
    const float toOne = std::clamp((px[2] - slot) / (px[2] - px[3]), 0.0F, 1.0F);
    const float limit = 2.0F * std::clamp((distance - lod.MinimumDistance) * lod.RampInverse, 0.0F, 1.0F);
    return std::min(toThree + toOne, limit);
}

GrassStreamKind GrassLodKind(float coordinate) noexcept {
    if (coordinate >= 2.0F) return GrassStreamKind::Tufts1;
    if (coordinate >= 1.0F) return GrassStreamKind::Tufts3;
    return GrassStreamKind::Tufts;
}

uint32_t GrassLodBlades(GrassStreamKind kind) noexcept {
    switch (kind) {
    case GrassStreamKind::Tufts3: return 3U;
    case GrassStreamKind::Tufts1: return 1U;
    default: return 5U;
    }
}

uint32_t GrassLodTargetBlades(GrassStreamKind kind) noexcept {
    switch (kind) {
    case GrassStreamKind::Tufts: return 3U;
    case GrassStreamKind::Tufts3:
    case GrassStreamKind::Tufts1: return 1U;
    default: return 5U;
    }
}

GrassStreamStats GrassInstanceStreamBuilder::Build(std::span<const GrassStreamPlacement> placements,
                                                   const GrassStreamSettings& settings,
                                                   std::span<GrassStreamInstance> output,
                                                   std::vector<GrassStreamRange>& ranges) {
    GrassStreamStats stats;
    ranges.clear();
    mCandidates.clear();
    mSelected.clear();
    mPacked.clear();
    const uint32_t budget = static_cast<uint32_t>(std::min<size_t>(settings.MaxInstances, output.size()));
    if (budget == 0U || !(settings.DrawDistance > 0.0F) || placements.size() > 0x7fffU) return stats;

    // 1. Clusters inside the (grown) frustum and the draw distance.
    for (uint32_t placementIndex = 0U; placementIndex < placements.size(); ++placementIndex) {
        const auto& placement = placements[placementIndex];
        if (!placement.World || placement.World->CullingAnchors.size() != placement.World->Anchors.size()) continue;
        const auto frustum = BuildGrassFrustumRadiusScale(placement.PositionToClip);
        const auto& clusters = placement.World->Clusters;
        for (uint32_t clusterIndex = 0U; clusterIndex < clusters.size(); ++clusterIndex) {
            const auto& cluster = clusters[clusterIndex];
            ++stats.ClustersTested;
            const float dx = cluster.Center[0] - placement.Eye[0];
            const float dy = cluster.Center[1] - placement.Eye[1];
            const float dz = cluster.Center[2] - placement.Eye[2];
            const float centerDistance = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float nearest = std::max(0.0F, centerDistance - cluster.Radius);
            if (!(nearest <= settings.DrawDistance)) continue;
            const float margin = settings.CullMargin + settings.CullMarginPerDistance * nearest;
            if (!GrassSphereIntersectsFrustum(placement.PositionToClip, frustum, cluster.Center,
                                              cluster.Radius + margin))
                continue;
            mCandidates.push_back({nearest, placementIndex, clusterIndex});
        }
    }
    std::sort(mCandidates.begin(), mCandidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.Distance != b.Distance) return a.Distance < b.Distance;
        if (a.Placement != b.Placement) return a.Placement < b.Placement;
        return a.Cluster < b.Cluster;
    });
    stats.ClustersVisible = static_cast<uint32_t>(mCandidates.size());

    // 2. Their anchors, nearest clusters first, until the budget is spent.
    const float drawSquared = settings.DrawDistance * settings.DrawDistance;
    const float fullSquared = settings.FullDensityDistance * settings.FullDensityDistance;
    const float bladeSquared = settings.SingleBladeDistance * settings.SingleBladeDistance;
    const float fadeRange = std::max(settings.DrawDistance - settings.FadeStart, 1.0e-3F);
    const float fullDistance = std::max(settings.FullDensityDistance, 1.0F);
    // The first fill tuft starts to grow here, where every tuft is kept (a
    // fill tuft of a tuft the thinning drops would appear with it, at once).
    const float fillReach = std::min(settings.FillDistance, fullDistance);
    mBucketCounts.assign(placements.size() * kGrassStreamKinds, 0U);
    // config/grass_lod on: near the camera the near draw (single blades
    // and their turn into tufts, at the current camera's distance, which
    // may be up to the cull margin nearer or farther); past it the level
    // its coordinate needs at the cull margin nearer (GrassLodCoordinate).
    const float nearEnd = 1.5F * settings.SingleBladeDistance;
    const auto select = [&](const GrassStreamPlacement& placement, const GrassLodParameters& lod,
                            uint32_t placementIndex, uint32_t anchor, bool blades, uint8_t fade, uint8_t widthScale,
                            uint8_t child) {
        auto kind = blades ? GrassStreamKind::Blades : GrassStreamKind::Tufts;
        if (settings.Lod) {
            const auto& tuft = placement.World->Anchors[anchor];
            const auto instance = child == 0U ? PackGrassStreamInstance(tuft, fade / 255.0F, widthScale / 64.0F)
                                              : PackGrassFillInstance(tuft, child, fade / 255.0F, settings.FillRadius,
                                                                      widthScale / 64.0F);
            const float dx = instance.PositionHeight[0] - placement.Eye[0];
            const float dy = instance.PositionHeight[1] - placement.Eye[1];
            const float dz = instance.PositionHeight[2] - placement.Eye[2];
            const float own = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float margin = settings.CullMargin + settings.CullMarginPerDistance * own;
            if (own < nearEnd + margin) {
                kind = GrassStreamKind::Blades;
            } else {
                const float halfWidth = UnpackGrassHalf(static_cast<uint16_t>(instance.WidthPhaseFade & 0xffffU));
                kind = GrassLodKind(GrassLodCoordinate(lod, own - margin, halfWidth));
            }
            mPacked.push_back(instance);
        }
        const uint32_t bucket = placementIndex * kGrassStreamKinds + static_cast<uint32_t>(kind);
        mSelected.push_back({anchor, bucket, fade, widthScale, child});
        ++mBucketCounts[bucket];
        switch (kind) {
        case GrassStreamKind::Tufts: ++stats.Tufts; break;
        case GrassStreamKind::Blades: ++stats.Blades; break;
        case GrassStreamKind::Tufts3: ++stats.Tufts3; break;
        case GrassStreamKind::Tufts1: ++stats.Tufts1; break;
        }
    };
    for (size_t candidateIndex = 0U; candidateIndex < mCandidates.size(); ++candidateIndex) {
        if (mSelected.size() >= budget) {
            stats.ClustersOverBudget = static_cast<uint32_t>(mCandidates.size() - candidateIndex);
            break;
        }
        const auto& candidate = mCandidates[candidateIndex];
        const auto& placement = placements[candidate.Placement];
        const auto frustum = BuildGrassFrustumRadiusScale(placement.PositionToClip);
        const auto lod = settings.Lod ? GrassLodParametersFor(settings, placement.PositionToClip) : GrassLodParameters{};
        const auto& cluster = placement.World->Clusters[candidate.Cluster];
        const auto& culling = placement.World->CullingAnchors;
        const uint32_t end = static_cast<uint32_t>(
            std::min<size_t>(culling.size(), size_t{cluster.FirstAnchor} + cluster.AnchorCount));
        // A cluster's anchors are sorted by stable visibility: past the
        // retention at its nearest point none further on is kept.
        const float clusterRetention = GrassStreamRetention(settings, candidate.Distance);
        for (uint32_t anchor = cluster.FirstAnchor; anchor < end && mSelected.size() < budget; ++anchor) {
            if (culling[anchor].StableVisibility >= clusterRetention) {
                stats.AnchorsThinned += end - anchor;
                break;
            }
            ++stats.AnchorsTested;
            const auto& position = culling[anchor].Position;
            const float dx = position[0] - placement.Eye[0];
            const float dy = position[1] - placement.Eye[1];
            const float dz = position[2] - placement.Eye[2];
            const float squared = dx * dx + dy * dy + dz * dz;
            if (!(squared <= drawSquared)) continue;
            const float retention = squared > fullSquared ? GrassStreamRetention(settings, std::sqrt(squared)) : 1.0F;
            const float visibility = culling[anchor].StableVisibility;
            if (visibility >= retention) {
                ++stats.AnchorsThinned;
                continue;
            }
            const float distance = std::sqrt(squared);
            // Per tuft, the cluster's test with the tuft's own sphere (it and
            // its fill tufts): a visible cluster of 300 units straddling the
            // frustum's side brought its tufts off the screen too (about 40%
            // of them in a field, each one vertex work).
            if (!GrassSphereIntersectsFrustum(placement.PositionToClip, frustum, position,
                                              cluster.MaximumBladeHeight + settings.FillRadius + settings.CullMargin +
                                                  settings.CullMarginPerDistance * distance)) {
                ++stats.OffScreen;
                continue;
            }
            const float widest = GrassStreamWidthScale(retention);
            const bool blades = squared < bladeSquared;
            const float fade = std::clamp((settings.DrawDistance - distance) / fadeRange, 0.0F, 1.0F);
            // First kept at full / sqrt(visibility): comes in from there, over
            // the camera's next 10% nearer. config/grass_fill fixed: it widens
            // in at its full height; test.9: it grows out of the ground.
            const float kept = visibility > 0.0F ? fullDistance / std::sqrt(visibility) : settings.DrawDistance;
            if (settings.FillFixed) {
                const float thinGrowth = GrassGrowth(kept, distance, kGrassThinGrowth);
                select(placement, lod, candidate.Placement, anchor, blades, static_cast<uint8_t>(std::lround(fade * 255.0F)),
                       static_cast<uint8_t>(std::lround(widest * thinGrowth * 64.0F)), 0U);
                // Fill tufts: the same ones at their full width within
                // kGrassFixedFillNear, wherever the camera is; past it child c
                // (stable value v) narrows away as the rank falls below c - 1 + v.
                const float rank = GrassFixedFillRank(settings, distance);
                if (!(rank > 0.0F)) continue;
                const auto& tuft = placement.World->Anchors[anchor];
                for (uint32_t child = 1U; child <= kGrassMaxFillPerTuft && mSelected.size() < budget; ++child) {
                    const auto width = static_cast<uint8_t>(
                        std::lround(GrassFixedFillWidth(rank, child, GrassFillStableValue(tuft.StableId, child)) * 64.0F));
                    if (width == 0U) break; // the next ones rank higher still
                    select(placement, lod, candidate.Placement, anchor, blades,
                           static_cast<uint8_t>(std::lround(fade * 255.0F)), width, static_cast<uint8_t>(child));
                    ++stats.Fill;
                }
                continue;
            }
            const auto widthScale = static_cast<uint8_t>(std::lround(widest * 64.0F));
            const float grown = std::min(fade, GrassGrowth(kept, distance, kGrassThinGrowth));
            select(placement, lod, candidate.Placement, anchor, blades, static_cast<uint8_t>(std::lround(grown * 255.0F)),
                   widthScale, 0U);
            // Fill tufts: child c (stable value v) is drawn at half height at
            // FillDistance / (c + v), so FillDistance / distance tufts in all.
            if (!(distance < fillReach)) continue;
            const auto& tuft = placement.World->Anchors[anchor];
            for (uint32_t child = 1U; child <= kGrassMaxFillPerTuft && mSelected.size() < budget; ++child) {
                const float appear =
                    fillReach / (static_cast<float>(child) + GrassFillStableValue(tuft.StableId, child));
                const float growth = GrassGrowth(appear, distance, kGrassFillGrowth);
                if (!(growth > 0.0F)) break; // the next ones come nearer still
                select(placement, lod, candidate.Placement, anchor, blades,
                       static_cast<uint8_t>(std::lround(std::min(fade, growth) * 255.0F)), 64U,
                       static_cast<uint8_t>(child));
                ++stats.Fill;
            }
        }
    }

    // 3. Grouped by placement and kind: one draw per range.
    uint32_t first = 0U;
    std::vector<uint32_t>& offsets = mBucketCounts; // counts become write cursors
    for (uint32_t bucket = 0U; bucket < offsets.size(); ++bucket) {
        const uint32_t count = offsets[bucket];
        if (count != 0U) {
            ranges.push_back({bucket / kGrassStreamKinds, static_cast<GrassStreamKind>(bucket % kGrassStreamKinds),
                              first, count});
        }
        offsets[bucket] = first;
        first += count;
    }
    for (size_t index = 0U; index < mSelected.size(); ++index) {
        const auto& selected = mSelected[index];
        auto& slot = output[offsets[selected.Bucket]++];
        if (settings.Lod) {
            slot = mPacked[index];
            continue;
        }
        const auto& placement = placements[selected.Bucket / kGrassStreamKinds];
        const auto& anchor = placement.World->Anchors[selected.Anchor];
        slot = selected.Child == 0U
                   ? PackGrassStreamInstance(anchor, selected.Fade / 255.0F, selected.WidthScale / 64.0F)
                   : PackGrassFillInstance(anchor, selected.Child, selected.Fade / 255.0F, settings.FillRadius,
                                           selected.WidthScale / 64.0F);
    }
    return stats;
}

} // namespace Fast::Oot3d
