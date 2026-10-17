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

GlWidescreenDrawEvidence RelocatedMenuDraw();

// Menu de baixo realocado no quadro, com os cantos de baixo nas bordas.
const GlWidescreenDrawPlacement kRelocatedCanvas{true, false, false, 1.0F, true};

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
              (kRelocatedCanvas));
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
              (kRelocatedCanvas));
    // Cópia do quadro: fica com a cena.
    menu.SamplesRenderedCopy = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(menu, kFactor169, true).Canvas);
}

TEST(GlWidescreenMenuPolicy, UiFadeOfTheRelocatedMenuCoversTheWholeTarget) {
    // Trace do host, transição seleção de arquivo -> jogo: o preto é um quad
    // do domínio Ui (dom=2, camada 4), sem textura, 4 vértices, com a
    // ortográfica 400x240 da tela de cima (c1 = -1/200) no viewport do menu
    // 0,40,480,320 (anisotropia 0,8). Antes: quadro, só x 160..1120.
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
    // No trace o recorte do jogo está desligado (modo 0); ligado, respeita-se.
    fade.GuestScissor = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(fade, kFactor169, true).FillTarget);
    // Desligado: regra anterior do domínio Ui (quadro).
    fade.GuestScissor = false;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(fade, kFactor169, false),
              (GlWidescreenDrawPlacement{true, false}));
}

TEST(GlWidescreenMenuPolicy, RelocatedMenuIgnoresTheAtmosphereLayer) {
    // Trace do host (seleção/escolha de modo com TopScreen): os draws do menu
    // realocado vêm com camada 3 (atmosfera), ortográficos 320x240
    // (c0 = 0,-1/120,0,1; c1 = -1/160,0,0,1), anisotropia 1. Antes iam
    // esticados: a aba BACK em x 128..310 de 1280, fora do canto do menu.
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
    // O fade dele também cobre a tela.
    back.Untextured = true;
    back.GeometryCoversViewport = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(back, kFactor169, true).FillTarget);
    // Na tela inteira (Y 0..400) a atmosfera continua presa à cena.
    auto flare = FullViewport();
    flare.Atmosphere = true;
    flare.Projection = GlProjectionShape::Orthographic;
    flare.PixelAnisotropy = 1.0F;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(flare, kFactor169, true).Canvas);
    // Desligado: como antes.
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(back, kFactor169, false).Canvas);
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

// Seleção de arquivo/escolha de modo com TopScreen: o menu de baixo realocado
// (viewport Y 40..360 de 400) com o fade/escurecimento de cor sólida.
GlWidescreenDrawEvidence RelocatedMenuDraw() {
    GlWidescreenDrawEvidence menu;
    menu.ViewportSpansPresentedWidth = false;
    return menu;
}

TEST(GlWidescreenMenuPolicy, RelocatedSolidFadeCoversTheWholeTarget) {
    // Fade de transição (seleção de arquivo -> jogo, escolha de modo ->
    // seleção): cor sólida sobre todo o menu. Antes ficava no 4:3 do menu com
    // o céu da cena nas laterais.
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
    // Em perspectiva alargada pelo hook, a borda nativa fica em 1/fator.
    fade.Projection = GlProjectionShape::Perspective;
    fade.PixelAnisotropy = 1.0F / kFactor169;
    EXPECT_FLOAT_EQ(GlWidescreenNativeHorizontalExtent(fade, kFactor169), 1.0F / kFactor169);
    placement = ResolveGlWidescreenDrawPlacement(fade, kFactor169, true);
    EXPECT_TRUE(placement.FillTarget);
    EXPECT_FLOAT_EQ(placement.FillHorizontalExtent, 1.0F / kFactor169);
}

