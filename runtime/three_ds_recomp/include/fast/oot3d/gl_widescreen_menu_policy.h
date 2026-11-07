#pragma once

#include "fast/oot3d/gl_widescreen_policy.h"
#include "fast/oot3d/scene_presentation_policy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

// 2D menus and text in the OpenGL backend's widescreen (see gl_widescreen_policy.h).
//
// With the top-screen target widened, the GL backend only put into a centered 5:3
// frame (native aspect ratio) the draws the runtime assigns to the `Ui` domain
// (HUD, text boxes, pause pages inside the scene). Outside the game itself
// (title screen, file select, name entry, Normal/Master Quest choice) the domain
// is `Unknown`/`Scene` and everything stretched 16:9/5:3
// (~6.7% wider), including the bottom menu that TopScreen relocates to the
// top screen. This policy recognizes those 2D draws from the PICA state itself
// and puts them in the same centered frame:
//
// * a viewport that doesn't cover the whole presented horizontal axis (the
//   relocated bottom menu: Y 40..360 of 400) - no scene draw uses that;
// * an orthographic projection (c0-c3, w row = 0,0,0,1) with square pixels in the
//   native mapping, whose geometry doesn't cover the viewport (a panel, a
//   text, a logo; a full-screen wash/fade stays stretched).
//
// Never included: draws that sample copies of the rendered frame (the redraw
// has to land on the copied pixels) and the atmosphere layer (environment
// screen effects tied to the scene, such as the lens flare).
//
// Also, a perspective draw put into the 5:3 frame whose frustum the projection
// hook widened (measured anisotropy = 1/factor) would be squeezed; for it the
// viewport is widened by the same factor around the center (3D models in the
// menus: Link in Equipment, spinning items), clipped to the frame.
//
// Read once: on the Switch from sdmc:/switch/oot3dre/config/widescreen_menus
// ("0" turns it off; without the file, on); elsewhere from the
// TRIAEVUM_GL_WIDESCREEN_MENUS variable (without it, on). Only has an effect with
// widescreen on.
[[nodiscard]] inline bool GlWidescreenMenusEnabled() {
    static const bool enabled = [] {
        constexpr bool kDefault = true;
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/widescreen_menus");
        std::string text;
        if (!file || !std::getline(file, text))
            return kDefault;
        return ParseGlWidescreenSetting(text, kDefault);
#else
        const char* value = std::getenv("TRIAEVUM_GL_WIDESCREEN_MENUS");
        return value == nullptr ? kDefault : ParseGlWidescreenSetting(value, kDefault);
#endif
    }();
    return enabled;
}

enum class GlProjectionShape : std::uint8_t {
    Unknown = 0,
    Orthographic,
    Perspective,
};

// x/y scales of the projection that OoT3D programs read from c0-c3 (rows,
// rotated with the framebuffer: row 0 gives clip x = presented vertical axis,
// row 1 clip y = presented horizontal axis). To not depend on the
// rotation direction, each row's scale is the magnitude of columns 0 and 1.
struct GlProjectionScales {
    GlProjectionShape Shape = GlProjectionShape::Unknown;
    // Clip units (divided by w in orthographic) per view unit.
    float Vertical = 0.0F;   // row 0
    float Horizontal = 0.0F; // row 1
};

[[nodiscard]] inline GlProjectionScales DecodeGlProjection(
    const std::array<float, 16>& rows) noexcept {
    GlProjectionScales result;
    for (const float value : rows) {
        if (!std::isfinite(value))
            return result;
    }
    constexpr float kEpsilon = 1.0e-6F;
    const auto zero = [](float value) { return std::abs(value) <= kEpsilon; };
    if (!zero(rows[12]) || !zero(rows[13]))
        return result;
    float w = 1.0F;
    if (zero(rows[14]) && rows[15] > kEpsilon) {
        result.Shape = GlProjectionShape::Orthographic;
        w = rows[15];
    } else if (!zero(rows[14]) && zero(rows[15])) {
        result.Shape = GlProjectionShape::Perspective;
    } else {
        return result;
    }
    result.Vertical = std::hypot(rows[0], rows[1]) / w;
    result.Horizontal = std::hypot(rows[4], rows[5]) / w;
    if (!(result.Vertical > kEpsilon) || !(result.Horizontal > kEpsilon) ||
        !std::isfinite(result.Vertical) || !std::isfinite(result.Horizontal)) {
        return {};
    }
    return result;
}

