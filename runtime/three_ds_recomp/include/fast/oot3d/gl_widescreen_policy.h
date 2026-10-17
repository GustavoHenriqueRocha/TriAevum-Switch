#pragma once

#include "fast/oot3d/scene_presentation_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

// Widescreen do backend OpenGL (Switch e GL de desktop).
//
// O Vulkan do PC já renderiza o alvo da tela de cima no formato da janela e o
// hook de projeção (ResolveScenePresentationPolicy) alarga o frustum na mesma
// proporção. O GL renderiza o alvo nativo 400x240 (x escala N) e apresenta em
// 5:3 com faixas; com esta opção ligada ele alarga só o eixo apresentado
// horizontalmente do alvo da tela de cima (480x400 guardado girado), na
// proporção da janela, e o hook alarga o FOV na mesma razão. Com ela
// desligada, o hook precisa usar o aspecto nativo, senão a cena sai espremida
// dentro do 5:3.
//
// Lido uma vez: no Switch de sdmc:/switch/oot3dre/config/widescreen ("1" liga,
// "0" desliga; sem o arquivo vale o padrão do console, ligado); fora dele da
// variável TRIAEVUM_GL_WIDESCREEN (sem ela, desligado).
[[nodiscard]] inline bool ParseGlWidescreenSetting(const std::string& text,
                                                   bool fallback) noexcept {
    for (const char c : text) {
        if (c == '1' || c == 'y' || c == 'Y' || c == 't' || c == 'T')
            return true;
        if (c == '0' || c == 'n' || c == 'N' || c == 'f' || c == 'F')
            return false;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }
    return fallback;
}

[[nodiscard]] inline bool GlWidescreenEnabled() {
    static const bool enabled = [] {
#if defined(__SWITCH__)
        constexpr bool kConsoleDefault = true;
        std::ifstream file("sdmc:/switch/oot3dre/config/widescreen");
        std::string text;
        if (!file || !std::getline(file, text))
            return kConsoleDefault;
        return ParseGlWidescreenSetting(text, kConsoleDefault);
#else
        const char* value = std::getenv("TRIAEVUM_GL_WIDESCREEN");
        return value != nullptr && ParseGlWidescreenSetting(value, false);
#endif
    }();
    return enabled;
}

// Quanto o eixo horizontal apresentado da tela de cima cresce em relação ao
// 400 nativo para preencher uma saída `width`x`height` (1 = sem alargar).
// Só alarga (saídas mais estreitas que 5:3 continuam com faixas), e no máximo
// até 21:9 para não estourar o alvo.
[[nodiscard]] inline float GlWidescreenFactor(std::uint32_t width,
                                              std::uint32_t height) noexcept {
    if (width == 0U || height == 0U)
        return 1.0F;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    if (!std::isfinite(aspect) || aspect <= kNativeTopScreenAspect + 1.0e-4F)
        return 1.0F;
    return std::min(aspect / kNativeTopScreenAspect, (21.0F / 9.0F) / kNativeTopScreenAspect);
}

} // namespace Fast::Oot3d
