#pragma once

// Distribuição das threads do processo pelos núcleos do Switch (Horizon).
// Só a política: puro e testável no host (oot3d_switch_thread_layout_tests);
// quem aplica é oot3d_switch_threads.cpp.
//
// Regras do escalonador do Horizon que a política respeita:
// - prioridade menor = mais urgente; só a 0x3B reveza tempo entre threads de
//   mesma prioridade. Em qualquer outra, uma thread ocupada impede as de
//   prioridade igual ou menor no mesmo núcleo, mesmo com outros núcleos
//   ociosos (a migração só acontece nos pontos de reescalonamento);
// - uma thread com máscara de vários núcleos pode ser levada a um núcleo
//   ocioso; com máscara de um núcleo, nunca.
//
// Layouts (arquivo sdmc:/switch/oot3dre/config/thread_layout, primeira
// palavra; ausente = fixed):
// - fixed  : jogo sozinho no núcleo 0; render thread fixa no 2; worker de
//            planos, áudio do host e áudio do SDL no 1; threads de serviço
//            (log, cache de shader do Mesa, recursos, carregadores) na 0x3B
//            com máscara 1|2: nunca tomam o lugar de quem desenha ou toca som.
// - float  : como fixed, mas render thread e worker de planos com máscara
//            1|2 (núcleo ideal 2 e 1): o kernel pode levá-los ao núcleo livre.
//            (StevensND/nfsmw-nx: fixar a render thread piorou o pior caso.)
// - legacy : o comportamento anterior, para A/B: toda pthread nova na
//            prioridade do jogo (0x2C), alternando núcleos 1/2; áudio do SDL
//            no núcleo em que nasceu; spin de 500 us no pacer.
// Opções depois do modo: pacer_spin_us=N (espera ativa antes do prazo do
// pacer; padrão 0 em fixed/float — svcSleepThread acorda no prazo — e 500
// em legacy).

#include <cstdint>
#include <string_view>

namespace Oot3dNativeGame {

enum class SwitchThreadLayout : uint8_t { Fixed, Float, Legacy };

enum class SwitchThreadRole : uint8_t {
    Game,       // thread principal: o jogo (CPU guest, DSP HLE, submissão)
    Render,     // NativeRenderCommandStream: todo o GL
    PicaPlan,   // pré-montagem dos planos PICA
    HostAudio,  // HostAudioStream: stretch, reamostragem, fila do SDL
    SdlAudio,   // thread de áudio do SDL (audren)
    Service,    // qualquer outra pthread (log, Mesa, recursos, carregadores)
    Count
};

inline constexpr int kSwitchGamePriority = 0x2C;
inline constexpr int kSwitchRenderPriority = 0x2D;
inline constexpr int kSwitchPicaPlanPriority = 0x2E;
inline constexpr int kSwitchAudioPriority = 0x2B;
// A única prioridade com revezamento de tempo; abaixo de todas as outras.
inline constexpr int kSwitchServicePriority = 0x3B;

struct SwitchThreadPlacement {
    int Priority = kSwitchServicePriority;
    // Núcleo ideal (0-2); -1 = não mexer na afinidade; -2 = alternar 1/2 na
    // criação (legacy).
    int IdealCore = -1;
    uint32_t CoreMask = 0U;  // bits 0-2; 0 = não mexer
};

struct SwitchThreadLayoutConfig {
    SwitchThreadLayout Layout = SwitchThreadLayout::Fixed;
    // Espera ativa do pacer antes do prazo, em microssegundos.
    int PacerSpinMicroseconds = 0;
    bool Valid = true;  // false: texto não reconhecido (cai em fixed)
};

[[nodiscard]] constexpr const char* SwitchThreadLayoutName(
    SwitchThreadLayout layout) noexcept {
    switch (layout) {
    case SwitchThreadLayout::Fixed: return "fixed";
    case SwitchThreadLayout::Float: return "float";
    case SwitchThreadLayout::Legacy: return "legacy";
    }
    return "?";
}

[[nodiscard]] constexpr const char* SwitchThreadRoleName(
    SwitchThreadRole role) noexcept {
    switch (role) {
    case SwitchThreadRole::Game: return "game";
    case SwitchThreadRole::Render: return "render";
    case SwitchThreadRole::PicaPlan: return "plan";
    case SwitchThreadRole::HostAudio: return "audio";
    case SwitchThreadRole::SdlAudio: return "sdl_audio";
    case SwitchThreadRole::Service: return "service";
    case SwitchThreadRole::Count: break;
    }
    return "?";
}

[[nodiscard]] constexpr int DefaultSwitchPacerSpinMicroseconds(
    SwitchThreadLayout layout) noexcept {
    return layout == SwitchThreadLayout::Legacy ? 500 : 0;
}

// Conteúdo do arquivo config/thread_layout. Vazio = fixed.
[[nodiscard]] constexpr SwitchThreadLayoutConfig ParseSwitchThreadLayoutConfig(
    std::string_view text) noexcept {
    SwitchThreadLayoutConfig config;
    bool first = true;
    bool spinGiven = false;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' ||
                text[position] == '\r' || text[position] == '\n' ||
                text[position] == ',')) {
            ++position;
        }
        const size_t start = position;
        while (position < text.size() && text[position] != ' ' &&
               text[position] != '\t' && text[position] != '\r' &&
               text[position] != '\n' && text[position] != ',') {
            ++position;
        }
        const std::string_view token = text.substr(start, position - start);
        if (token.empty()) {
            break;
        }
        constexpr std::string_view kSpin = "pacer_spin_us=";
        if (token.substr(0, kSpin.size()) == kSpin) {
            const std::string_view digits = token.substr(kSpin.size());
            int value = 0;
            bool ok = !digits.empty() && digits.size() <= 5U;
            for (const char c : digits) {
                if (c < '0' || c > '9') {
                    ok = false;
                    break;
                }
                value = value * 10 + (c - '0');
            }
            if (ok && value <= 4000) {
                config.PacerSpinMicroseconds = value;
                spinGiven = true;
            } else {
                config.Valid = false;
            }
        } else if (first && token == "fixed") {
            config.Layout = SwitchThreadLayout::Fixed;
        } else if (first && token == "float") {
            config.Layout = SwitchThreadLayout::Float;
        } else if (first && token == "legacy") {
            config.Layout = SwitchThreadLayout::Legacy;
        } else {
            config.Valid = false;
        }
        first = false;
    }
    if (!spinGiven) {
        config.PacerSpinMicroseconds =
            DefaultSwitchPacerSpinMicroseconds(config.Layout);
    }
    return config;
}

