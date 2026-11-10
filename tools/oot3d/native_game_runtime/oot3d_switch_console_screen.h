#pragma once
// Console de texto do libnx para a tela de início (importação dos dados do
// jogo). Só no Switch; fechado antes de o SDL/deko3d assumir a janela.
#if defined(__SWITCH__)

#include "oot3d_rom_import.h"

#include <switch.h>
#include <sys/iosupport.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace Oot3dNativeGame {

// O consoleInit aponta stdout e stderr para o console e o consoleExit não os
// devolve: a primeira escrita depois da tela (o sink de console do spdlog, na
// abertura do jogo) desenhava no console já liberado e travava
// (ConsoleSwRenderer_drawChar, escrita em nulo). Os devoptabs anteriores
// voltam na saída.
class SwitchConsoleScreen {
public:
    explicit SwitchConsoleScreen(std::string title)
        : mTitle(std::move(title)), mStdout(devoptab_list[STD_OUT]), mStderr(devoptab_list[STD_ERR]) {
        consoleInit(nullptr);
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&mPad);
    }
    ~SwitchConsoleScreen() {
        consoleExit(nullptr);
        devoptab_list[STD_OUT] = mStdout;
        devoptab_list[STD_ERR] = mStderr;
    }
    SwitchConsoleScreen(const SwitchConsoleScreen&) = delete;
    SwitchConsoleScreen& operator=(const SwitchConsoleScreen&) = delete;

    // O console desenha byte a byte com a fonte padrão: o texto em pt-BR
    // passa por ConsoleAscii (sem acentos) em vez de virar glifos trocados.
    void Show(const std::string& body) {
        consoleClear();
        std::printf("\x1b[1;1H\x1b[36m%s\x1b[0m\n\n%s", RomImport::ConsoleAscii(mTitle).c_str(),
                    RomImport::ConsoleAscii(body).c_str());
        consoleUpdate(nullptr);
    }

    // Botões apertados desde a última chamada (um quadro: o consoleUpdate
    // espera o vsync).
    u64 Poll() {
        padUpdate(&mPad);
        const u64 down = padGetButtonsDown(&mPad);
        consoleUpdate(nullptr);
        return down;
    }

    // Espera um dos `buttons`; nullopt quando o jogador sai do app.
    std::optional<u64> Wait(u64 buttons) {
        while (appletMainLoop()) {
            const u64 down = Poll() & buttons;
            if (down)
                return down;
        }
        return std::nullopt;
    }

private:
    std::string mTitle;
    PadState mPad{};
    const devoptab_t* mStdout;
    const devoptab_t* mStderr;
};

} // namespace Oot3dNativeGame

#endif