TEST(GlWidescreenMenuPolicy, OtherRelocatedDrawsStayOnTheCanvas) {
    // Sem a prova de cobertura (um botão, uma barra): quadro.
    auto bar = RelocatedMenuDraw();
    bar.Untextured = true;
    EXPECT_TRUE(ResolveGlWidescreenDrawPlacement(bar, kFactor169, true).Canvas);
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(bar, kFactor169, true).FillTarget);
    // Com textura (uma imagem que cobre o menu): quadro, nunca esticada.
    auto picture = RelocatedMenuDraw();
    picture.GeometryCoversViewport = true;
    EXPECT_FALSE(GlWidescreenRelocatedFillCandidate(picture));
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(picture, kFactor169, true),
              (kRelocatedCanvas));
    // Recorte do próprio jogo: respeitado no quadro.
    auto clipped = RelocatedMenuDraw();
    clipped.Untextured = true;
    clipped.GeometryCoversViewport = true;
    clipped.GuestScissor = true;
    EXPECT_FALSE(ResolveGlWidescreenDrawPlacement(clipped, kFactor169, true).FillTarget);
    // Cópia do quadro segue com a cena.
    auto copy = RelocatedMenuDraw();
    copy.Untextured = true;
    copy.GeometryCoversViewport = true;
    copy.SamplesRenderedCopy = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(copy, kFactor169, true),
              (GlWidescreenDrawPlacement{false, false}));
    // Painéis do Game Over (domínio Ui no mesmo viewport 0,40,480,320, com
    // textura): no quadro, como antes.
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
    // Menus desligados: regra anterior (esticado).
    auto off = RelocatedMenuDraw();
    off.Untextured = true;
    off.GeometryCoversViewport = true;
    EXPECT_EQ(ResolveGlWidescreenDrawPlacement(off, kFactor169, false),
              (GlWidescreenDrawPlacement{false, false}));
}

// Console: escala 2, saída 1280x720 -> alvo 853 de altura (fator 16:9/5:3),
// quadro 5:3 de 800 com deslocamento 26. Na saída cada pixel vale 1280/853.
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
    // Painel ortográfico do menu: viewport nativo Y 40..360 no quadro.
    const auto axis = ResolveGlWidescreenPresentedAxis({true, false},
                                                       ConsoleAxis(40.0F, 320.0F));
    EXPECT_EQ(axis.ViewportBegin, 26 + 80);
    EXPECT_EQ(axis.ViewportEnd, 26 + 720);
    // Recorte: o quadro inteiro (26..826).
    EXPECT_EQ(axis.ScissorBegin, 26);
    EXPECT_EQ(axis.ScissorEnd, 826);
}

TEST(GlWidescreenMenuPolicy, CompensatedModelIsClippedAtItsNativeViewport) {
    // Modelo 3D de menu no viewport realocado: perspectiva alargada pelo hook,
    // viewport alargado em torno do centro (85..767). O recorte fica no
    // viewport nativo (antes: o quadro inteiro, 26..826), então o que no 3DS
    // está fora da tela de baixo continua fora.
    const auto axis = ResolveGlWidescreenPresentedAxis({true, true},
                                                       ConsoleAxis(40.0F, 320.0F));
    EXPECT_EQ(axis.ViewportBegin, 85);
    EXPECT_EQ(axis.ViewportEnd, 767);
    EXPECT_EQ(axis.ScissorBegin, 26 + 80);
    EXPECT_EQ(axis.ScissorEnd, 26 + 720);
    // Na saída 1280: 159..1119 (o retângulo 4:3 do menu).
    EXPECT_NEAR(axis.ScissorBegin * 1280.0 / 853.0, 160.0, 1.5);
    EXPECT_NEAR(axis.ScissorEnd * 1280.0 / 853.0, 1120.0, 1.5);
    // Proporções: largura do viewport / largura nativa do menu = fator.
    EXPECT_NEAR(static_cast<double>(axis.ViewportEnd - axis.ViewportBegin) / 640.0,
                kFactor169, 0.005);
    // Recorte do jogo mais estreito continua valendo.
    auto input = ConsoleAxis(40.0F, 320.0F);
    input.GuestScissor = true;
    input.ScissorY1 = 100;
    input.ScissorY2 = 199;
    const auto guest = ResolveGlWidescreenPresentedAxis({true, true}, input);
    EXPECT_EQ(guest.ScissorBegin, 26 + 200);
    EXPECT_EQ(guest.ScissorEnd, 26 + 400);
}