// Presented pixels per view unit horizontally over those vertically,
// in the viewport's native mapping (1 = square pixels, no distortion).
// The top-screen target stores the presented vertical axis in X with
// `framebufferWidth / 240` samples per row (480 -> 2) and the horizontal one in Y
// 1:1. 0 when it can't be measured.
[[nodiscard]] inline float GlNativePixelAnisotropy(
    const GlProjectionScales& projection, float viewportWidth,
    float viewportHeight, std::uint32_t framebufferWidth) noexcept {
    if (projection.Shape == GlProjectionShape::Unknown || framebufferWidth == 0U ||
        !(viewportWidth > 0.0F) || !(viewportHeight > 0.0F))
        return 0.0F;
    const float density = static_cast<float>(framebufferWidth) /
                          static_cast<float>(kNativeTopScreenLogicalHeight);
    const float vertical = projection.Vertical * viewportWidth * 0.5F / density;
    const float horizontal = projection.Horizontal * viewportHeight * 0.5F;
    const float anisotropy = horizontal / vertical;
    return std::isfinite(anisotropy) && anisotropy > 0.0F ? anisotropy : 0.0F;
}

// What the backend knows about a draw in the widened top-screen target.
struct GlWidescreenDrawEvidence {
    // `Ui` domain assigned by the runtime (HUD, text, pause).
    bool UiDomain = false;
    // Atmosphere layer (environment screen effects, tied to the scene).
    bool Atmosphere = false;
    // The viewport covers the whole native target.
    bool ViewportCoversTarget = false;
    // The viewport covers the whole presented horizontal axis (Y 0..height).
    bool ViewportSpansPresentedWidth = false;
    // The projected geometry covers the whole viewport (fade, wash).
    bool GeometryCoversViewport = false;
    // Samples some copy of the rendered frame.
    bool SamplesRenderedCopy = false;
    GlProjectionShape Projection = GlProjectionShape::Unknown;
    // GlNativePixelAnisotropy (0 = unknown).
    float PixelAnisotropy = 0.0F;
    // Samples no texture at all (solid color: fade, dimming).
    bool Untextured = false;
    // The game's own scissor enabled.
    bool GuestScissor = false;
    // NDC bounds of the projected geometry (axis 0 = presented vertical,
    // axis 1 = presented horizontal; -1 = viewport start in the target).
    bool BoundsKnown = false;
    std::array<float, 2> NdcMin{};
    std::array<float, 2> NdcMax{};
};

struct GlWidescreenDrawPlacement {
    // In the centered native frame (otherwise stretched over the whole target, like
    // the scene).
    bool Canvas = false;
    // In the frame, widen the viewport by the widescreen factor around its
    // center (perspective the projection hook widened). The scissor stays at the
    // native viewport: what the 3DS doesn't show (outside the bottom screen) stays
    // outside.
    bool CompensateWidening = false;
    // Solid color covering the whole relocated menu (bottom-screen
    // fade/dimming): covers the whole widened target, with the scene underneath.
    bool FillTarget = false;
    // With FillTarget: how far the geometry reaches on the presented horizontal
    // axis (NDC; 1/factor when the hook widened the frustum).
    float FillHorizontalExtent = 1.0F;
    // Draw of the relocated bottom menu: the bottom corners (the BACK tab) go
    // to the corners of the widened screen (ResolveGlWidescreenCornerPasses).
    bool SplitCorners = false;

    bool operator==(const GlWidescreenDrawPlacement&) const = default;
};

inline constexpr float kGlWidescreenAnisotropyTolerance = 0.03F;

[[nodiscard]] inline bool GlAnisotropyNear(float anisotropy, float expected) noexcept {
    return anisotropy > 0.0F && expected > 0.0F &&
           std::abs(anisotropy / expected - 1.0F) <= kGlWidescreenAnisotropyTolerance;
}

// A draw outside the `Ui` domain only needs the coverage proof (expensive: projects
// the vertices) when everything else would already put it in the frame.
[[nodiscard]] inline bool GlWidescreenOrthographicPanelCandidate(
    const GlWidescreenDrawEvidence& evidence) noexcept {
    return !evidence.UiDomain && !evidence.Atmosphere &&
           !evidence.SamplesRenderedCopy &&
           evidence.ViewportSpansPresentedWidth &&
           evidence.Projection == GlProjectionShape::Orthographic &&
           GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F);
}

