#pragma once
// A libnx text console for the start-up screens (game data import, HUD
// texture download). Switch only; closed before SDL/deko3d take the window
// over. The screens share one console (StartupScreen), so the import hands
// over to the HUD texture offer without the screen going away in between.
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
        // libnx points stdout at the console only on its first consoleInit
        // (didFirstConsoleInit, console.c). A later screen (the HUD texture
        // offer right after the game data import) printed into the devoptab
        // the previous screen put back, and the console stayed black.
        static const devoptab_t* const console = devoptab_list[STD_OUT];
        devoptab_list[STD_OUT] = console;
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

    void SetTitle(std::string title) { mTitle = std::move(title); }

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

inline std::optional<SwitchConsoleScreen>& StartupScreenStorage() {
    static std::optional<SwitchConsoleScreen> screen;
    return screen;
}

// The start-up console under `title`, opened on first use and kept open for
// the next start-up step.
inline SwitchConsoleScreen& StartupScreen(std::string title) {
    auto& screen = StartupScreenStorage();
    if (screen)
        screen->SetTitle(std::move(title));
    else
        screen.emplace(std::move(title));
    return *screen;
}

// main(), after the last start-up step: gives the window back for SDL/deko3d.
inline void CloseStartupScreen() { StartupScreenStorage().reset(); }

} // namespace Oot3dNativeGame

#endif
