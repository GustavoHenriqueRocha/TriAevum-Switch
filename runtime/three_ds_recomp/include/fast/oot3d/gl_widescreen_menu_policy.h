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
    // Não amostra textura nenhuma (cor sólida: fade, escurecimento).
    bool Untextured = false;
    // Recorte (scissor) do próprio jogo ligado.
    bool GuestScissor = false;
};

struct GlWidescreenDrawPlacement {
    // No quadro nativo centralizado (senão esticado sobre o alvo todo, como
    // a cena).
    bool Canvas = false;
    // No quadro, alargar o viewport pelo fator do widescreen em torno do seu
    // centro (perspectiva que o hook de projeção alargou). O recorte fica no
    // viewport nativo: o que o 3DS não mostra (fora da tela de baixo) continua
    // fora.
    bool CompensateWidening = false;
    // Cor sólida que cobre todo o menu realocado (fade/escurecimento da tela
    // de baixo): cobre o alvo alargado inteiro, com a cena embaixo.
    bool FillTarget = false;
    // Com FillTarget: até onde a geometria chega no eixo horizontal
    // apresentado (NDC; 1/fator quando o hook alargou o frustum).
    float FillHorizontalExtent = 1.0F;

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

// O hook de projeção alargou este frustum de perspectiva pelo fator do
// widescreen (anisotropia medida = 1/fator).
[[nodiscard]] inline bool GlWidescreenPerspectiveWidened(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor) noexcept {
    return widescreenFactor > 1.0F &&
           evidence.Projection == GlProjectionShape::Perspective &&
           GlAnisotropyNear(evidence.PixelAnisotropy, 1.0F / widescreenFactor);
}

// Até onde (em NDC, eixo horizontal apresentado = linha 1 do clip) a geometria
// de um draw tem de chegar para cobrir o viewport nativo: o frustum alargado
// pelo hook põe a borda nativa em 1/fator.
[[nodiscard]] inline float GlWidescreenNativeHorizontalExtent(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor) noexcept {
    return GlWidescreenPerspectiveWidened(evidence, widescreenFactor)
               ? 1.0F / widescreenFactor
               : 1.0F;
}

// Um draw do menu realocado (fora do domínio Ui) que só precisa da prova de
// cobertura para virar fade de tela inteira. A camada de atmosfera não conta
// aqui: no trace do host os draws do menu realocado vêm atribuídos a ela (o
// menu da tela de título roda dentro da fila de ambiente do Gameplay_Draw), e
// nenhum efeito de ambiente da cena usa o viewport Y 40..360.
// O fade da transição (seleção de arquivo -> jogo) vem no domínio Ui, sem
// textura, com a ortográfica 400x240 da tela de cima no viewport do menu
// (trace do host); vale para os dois domínios.
[[nodiscard]] inline bool GlWidescreenRelocatedFillCandidate(
    const GlWidescreenDrawEvidence& evidence) noexcept {
    return !evidence.SamplesRenderedCopy && !evidence.ViewportSpansPresentedWidth &&
           evidence.Untextured && !evidence.GuestScissor;
}

[[nodiscard]] inline GlWidescreenDrawPlacement ResolveGlWidescreenDrawPlacement(
    const GlWidescreenDrawEvidence& evidence, float widescreenFactor,
    bool menusEnabled) noexcept {
    GlWidescreenDrawPlacement placement;
    if (menusEnabled && !evidence.ViewportSpansPresentedWidth &&
        GlWidescreenRelocatedFillCandidate(evidence) &&
        evidence.GeometryCoversViewport) {
        // Fade/escurecimento de cor sólida sobre todo o menu de baixo
        // realocado (Y 40..360): no 3DS cobre a tela de baixo inteira; aqui
        // cobre a tela toda, senão a cena (céu) sobra nas laterais do 4:3.
        placement.FillTarget = true;
        placement.FillHorizontalExtent =
            GlWidescreenNativeHorizontalExtent(evidence, widescreenFactor);
        return placement;
    }
    if (evidence.UiDomain) {
        // Regra anterior: UI que cobre o alvo inteiro é conteúdo de quadro
        // (fade, redesenho da cópia) e estica; o resto fica no quadro.
        placement.Canvas =
            !(evidence.ViewportCoversTarget && evidence.GeometryCoversViewport);
    } else if (menusEnabled && !evidence.SamplesRenderedCopy &&
               (!evidence.Atmosphere || !evidence.ViewportSpansPresentedWidth)) {
        if (!evidence.ViewportSpansPresentedWidth) {
            // O menu de baixo realocado (Y 40..360), mesmo atribuído à camada
            // de atmosfera (antes isso o esticava: a aba BACK saía do canto,
            // x=128 em vez de 160 de 1280, e o cartão que entra deslizando
            // aparecia até x=1152).
            placement.Canvas = true;
        } else {
            placement.Canvas = GlWidescreenOrthographicPanelCandidate(evidence) &&
                               !evidence.GeometryCoversViewport;
        }
    }
    placement.CompensateWidening =
        placement.Canvas && menusEnabled &&
        GlWidescreenPerspectiveWidened(evidence, widescreenFactor);
    return placement;
}

// Eixo horizontal apresentado (Y do alvo alargado, em pixels do alvo) de um
// draw: viewport e recorte. O eixo vertical (X) não muda com o widescreen.
struct GlWidescreenPresentedAxisInput {
    float ViewportY = 0.0F;      // nativo
    float ViewportHeight = 0.0F; // nativo
    // Recorte do jogo (ScissorMode 3): Y1 e Y2 nativos, inclusivos.
    bool GuestScissor = false;
    std::uint16_t ScissorY1 = 0;
    std::uint16_t ScissorY2 = 0;
    std::uint16_t FramebufferHeight = 0; // 400 na tela de cima
    std::uint32_t TargetHeight = 0;      // alvo alargado (pixels)
    float ResolutionScale = 1.0F;        // pixels por pixel nativo (quadro)
    float TargetScaleY = 1.0F;           // pixels por pixel nativo (esticado)
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
        // A geometria chega a FillHorizontalExtent do viewport: essa extensão
        // vai para a largura apresentada inteira do alvo.
        const float centre = static_cast<float>(input.TargetHeight) * 0.5F;
        const float half = centre / std::max(placement.FillHorizontalExtent, 1.0e-3F);
        axis.ViewportBegin = static_cast<std::int32_t>(std::floor(centre - half));
        axis.ViewportEnd = static_cast<std::int32_t>(std::ceil(centre + half));
    } else if (placement.CompensateWidening) {
        // O hook alargou o frustum pelo fator do alvo: o viewport cresce o
        // mesmo tanto em torno do centro e as proporções voltam às nativas.
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
        // O que vai para o quadro nunca passa dele.
        const auto canvasBegin = static_cast<std::int32_t>(offset);
        const auto canvasEnd = canvasBegin + static_cast<std::int32_t>(std::llround(
            static_cast<double>(input.FramebufferHeight) *
            static_cast<double>(input.ResolutionScale)));
        axis.ScissorBegin = std::max(axis.ScissorBegin, canvasBegin);
        axis.ScissorEnd = std::min(axis.ScissorEnd, canvasEnd);
    }
    if (placement.CompensateWidening) {
        // O viewport alargado só devolve as proporções; o que estava fora do
        // viewport nativo (fora da tela no 3DS: um modelo 3D de menu passando
        // da borda) continua cortado ali, como no console.
        axis.ScissorBegin = std::max(axis.ScissorBegin, edge(input.ViewportY));
        axis.ScissorEnd =
            std::min(axis.ScissorEnd, edge(input.ViewportY + input.ViewportHeight));
    }
    axis.ScissorEnd = std::max(axis.ScissorEnd, axis.ScissorBegin);
    axis.ViewportEnd = std::max(axis.ViewportEnd, axis.ViewportBegin);
    return axis;
}

