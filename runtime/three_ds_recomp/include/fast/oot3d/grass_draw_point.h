#pragma once

// Where the deko3d grass goes in a frame (gfx_deko3d_pica_draw.cpp,
// gfx_deko3d_grass.cpp). Pure bookkeeping, unit-tested on the host
// (oot3d_grass_switch_tests, Oot3dGrassDrawPoint).
//
// The grass is drawn once per frame into the target that frame's opaque world
// was drawn to, after that frame's grass sources (its grounds) were drawn
// there, at the first draw point reached:
// - Translucent: before a translucent world draw of the scene;
// - Overlay: before a UI or screen draw into that target (the HUD, the
//   TopScreen minimap, the area title card, messages). They are drawn over the
//   finished scene without depth, so grass drawn after one covers it
//   (0.1.2-test.16: the Lost Woods title card and minimap under the blades);
// - Transfer: before the display transfer copies the target out.
// A point reached before the target's opaque world or before its sources of
// the frame draws nothing, and the next point is tried. Sky and atmosphere
// draws are no point: in 0.1.2-test.2 they came before the ground, and the
// frame's placements were matched while still empty.

#include "fast/renderer3ds/pica_composition.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Fast::Oot3d {

enum class GrassDrawPoint : uint8_t { Translucent, Overlay, Transfer };
inline constexpr size_t kGrassDrawPointKinds = 3U;

// Drawn over the finished scene in screen space: the frontend's UI (lists of
// the HUD phase, UI spans) and screen content. The domain is the one the
// frontend resolved: a late scene list (IsLateCompositionCommandList, test.10)
// is Scene; with config/late_lists_as_ui it is UI and its first draw is a point.
[[nodiscard]] constexpr bool GrassOverlayDraw(Renderer3ds::PicaCompositionDomain domain,
                                              Renderer3ds::PicaCompositionLayer layer) noexcept {
    return domain == Renderer3ds::PicaCompositionDomain::Ui || domain == Renderer3ds::PicaCompositionDomain::Screen ||
           layer == Renderer3ds::PicaCompositionLayer::Ui;
}

// The draw point a PICA draw is (reached before it is drawn), if any.
[[nodiscard]] constexpr std::optional<GrassDrawPoint> GrassDrawPointOf(Renderer3ds::PicaCompositionDomain domain,
                                                                       Renderer3ds::PicaCompositionLayer layer) noexcept {
    if (domain == Renderer3ds::PicaCompositionDomain::Scene &&
        layer == Renderer3ds::PicaCompositionLayer::TransparentWorld) {
        return GrassDrawPoint::Translucent;
    }
    if (GrassOverlayDraw(domain, layer)) return GrassDrawPoint::Overlay;
    return std::nullopt;
}

struct GrassDrawPointReach {
    bool Draw = false;         // draw the grass now
    bool AfterOverlay = false; // an overlay already went over this frame's opaque world in the target
};

// Per render thread: the target and frame of the last opaque world draw, the
// grass drawn, the overlay drawn. `Target` is the backend's target handle.
template <typename Target>
class GrassDrawPointGate {
  public:
    // An opaque world draw with depth test and write into `target`: the grass
    // of this frame goes there, with its viewport and depth test.
    void Scene(Target target, uint64_t frame) noexcept { mScene = {target, frame}; }

    // A draw point into `target`; `sources`: this frame's grass sources were
    // drawn into it. Draws once per target and frame, into the target of this
    // frame's opaque world. An overlay point marks the target after its
    // decision: a later point would draw over that overlay (`AfterOverlay`;
    // only when the overlay came before the sources).
    [[nodiscard]] GrassDrawPointReach Reach(GrassDrawPoint point, Target target, uint64_t frame,
                                            bool sources) noexcept {
        const Mark here{target, frame};
        GrassDrawPointReach reach;
        if (mScene == here && mDrawn != here && sources) {
            mDrawn = here;
            reach.Draw = true;
            reach.AfterOverlay = mOverlay == here;
        }
        if (point == GrassDrawPoint::Overlay && mScene == here) mOverlay = here;
        return reach;
    }

  private:
    struct Mark {
        Target Handle{};
        uint64_t Frame = ~0ULL;
        bool operator==(const Mark&) const = default;
    };
    Mark mScene;
    Mark mDrawn;
    Mark mOverlay;
};

// The window's draw points (deko3d.txt, third `grass:` line).
struct GrassDrawPointCounts {
    std::array<uint64_t, kGrassDrawPointKinds> Drawn{};
    uint64_t AfterOverlay = 0;
    // Grass sources drawn into the target after its grass this frame: those
    // grounds were not matched to placements in that frame (should be 0).
    uint64_t SourcesAfter = 0;

    void Count(GrassDrawPoint point, const GrassDrawPointReach& reach) noexcept {
        ++Drawn[static_cast<size_t>(point)];
        if (reach.AfterOverlay) ++AfterOverlay;
    }
    [[nodiscard]] uint64_t At(GrassDrawPoint point) const noexcept { return Drawn[static_cast<size_t>(point)]; }
};

} // namespace Fast::Oot3d