TEST(GlWidescreenMenuPolicy, RelocatedFadeFillsTheTarget) {
    // Ortográfico: a geometria chega a 1, o viewport é o alvo inteiro.
    GlWidescreenDrawPlacement fill;
    fill.FillTarget = true;
    auto axis = ResolveGlWidescreenPresentedAxis(fill, ConsoleAxis(40.0F, 320.0F));
    EXPECT_LE(axis.ViewportBegin, 0);
    EXPECT_GE(axis.ViewportEnd, 853);
    EXPECT_EQ(axis.ScissorBegin, 0);
    EXPECT_EQ(axis.ScissorEnd, 853);
    // Perspectiva alargada: a geometria chega a 1/fator; o viewport cresce o
    // fator para a borda dela cair fora do alvo.
    fill.FillHorizontalExtent = 1.0F / kFactor169;
    axis = ResolveGlWidescreenPresentedAxis(fill, ConsoleAxis(40.0F, 320.0F));
    const double centre = 853.0 / 2.0;
    const double half = (axis.ViewportEnd - axis.ViewportBegin) / 2.0;
    EXPECT_LE(centre - half / kFactor169, 0.0 + 0.5);
    EXPECT_GE(centre + half / kFactor169, 853.0 - 0.5);
    EXPECT_EQ(axis.ScissorBegin, 0);
    EXPECT_EQ(axis.ScissorEnd, 853);
}

// Quad com transformação tipada (projeção c0-c3, view c4-c6, model c20-c22),
// como o fixture de cobertura de oot3d_graphics_foundation_tests.
struct QuadDraw {
    std::vector<float> Positions;
    std::array<uint16_t, 4> Indices{0, 1, 2, 3};
    std::array<float, 96> Uniforms{};
    Fast::Renderer3ds::PicaVertexAttributeView Attribute{};
    Fast::Renderer3ds::PicaVertexBindingView Binding{};
    Fast::Renderer3ds::PicaDrawView View;

