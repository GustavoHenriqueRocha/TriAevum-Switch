#include "fast/oot3d/gl_widescreen_menu_policy.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>

namespace Fast::Oot3d {
namespace {

constexpr float kFactor169 = (16.0F / 9.0F) / (5.0F / 3.0F); // 1.0667

// Ortográfica girada com o framebuffer, como Mtx4x4_BuildOrthographicProjectionRotated:
// clip x a partir do y de view (altura `height`), clip y a partir do x (largura `width`).
std::array<float, 16> RotatedOrtho(float width, float height) {
    return {0.0F, 2.0F / height, 0.0F, 0.0F,
            -2.0F / width, 0.0F, 0.0F, 0.0F,
            0.0F, 0.0F, 0.5F, -0.5F,
            0.0F, 0.0F, 0.0F, 1.0F};
}

// Perspectiva girada: `aspect` = largura/altura do frustum, `widen` = quanto o
// hook de projeção alargou o eixo horizontal.
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
    // Cena: frustum 5:3 no viewport 480x400 (eixo vertical com 2 amostras/linha).
    const auto scene = DecodeGlProjection(RotatedPerspective(5.0F / 3.0F));
    EXPECT_NEAR(GlNativePixelAnisotropy(scene, 480.0F, 400.0F, 480U), 1.0F, 1.0e-4F);
    // O hook alargou o frustum: 1/fator.
    const auto wide = DecodeGlProjection(RotatedPerspective(5.0F / 3.0F, kFactor169));
    EXPECT_NEAR(GlNativePixelAnisotropy(wide, 480.0F, 400.0F, 480U), 1.0F / kFactor169, 1.0e-4F);
    // UI ortográfica 400x240 na tela inteira e o menu de baixo realocado
    // (320x240 no viewport Y 40..360).
    EXPECT_NEAR(GlNativePixelAnisotropy(DecodeGlProjection(RotatedOrtho(400.0F, 240.0F)),
                                        480.0F, 400.0F, 480U), 1.0F, 1.0e-4F);
    EXPECT_NEAR(GlNativePixelAnisotropy(DecodeGlProjection(RotatedOrtho(320.0F, 240.0F)),
                                        480.0F, 320.0F, 480U), 1.0F, 1.0e-4F);
    // Ortográfica -1..1 nos dois eixos (lavagem de tela): não é quadrada.
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

TEST(GlWidescreenMenuPolicy, KeepsFormerRulesWhenDisabled) {
    auto ui = FullViewport();
    ui.UiDomain = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(ui, kFactor169, false),
              (GlWidescreenDrawPlacement{true, false}));
    ui.GeometryCoversViewport = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(ui, kFactor169, false),
              (GlWidescreenDrawPlacement{false, false}));
    // Fora do domínio Ui nada muda, nem o menu realocado nem um painel.
    auto relocated = FullViewport();
    relocated.ViewportCoversTarget = false;
    relocated.ViewportSpansPresentedWidth = false;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(relocated, kFactor169, false).Canvas);
    auto panel = FullViewport();
    panel.Projection = GlProjectionShape::Orthographic;
    panel.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, false).Canvas);
    // Nem a compensação da perspectiva na UI.
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
    // Sem projeção reconhecível, também.
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(FullViewport(), kFactor169, true).Canvas);
    // Fade/lavagem ortográfica que cobre a tela: estica.
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
    // Pixels não quadrados (desenho de tela inteira intencional): não.
    panel.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(panel, kFactor169, true).Canvas);
    // Redesenho de cópia do quadro e atmosfera ficam com a cena.
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
              (GlWidescreenDrawPlacement{true, false}));
    // Mesmo um fundo que cobre o próprio viewport do menu.
    menu.GeometryCoversViewport = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true).Canvas);
    // Um modelo 3D do menu cujo frustum o hook alargou é compensado.
    menu.Projection = GlProjectionShape::Perspective;
    menu.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true),
              (GlWidescreenDrawPlacement{true, true}));
    // Perspectiva não alargada: só o quadro.
    menu.PixelAnisotropy = 1.0F;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
    // Cópia do quadro: fica com a cena.
    menu.SamplesRenderedCopy = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true).Canvas);
}

TEST(GlWidescreenMenuPolicy, CompensatesWidenedPerspectiveUi) {
    auto model = FullViewport();
    model.UiDomain = true;
    model.Projection = GlProjectionShape::Perspective;
    model.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, kFactor169, true),
              (GlWidescreenDrawPlacement{true, true}));
    // Sem alargar (fator 1) não há o que compensar.
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, 1.0F, true),
              (GlWidescreenDrawPlacement{true, false}));
    // UI ortográfica: só o quadro.
    model.Projection = GlProjectionShape::Orthographic;
    model.PixelAnisotropy = 1.0F;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(model, kFactor169, true),
              (GlWidescreenDrawPlacement{true, false}));
}

TEST(GlWidescreenMenuPolicy, ParsesTheSwitch) {
    EXPECT_FALSE(ParseGlWidescreenSetting("0\n", true));
    EXPECT_TRUE(ParseGlWidescreenSetting("1", false));
    EXPECT_TRUE(ParseGlWidescreenSetting("", true));
}

} // namespace
} // namespace Fast::Oot3d