// processMask: núcleos permitidos ao processo (bits 0-2). Núcleos fora dela
// saem da máscara; sem nenhum núcleo sobrando, a afinidade fica como está.
[[nodiscard]] constexpr SwitchThreadPlacement ResolveSwitchThreadPlacement(
    SwitchThreadLayout layout, SwitchThreadRole role,
    uint32_t processMask) noexcept {
    SwitchThreadPlacement placement;
    const bool legacy = layout == SwitchThreadLayout::Legacy;
    const bool floating = layout == SwitchThreadLayout::Float;
    switch (role) {
    case SwitchThreadRole::Game:
        placement = {kSwitchGamePriority, 0, 0x1U};
        break;
    case SwitchThreadRole::Render:
        placement = {kSwitchRenderPriority, 2, floating ? 0x6U : 0x4U};
        break;
    case SwitchThreadRole::PicaPlan:
        placement = {kSwitchPicaPlanPriority, 1, floating ? 0x6U : 0x2U};
        break;
    case SwitchThreadRole::HostAudio:
        placement = {kSwitchAudioPriority, 1, 0x2U};
        break;
    case SwitchThreadRole::SdlAudio:
        placement = legacy ? SwitchThreadPlacement{kSwitchAudioPriority, -1, 0U}
                           : SwitchThreadPlacement{kSwitchAudioPriority, 1, 0x2U};
        break;
    case SwitchThreadRole::Service:
    case SwitchThreadRole::Count:
        placement = legacy
                        ? SwitchThreadPlacement{kSwitchGamePriority, -2, 0U}
                        : SwitchThreadPlacement{kSwitchServicePriority, 1, 0x6U};
        break;
    }
    if (placement.CoreMask != 0U) {
        placement.CoreMask &= processMask & 0x7U;
        if (placement.CoreMask == 0U) {
            placement.IdealCore = -1;
        } else if ((placement.CoreMask & (1U << placement.IdealCore)) == 0U) {
            // O núcleo ideal precisa estar na máscara: o menor que sobrou.
            for (int core = 0; core < 3; ++core) {
                if ((placement.CoreMask & (1U << core)) != 0U) {
                    placement.IdealCore = core;
                    break;
                }
            }
        }
    }
    return placement;
}

} // namespace Oot3dNativeGame
