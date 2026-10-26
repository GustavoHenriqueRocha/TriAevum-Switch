#pragma once

// Grass sources from native PICA draws, for backends without the Vulkan
// renderer (deko3d on the Switch). A copy of the source path of
// ResolveNativePicaGrassSurface/PublishNativePicaGrassSurface
// (gfx_vulkan_pica_draw.cpp) without overlay masks or diagnostics: a
// configured opaque world draw becomes a GrassSceneMesh in GrassSceneBridge,
// where the placement builder finds it. The Vulkan copy is left untouched
// until the PC build can be checked with a shared version.

#include "fast/oot3d/grass_async_placement_builder.h"
#include "fast/oot3d/grass_draw_memo.h"
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

// The draw must be an opaque world draw of the scene (the caller checks the
// composition layer). Returns true when a mesh was published.
bool PublishPicaGrassSource(const Renderer3ds::PicaDrawView& draw, uint64_t frameId,
                            const InteractiveGrassSettings& settings, PicaGrassSourceState& state,
                            uint64_t sequenceId, uint64_t drawOrder);

// The placements of this frame's sources (the request side of
// InteractiveGrassPass::Prepare without lighting, shadows or overlays): one
// builder worker; a missing placement is built off the calling thread and
// shows up in a later frame.
struct PicaGrassPlacementStats {
    uint32_t Rules = 0;          // rules whose texture has been seen
    uint32_t Meshes = 0;         // source meshes matched this frame
    uint32_t Ready = 0;          // placements available
    uint32_t Queued = 0;         // placements being built
    uint64_t Anchors = 0;        // blades in the ready placements
    double BuildMilliseconds = 0.0; // worker time of the placements finished this frame
};

class PicaGrassPlacements {
  public:
    PicaGrassPlacements();
    // Placements of the meshes published for `frameId`.
    const std::vector<std::shared_ptr<const GrassWorldPlacement>>& Update(uint64_t frameId,
                                                                          const InteractiveGrassSettings& settings);
    [[nodiscard]] const PicaGrassPlacementStats& Stats() const noexcept { return mStats; }

  private:
    GrassAsyncPlacementBuilder mBuilder;
    std::vector<std::shared_ptr<const GrassWorldPlacement>> mReady;
    PicaGrassPlacementStats mStats;
};

} // namespace Fast::Oot3d