// The projection hook widened this perspective frustum by the widescreen
// factor (measured anisotropy = 1/factor).
[[nodiscard]] inline bool GlWidescreenPerspectiveWidened(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor) noexcept {
    return widescreenFactor > 1.0F &&
           evidence.Projection == GlProjectionShape::Perspective &&
           GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F / widescreenFactor);
}

// How far (in NDC, presented horizontal axis = clip row 1) a draw's geometry
// has to reach to cover the native viewport: the frustum widened
// by the hook puts the native edge at 1/factor.
[[nodiscard]] inline float GlWidescreenNativeHorizontalExtent(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor) noexcept {
    return GlWidescreenPerspectiveWidened(evidence, widescreenFactor)
               ? 1.0F / widescreenFactor
               : 1.0F;
}

// A relocated-menu draw (outside the Ui domain) that only needs the coverage
// proof to become a full-screen fade. The atmosphere layer doesn't count
// here: in the host trace the relocated-menu draws come assigned to it (the
// title-screen menu runs inside Gameplay_Draw's environment queue), and
// no scene environment effect uses the Y 40..360 viewport.
// The transition fade (file select -> game) comes in the Ui domain, without
// texture, with the top screen's 400x240 orthographic in the menu viewport
// (host trace); applies to both domains.
[[nodiscard]] inline bool GlWidescreenRelocatedFillCandidate(
    const GlWidescreenDrawEvidence& evidence) noexcept {
    return !evidence.SamplesRenderedCopy && !evidence.ViewportSpansPresentedWidth &&
           evidence.Untextured && !evidence.GuestScissor;
}

// Bottom corners of the relocated bottom menu (the BACK tab against the corner
// of the bottom screen). In a widescreen game they sit in the screen corners, not
// in the corners of the menu's 4:3. The tab comes in the same draw as the cards, the
// header and the texts (host trace: 1104 vertices, a single draw), so the
// draw is repeated by scissor regions: the middle in place, each corner shifted by the
// margin to the edge of the widened target.
// Read once: Switch, config/widescreen_menu_corners ("0" turns it off; without the
// file, on); elsewhere, TRIAEVUM_GL_WIDESCREEN_MENU_CORNERS.
[[nodiscard]] inline bool GlWidescreenMenuCornersEnabled() {
    static const bool enabled = [] {
        constexpr bool kDefault = true;
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/widescreen_menu_corners");
        std::string text;
        if (!file || !std::getline(file, text))
            return kDefault;
        return ParseGlWidescreenSetting(text, kDefault);
#else
        const char* value = std::getenv("TRIAEVUM_GL_WIDESCREEN_MENU_CORNERS");
        return value == nullptr ? kDefault : ParseGlWidescreenSetting(value, kDefault);
#endif
    }();
    return enabled;
}

// Size of each corner, in the native 320x240 menu: the BACK tab takes 57x37.
// The bottom cards and buttons end more than 43 rows above the edge and more
// than 70 px from the side.
inline constexpr std::int32_t kGlWidescreenCornerWidth = 64;  // presented px
inline constexpr std::int32_t kGlWidescreenCornerHeight = 40; // rows

