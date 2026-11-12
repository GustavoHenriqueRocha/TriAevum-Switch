#pragma once

// The covering overlays of the Switch's grass sources (SWITCH_PORT.md, "Grass
// on deko3d: overlays"). deko3d asks for a frame's placements at its first
// grass draw point, before the draws after it (paths and decals drawn over a
// ground, often translucent) have been seen; the PC prepares every draw of the
// frame first. So the overlay set of each source is taken at the end of a real
// frame, when all of its draws have been published to GrassSceneBridge, and
// the next frame's placement requests use it.

#include "fast/oot3d/grass_async_placement_builder.h"
#include "fast/oot3d/grass_overlay_mask.h"
#include "fast/oot3d/grass_scene_bridge.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>

namespace Fast::Oot3d {

class GrassLateOverlays {
  public:
    struct Lookup {
        // False: wait (no request this frame). True with a null set: no overlay covers it.
        bool Ready = true;
        std::shared_ptr<const GrassOverlaySet> Set;
    };
    using OverlaysFor = std::function<std::shared_ptr<const GrassOverlaySet>(const GrassSceneMesh&)>;
    // At the end of `frame`: the overlay set of each source mesh drawn in it.
    void Collect(uint64_t frame, std::span<const GrassSceneMesh> sources, const OverlaysFor& overlaysFor);
    // A source's set for a placement request in `frame`: the last one
    // collected. A source never collected waits for one frame, so its first
    // placement is not built without its overlays and then again with them.
    [[nodiscard]] Lookup Find(uint64_t instanceId, uint64_t frame);
    void Clear();
    [[nodiscard]] size_t Size() const noexcept { return mSets.size(); }

  private:
    struct Entry {
        uint64_t Frame = 0;
        std::shared_ptr<const GrassOverlaySet> Set;
    };
    std::unordered_map<uint64_t, Entry> mSets;
    std::unordered_map<uint64_t, uint64_t> mWaiting; // instance -> the frame it first waited in
};

// The placement last shown for each ground (its world identity). When the
// overlays covering a ground change (a path the game stops drawing off
// screen), its placement key changes and the new one builds for a while: the
// ground keeps showing the last one meanwhile instead of blinking off (the
// PC's GrassStaticPlacementCache::mShown).
class GrassShownPlacements {
  public:
    // Everything of the request but its overlays: what must match to keep
    // showing the last placement.
    [[nodiscard]] static uint64_t Base(const GrassAsyncPlacementRequest& request);
    // A ready placement is remembered; a pending result of the same base
    // becomes the shown placement, ready.
    void Resolve(uint64_t world, uint64_t base, uint64_t frame, GrassAsyncPlacementResult& result);
    // Keeps the max(32, `keep`) most recently shown.
    void Prune(size_t keep);
    [[nodiscard]] size_t Size() const noexcept { return mShown.size(); }

  private:
    struct Shown {
        uint64_t Base = 0;
        std::shared_ptr<const GrassWorldPlacement> Placement;
        double BuildMilliseconds = 0.0;
        uint64_t LastFrame = 0;
    };
    std::unordered_map<uint64_t, Shown> mShown;
};

// Sets of sources not drawn for this many frames are dropped.
inline constexpr uint64_t kGrassLateOverlayKeepFrames = 600;

} // namespace Fast::Oot3d
