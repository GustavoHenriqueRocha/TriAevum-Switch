#include "fast/oot3d/grass_draw_preparation.h"

#include "fast/oot3d/grass_anchor_codec.h"
#include "fast/oot3d/grass_cluster_limits.h"
#include "fast/oot3d/grass_collision_resolver.h"
#include "fast/oot3d/grass_interaction_field.h"
#include "fast/oot3d/grass_midrange_clusters.h"
#include "fast/oot3d/grass_visibility.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace Fast::Oot3d {

size_t GrassLodBinIndex(uint8_t bladeSegments, uint8_t planeCount) {
    const auto segments = std::clamp<uint8_t>(bladeSegments, kMinimumGrassBladeSegments, kMaximumGrassBladeSegments);
    const auto planes = std::clamp<uint8_t>(planeCount, 1U, 2U);
    return (static_cast<size_t>(segments) - 1U) * 2U + (static_cast<size_t>(planes) - 1U);
}

GrassEnvironmentRecord BuildGrassEnvironmentRecord(
    const GrassPicaFogState& fog, bool receiveFog, bool picaDepthAvailable,
    const InteractiveGrassSettings& settings, const GrassShadingEnvironment& shading,
    const std::optional<std::array<float, 3>>& textureAverage, double visualSeconds,
    const ::Fast::Renderer3ds::PicaPerspectiveCameraState& view, const std::array<float, 3>& viewForward,
    const std::array<float, 3>& viewSide, const std::array<float, 3>& viewUp) {
    GrassEnvironmentRecord result;
    result.RimDistance = {settings.Appearance.ToonRimFadeStart, settings.Appearance.ToonRimFadeEnd,
        settings.Appearance.ToonRimEnabled ? 1.0F : 0.0F, 0.0F};
    result.TuftLod = {settings.LodEndFraction,
        settings.LodReferenceDistance > 0.0F ? settings.LodReferenceDistance : settings.DrawDistance,
        static_cast<float>(settings.FarTuftBladeCount), settings.TuftTransitionFraction};
    result.DistanceLod = {settings.DrawDistance, settings.LodStartFraction, settings.FarDensity, settings.DensityFadeFraction};
    result.TuftStyle = {settings.FarTuftSpread, settings.FarTuftDensity, settings.DrawFadeFraction,
        settings.FarTuftsEnabled && !settings.MidrangeClustersEnabled ? 1.0F : 0.0F};
    result.BladeShape = {
        settings.Appearance.BladeCurvature,
        settings.Appearance.BladeDroop,
        settings.Appearance.ShapeVariation,
        settings.Appearance.BladeTwistDegrees * std::numbers::pi_v<float> / 180.0F};
    if (receiveFog && fog.Enabled && picaDepthAvailable) {
        result.ColorAndMode = {fog.Color[0], fog.Color[1], fog.Color[2], fog.Flip ? 2.0F : 1.0F};
        result.Lut = fog.Lut;
    }
    const float direction = settings.WindDirectionDegrees * std::numbers::pi_v<float> / 180.0F;
    result.WindDirectionTime = {std::cos(direction), std::sin(direction), static_cast<float>(visualSeconds),
                                settings.WindStrength};
    result.WindPrimary = {settings.WindSpeed, settings.WindSpatialScale, settings.WindGustStrength,
                          settings.WindGustFrequency};
    result.WindDetail = {settings.WindTurbulence, settings.WindRandomness, settings.MaximumBend,
                         settings.WindStrength > 0.0F && settings.MaximumBend > 0.0F ? 1.0F : 0.0F};
    result.CameraPosition = {view.Eye[0], view.Eye[1], view.Eye[2], 0.0F};
    result.AppearanceRoot = {settings.Appearance.RootColor[0], settings.Appearance.RootColor[1],
                             settings.Appearance.RootColor[2], 1.0F};
    result.AppearanceTip = {settings.Appearance.TipColor[0], settings.Appearance.TipColor[1],
                            settings.Appearance.TipColor[2], 1.0F};
    if (textureAverage.has_value()) {
        result.TextureColor = {(*textureAverage)[0], (*textureAverage)[1], (*textureAverage)[2],
                               settings.Appearance.TextureColorInfluence};
    }
    result.TextureBrightnessFlags = {settings.Appearance.TextureRootBrightness,
                                     settings.Appearance.TextureTipBrightness,
                                     settings.Appearance.ReceiveLighting ? 1.0F : 0.0F,
                                     static_cast<float>(shading.ActiveLightCount)};
    for (size_t index = 0U; index < shading.ActiveLightCount && index < kGrassNativeLightCount; ++index) {
        const auto& light = shading.Lights[index];
        result.LightDirections[index] = {light.DirectionWorldTowardSource[0], light.DirectionWorldTowardSource[1],
                                         light.DirectionWorldTowardSource[2], light.Valid ? 1.0F : 0.0F};
        result.LightDiffuse[index] = {light.Diffuse[0], light.Diffuse[1], light.Diffuse[2], 0.0F};
        result.LightAmbient[index] = {light.Ambient[0], light.Ambient[1], light.Ambient[2], 0.0F};
    }
    result.ViewForward = {viewForward[0], viewForward[1], viewForward[2], 0.0F};
    result.ViewSide = {viewSide[0], viewSide[1], viewSide[2], 0.0F};
    result.ViewUp = {viewUp[0], viewUp[1], viewUp[2], 0.0F};
    return result;
}