[[nodiscard]] inline GlWidescreenDrawPlacement ResolveGlWidescreenDrawPlacement(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor,
    bool menusEnabled) noexcept {
    GlWidescreenDrawPlacement placement;
    if (menusEnabled && !evidence.ViewportSpansPresentedWidth &&
        GlWidescreenRelocatedFillCandidate(evidence) &&
        evidence.GeometryCoversViewport) {
        // Solid-color fade/dimming over the whole relocated bottom menu
        // (Y 40..360): on the 3DS it covers the whole bottom screen; here
        // it covers the whole screen, otherwise the scene (sky) shows at the sides of the 4:3.
        placement.FillTarget = true;
        placement.FillHorizontalExtent =
            GlWidescreenNativeHorizontalExtent(evidence, widescreenFactor);
        return placement;
    }
    if (evidence.UiDomain) {
        // Previous rule: UI covering the whole target is frame content
        // (fade, copy redraw) and stretches; the rest stays in the frame.
        placement.Canvas =
            !(evidence.ViewportCoversTarget && evidence.GeometryCoversViewport);
    } else if (menusEnabled && !evidence.SamplesRenderedCopy &&
               (!evidence.Atmosphere || !evidence.ViewportSpansPresentedWidth)) {
        if (!evidence.ViewportSpansPresentedWidth) {
            // The relocated bottom menu (Y 40..360), even when assigned to the atmosphere
            // layer (this used to stretch it: the BACK tab left the corner,
            // x=128 instead of 160 of 1280, and the card sliding in
            // appeared up to x=1152).
            placement.Canvas = true;
        } else {
            placement.Canvas = GlWidescreenOrthographicPanelCandidate(evidence) &&
                               !evidence.GeometryCoversViewport;
        }
    }
    placement.CompensateWidening =
        placement.Canvas && menusEnabled &&
        GlWidescreenPerspectiveWidened(evidence, widescreenFactor);
    placement.SplitCorners = placement.Canvas && menusEnabled &&
                             widescreenFactor > 1.0F && !evidence.UiDomain &&
                             !evidence.ViewportSpansPresentedWidth &&
                             !placement.CompensateWidening;
    return placement;
}

// Textures of the relocated bottom screen (file select, Normal/
// Master Quest choice, name entry) sampled with GL_NEAREST on magnification.
// The menu is assembled from atlas pieces drawn one texel per native pixel
// (orthographic 320x240, anisotropy 1): at the 3DS resolution each pixel lands at the
// center of a texel and the linear filter returns the exact texel. With internal
// resolution N×, the fragments at a piece's edge land 1/(2N) of a texel from the
// boundary and linear blends in the neighboring atlas texel: in the "Please
// select a file." header the seam between the corner and the middle became a light
// vertical line ~28 px from each edge (gone at scale 1, shows at 2 and 3).
// Nearest at an integer scale reproduces the native result pixel by pixel (the
// same texel in each of the N×N fragments) and doesn't read outside the piece; minification
// stays linear. A fractional scale (1.5×) stays linear: nearest would give
// texels of unequal widths.
// Read once: Switch, config/menu_nearest ("0" turns it off; without the file,
// on); elsewhere, TRIAEVUM_GL_MENU_NEAREST.
[[nodiscard]] inline bool GlMenuNearestEnabled() {
    static const bool enabled = [] {
        constexpr bool kDefault = true;
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/menu_nearest");
        std::string text;
        if (!file || !std::getline(file, text))
            return kDefault;
        return ParseGlWidescreenSetting(text, kDefault);
#else
        const char* value = std::getenv("TRIAEVUM_GL_MENU_NEAREST");
        return value == nullptr ? kDefault : ParseGlWidescreenSetting(value, kDefault);
#endif
    }();
    return enabled;
}

// A piece of the relocated bottom screen (TopScreen moves the game's lower
// screen into a viewport of the top one): a viewport that doesn't cover the
// presented horizontal axis, square-pixel orthographic, with its own texture.
// File select, name entry, the Normal/Master Quest choice and the Game Over
// panels; the file-select menu comes attributed to the title scene's
// atmosphere layer (Scene domain), the others in the Ui domain.
[[nodiscard]] inline bool GlRelocatedMenuDraw(const GlWidescreenDrawEvidence& evidence) noexcept {
    return !evidence.ViewportSpansPresentedWidth && !evidence.Untextured && !evidence.SamplesRenderedCopy &&
           evidence.Projection == GlProjectionShape::Orthographic &&
           GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F);
}

// The draw is a piece of the relocated bottom screen (GlRelocatedMenuDraw)
// and the internal scale is an integer >= 2. Applies in both domains: the
// name entry (keyboard and header, same seam) comes in the Ui domain, as do
// the Game Over panels and the centered pause pages; HUD, Items, Map and
// System cover the full width and stay linear.
[[nodiscard]] inline bool GlRelocatedMenuNearestSampling(
    const GlWidescreenDrawEvidence& evidence, float resolutionScale) noexcept {
    const bool integerScale = resolutionScale >= 2.0F - 1.0e-3F &&
                              std::abs(resolutionScale - std::round(resolutionScale)) <= 1.0e-3F;
    return integerScale && GlRelocatedMenuDraw(evidence);
}

