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

GrassStreamInstance PackGrassFillInstance(const GrassWorldAnchor& tuft, uint32_t child, float fade,
                                          float radius) noexcept {
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
    return PackGrassStreamInstance(fill, fade);
}

GrassStreamStats GrassInstanceStreamBuilder::Build(std::span<const GrassStreamPlacement> placements,
                                                   const GrassStreamSettings& settings,
                                                   std::span<GrassStreamInstance> output,
                                                   std::vector<GrassStreamRange>& ranges) {
    GrassStreamStats stats;
    ranges.clear();
    mCandidates.clear();
    mSelected.clear();
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
    mBucketCounts.assign(placements.size() * 2U, 0U);
    for (size_t candidateIndex = 0U; candidateIndex < mCandidates.size(); ++candidateIndex) {
        if (mSelected.size() >= budget) {
            stats.ClustersOverBudget = static_cast<uint32_t>(mCandidates.size() - candidateIndex);
            break;
        }
        const auto& candidate = mCandidates[candidateIndex];
        const auto& placement = placements[candidate.Placement];
        const auto frustum = BuildGrassFrustumRadiusScale(placement.PositionToClip);
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
            const auto widthScale =
                static_cast<uint8_t>(std::lround(GrassStreamWidthScale(retention) * 64.0F));
            const bool blades = squared < bladeSquared;
            const float fade = std::clamp((settings.DrawDistance - distance) / fadeRange, 0.0F, 1.0F);
            // First kept at full / sqrt(visibility): grows from there.
            const float kept = visibility > 0.0F ? fullDistance / std::sqrt(visibility) : settings.DrawDistance;
            const float grown = std::min(fade, GrassGrowth(kept, distance, kGrassThinGrowth));
            const uint16_t bucket = static_cast<uint16_t>(candidate.Placement * 2U + (blades ? 1U : 0U));
            mSelected.push_back(
                {anchor, bucket, static_cast<uint8_t>(std::lround(grown * 255.0F)), widthScale, 0U});
            ++mBucketCounts[bucket];
            ++(blades ? stats.Blades : stats.Tufts);
            // Fill tufts: child c (stable value v) is drawn at half height at
            // FillDistance / (c + v), so FillDistance / distance tufts in all.
            if (!(distance < fillReach)) continue;
            const auto& tuft = placement.World->Anchors[anchor];
            for (uint32_t child = 1U; child <= kGrassMaxFillPerTuft && mSelected.size() < budget; ++child) {
                const float appear =
                    fillReach / (static_cast<float>(child) + GrassFillStableValue(tuft.StableId, child));
                const float growth = GrassGrowth(appear, distance, kGrassFillGrowth);
                if (!(growth > 0.0F)) break; // the next ones come nearer still
                mSelected.push_back({anchor, bucket, static_cast<uint8_t>(std::lround(std::min(fade, growth) * 255.0F)),
                                     64U, static_cast<uint8_t>(child)});
                ++mBucketCounts[bucket];
                ++(blades ? stats.Blades : stats.Tufts);
                ++stats.Fill;
            }
        }
    }

    // 3. Grouped by placement and kind: one instanced draw per range.
    uint32_t first = 0U;
    std::vector<uint32_t>& offsets = mBucketCounts; // counts become write cursors
    for (uint32_t bucket = 0U; bucket < offsets.size(); ++bucket) {
        const uint32_t count = offsets[bucket];
        if (count != 0U) {
            ranges.push_back({bucket / 2U, (bucket & 1U) ? GrassStreamKind::Blades : GrassStreamKind::Tufts, first,
                              count});
        }
        offsets[bucket] = first;
        first += count;
    }
    for (const auto& selected : mSelected) {
        const auto& placement = placements[selected.Bucket / 2U];
        const auto& anchor = placement.World->Anchors[selected.Anchor];
        output[offsets[selected.Bucket]++] =
            selected.Child == 0U
                ? PackGrassStreamInstance(anchor, selected.Fade / 255.0F, selected.WidthScale / 64.0F)
                : PackGrassFillInstance(anchor, selected.Child, selected.Fade / 255.0F, settings.FillRadius);
    }
    return stats;
}

} // namespace Fast::Oot3d
