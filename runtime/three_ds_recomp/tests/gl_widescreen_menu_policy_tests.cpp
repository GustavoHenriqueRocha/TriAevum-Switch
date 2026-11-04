#include "fast/oot3d/gl_widescreen_menu_policy.h"
#include "fast/renderer3ds/pica_draw_coverage.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Fast::Oot3d {
namespace {

constexpr float kFactor169 = (16.0F / 9.0F) / (5.0F / 3.0F); // 1.0667

// Orthographic rotated with the framebuffer, like Mtx4x4_BuildOrthographicProjectionRotated:
// clip x from view y (height `height`), clip y from x (width `width`).
std::array<float, 16> RotatedOrtho(float width, float height) {
    return {0.0F, 2.0F / height, 0.0F, 0.0F,
            -2.0F / width, 0.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 0.5F, -0.5F,
            0.0F, 0.0F, 0.0F, 1.0F};
}

// Rotated perspective: `aspect` = frustum width/height, `widen` = how much the
// projection hook widened the horizontal axis.
std::array<float, 16> RotatedPerspective(float aspect, float widen = 1.0F) {
    const float fy = 1.0F / std::tan(0.5F);
    const float fx = fy / aspect / widen;
    return {0.0F, fy, 0.0F, 0.0F,
            -fx, 0.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 0.5F, -2.0F,
            0.0F, 0.0F, -1.0F, 0.0F};
}

TEST(GlWidescreenMenuPolicy, DecodesProjectionShapes) {
    const auto ortho = DecodeGlProjection(RotatedOrtho(400.0F, 240.0F));
    EXPECT_EQ(ortho.Shape, GlProjectionShape::Orthographic);
    EXPECT_FLOAT_EQ(ortho.Vertical, 2.0F / 240.0F);
    EXPECT_FLOAT_EQ(ortho.Horizontal, 2.0F / 400.0F);
    EXPECT_EQ(DecodeGlProjection(RotatedPerspective(5.0F / 3.0F)).Shape,
              GlProjectionShape::Perspective);
    std::array<float, 16> garbage{};
    EXPECT_EQ(DecodeGlProjection(garbage).Shape, GlProjectionShape::Unknown);
    garbage = RotatedOrtho(400.0F, 240.0F);
    garbage[2] = NAN;
    EXPECT_EQ(DecodeGlProjection(garbage).Shape, GlProjectionShape::Unknown);
}

TEST(GlWidescreenMenuPolicy, NativeMappingIsSquareForSceneAndUi) {
    // Scene: 5:3 frustum in the 480x400 viewport (vertical axis with 2 samples/row).
    const auto scene = DecodeGlProjection(RotatedPerspective(5.0F / 3.0F));
    EXPECT_NEAR(GlNativePixelAnisotropy(scene, 480.0F, 400.0F, 480U), 1.0F, 1.0e-4F);
    // The hook widened the frustum: 1/factor.
    const auto wide = DecodeGlProjection(RotatedPerspective(5.0F / 3.0F, kFactor169));
    EXPECT_NEAR(GlNativePixelAnisotropy(wide, 480.0F, 400.0F, 480U), 1.0F / kFactor169, 1.0e-4F);
    // Orthographic 400x240 UI over the whole screen and the relocated bottom menu
    // (320x240 in the Y 40..360 viewport).
    EXPECT_NEAR(GlNativePixelAnisotropy(DecodeGlProjection(RotatedOrtho(400.0F, 240.0F)),
                                        480.0F, 400.0F, 480U), 1.0F, 1.0e-4F);
    EXPECT_NEAR(GlNativePixelAnisotropy(DecodeGlProjection(RotatedOrtho(320.0F, 240.0F)),
                                        480.0F, 320.0F, 480U), 1.0F, 1.0e-4F);
    // Orthographic -1..1 on both axes (screen wash): not square.
    EXPECT_FALSE(GlAnisotropyNear(
        GlNativePixelAnisotropy(DecodeGlProjection(RotatedOrtho(2.0F, 2.0F)), 480.0F, 400.0F, 480U),
        1.0F));
}

GlWidescreenDrawEvidence FullViewport() {
    GlWidescreenDrawEvidence evidence;
    evidence.ViewportCoversTarget = true;
    evidence.ViewportSpansPresentedWidth = true;
    return evidence;
}

GlWidescreenDrawEvidence RelocatedMenuDraw();

// Relocated bottom menu in the frame, with the bottom corners at the edges.
const GlWidescreenDrawPlacement kRelocatedCanvas{true, false, false, 1.0F, true};

TEST(GlWidescreenMenuPolicy, KeepsFormerRulesWhenDisabled) {
    auto ui = FullViewport();
    ui.UiDomain = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(ui, kFactor169, false),
              (GlWidescreenDrawPlacement{true, false}));
    ui.GeometryCoversViewport = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(ui, kFactor169, false),
              (GlWidescreenDrawPlacement{false, false}));
    // Outside the Ui domain nothing changes, neither the relocated menu nor a panel.
    auto relocated = FullViewport();
    relocated.ViewportCoversTarget = false;
    relocated.ViewportSpansPresentedWidth = false;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(relocated, kFactor169, false).Canvas);
    auto panel = FullViewport();
    panel.Projection = GlProjectionShape::Orthographic;
    panel.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, false).Canvas);
    // Nor the perspective compensation in the UI.
    auto model = FullViewport();
    model.UiDomain = true;
    model.Projection = GlProjectionShape::Perspective;
    model.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, kFactor169, false),
              (GlWidescreenDrawPlacement{true, false}));
}

