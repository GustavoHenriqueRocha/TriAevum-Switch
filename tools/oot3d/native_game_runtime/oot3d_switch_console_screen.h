#pragma once
// A libnx text console for the start-up screens (game data import, HUD
// texture download). Switch only; released before SDL/deko3d take the
// window over.
#if defined(__SWITCH__)

#include <switch.h>
#include <sys/iosupport.h>

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

namespace Oot3dNativeGame {

// consoleInit points stdout and stderr at the console and consoleExit does
// not point them back: the first write after the screen (spdlog's console
// sink, as the game starts) drew into the freed console and crashed
// (ConsoleSwRenderer_drawChar, null write). The previous devoptabs are put
// back on exit.
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

    void Show(const std::string& body) {
        consoleClear();
        std::printf("\x1b[1;1H\x1b[36m%s\x1b[0m\n\n%s", mTitle.c_str(), body.c_str());
        consoleUpdate(nullptr);
    }

    // Buttons pressed since the last call (one frame: consoleUpdate waits
    // for vsync).
    u64 Poll() {
        padUpdate(&mPad);
        const u64 down = padGetButtonsDown(&mPad);
        consoleUpdate(nullptr);
        return down;
    }

    // Waits for one of `buttons`; nullopt when the player leaves the app.
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