// Presented horizontal axis (Y of the widened target, in target pixels) of a
// draw: viewport and scissor. The vertical axis (X) doesn't change with widescreen.
struct GlWidescreenPresentedAxisInput {
    float ViewportY = 0.0F;      // native
    float ViewportHeight = 0.0F; // native
    // Game scissor (ScissorMode 3): native Y1 and Y2, inclusive.
    bool GuestScissor = false;
    std::uint16_t ScissorY1 = 0;
    std::uint16_t ScissorY2 = 0;
    std::uint16_t FramebufferHeight = 0; // 400 on the top screen
    std::uint32_t TargetHeight = 0;      // widened target (pixels)
    float ResolutionScale = 1.0F;        // pixels per native pixel (frame)
    float TargetScaleY = 1.0F;           // pixels per native pixel (stretched)
};

struct GlWidescreenPresentedAxis {
    std::int32_t ViewportBegin = 0;
    std::int32_t ViewportEnd = 0;
    std::int32_t ScissorBegin = 0;
    std::int32_t ScissorEnd = 0;

    bool operator==(const GlWidescreenPresentedAxis&) const = default;
};

[[nodiscard]] inline GlWidescreenPresentedAxis ResolveGlWidescreenPresentedAxis(
    const GlWidescreenDrawPlacement& placement,
    const GlWidescreenPresentedAxisInput& input) noexcept {
    const bool canvas = placement.Canvas && !placement.FillTarget;
    const float scale = canvas ? input.ResolutionScale : input.TargetScaleY;
    const float offset =
        canvas ? std::floor((static_cast<float>(input.TargetHeight) -
                             static_cast<float>(input.FramebufferHeight) *
                                 input.ResolutionScale) *
                            0.5F)
               : 0.0F;
    const auto edge = [&](float native) {
        return static_cast<std::int32_t>(std::lround(offset + native * scale));
    };
    GlWidescreenPresentedAxis axis;
    axis.ViewportBegin = edge(input.ViewportY);
    axis.ViewportEnd = edge(input.ViewportY + input.ViewportHeight);
    if (placement.FillTarget) {
        // The geometry reaches FillHorizontalExtent of the viewport: that extent
        // goes to the target's whole presented width.
        const float centre = static_cast<float>(input.TargetHeight) * 0.5F;
        const float half = centre / std::max(placement.FillHorizontalExtent, 1.0e-3F);
        axis.ViewportBegin = static_cast<std::int32_t>(std::floor(centre - half));
        axis.ViewportEnd = static_cast<std::int32_t>(std::ceil(centre + half));
    } else if (placement.CompensateWidening) {
        // The hook widened the frustum by the target factor: the viewport grows by the
        // same amount around the center and the proportions return to native.
        const float centre =
            offset + (input.ViewportY + input.ViewportHeight * 0.5F) * scale;
        const float half = input.ViewportHeight * 0.5F * input.TargetScaleY;
        axis.ViewportBegin = static_cast<std::int32_t>(std::lround(centre - half));
        axis.ViewportEnd = static_cast<std::int32_t>(std::lround(centre + half));
    }
    axis.ScissorBegin = 0;
    axis.ScissorEnd = static_cast<std::int32_t>(input.TargetHeight);
    if (input.GuestScissor) {
        axis.ScissorBegin = edge(static_cast<float>(input.ScissorY1));
        axis.ScissorEnd = input.ScissorY2 >= input.ScissorY1
                              ? edge(static_cast<float>(input.ScissorY2) + 1.0F)
                              : axis.ScissorBegin;
    }
    if (canvas) {
        // What goes into the frame never goes past it.
        const auto canvasBegin = static_cast<std::int32_t>(offset);
        const auto canvasEnd = canvasBegin + static_cast<std::int32_t>(std::llround(
            static_cast<double>(input.FramebufferHeight) *
            static_cast<double>(input.ResolutionScale)));
        axis.ScissorBegin = std::max(axis.ScissorBegin, canvasBegin);
        axis.ScissorEnd = std::min(axis.ScissorEnd, canvasEnd);
    }
    if (placement.CompensateWidening) {
        // The widened viewport only restores the proportions; what was outside the
        // native viewport (off screen on the 3DS: a 3D menu model going past
        // the edge) stays cut there, as on the console.
        axis.ScissorBegin = std::max(axis.ScissorBegin, edge(input.ViewportY));
        axis.ScissorEnd =
            std::min(axis.ScissorEnd, edge(input.ViewportY + input.ViewportHeight));
    }
    axis.ScissorEnd = std::max(axis.ScissorEnd, axis.ScissorBegin);
    axis.ViewportEnd = std::max(axis.ViewportEnd, axis.ViewportBegin);
    return axis;
}