// Diagnóstico da classificação (desligado sem o arquivo): com
// sdmc:/switch/oot3dre/config/widescreen_menus_log presente (desktop: variável
// TRIAEVUM_GL_WIDESCREEN_MENUS_LOG com o caminho do arquivo de saída), o
// backend grava em logs/widescreen_menus.txt uma linha por assinatura distinta
// de draw no alvo alargado (viewport, projeção, cobertura, domínio, camada,
// textura, decisão), até um teto. Serve para conferir no console o que caiu no
// quadro 5:3 e o que esticou (botão B, fades, painéis) sem trace PICA.
struct GlWidescreenDrawLogFacts {
    std::array<float, 4> Viewport{}; // X, Y, largura, altura (nativos)
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
        (placement.FillTarget ? 0x40000U : 0U));
    mix(static_cast<std::uint64_t>(std::lround(evidence.PixelAnisotropy * 1000.0F)));
    return hash;
}

[[nodiscard]] inline std::string FormatGlWidescreenDrawLogLine(
    const GlWidescreenDrawLogFacts& facts, const GlWidescreenDrawEvidence& evidence,
    const GlWidescreenDrawPlacement& placement) {
    const char* shape = evidence.Projection == GlProjectionShape::Orthographic ? "orto"
                        : evidence.Projection == GlProjectionShape::Perspective ? "persp"
                                                                                : "?";
    char text[640];
    int length = std::snprintf(
        text, sizeof(text),
        "%s%s vp=%.0f,%.0f,%.0f,%.0f fb=%ux%u sc=%u:%u,%u,%u,%u dom=%u cam=%u "
        "proj=%s aniso=%.4f cobre_vp=%d vp_largura=%d vp_alvo=%d copia=%d "
        "atm=%d tipada=%d sem_tex=%d vtx=%u vs=%016llx tex=%u:%08x/%u",
        placement.FillTarget ? "TELA" : placement.Canvas ? "QUADRO" : "ESTICA",
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

// Arquivo de saída do diagnóstico, ou vazio se desligado. Lido uma vez.
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