std::optional<std::array<float, 16>> InvertAffineRowMajor(const std::array<float, 16>& matrix) {
    const float determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[6] * matrix[9]) -
                              matrix[1] * (matrix[4] * matrix[10] - matrix[6] * matrix[8]) +
                              matrix[2] * (matrix[4] * matrix[9] - matrix[5] * matrix[8]);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12F) return std::nullopt;
    const float inverseDeterminant = 1.0F / determinant;
    std::array<float, 16> inverse{
        (matrix[5] * matrix[10] - matrix[6] * matrix[9]) * inverseDeterminant,
        (matrix[2] * matrix[9] - matrix[1] * matrix[10]) * inverseDeterminant,
        (matrix[1] * matrix[6] - matrix[2] * matrix[5]) * inverseDeterminant,
        0.0F,
        (matrix[6] * matrix[8] - matrix[4] * matrix[10]) * inverseDeterminant,
        (matrix[0] * matrix[10] - matrix[2] * matrix[8]) * inverseDeterminant,
        (matrix[2] * matrix[4] - matrix[0] * matrix[6]) * inverseDeterminant,
        0.0F,
        (matrix[4] * matrix[9] - matrix[5] * matrix[8]) * inverseDeterminant,
        (matrix[1] * matrix[8] - matrix[0] * matrix[9]) * inverseDeterminant,
        (matrix[0] * matrix[5] - matrix[1] * matrix[4]) * inverseDeterminant,
        0.0F,
        0.0F, 0.0F, 0.0F, 1.0F};
    inverse[3] = -(inverse[0] * matrix[3] + inverse[1] * matrix[7] + inverse[2] * matrix[11]);
    inverse[7] = -(inverse[4] * matrix[3] + inverse[5] * matrix[7] + inverse[6] * matrix[11]);
    inverse[11] = -(inverse[8] * matrix[3] + inverse[9] * matrix[7] + inverse[10] * matrix[11]);
    return inverse;
}

std::array<float, 16> MultiplyRowMajor(const std::array<float, 16>& left, const std::array<float, 16>& right) {
    std::array<float, 16> result{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column)
            for (size_t inner = 0; inner < 4U; ++inner)
                result[row * 4U + column] += left[row * 4U + inner] * right[inner * 4U + column];
    return result;
}

GrassPushConstants ProjectionForMesh(const GrassSceneMesh& mesh, const std::array<float, 16>& fallbackWorldToClip) {
    GrassPushConstants push;
    push.PositionToClip = fallbackWorldToClip;
    if (!mesh.PicaModelToClipAvailable || mesh.TransformBakedIntoVertices) return push;
    const auto worldToModel = InvertAffineRowMajor(mesh.ModelToWorld);
    if (!worldToModel.has_value()) return push;
    std::array<float, 16> picaModelToClipRowMajor{};
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column)
            picaModelToClipRowMajor[row * 4U + column] = mesh.PicaModelToClip[column * 4U + row];
    const auto worldToClipRowMajor = MultiplyRowMajor(picaModelToClipRowMajor, *worldToModel);
    for (size_t row = 0; row < 4U; ++row)
        for (size_t column = 0; column < 4U; ++column)
            push.PositionToClip[column * 4U + row] = worldToClipRowMajor[row * 4U + column];
    push.Flags[0] = 1U;
    return push;
}