// One pass of a draw in the widened target: the presented-axis viewport
// shifted and the scissor (X = presented vertical axis, Y = horizontal).
struct GlWidescreenPass {
    std::int32_t ViewportShift = 0;
    std::int32_t ScissorX0 = 0;
    std::int32_t ScissorX1 = 0;
    std::int32_t ScissorY0 = 0;
    std::int32_t ScissorY1 = 0;

    bool operator==(const GlWidescreenPass&) const = default;
};

struct GlWidescreenCornerInput {
    // Scissor already resolved on the presented vertical axis (target X).
    std::int32_t ScissorX0 = 0;
    std::int32_t ScissorX1 = 0;
    // Viewport start in X (the bottom row of the screen) and how many target
    // pixels the kGlWidescreenCornerHeight bottom rows take.
    std::int32_t BottomX = 0;
    std::int32_t CornerHeight = 0;
    // Target pixels of the menu's kGlWidescreenCornerWidth columns.
    std::int32_t CornerWidth = 0;
    std::int32_t TargetHeight = 0;
};

// Up to 4 passes: the middle of the menu (everything above plus the bottom strip between the
// corners) in place, and the two bottom corners against the target's edges.
// Each menu pixel comes out in exactly one pass.
[[nodiscard]] inline std::size_t ResolveGlWidescreenCornerPasses(
    const GlWidescreenPresentedAxis& axis, const GlWidescreenCornerInput& input,
    std::array<GlWidescreenPass, 4>& passes) noexcept {
    std::size_t count = 0;
    const auto add = [&](std::int32_t shift, std::int32_t x0, std::int32_t x1,
                         std::int32_t y0, std::int32_t y1) {
        if (x1 <= x0 || y1 <= y0)
            return;
        passes[count++] = {shift, x0, x1, y0 + shift, y1 + shift};
    };
    const std::int32_t band = input.BottomX + input.CornerHeight;
    const std::int32_t begin = axis.ViewportBegin;
    const std::int32_t end = axis.ViewportEnd;
    const std::int32_t sy0 = axis.ScissorBegin;
    const std::int32_t sy1 = axis.ScissorEnd;
    const std::int32_t bandX0 = input.ScissorX0;
    const std::int32_t bandX1 = std::min(input.ScissorX1, band);
    add(0, std::max(input.ScissorX0, band), input.ScissorX1, sy0, sy1);
    add(0, bandX0, bandX1, std::max(sy0, begin + input.CornerWidth),
        std::min(sy1, end - input.CornerWidth));
    add(-begin, bandX0, bandX1, std::max(sy0, begin),
        std::min(sy1, begin + input.CornerWidth));
    add(input.TargetHeight - end, bandX0, bandX1,
        std::max(sy0, end - input.CornerWidth), std::min(sy1, end));
    return count;
}

// Touch/pointer with the shifted corners (same rule as drawing): a point
// in a bottom corner of the window, outside the menu, goes to the equivalent corner
// of the relocated menu, where the game expects it. `width`x`height` = window.
struct GlWidescreenPointer {
    float X = 0.0F;
    float Y = 0.0F;
};

[[nodiscard]] inline GlWidescreenPointer RemapGlWidescreenCornerPointer(
    float x, float y, float width, float height) noexcept {
    GlWidescreenPointer pointer{x, y};
    if (!(width > 0.0F) || !(height > 0.0F))
        return pointer;
    const float canvasWidth = std::min(width, height * kNativeTopScreenAspect);
    const float canvasX = (width - canvasWidth) * 0.5F;
    const float unit = canvasWidth / 400.0F; // native presented pixel
    const float menuBegin = canvasX + 40.0F * unit;
    const float menuEnd = canvasX + 360.0F * unit;
    const float cornerWidth = static_cast<float>(kGlWidescreenCornerWidth) * unit;
    const float cornerTop =
        height - static_cast<float>(kGlWidescreenCornerHeight) * height / 240.0F;
    // No margin beyond the 5:3 frame (5:3 window): nothing to shift.
    if (canvasX <= 0.5F || y < cornerTop)
        return pointer;
    if (x < cornerWidth)
        pointer.X = x + menuBegin;
    else if (x >= width - cornerWidth)
        pointer.X = x - (width - menuEnd);
    return pointer;
}

