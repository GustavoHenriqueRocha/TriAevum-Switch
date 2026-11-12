#pragma once

// Grass sources from native PICA draws, for backends without the Vulkan
// renderer (deko3d on the Switch). A copy of
// ResolveNativePicaGrassSurface/PublishNativePicaGrassSurface
// (gfx_vulkan_pica_draw.cpp) without diagnostics: a configured opaque world
// draw becomes a GrassSceneMesh in GrassSceneBridge, where the placement
// builder finds it, and (masksOnly) a world draw over it becomes a covering
// overlay whose coverage removes the grass under it. The Vulkan copy is left untouched
// until the PC build can be checked with a shared version.

#include "fast/oot3d/grass_async_placement_builder.h"
#include "fast/oot3d/grass_draw_memo.h"
#include "fast/oot3d/grass_late_overlays.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_geometry_registry.h"
#include "fast/oot3d/graphics_settings.h"
#include "fast/renderer3ds/pica_render_backend.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <unordered_map>

namespace Fast::Oot3d {

struct PicaGrassSourceState {
    GrassGeometryRegistry Geometry;
    GrassDrawMemo Memo;
    // Occurrences of one draw identity per render target, this frame.
    std::unordered_map<uint64_t, uint32_t> Occurrences;
    uint64_t Frame = 0;
};

// A source: the draw must be an opaque world draw of the scene (the caller
// checks the composition layer). `masksOnly`: a world draw of the scene drawn
// after a source into its target, published as a covering overlay (never a
// configured ground). Returns true when a mesh was published.
bool PublishPicaGrassSource(const Renderer3ds::PicaDrawView& draw, uint64_t frameId,
                            const InteractiveGrassSettings& settings, PicaGrassSourceState& state,
                            uint64_t sequenceId, uint64_t drawOrder, bool masksOnly = false);

// The placements of this frame's sources (the request side of
// InteractiveGrassPass::Prepare without lighting or shadows; overlays one
// frame late, GrassLateOverlays): one
// builder worker; a missing placement is built off the calling thread and
// shows up in a later frame.
struct PicaGrassPlacementStats {
    uint32_t Rules = 0;          // rules whose texture has been seen
    uint32_t Meshes = 0;         // source meshes matched this frame
    uint32_t Ready = 0;          // placements available
    uint32_t Empty = 0;          // built with no tuft (mask, slope or size left nothing)
    uint32_t Queued = 0;         // placements being built (or waiting for their overlays)
    uint32_t WaitingOverlays = 0; // sources not requested: their overlays come at the end of this frame
    uint32_t Overlaid = 0;       // requests with covering overlays
    uint64_t Anchors = 0;        // tufts in the ready placements
    double BuildMilliseconds = 0.0; // worker time of the placements finished this frame
    size_t ResidentBytes = 0;    // placements kept by the builder (all frames)
};

// A ready placement and the source mesh of this frame it belongs to (its
// model-to-clip, fog, lights and render target).
struct PicaGrassDrawable {
    std::shared_ptr<const GrassWorldPlacement> Placement;
    GrassSceneMesh Mesh;
};

class PicaGrassPlacements {
  public:
    // `retainedBytes` caps the placements kept in memory; the least recently
    // drawn go first.
    explicit PicaGrassPlacements(size_t retainedBytes = 512U * 1024U * 1024U);
    // Placements of the meshes published for `frameId`.
    const std::vector<PicaGrassDrawable>& Update(uint64_t frameId, const InteractiveGrassSettings& settings);
    // At the end of a real frame (all of its draws published): the covering
    // overlays of its sources, for the next frame's requests.
    void CollectOverlays(uint64_t frameId, const InteractiveGrassSettings& settings);
    [[nodiscard]] const PicaGrassPlacementStats& Stats() const noexcept { return mStats; }
    // Built placements kept on disk and loaded instead of built again
    // (config/grass_placement_cache); the builder's totals (builds, loads).
    void SetCacheDirectory(std::filesystem::path directory, uint64_t byteLimit, size_t entryLimit) {
        mBuilder.SetCacheDirectory(std::move(directory), byteLimit, entryLimit);
    }
    // config/grass_placement_reuse (GrassPlacementDiskMatch).
    void SetDiskReuse(bool reuse) { mBuilder.SetDiskReuse(reuse); }
    [[nodiscard]] GrassAsyncPlacementStats BuilderStats() const { return mBuilder.Stats(); }
    // config/grass_hold: until the placements queued so far are made, at most `timeout`.
    [[nodiscard]] bool WaitForPending(std::chrono::microseconds timeout) { return mBuilder.WaitForPending(timeout); }

  private:
    GrassAsyncPlacementBuilder mBuilder;
    GrassLateOverlays mLateOverlays;
    GrassShownPlacements mShown;
    std::vector<PicaGrassDrawable> mReady;
    PicaGrassPlacementStats mStats;
};

} // namespace Fast::Oot3d
