#include "fast/oot3d/grass_instance_stream.h"

#include "fast/oot3d/grass_visibility.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace Fast::Oot3d {

GrassStreamSettings GrassStreamSettingsFromProfile(const GrassSwitchProfile& profile) noexcept {
    GrassStreamSettings settings;
    settings.DrawDistance = profile.DrawDistance;
    settings.FadeStart = std::min(profile.FadeStart, profile.DrawDistance);
    settings.SingleBladeDistance = profile.SingleBladeDistance;
    settings.MaxInstances = profile.MaxVisibleInstances;
    return settings;
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

GrassStreamInstance PackGrassStreamInstance(const GrassWorldAnchor& anchor, float fade) noexcept {
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
    instance.WidthPhaseFade = PackGrassHalf(anchor.HalfWidthPhase[0]) | (unorm8(phase / kTau) << 16U) |
                              (unorm8(fade) << 24U);
    return instance;
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
    const float bladeSquared = settings.SingleBladeDistance * settings.SingleBladeDistance;
    const float fadeRange = std::max(settings.DrawDistance - settings.FadeStart, 1.0e-3F);
    mBucketCounts.assign(placements.size() * 2U, 0U);
    for (size_t candidateIndex = 0U; candidateIndex < mCandidates.size(); ++candidateIndex) {
        if (mSelected.size() >= budget) {
            stats.ClustersOverBudget = static_cast<uint32_t>(mCandidates.size() - candidateIndex);
            break;
        }
        const auto& candidate = mCandidates[candidateIndex];
        const auto& placement = placements[candidate.Placement];
        const auto& cluster = placement.World->Clusters[candidate.Cluster];
        const auto& culling = placement.World->CullingAnchors;
        const uint32_t end = static_cast<uint32_t>(
            std::min<size_t>(culling.size(), size_t{cluster.FirstAnchor} + cluster.AnchorCount));
        for (uint32_t anchor = cluster.FirstAnchor; anchor < end && mSelected.size() < budget; ++anchor) {
            ++stats.AnchorsTested;
            const auto& position = culling[anchor].Position;
            const float dx = position[0] - placement.Eye[0];
            const float dy = position[1] - placement.Eye[1];
            const float dz = position[2] - placement.Eye[2];
            const float squared = dx * dx + dy * dy + dz * dz;
            if (!(squared <= drawSquared)) continue;
            const bool blades = squared < bladeSquared;
            const float fade = std::clamp((settings.DrawDistance - std::sqrt(squared)) / fadeRange, 0.0F, 1.0F);
            const uint16_t bucket = static_cast<uint16_t>(candidate.Placement * 2U + (blades ? 1U : 0U));
            mSelected.push_back({anchor, bucket, static_cast<uint8_t>(std::lround(fade * 255.0F))});
            ++mBucketCounts[bucket];
            ++(blades ? stats.Blades : stats.Tufts);
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
        output[offsets[selected.Bucket]++] =
            PackGrassStreamInstance(placement.World->Anchors[selected.Anchor], selected.Fade / 255.0F);
    }
    return stats;
}

} // namespace Fast::Oot3d