// Classification diagnostics (off without the file): with
// sdmc:/switch/oot3dre/config/widescreen_menus_log present (desktop: variable
// TRIAEVUM_GL_WIDESCREEN_MENUS_LOG with the output file path), the
// backend writes to logs/widescreen_menus.txt one line per distinct draw signature
// in the widened target (viewport, projection, coverage, domain, layer,
// texture, decision), up to a cap. Used to check on the console what landed in the
// 5:3 frame and what stretched (B button, fades, panels) without a PICA trace.
struct GlWidescreenDrawLogFacts {
    std::array<float, 4> Viewport{}; // X, Y, width, height (native)
    std::uint16_t FramebufferWidth = 0;
    std::uint16_t FramebufferHeight = 0;
    std::uint8_t ScissorMode = 0;
    std::array<std::uint16_t, 4> Scissor{}; // X1, Y1, X2, Y2
    std::uint8_t Domain = 0;
    std::uint8_t Layer = 0;
    std::uint32_t VertexCount = 0;
    std::uint64_t VertexShaderKey = 0;
    std::uint8_t TextureCount = 0;
    std::uint32_t Texture0Address = 0;
    std::uint8_t Texture0Format = 0;
    bool TypedTransform = false;
    bool RowsAvailable = false;
    std::array<float, 16> Rows{};
};

[[nodiscard]] inline std::uint64_t GlWidescreenDrawLogSignature(
    const GlWidescreenDrawLogFacts& facts, const GlWidescreenDrawEvidence& evidence,
    const GlWidescreenDrawPlacement& placement) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFU;
            hash *= 1099511628211ULL;
        }
    };
    for (const float value : facts.Viewport)
        mix(static_cast<std::uint64_t>(std::lround(value * 4.0F)));
    mix(facts.FramebufferWidth | (std::uint64_t{facts.FramebufferHeight} << 16U));
    mix(facts.ScissorMode);
    for (const auto value : facts.Scissor)
        mix(value);
    mix(facts.Domain | (std::uint64_t{facts.Layer} << 8U) |
        (std::uint64_t{facts.TextureCount} << 16U) |
        (std::uint64_t{facts.Texture0Format} << 24U));
    mix(facts.VertexCount);
    mix(facts.VertexShaderKey);
    mix(facts.Texture0Address);
    mix((evidence.UiDomain ? 1U : 0U) | (evidence.Atmosphere ? 2U : 0U) |
        (evidence.ViewportCoversTarget ? 4U : 0U) |
        (evidence.ViewportSpansPresentedWidth ? 8U : 0U) |
        (evidence.GeometryCoversViewport ? 16U : 0U) |
        (evidence.SamplesRenderedCopy ? 32U : 0U) |
        (placement.Canvas ? 64U : 0U) | (placement.CompensateWidening ? 128U : 0U) |
        (static_cast<std::uint64_t>(evidence.Projection) << 8U) |
        (evidence.Untextured ? 0x10000U : 0U) | (evidence.GuestScissor ? 0x20000U : 0U) |
        (placement.FillTarget ? 0x40000U : 0U) |
        (placement.SplitCorners ? 0x100000U : 0U));
    if (evidence.BoundsKnown) {
        for (std::size_t axis = 0; axis < 2U; ++axis) {
            mix(static_cast<std::uint64_t>(std::lround(evidence.NdcMin[axis] * 20.0F) + 1000));
            mix(static_cast<std::uint64_t>(std::lround(evidence.NdcMax[axis] * 20.0F) + 1000));
        }
    }
    mix(static_cast<std::uint64_t>(std::lround(evidence.PixelAnisotropy * 1000.0F)));
    return hash;
}

