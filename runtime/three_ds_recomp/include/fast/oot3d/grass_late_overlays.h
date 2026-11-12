#pragma once

// The covering overlays of the Switch's grass sources (SWITCH_PORT.md, "Grass
// on deko3d: overlays"). deko3d asks for a frame's placements at its first
// grass draw point, before the draws after it (paths and decals drawn over a
// ground, often translucent) have been seen; the PC prepares every draw of the
// frame first. So the overlay set of each source is taken at the end of a real
// frame, when all of its draws have been published to GrassSceneBridge, and
// the next frame's placement requests use it.

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

// Sets of sources not drawn for this many frames are dropped.
inline constexpr uint64_t kGrassLateOverlayKeepFrames = 600;

} // namespace Fast::Oot3d