    // Ortográfica 400x240 -> NDC, com o eixo da linha 1 multiplicado por
    // `squeeze` (1/fator: frustum alargado pelo hook).
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
    // Fade do menu: o retângulo nativo inteiro (400x240 no espaço da view).
    const QuadDraw fade({0, 0, 0, 400, 0, 0, 0, 240, 0, 400, 240, 0}, 1.0F / kFactor169);
    const auto bounds = fade.Bounds();
    ASSERT_TRUE(bounds.has_value());
    // Com o frustum alargado não chega a 1 (a prova antiga recusava)...
    EXPECT_FALSE(bounds->Reaches({1.0F, 1.0F}));
    // ...mas cobre o viewport nativo, cuja borda está em 1/fator.
    EXPECT_TRUE(bounds->Reaches({1.0F, 1.0F / kFactor169}));
    // Um botão não cobre nada disso.
    const QuadDraw button({0, 0, 0, 60, 0, 0, 0, 40, 0, 60, 40, 0}, 1.0F / kFactor169);
    ASSERT_TRUE(button.Bounds().has_value());
    EXPECT_FALSE(button.Bounds()->Reaches({1.0F, 1.0F / kFactor169}));
    // Sem frustum alargado, a regra antiga (PicaDrawGeometryCoversViewport).
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
    // Menu de baixo realocado (fora do domínio Ui): sim.
    auto menu = RelocatedMenuDraw();
    menu.Atmosphere = true;
    menu.Projection = GlProjectionShape::Orthographic;
    menu.PixelAnisotropy = 1.0F;
    auto placement = ResolveGlWidescreenDrawPlacement(menu, kFactor169, true);
    EXPECT_TRUE(placement.Canvas);
    EXPECT_TRUE(placement.SplitCorners);
    // Sem alargar (fator 1), com menus desligados, no domínio Ui (Game Over),
    // compensado, fade de tela inteira ou na tela inteira: não.
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
    // Console: alvo 853 de altura, menu 106..746 (quadro em 26); X do alvo
    // 0..960 (240 linhas x 2 amostras x escala 2), linha de baixo em X = 0.
    // Canto: 64 px do menu (128 no alvo) por 40 linhas (160 no alvo).
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
    // Tudo acima da faixa de baixo, no lugar (o recorte do quadro).
    EXPECT_EQ(passes[0], (GlWidescreenPass{0, 160, 960, 26, 826}));
    // A faixa de baixo entre os cantos, no lugar.
    EXPECT_EQ(passes[1], (GlWidescreenPass{0, 0, 160, 106 + 128, 746 - 128}));
    // Canto do começo encostado em 0 (na saída 1280: x 0..192), canto do fim
    // em 853.
    EXPECT_EQ(passes[2], (GlWidescreenPass{-106, 0, 160, 0, 128}));
    EXPECT_EQ(passes[3], (GlWidescreenPass{853 - 746, 0, 160, 853 - 128, 853}));
    // Os pixels do menu saem uma vez só: as áreas somam a do menu no quadro.
    std::int64_t area = 0;
    for (const auto& pass : passes)
        area += std::int64_t{pass.ScissorX1 - pass.ScissorX0} * (pass.ScissorY1 - pass.ScissorY0);
    EXPECT_EQ(area, std::int64_t{960} * 800 - std::int64_t{160} * (106 - 26) -
                        std::int64_t{160} * (826 - 746));
    // Recorte do jogo que não toca a faixa de baixo: só o passe de cima.
    GlWidescreenCornerInput high = input;
    high.ScissorX0 = 400;
    EXPECT_EQ(ResolveGlWidescreenCornerPasses(axis, high, passes), 1U);
    EXPECT_EQ(passes[0], (GlWidescreenPass{0, 400, 960, 26, 826}));
}

TEST(GlWidescreenMenuPolicy, TouchInTheWindowCornerReachesTheMenuCorner) {
    // Janela 1280x720: quadro 5:3 em x 40..1240, menu em 160..1120; canto de
    // 64 px nativos = 192 px por 40 linhas = 120 px.
    // A aba BACK desenhada em x 0..170 (y 610..720) toca o BACK do jogo.
    auto pointer = RemapGlWidescreenCornerPointer(20.0F, 680.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 180.0F);
    EXPECT_FLOAT_EQ(pointer.Y, 680.0F);
    pointer = RemapGlWidescreenCornerPointer(170.0F, 700.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 330.0F);
    // Canto da direita, espelhado.
    pointer = RemapGlWidescreenCornerPointer(1270.0F, 650.0F, 1280.0F, 720.0F);
    EXPECT_FLOAT_EQ(pointer.X, 1110.0F);
    // Fora da faixa de baixo ou no meio: inalterado (cartões, botões).
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(20.0F, 590.0F, 1280.0F, 720.0F).X, 20.0F);
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(640.0F, 700.0F, 1280.0F, 720.0F).X, 640.0F);
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(400.0F, 700.0F, 1280.0F, 720.0F).X, 400.0F);
    // Janela 5:3 (sem margem): nada a remapear.
    EXPECT_FLOAT_EQ(RemapGlWidescreenCornerPointer(20.0F, 700.0F, 1200.0F, 720.0F).X, 20.0F);
}

TEST(GlWidescreenMenuPolicy, ParsesTheSwitch) {
    EXPECT_FALSE(ParseGlWidescreenSetting("0\n", true));
    EXPECT_TRUE(ParseGlWidescreenSetting("1", false));
    EXPECT_TRUE(ParseGlWidescreenSetting("", true));
}

} // namespace
} // namespace Fast::Oot3d