[[nodiscard]] inline std::string FormatGlWidescreenDrawLogLine(
    const GlWidescreenDrawLogFacts& facts, const GlWidescreenDrawEvidence& evidence,
    const GlWidescreenDrawPlacement& placement) {
    const char* shape = evidence.Projection == GlProjectionShape::Orthographic ? "ortho"
                        : evidence.Projection == GlProjectionShape::Perspective ? "persp"
                                                                                : "?";
    char text[640];
    int length = std::snprintf(
        text, sizeof(text),
        "%s%s vp=%.0f,%.0f,%.0f,%.0f fb=%ux%u sc=%u:%u,%u,%u,%u dom=%u cam=%u "
        "proj=%s aniso=%.4f covers_vp=%d vp_width=%d vp_target=%d copy=%d "
        "atm=%d typed=%d no_tex=%d vtx=%u vs=%016llx tex=%u:%08x/%u",
        placement.FillTarget ? "SCREEN" : placement.Canvas ? "FRAME" : "STRETCH",
        placement.CompensateWidening ? "+COMP" : "",
        static_cast<double>(facts.Viewport[0]), static_cast<double>(facts.Viewport[1]),
        static_cast<double>(facts.Viewport[2]), static_cast<double>(facts.Viewport[3]),
        static_cast<unsigned>(facts.FramebufferWidth),
        static_cast<unsigned>(facts.FramebufferHeight),
        static_cast<unsigned>(facts.ScissorMode), static_cast<unsigned>(facts.Scissor[0]),
        static_cast<unsigned>(facts.Scissor[1]), static_cast<unsigned>(facts.Scissor[2]),
        static_cast<unsigned>(facts.Scissor[3]), static_cast<unsigned>(facts.Domain),
        static_cast<unsigned>(facts.Layer), shape,
        static_cast<double>(evidence.PixelAnisotropy),
        evidence.GeometryCoversViewport ? 1 : 0,
        evidence.ViewportSpansPresentedWidth ? 1 : 0,
        evidence.ViewportCoversTarget ? 1 : 0, evidence.SamplesRenderedCopy ? 1 : 0,
        evidence.Atmosphere ? 1 : 0, facts.TypedTransform ? 1 : 0,
        evidence.Untextured ? 1 : 0, static_cast<unsigned>(facts.VertexCount),
        static_cast<unsigned long long>(facts.VertexShaderKey),
        static_cast<unsigned>(facts.TextureCount),
        static_cast<unsigned>(facts.Texture0Address),
        static_cast<unsigned>(facts.Texture0Format));
    std::string line(text, length > 0 ? std::min<std::size_t>(static_cast<std::size_t>(length),
                                                               sizeof(text) - 1U)
                                       : 0U);
    if (placement.SplitCorners)
        line.insert(0, "CORNERS ");
    if (evidence.BoundsKnown) {
        length = std::snprintf(text, sizeof(text), " ndc=v%.3f..%.3f,h%.3f..%.3f",
                               static_cast<double>(evidence.NdcMin[0]),
                               static_cast<double>(evidence.NdcMax[0]),
                               static_cast<double>(evidence.NdcMin[1]),
                               static_cast<double>(evidence.NdcMax[1]));
        if (length > 0)
            line.append(text, static_cast<std::size_t>(length));
    }
    if (facts.RowsAvailable) {
        line += " c0-c3=";
        for (std::size_t index = 0; index < facts.Rows.size(); ++index) {
            length = std::snprintf(text, sizeof(text), "%s%.6g", index == 0U ? "" : ",",
                                   static_cast<double>(facts.Rows[index]));
            if (length > 0)
                line.append(text, static_cast<std::size_t>(length));
        }
    }
    return line;
}

// Diagnostic output file, or empty if off. Read once.
[[nodiscard]] inline const std::string& GlWidescreenMenusLogPath() {
    static const std::string path = []() -> std::string {
#if defined(__SWITCH__)
        std::ifstream file("sdmc:/switch/oot3dre/config/widescreen_menus_log");
        if (!file)
            return {};
        std::string text;
        std::getline(file, text);
        if (!ParseGlWidescreenSetting(text, true))
            return {};
        return "sdmc:/switch/oot3dre/logs/widescreen_menus.txt";
#else
        const char* value = std::getenv("TRIAEVUM_GL_WIDESCREEN_MENUS_LOG");
        return value == nullptr ? std::string{} : std::string(value);
#endif
    }();
    return path;
}

} // namespace Fast::Oot3d