TEST(GlWidescreenMenuPolicy, SceneStaysStretched) {
    auto scene = FullViewport();
    scene.Projection = GlProjectionShape::Perspective;
    scene.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(scene, kFactor169, true),
              (GlWidescreenDrawPlacement{false, false}));
    // Without a recognizable projection, too.
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(FullViewport(), kFactor169, true).Canvas);
    // Orthographic fade/wash covering the screen: stretches.
    auto wash = FullViewport();
    wash.Projection = GlProjectionShape::Orthographic;
    wash.PixelAnisotropy = 1.0F;
    wash.GeometryCoversViewport = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(wash, kFactor169, true).Canvas);
}

TEST(GlWidescreenMenuPolicy, OrthographicPanelsGoToTheCanvas) {
    auto panel = FullViewport();
    panel.Projection = GlProjectionShape::Orthographic;
    panel.PixelAnisotropy = 1.01F;
    EXPECT_TRUE(GlWidescreenOrthographicPanelCandidate(panel));
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
    // Non-square pixels (intentional full-screen drawing): no.
    panel.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true).Canvas);
    // Frame-copy redraw and atmosphere stay with the scene.
    panel.PixelAnisotropy = 1.0F;
    panel.SamplesRenderedCopy = true;
    EXPECT_FALSE(GlWidescreenOrthographicPanelCandidate(panel));
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true).Canvas);
    panel.SamplesRenderedCopy = false;
    panel.Atmosphere = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true).Canvas);
}

TEST(GlWidescreenMenuPolicy, RelocatedLowerMenuGoesToTheCanvas) {
    GlWidescreenDrawEvidence menu;
    menu.ViewportSpansPresentedWidth = false; // Y 40..360
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true),
              (kRelocatedCanvas));
    // Even a background covering the menu's own viewport.
    menu.GeometryCoversViewport = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true).Canvas);
    // A 3D menu model whose frustum the hook widened is compensated.
    menu.Projection = GlProjectionShape::Perspective;
    menu.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true),
              (GlWidescreenDrawPlacement{true, true}));
    // Non-widened perspective: just the frame.
    menu.PixelAnisotropy = 1.0F;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true),
              (kRelocatedCanvas));
    // Frame copy: stays with the scene.
    menu.SamplesRenderedCopy = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true).Canvas);
}