void AppendGrassPlacementBatches(std::span<const GrassVisibleIndexBins* const> sources,
                                 const GrassSelectedClusterRanges& selectedClusterRanges,
                                 const GrassWorldPlacement& world, const GrassPushConstants& push,
                                 uint32_t staticBaseIndex, uint32_t placementIndex, bool midrangeClusters,
                                 bool clusterOwned, GrassBatchOutput output) {
    auto& visible = output.VisibleAnchorIndices;
    auto& drawGroups = output.DrawGroups;
    for (size_t binIndex = 0U; binIndex < kGrassLodBinCount; ++binIndex) {
        const uint32_t firstInstance = static_cast<uint32_t>(visible.size());
        uint32_t binInstanceCount = 0U;
        for (const auto* source : sources) {
            const auto& bin = (*source)[binIndex];
            visible.insert(visible.end(), bin.begin(), bin.end());
            binInstanceCount += static_cast<uint32_t>(bin.size());
        }
        if (binInstanceCount == 0U) continue;
        const uint8_t bladeSegments = static_cast<uint8_t>(binIndex / 2U + 1U);
        const uint8_t planeCount = static_cast<uint8_t>(binIndex % 2U + 1U);
        auto batchPush = push;
        const bool tuft = binIndex == kGrassLodBinCount - 1U;
        batchPush.Flags[1] = tuft ? 0U : bladeSegments;
        batchPush.Flags[2] = planeCount;
        batchPush.Flags[3] = placementIndex;
        const bool grouped = midrangeClusters && !tuft && bladeSegments <= 2 && !world.Midrange.Groups.empty();
        if (!grouped) {
            output.Batches.push_back({firstInstance, binInstanceCount, static_cast<uint8_t>(tuft ? 0U : bladeSegments),
                                      planeCount, batchPush, placementIndex});
            continue;
        }
        const auto& map = world.Midrange.GroupForRoot;
        const uint32_t firstGroup = static_cast<uint32_t>(drawGroups.size());
        const auto groups = clusterOwned
            ? RebaseGrassMidrangeDraws(selectedClusterRanges[binIndex], binInstanceCount, firstInstance)
            : PackGrassMidrangeDraws(std::span<uint32_t>(visible).subspan(firstInstance, binInstanceCount), map,
                                     staticBaseIndex, firstInstance, clusterOwned);
        drawGroups.insert(drawGroups.end(), groups.begin(), groups.end());
        batchPush.Flags[1] |= 0x80000000U;
        for (uint32_t first = firstGroup; first < drawGroups.size();) {
            uint32_t end = first + 1;
            const auto count = GrassClusterDrawCapacity(drawGroups[first][1]);
            while (end < drawGroups.size() && GrassClusterDrawCapacity(drawGroups[end][1]) == count) ++end;
            output.Batches.push_back({first, end - first, bladeSegments, planeCount, batchPush, placementIndex, true,
                                      count});
            first = end;
        }
    }
}

void WriteGrassCpuInstances(std::span<const GrassWorldAnchor> anchors, std::span<const uint32_t> visible,
                            const GrassInteractionField& field, std::span<const GrassInteractor> actors,
                            const InteractiveGrassSettings& settings, GrassInstance* output) {
    for (size_t instanceIndex = 0U; instanceIndex < visible.size(); ++instanceIndex) {
        const uint32_t anchorIndex = visible[instanceIndex];
        if (anchorIndex >= anchors.size()) throw std::runtime_error("interactive grass visible anchor is invalid");
        const auto& anchor = anchors[anchorIndex];
        const std::array<float, 3> base{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]};
        std::array<float, 2> dynamicBend = field.Contains(base[0], base[2])
            ? field.Sample(base[0], base[2], base[1], settings.InteractionVerticalMargin)
            : std::array<float, 2>{};
        for (const auto& actor : actors) {
            const auto collision = ResolveGrassCollision(base, anchor.BaseHeight[3], actor, settings);
            dynamicBend[0] += collision.Bend[0];
            dynamicBend[1] += collision.Bend[1];
        }
        const float bendLength = std::hypot(dynamicBend[0], dynamicBend[1]);
        if (bendLength > settings.MaximumBend && bendLength > 0)
            for (auto& value : dynamicBend) value *= settings.MaximumBend / bendLength;
        output[instanceIndex] = {
            anchor.BaseHeight,
            {dynamicBend[0], dynamicBend[1], anchor.HalfWidthPhase[0], anchor.HalfWidthPhase[1]},
            UnpackGrassDirection(anchor.PackedWidthAxis),
            UnpackGrassNormal(anchor.PackedWorldNormal),
            anchor.SurfaceColor,
            anchor.SurfaceReference,
        };
        output[instanceIndex].WorldNormal[3] = GrassStableVisibilityValue(anchor.StableId);
    }
}

} // namespace Fast::Oot3d
