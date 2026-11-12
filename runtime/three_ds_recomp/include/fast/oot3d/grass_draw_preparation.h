#pragma once

// The backend-neutral half of drawing grass: the shading environment of a
// placement, the projection a placement draws with, the LOD bins and their
// draw batches, and the CPU instance fallback. Used by the Vulkan pass
// (interactive_grass_pass.cpp) and readable by any other backend; nothing
// here touches a graphics API.

#include "fast/oot3d/grass_blade_geometry.h"
#include "fast/oot3d/grass_draw_state.h"
#include "fast/oot3d/grass_instance_layout.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_types.h"
#include "fast/oot3d/grass_world_placement_cache.h"
#include "fast/oot3d/graphics_settings.h"
#include "fast/renderer3ds/pica_scene_payloads.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Fast::Oot3d {

class GrassInteractionField;

// One bin per (segments, planes) pair, plus the distant tufts in the last.
inline constexpr size_t kGrassLodBinCount = static_cast<size_t>(kMaximumGrassBladeSegments) * 2U + 1U;
using GrassVisibleIndexBins = std::array<std::vector<uint32_t>, kGrassLodBinCount>;
using GrassSelectedClusterRanges = std::array<std::vector<std::array<uint32_t, 2>>, kGrassLodBinCount>;

[[nodiscard]] size_t GrassLodBinIndex(uint8_t bladeSegments, uint8_t planeCount);

// One instanced draw: a LOD bin of one placement, or a run of midrange
// cluster groups with the same draw capacity.
struct GrassDrawBatch {
    uint32_t FirstInstance = 0;
    uint32_t InstanceCount = 0;
    uint8_t BladeSegments = 1U;
    uint8_t PlaneCount = 1U;
    GrassPushConstants Push{};
    uint32_t PlacementIndex = 0;
    bool Grouped = false;
    uint32_t GroupDrawCapacity = 0;
};

// The per-placement record of the grass shaders: fog and native lights of the
// source draw, wind, appearance, LOD and the view basis.
[[nodiscard]] GrassEnvironmentRecord BuildGrassEnvironmentRecord(
    const GrassPicaFogState& fog, bool receiveFog, bool picaDepthAvailable,
    const InteractiveGrassSettings& settings, const GrassShadingEnvironment& shading,
    const std::optional<std::array<float, 3>>& textureAverage, double visualSeconds,
    const ::Fast::Renderer3ds::PicaPerspectiveCameraState& view, const std::array<float, 3>& viewForward,
    const std::array<float, 3>& viewSide, const std::array<float, 3>& viewUp);

[[nodiscard]] std::optional<std::array<float, 16>> InvertAffineRowMajor(const std::array<float, 16>& matrix);
[[nodiscard]] std::array<float, 16> MultiplyRowMajor(const std::array<float, 16>& left,
                                                     const std::array<float, 16>& right);

// World to the source draw's own PICA clip space (column-major), so the
// blades meet the native depth of this very frame (interpolated frames
// included): the draw's model-to-clip times the inverse model-to-world.
// Flags[0] is 1 then. Meshes without that transform (or with the transform
// baked into the vertices) draw with `fallbackWorldToClip` and Flags[0] 0.
[[nodiscard]] GrassPushConstants ProjectionForMesh(const GrassSceneMesh& mesh,
                                                   const std::array<float, 16>& fallbackWorldToClip);

// Appends one placement's selected bins to the visible anchor list and its
// draw batches. `sources` are the bins of each culling worker in worker
// order (one entry when culled on one thread). Grouped midrange clusters
// become runs of equal draw capacity; every other bin is one batch.
struct GrassBatchOutput {
    std::vector<uint32_t>& VisibleAnchorIndices;
    std::vector<std::array<uint32_t, 2>>& DrawGroups;
    std::vector<GrassDrawBatch>& Batches;
};
void AppendGrassPlacementBatches(std::span<const GrassVisibleIndexBins* const> sources,
                                 const GrassSelectedClusterRanges& selectedClusterRanges,
                                 const GrassWorldPlacement& world, const GrassPushConstants& push,
                                 uint32_t staticBaseIndex, uint32_t placementIndex, bool midrangeClusters,
                                 bool clusterOwned, GrassBatchOutput output);

// The CPU fallback of the GPU instance compaction: one GrassInstance per
// visible anchor, bent by the interaction field and the colliders, the bend
// clamped to the settings' maximum.
void WriteGrassCpuInstances(std::span<const GrassWorldAnchor> anchors, std::span<const uint32_t> visible,
                            const GrassInteractionField& field, std::span<const GrassInteractor> actors,
                            const InteractiveGrassSettings& settings, GrassInstance* output);

} // namespace Fast::Oot3d