TEST(GlWidescreenMenuPolicy, UiFadeOfTheRelocatedMenuCoversTheWholeTarget) {
    // Host trace, file select -> game transition: the black is a quad
    // in the Ui domain (dom=2, layer 4), untextured, 4 vertices, with the
    // top screen's 400x240 orthographic (c1 = -1/200) in the menu viewport
    // 0,40,480,320 (anisotropy 0.8). Before: frame, only x 160..1120.
    auto fade = RelocatedMenuDraw();
    fade.UiDomain = true;
    fade.Untextured = true;
    fade.Projection = GlProjectionShape::Orthographic;
    fade.PixelAnisotropy = GlNativePixelAnisotropy(
        DecodeGlProjection({0.0F, -1.0F / 120.0F, 0.0F, 1.0F, -0.005F, 0.0F, 0.0F, 1.0F,
                            0.0F, 0.0F, 1.0F / 9.0F, 1.0F / 9.0F, 0.0F, 0.0F, 0.0F, 1.0F}),
        480.0F, 320.0F, 480U);
    EXPECT_NEAR(fade.PixelAnisotropy, 0.8F, 1.0e-4F);
    EXPECT_TRUE(GlWidescreenRelocatedFillCandidate(fade));
    fade.GeometryCoversViewport = true;
    const auto placement = ResolveGlWidescreenDrawPlacement(fade, kFactor169, true);
    EXPECT_TRUE(placement.FillTarget);
    EXPECT_FLOAT_EQ(placement.FillHorizontalExtent, 1.0F);
    // In the trace the game scissor is off (mode 0); when on, it is respected.
    fade.GuestScissor = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(fade, kFactor169, true).FillTarget);
    // Off: previous Ui-domain rule (frame).
    fade.GuestScissor = false;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(fade, kFactor169, false),
              (GlWidescreenDrawPlacement{true, false}));
}

TEST(GlWidescreenMenuPolicy, RelocatedMenuIgnoresTheAtmosphereLayer) {
    // Host trace (select/mode choice with TopScreen): the relocated menu's
    // draws come with layer 3 (atmosphere), orthographic 320x240
    // (c0 = 0,-1/120,0,1; c1 = -1/160,0,0,1), anisotropy 1. Before they were
    // stretched: the BACK tab at x 128..310 of 1280, outside the menu corner.
    auto back = RelocatedMenuDraw();
    back.Atmosphere = true;
    back.Projection = GlProjectionShape::Orthographic;
    back.PixelAnisotropy = GlNativePixelAnisotropy(
        DecodeGlProjection({0.0F, -1.0F / 120.0F, 0.0F, 1.0F, -0.00625F, 0.0F, 0.0F, 1.0F,
                            0.0F, 0.0F, 0.5F, -0.5F, 0.0F, 0.0F, 0.0F, 1.0F}),
        480.0F, 320.0F, 480U);
    EXPECT_NEAR(back.PixelAnisotropy, 1.0F, 1.0e-4F);
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(back, kFactor169, true),
              (kRelocatedCanvas));
    // Its fade also covers the screen.
    back.Untextured = true;
    back.GeometryCoversViewport = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(back, kFactor169, true).FillTarget);
    // Over the whole screen (Y 0..400) the atmosphere stays tied to the scene.
    auto flare = FullViewport();
    flare.Atmosphere = true;
    flare.Projection = GlProjectionShape::Orthographic;
    flare.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(flare, kFactor169, true).Canvas);
    // Off: as before.
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(back, kFactor169, false).Canvas);
}

TEST(GlWidescreenMenuPolicy, CompensatesWidenedPerspectiveUi) {
    auto model = FullViewport();
    model.UiDomain = true;
    model.Projection = GlProjectionShape::Perspective;
    model.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, kFactor169, true),
              (GlWidescreenDrawPlacement{true, true}));
    // Without widening (factor 1) there's nothing to compensate.
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, 1.0F, true),
              (GlWidescreenDrawPlacement{true, false}));
    // Orthographic UI: just the frame.
    model.Projection = GlProjectionShape::Orthographic;
    model.PixelAnisotropy = 1.0F;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
}

// File select/mode choice with TopScreen: the relocated bottom menu
// (viewport Y 40..360 of 400) with the solid-color fade/dimming.
GlWidescreenDrawEvidence RelocatedMenuDraw() {
    GlWidescreenDrawEvidence menu;
    menu.ViewportSpansPresentedWidth = false;
    return menu;
}

