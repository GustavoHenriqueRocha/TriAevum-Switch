#pragma once

#include "fast/oot3d/gl_widescreen_policy.h"
#include "fast/oot3d/scene_presentation_policy.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

// Menus e textos 2D no widescreen do backend OpenGL (ver gl_widescreen_policy.h).
//
// Com o alvo da tela de cima alargado, o backend GL só punha num quadro 5:3
// centralizado (proporção nativa) os draws que o runtime atribui ao domínio
// `Ui` (HUD, caixas de texto, páginas de pausa dentro da cena). Fora do jogo
// em si (tela de título, seleção de arquivo, criação de nome, escolha
// normal/Master Quest) o domínio é `Unknown`/`Scene` e tudo esticava 16:9/5:3
// (~6,7% mais largo), inclusive o menu de baixo que o TopScreen realoca para a
// tela de cima. Esta política reconhece esses desenhos 2D pelo próprio estado
// PICA e os põe no mesmo quadro centralizado:
//
// * viewport que não cobre o eixo horizontal apresentado inteiro (o menu de
//   baixo realocado: Y 40..360 de 400) - nenhum desenho da cena usa isso;
// * projeção ortográfica (c0-c3, linha w = 0,0,0,1) com pixels quadrados no
//   mapeamento nativo, cuja geometria não cobre o viewport (um painel, um
//   texto, um logotipo; uma lavagem/fade de tela inteira continua esticada).
//
// Nunca entram: draws que amostram cópias do quadro renderizado (o redesenho
// tem de cair sobre os pixels copiados) e a camada de atmosfera (efeitos de
// tela do ambiente presos à cena, como o lens flare).
//
// Além disso, um draw em perspectiva posto no quadro 5:3 cujo frustum o hook
// de projeção alargou (anisotropia medida = 1/fator) seria espremido; nele o
// viewport é alargado pelo mesmo fator em torno do centro (modelos 3D dos
// menus: Link no Equipamento, itens girando), recortado ao quadro.
//
// Lido uma vez: no Switch de sdmc:/switch/oot3dre/config/widescreen_menus
// ("0" desliga; sem o arquivo, ligado); fora dele da variável
// TRIAEVUM_GL_WIDESCREEN_MENUS (sem ela, ligado). Só tem efeito com o
// widescreen ligado.
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

// Escalas x/y da projeção que os programas do OoT3D leem de c0-c3 (linhas,
// girada com o framebuffer: a linha 0 dá o clip x = eixo vertical apresentado,
// a linha 1 o clip y = eixo horizontal apresentado). Para não depender do
// sentido da rotação, a escala de cada linha é o módulo das colunas 0 e 1.
struct GlProjectionScales {
    GlProjectionShape Shape = GlProjectionShape::Unknown;
    // Unidades de clip (divididas por w na ortográfica) por unidade de view.
    float Vertical = 0.0F;   // linha 0
    float Horizontal = 0.0F; // linha 1
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

// Pixels apresentados por unidade de view na horizontal sobre os da vertical,
// no mapeamento nativo do viewport (1 = pixels quadrados, sem distorção).
// O alvo da tela de cima guarda o eixo vertical apresentado em X com
// `framebufferWidth / 240` amostras por linha (480 -> 2) e o horizontal em Y
// 1:1. 0 quando não dá para medir.
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

// O que o backend sabe de um draw no alvo alargado da tela de cima.
struct GlWidescreenDrawEvidence {
    // Domínio `Ui` atribuído pelo runtime (HUD, texto, pausa).
    bool UiDomain = false;
    // Camada de atmosfera (efeitos de tela do ambiente, presos à cena).
    bool Atmosphere = false;
    // O viewport cobre o alvo nativo inteiro.
    bool ViewportCoversTarget = false;
    // O viewport cobre o eixo horizontal apresentado inteiro (Y 0..altura).
    bool ViewportSpansPresentedWidth = false;
    // A geometria projetada cobre o viewport inteiro (fade, lavagem).
    bool GeometryCoversViewport = false;
    // Amostra alguma cópia do quadro renderizado.
    bool SamplesRenderedCopy = false;
    GlProjectionShape Projection = GlProjectionShape::Unknown;
    // GlNativePixelAnisotropy (0 = desconhecida).
    float PixelAnisotropy = 0.0F;
};

struct GlWidescreenDrawPlacement {
    // No quadro nativo centralizado (senão esticado sobre o alvo todo, como
    // a cena).
    bool Canvas = false;
    // No quadro, alargar o viewport pelo fator do widescreen em torno do seu
    // centro (perspectiva que o hook de projeção alargou).
    bool CompensateWidening = false;

    bool operator==(const GlWidescreenDrawPlacement&) const = default;
};

inline constexpr float kGlWidescreenAnisotropyTolerance = 0.03F;

[[nodiscard]] inline bool GlAnisotropyNear(float anisotropy, float expected) noexcept {
    return anisotropy > 0.0F && expected > 0.0F &&
           std::abs(anisotropy / expected - 1.0F) <= kGlWidescreenAnisotropyTolerance;
}

// Um draw fora do domínio `Ui` só precisa da prova de cobertura (cara: projeta
// os vértices) quando todo o resto já o poria no quadro.
[[nodiscard]] inline bool GlWidescreenOrthographicPanelCandidate(
    const GlWidescreenDrawEvidence& evidence) noexcept {
    return !evidence.UiDomain && !evidence.Atmosphere &&
           !evidence.SamplesRenderedCopy &&
           evidence.ViewportSpansPresentedWidth &&
           evidence.Projection == GlProjectionShape::Orthographic &&
           GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F);
}

[[nodiscard]] inline GlWidescreenDrawPlacement ResolveGlWidescreenDrawPlacement(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor,
    bool menusEnabled) noexcept {
    GlWidescreenDrawPlacement placement;
    if (evidence.UiDomain) {
        // Regra anterior: UI que cobre o alvo inteiro é conteúdo de quadro
        // (fade, redesenho da cópia) e estica; o resto fica no quadro.
        placement.Canvas =
            !(evidence.ViewportCoversTarget && evidence.GeometryCoversViewport);
    } else if (menusEnabled && !evidence.Atmosphere &&
               !evidence.SamplesRenderedCopy) {
        if (!evidence.ViewportSpansPresentedWidth) {
            placement.Canvas = true;
        } else {
            placement.Canvas = GlWidescreenOrthographicPanelCandidate(evidence) &&
                               !evidence.GeometryCoversViewport;
        }
    }
    placement.CompensateWidening =
        placement.Canvas && menusEnabled && widescreenFactor > 1.0F &&
        evidence.Projection == GlProjectionShape::Perspective &&
        GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F / widescreenFactor);
    return placement;
}

} // namespace Fast::Oot3d
