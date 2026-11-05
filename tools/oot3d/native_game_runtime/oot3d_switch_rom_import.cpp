#include "oot3d_switch_rom_import.h"

#if defined(__SWITCH__)

#include "oot3d_rom_import.h"

#include <switch.h>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace Oot3dNativeGame {
namespace {

namespace fs = std::filesystem;
namespace ri = RomImport;

constexpr const char* kRoot = "sdmc:/switch/oot3dre";
constexpr const char* kGame = "sdmc:/switch/oot3dre/game";

class ImportCancelled : public std::exception {};

// libnx text console on the default window; released before SDL/deko3d
// take the window over.
class Screen {
public:
    Screen() {
        consoleInit(nullptr);
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&mPad);
    }
    ~Screen() { consoleExit(nullptr); }
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    void Show(const std::string& body) {
        consoleClear();
        std::printf("\x1b[1;1H\x1b[36mOcarina of Time 3D - game data import\x1b[0m\n\n%s", body.c_str());
        consoleUpdate(nullptr);
    }

    // Waits for one of `buttons`; nullopt when the player leaves the app.
    std::optional<u64> Wait(u64 buttons) {
        while (appletMainLoop()) {
            padUpdate(&mPad);
            const u64 down = padGetButtonsDown(&mPad);
            if (down & buttons)
                return down & buttons;
            consoleUpdate(nullptr);
        }
        return std::nullopt;
    }

private:
    PadState mPad{};
};

std::string Where() {
    return "Copy your own decrypted cartridge dump (.3ds or .cci) to\n"
           "  sdmc:/switch/oot3dre/\n"
           "or code.bin, exheader.bin and romfs.bin (from a decrypted dump) to\n"
           "  sdmc:/switch/oot3dre/import/\n"
           "and start the game again.\n\n"
           "Supported dumps:\n" + ri::SupportedRevisionsText() +
           "\nEncrypted dumps and .cia files are not supported.\n";
}

void Log(const std::string& line) {
    std::error_code error;
    fs::create_directories(std::string(kRoot) + "/logs", error);
    std::ofstream(std::string(kRoot) + "/logs/rom_import.txt", std::ios::app) << line << '\n';
}

std::string Bar(std::uint64_t done, std::uint64_t total) {
    const int width = 50;
    const int filled = total ? static_cast<int>(done * width / total) : 0;
    std::string bar = "[";
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '.');
    bar += "] " + std::to_string(total ? done * 100 / total : 0) + "%";
    return bar;
}

} // namespace

bool RunSwitchRomImport() {
    const fs::path game = kGame;
    std::error_code error;
    const bool haveGame = fs::is_regular_file(game / ri::kManifestName, error);
    std::vector<ri::Source> sources;
    try {
        sources = ri::FindSources(kRoot);
    } catch (const std::exception&) {
    }
    for (const ri::Source& source : sources)
        if (ri::ImportIsCurrent(source, game))
            return true;
    if (sources.empty() && haveGame)
        return true;

    Screen screen;
    if (sources.empty()) {
        screen.Show("No game data was found.\n\n" + Where() + "\nPress + to exit.\n");
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    // The first dump that matches a supported revision.
    std::optional<ri::Plan> plan;
    std::string rejected;
    for (const ri::Source& source : sources) {
        screen.Show("Checking " + source.Path.filename().string() + "...\n");
        try {
            plan = ri::Identify(source);
            break;
        } catch (const std::exception& ex) {
            rejected += source.Path.filename().string() + ": " + ex.what() + "\n\n";
        }
    }
    if (!plan) {
        Log("rejected: " + rejected);
        const std::string message = "This dump cannot be used:\n\n" + rejected + Where();
        if (haveGame) {
            screen.Show(message + "\nPress A to start with the existing game data, + to exit.\n");
            const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_Plus);
            return button && (*button & HidNpadButton_A);
        }
        screen.Show(message + "\nPress + to exit.\n");
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    const std::string header = "Preparing the game from " + plan->Input.Path.filename().string() +
                               "\n(" + plan->Recipe->Name + ")\n\n"
                               "This happens once and takes about a minute.\n"
                               "Do not remove the SD card or close the app.\n\n";
    screen.Show(header);
    appletSetAutoSleepDisabled(true);
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t lastShown = 0;
    std::string failure;
    try {
        const ri::Result result = ri::Import(*plan, game, [&](const ri::Progress& progress) {
            if (!appletMainLoop())
                throw ImportCancelled();
            if (progress.Done - lastShown < (16U << 20) && progress.Done != progress.Total)
                return;
            lastShown = progress.Done;
            screen.Show(header + progress.Stage + "\n" + Bar(progress.Done, progress.Total) + "\n");
        });
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Log(std::string("imported ") + plan->Input.Path.string() + " recipe=" + result.Recipe->Id +
            " code=" + ri::Hex(result.Code) + " romfs=" + ri::Hex(result.RomFs) +
            " seconds=" + std::to_string(seconds));
    } catch (const ImportCancelled&) {
        appletSetAutoSleepDisabled(false);
        Log("cancelled");
        return false;
    } catch (const std::exception& ex) {
        failure = ex.what();
    }
    appletSetAutoSleepDisabled(false);

    const bool haveGameNow = fs::is_regular_file(game / ri::kManifestName, error);
    if (!failure.empty()) {
        Log("failed: " + failure);
        const std::string message = "The import failed:\n\n" + failure + "\n\n" + Where();
        if (haveGameNow) {
            screen.Show(message + "\nPress A to start with the existing game data, + to exit.\n");
            const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_Plus);
            return button && (*button & HidNpadButton_A);
        }
        screen.Show(message + "\nPress + to exit.\n");
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    // Keeping the dump is the default; deleting it is the player's choice.
    const bool cartridge = plan->Input.Kind == ri::SourceKind::Cartridge;
    screen.Show(header + "Done. The game data is in sdmc:/switch/oot3dre/game/.\n\n" +
                (cartridge ? "Keep " + plan->Input.Path.filename().string() + "?\n"
                           : std::string("Keep the files in import/?\n")) +
                "  A: keep it and start the game\n"
                "  Y: delete it to free space and start the game\n");
    const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus | HidNpadButton_Y);
    if (!button)
        return false;
    if (*button & HidNpadButton_Y) {
        try {
            ri::RemoveSource(plan->Input);
            Log("deleted " + plan->Input.Path.string());
        } catch (const std::exception& ex) {
            Log(std::string("delete failed: ") + ex.what());
            screen.Show(std::string("Could not delete the dump: ") + ex.what() +
                        "\n\nPress A to start the game.\n");
            if (!screen.Wait(HidNpadButton_A))
                return false;
        }
    }
    return true;
}

} // namespace Oot3dNativeGame

#endif