TEST(GlWidescreenMenuPolicy, RelocatedSolidFadeCoversTheWholeTarget) {
    // Transition fade (file select -> game, mode choice ->
    // select): solid color over the whole menu. Before it stayed in the menu's 4:3 with
    // the scene's sky at the sides.
    auto fade = RelocatedMenuDraw();
    fade.Untextured = true;
    fade.Projection = GlProjectionShape::Orthographic;
    fade.PixelAnisotropy = 1.0F;
    EXPECT_TRUE(GlWidescreenRelocatedFillCandidate(fade));
    fade.GeometryCoversViewport = true;
    auto placement = ResolveGlWidescreenDrawPlacement(fade, kFactor169, true);
    EXPECT_TRUE(placement.FillTarget);
    EXPECT_FALSE(placement.Canvas);
    EXPECT_FALSE(placement.CompensateWidening);
    EXPECT_FLOAT_EQ(placement.FillHorizontalExtent, 1.0F);
    // In perspective widened by the hook, the native edge is at 1/factor.
    fade.Projection = GlProjectionShape::Perspective;
    fade.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_FLOAT_EQ(GlWidescreenNativeHorizontalExtent(fade, kFactor169), 1.0F / kFactor169);
    placement = ResolveGlWidescreenDrawPlacement(fade, kFactor169, true);
    EXPECT_TRUE(placement.FillTarget);
    EXPECT_FLOAT_EQ(placement.FillHorizontalExtent, 1.0F / kFactor169);
}

TEST(GlWidescreenMenuPolicy, OtherRelocatedDrawsStayOnTheCanvas) {
    // Without the coverage proof (a button, a bar): frame.
    auto bar = RelocatedMenuDraw();
    bar.Untextured = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(bar, kFactor169, true).Canvas);
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(bar, kFactor169, true).FillTarget);
    // Textured (an image covering the menu): frame, never stretched.
    auto picture = RelocatedMenuDraw();
    picture.GeometryCoversViewport = true;
    EXPECT_FALSE(GlWidescreenRelocatedFillCandidate(picture));
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(picture, kFactor169, true),
              (kRelocatedCanvas));
    // The game's own scissor: respected in the frame.
    auto clipped = RelocatedMenuDraw();
    clipped.Untextured = true;
    clipped.GeometryCoversViewport = true;
    clipped.GuestScissor = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(clipped, kFactor169, true).FillTarget);
    // Frame copy stays with the scene.
    auto copy = RelocatedMenuDraw();
    copy.Untextured = true;
    copy.GeometryCoversViewport = true;
    copy.SamplesRenderedCopy = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(copy, kFactor169, true),
              (GlWidescreenDrawPlacement{false, false}));
    // Game Over panels (Ui domain in the same 0,40,480,320 viewport, with
    // texture): in the frame, as before.
    auto gameOver = RelocatedMenuDraw();
    gameOver.UiDomain = true;
    gameOver.GeometryCoversViewport = true;
    EXPECT_FALSE(GlWidescreenRelocatedFillCandidate(gameOver));
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(gameOver, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
    gameOver.GeometryCoversViewport = false;
    gameOver.Untextured = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(gameOver, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
    // Menus off: previous rule (stretched).
    auto off = RelocatedMenuDraw();
    off.Untextured = true;
    off.GeometryCoversViewport = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(off, kFactor169, false),
              (GlWidescreenDrawPlacement{false, false}));
}

// Console: scale 2, 1280x720 output -> target 853 high (factor 16:9/5:3),
// 5:3 frame of 800 with offset 26. In the output each pixel is worth 1280/853.
GlWidescreenPresentedAxisInput ConsoleAxis(float viewportY, float viewportHeight) {
    GlWidescreenPresentedAxisInput input;
    input.ViewportY = viewportY;
    input.ViewportHeight = viewportHeight;
    input.FramebufferHeight = 400U;
    input.TargetHeight = 853U;
    input.ResolutionScale = 2.0F;
    input.TargetScaleY = 853.0F / 400.0F;
    return input;
}

TEST(GlWidescreenMenuPolicy, SceneStretchesOverTheWholeTarget) {
    const auto axis = ResolveGlWidescreenPresentedAxis({}, ConsoleAxis(0.0F, 400.0F));
    EXPECT_EQ(axis, (GlWidescreenPresentedAxis{0, 853, 0, 853}));
}

TEST(GlWidescreenMenuPolicy, RelocatedMenuSitsInTheCanvas) {
    // Orthographic menu panel: native viewport Y 40..360 in the frame.
    const auto axis = ResolveGlWidescreenPresentedAxis({true, false},
                                                       ConsoleAxis(40.0F, 320.0F));
    EXPECT_EQ(axis.ViewportBegin, 26 + 80);
    EXPECT_EQ(axis.ViewportEnd, 26 + 720);
    // Scissor: the whole frame (26..826).
    EXPECT_EQ(axis.ScissorBegin, 26);
    EXPECT_EQ(axis.ScissorEnd, 826);
}

TEST(GlWidescreenMenuPolicy, CompensatedModelIsClippedAtItsNativeViewport) {
    // 3D menu model in the relocated viewport: perspective widened by the hook,
    // viewport widened around the center (85..767). The scissor stays at the
    // native viewport (before: the whole frame, 26..826), so what on the 3DS
    // is outside the bottom screen stays outside.
    const auto axis = ResolveGlWidescreenPresentedAxis({true, true},
                                                       ConsoleAxis(40.0F, 320.0F));
    EXPECT_EQ(axis.ViewportBegin, 85);
    EXPECT_EQ(axis.ViewportEnd, 767);
    EXPECT_EQ(axis.ScissorBegin, 26 + 80);
    EXPECT_EQ(axis.ScissorEnd, 26 + 720);
    // At 1280 output: 159..1119 (the menu's 4:3 rectangle).
    EXPECT_NEAR(axis.ScissorBegin * 1280.0 / 853.0, 160.0, 1.5);
    EXPECT_NEAR(axis.ScissorEnd * 1280.0 / 853.0, 1120.0, 1.5);
    // Proportions: viewport width / native menu width = factor.
    EXPECT_NEAR(static_cast<double>(axis.ViewportEnd - axis.ViewportBegin) / 640.0,
                kFactor169, 0.005);
    // A narrower game scissor still applies.
    auto input = ConsoleAxis(40.0F, 320.0F);
    input.GuestScissor = true;
    input.ScissorY1 = 100;
    input.ScissorY2 = 199;
    const auto guest = ResolveGlWidescreenPresentedAxis({true, true}, input);
    EXPECT_EQ(guest.ScissorBegin, 26 + 200);
    EXPECT_EQ(guest.ScissorEnd, 26 + 400);
}

TEST(GlWidescreenMenuPolicy, RelocatedFadeFillsTheTarget) {
    // Orthographic: the geometry reaches 1, the viewport is the whole target.
    GlWidescreenDrawPlacement fill;
    fill.FillTarget = true;
    auto axis = ResolveGlWidescreenPresentedAxis(fill, ConsoleAxis(40.0F, 320.0F));
    EXPECT_LE(axis.ViewportBegin, 0);
    EXPECT_GE(axis.ViewportEnd, 853);
    EXPECT_EQ(axis.ScissorBegin, 0);
    EXPECT_EQ(axis.ScissorEnd, 853);
    // Widened perspective: the geometry reaches 1/factor; the viewport grows by the
    // factor so its edge lands outside the target.
    fill.FillHorizontalExtent = 1.0F / kFactor169;
    axis = ResolveGlWidescreenPresentedAxis(fill, ConsoleAxis(40.0F, 320.0F));
    const double centre = 853.0 / 2.0;
    const double half = (axis.ViewportEnd - axis.ViewportBegin) / 2.0;
    EXPECT_LE(centre - half / kFactor169, 0.0 + 0.5);
    EXPECT_GE(centre + half / kFactor169, 853.0 - 0.5);
    EXPECT_EQ(axis.ScissorBegin, 0);
    EXPECT_EQ(axis.ScissorEnd, 853);
}

// Quad with typed transform (projection c0-c3, view c4-c6, model c20-c22),
// like the coverage fixture in oot3d_graphics_foundation_tests.
struct QuadDraw {
    std::vector<float> Positions;
    std::array<uint16_t, 4> Indices{0, 1, 2, 3};
    std::array<float, 96> Uniforms{};
    Fast::Renderer3ds::PicaVertexAttributeView Attribute{};
    Fast::Renderer3ds::PicaVertexBindingView Binding{};
    Fast::Renderer3ds::PicaDrawView View;

    // Orthographic 400x240 -> NDC, with the row-1 axis multiplied by
    // `squeeze` (1/factor: frustum widened by the hook).
    QuadDraw(std::vector<float> positions, float squeeze) : Positions(std::move(positions)) {
        const auto row = [this](size_t uniform, std::array<float, 4> values) {
            std::copy(values.begin(), values.end(), Uniforms.begin() + uniform * 4U);
        };
        row(0, {2.0F / 400.0F, 0.0F, 0.0F, -1.0F});
        row(1, {0.0F, 2.0F / 240.0F * squeeze, 0.0F, -squeeze});
        row(2, {0.0F, 0.0F, 1.0F, 0.0F});
        row(3, {0.0F, 0.0F, 0.0F, 1.0F});
        row(4, {1.0F, 0.0F, 0.0F, 0.0F});
        row(5, {0.0F, 1.0F, 0.0F, 0.0F});
        row(6, {0.0F, 0.0F, 1.0F, 0.0F});
        row(20, {1.0F, 0.0F, 0.0F, 0.0F});
        row(21, {0.0F, 1.0F, 0.0F, 0.0F});
        row(22, {0.0F, 0.0F, 1.0F, 0.0F});
        auto& transform = View.TemporalVertexProgram.Hooks.Transform;
        transform.Operation =
            Fast::Renderer3ds::PicaVertexTransformOperation::ModelViewProjection3x4;
        transform.ProjectionFirstUniform = 0U;
        transform.ProjectionRowCount = 4U;
        transform.ViewFirstUniform = 4U;
        transform.ViewRowCount = 3U;
        transform.ModelFirstUniform = 20U;
        transform.ModelRowCount = 3U;
        transform.PositionInputRegister = 0U;
        transform.NormalInputRegister = 1U;
        Attribute.Location = 0U;
        Attribute.Binding = 0U;
        Attribute.Format = Fast::Renderer3ds::PicaVertexFormat::Float;
        Attribute.ComponentCount = 3U;
        Binding.Binding = 0U;
        Binding.ByteStride = 3U * sizeof(float);
        Binding.Bytes = {reinterpret_cast<const uint8_t*>(Positions.data()),
                         Positions.size() * sizeof(float)};
        View.VertexAttributes = {&Attribute, 1U};
        View.VertexBindings = {&Binding, 1U};
        View.IndexBytes = {reinterpret_cast<const uint8_t*>(Indices.data()),
                           Indices.size() * sizeof(uint16_t)};
        View.Indexed = true;
        View.IndicesAre16Bit = true;
        View.VertexCount = 4U;
    }

    std::optional<Fast::Renderer3ds::PicaDrawNdcBounds> Bounds() const {
        return Fast::Renderer3ds::PicaDrawGeometryNdcBounds(
            View, 0U,
            {reinterpret_cast<const uint8_t*>(Uniforms.data()),
             Uniforms.size() * sizeof(float)});
    }
};

TEST(GlWidescreenMenuPolicy, CoverageProofFollowsTheWidenedFrustum) {
    // Menu fade: the whole native rectangle (400x240 in view space).
    const QuadDraw fade({0, 0, 0, 400, 0, 0, 0, 240, 0, 400, 240, 0}, 1.0F / kFactor169);
    const auto bounds = fade.Bounds();
    ASSERT_TRUE(bounds.has_value());
    // With the widened frustum it doesn't reach 1 (the old proof refused it)...
    EXPECT_FALSE(bounds->Reaches({1.0F, 1.0F}));
    // ...but it covers the native viewport, whose edge is at 1/factor.
    EXPECT_TRUE(bounds->Reaches({1.0F, 1.0F / kFactor169}));
    // A button covers none of that.
    const QuadDraw button({0, 0, 0, 60, 0, 0, 0, 40, 0, 60, 40, 0}, 1.0F / kFactor169);
    ASSERT_TRUE(button.Bounds().has_value());
    EXPECT_FALSE(button.Bounds()->Reaches({1.0F, 1.0F / kFactor169}));
    // Without a widened frustum, the old rule (PicaDrawGeometryCoversViewport).
    const QuadDraw native({0, 0, 0, 400, 0, 0, 0, 240, 0, 400, 240, 0}, 1.0F);
    EXPECT_TRUE(native.Bounds()->Reaches({1.0F, 1.0F}));
    EXPECT_TRUE(Fast::Renderer3ds::PicaDrawGeometryCoversViewport(
        native.View, 0U,
        {reinterpret_cast<const uint8_t*>(native.Uniforms.data()),
         native.Uniforms.size() * sizeof(float)}));
}

TEST(GlWidescreenMenuPolicy, DiagnosticLineNamesTheDecision) {
    GlWidescreenDrawLogFacts facts;
    facts.Viewport = {0.0F, 40.0F, 480.0F, 320.0F};
    facts.FramebufferWidth = 480U;
    facts.FramebufferHeight = 400U;
    facts.VertexCount = 4U;
    auto fade = RelocatedMenuDraw();
    fade.Untextured = true;
    fade.GeometryCoversViewport = true;
    const auto fill = ResolveGlWidescreenDrawPlacement(fade, kFactor169, true);
    const auto line = FormatGlWidescreenDrawLogLine(facts, fade, fill);
    EXPECT_EQ(line.rfind("TELA vp=0,40,480,320", 0), 0U) << line;
    EXPECT_NE(line.find("sem_tex=1"), std::string::npos) << line;
    auto card = RelocatedMenuDraw();
    card.Projection = GlProjectionShape::Perspective;
    card.PixelAnisotropy = 1.0F / kFactor169;
    const auto placed = ResolveGlWidescreenDrawPlacement(card, kFactor169, true);
    EXPECT_EQ(FormatGlWidescreenDrawLogLine(facts, card, placed).rfind("QUADRO+COMP", 0), 0U);
    EXPECT_NE(GlWidescreenDrawLogSignature(facts, fade, fill),
              GlWidescreenDrawLogSignature(facts, card, placed));
}

TEST(GlWidescreenMenuPolicy, OnlyRelocatedMenuDrawsSplitTheirCorners) {
    // Relocated bottom menu (outside the Ui domain): yes.
    auto menu = RelocatedMenuDraw();
    menu.Atmosphere = true;
    menu.Projection = GlProjectionShape::Orthographic;
    menu.PixelAnisotropy = 1.0F;
    auto placement = ResolveGlWidescreenDrawPlacement(menu, kFactor169, true);
    EXPECT_TRUE(placement.Canvas);
    EXPECT_TRUE(placement.SplitCorners);
    // Without widening (factor 1), with menus off, in the Ui domain (Game Over),
    // compensated, full-screen fade or over the whole screen: no.
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(menu, 1.0F, true).SplitCorners);
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, false).SplitCorners);
    auto gameOver = menu;
    gameOver.UiDomain = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(gameOver, kFactor169, true).SplitCorners);
    auto model = RelocatedMenuDraw();
    model.Projection = GlProjectionShape::Perspective;
    model.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(model, kFactor169, true).SplitCorners);
    auto fade = RelocatedMenuDraw();
    fade.Untextured = true;
    fade.GeometryCoversViewport = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(fade, kFactor169, true).SplitCorners);
    auto panel = FullViewport();
    panel.Projection = GlProjectionShape::Orthographic;
    panel.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true).SplitCorners);
}

TEST(GlWidescreenMenuPolicy, BottomCornersGoToTheWidenedTargetEdges) {
    // Console: target 853 high, menu 106..746 (frame at 26); target X
    // 0..960 (240 rows x 2 samples x scale 2), bottom row at X = 0.
    // Corner: 64 menu px (128 in the target) by 40 rows (160 in the target).
    const auto axis = ResolveGlWidescreenPresentedAxis({true, false},
                                                       ConsoleAxis(40.0F, 320.0F));
    GlWidescreenCornerInput input;
    input.ScissorX0 = 0;
    input.ScissorX1 = 960;
    input.BottomX = 0;
    input.CornerHeight = 160;
    input.CornerWidth = 128;
    input.TargetHeight = 853;
    std::array<GlWidescreenPass, 4> passes{};
    ASSERT_EQ(ResolveGlWidescreenCornerPasses(axis, input, passes), 4U);
    // Everything above the bottom strip, in place (the frame scissor).
    EXPECT_EQ(passes[0], (GlWidescreenPass{0, 160, 960, 26, 826}));
    // The bottom strip between the corners, in place.
    EXPECT_EQ(passes[1], (GlWidescreenPass{0, 0, 160, 106 + 128, 746 - 128}));
    // Start corner against 0 (at 1280 output: x 0..192), end corner
    // at 853.
    EXPECT_EQ(passes[2], (GlWidescreenPass{-106, 0, 160, 0, 128}));
    EXPECT_EQ(passes[3], (GlWidescreenPass{853 - 746, 0, 160, 853 - 128, 853}));
    // Menu pixels come out only once: the areas add up to the menu's in the frame.
    std::int64_t area = 0;
    for (const auto& pass : passes)
        area += std::int64_t{pass.ScissorX1 - pass.ScissorX0} * (pass.ScissorY1 - pass.ScissorY0);
    EXPECT_EQ(area, std::int64_t{960} * 800 - std::int64_t{160} * (106 - 26) -
                        std::int64_t{160} * (826 - 746));
    // Game scissor not touching the bottom strip: only the top pass.
    GlWidescreenCornerInput high = input;
    high.ScissorX0 = 400;
    EXPECT_EQ(ResolveGlWidescreenCornerPasses(axis, high, passes), 1U);
    EXPECT_EQ(passes[0], (GlWidescreenPass{0, 400, 960, 26, 826}));
}

TEST(GlWidescreenMenuPolicy, TouchInTheWindowCornerReachesTheMenuCorner) {
    // 1280x720 window: 5:3 frame at x 40..1240, menu at 160..1120; corner of
    // 64 native px = 192 px by 40 rows = 120 px.
    // The BACK tab drawn at x 0..170 (y 610..720) hits the game's BACK.
    auto pointer = RemapGlWidescreenCornerPointer(20.0F, 680.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 180.0F);
    EXPECT_FLOAT_EQ(pointer.Y, 680.0F);
    pointer = RemapGlWidescreenCornerPointer(170.0F, 700.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 330.0F);
    // Right corner, mirrored.
    pointer = RemapGlWidescreenCornerPointer(1270.0F, 650.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 1110.0F);
    // Outside the bottom strip or in the middle: unchanged (cards, buttons).
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(20.0F, 590.0F, 1280.0F, 720.0F).X, 20.0F);
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(640.0F, 700.0F, 1280.0F, 720.0F).X, 640.0F);
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(400.0F, 700.0F, 1280.0F, 720.0F).X, 400.0F);
    // 5:3 window (no margin): nothing to remap.
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(20.0F, 700.0F, 1200.0F, 720.0F).X, 20.0F);
}

TEST(GlWidescreenMenuPolicy, RelocatedMenuPiecesSampleTheirTexelsAtIntegerScales) {
    // Atlas piece of the relocated menu: orthographic 320x240 in the Y 40..360 viewport.
    GlWidescreenDrawEvidence piece = RelocatedMenuDraw();
    piece.Projection = GlProjectionShape::Orthographic;
    piece.PixelAnisotropy = GlNativePixelAnisotropy(
        DecodeGlProjection(RotatedOrtho(320.0F, 240.0F)), 480.0F, 320.0F, 480U);
    EXPECT_TRUE(GlRelocatedMenuNearestSampling(piece, 2.0F));
    EXPECT_TRUE(GlRelocatedMenuNearestSampling(piece, 3.0F));
    // Scale 1: linear already lands at the texel center. Fractional: unequal
    // texels with nearest; both stay linear.
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(piece, 1.0F));
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(piece, 1.5F));
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(piece, 2.5F));

    // Scene and full-screen panels stay linear.
    GlWidescreenDrawEvidence scene = FullViewport();
    scene.Projection = GlProjectionShape::Perspective;
    scene.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(scene, 2.0F));
    GlWidescreenDrawEvidence panel = FullViewport();
    panel.Projection = GlProjectionShape::Orthographic;
    panel.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(panel, 2.0F));
    // Ui domain in the same viewport (name entry, Game Over): also.
    GlWidescreenDrawEvidence ui = piece;
    ui.UiDomain = true;
    EXPECT_TRUE(GlRelocatedMenuNearestSampling(ui, 2.0F));
    // Full-screen HUD: linear.
    GlWidescreenDrawEvidence hud = panel;
    hud.UiDomain = true;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(hud, 2.0F));
    // Perspective model in the menu, frame copy and solid color (fade):
    // linear.
    GlWidescreenDrawEvidence model = piece;
    model.Projection = GlProjectionShape::Perspective;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(model, 2.0F));
    GlWidescreenDrawEvidence stretched = piece;
    stretched.PixelAnisotropy = 1.25F;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(stretched, 2.0F));
    GlWidescreenDrawEvidence copy = piece;
    copy.SamplesRenderedCopy = true;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(copy, 2.0F));
    GlWidescreenDrawEvidence fade = piece;
    fade.Untextured = true;
    EXPECT_FALSE(GlRelocatedMenuNearestSampling(fade, 2.0F));
}

TEST(GlWidescreenMenuPolicy, ParsesTheSwitch) {
    EXPECT_FALSE(ParseGlWidescreenSetting("0\n", true));
    EXPECT_TRUE(ParseGlWidescreenSetting("1", false));
    EXPECT_TRUE(ParseGlWidescreenSetting("", true));
}

} // namespace
} // namespace Fast::Oot3d
