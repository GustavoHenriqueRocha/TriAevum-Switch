#include "oot3d_switch_rom_import.h"

#if defined(__SWITCH__)

#include "oot3d_rom_import.h"
#include "oot3d_switch_console_screen.h"

#include <switch.h>

#include <chrono>
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

constexpr const char* kQuitLine = "\nAperte + para sair.\n";

constexpr const char* kRoot = "sdmc:/switch/oot3dre";
constexpr const char* kGame = "sdmc:/switch/oot3dre/game";
// A file here turns the import off: the boot reads game/ as before.
constexpr const char* kDisabled = "sdmc:/switch/oot3dre/config/no_rom_import";

class ImportCancelled : public std::exception {};

std::string Where() {
    return "Copie a dump do SEU cartucho, descriptografada (.3ds ou .cci), para\n"
           "  sdmc:/switch/oot3dre/\n"
           "ou code.bin, exheader.bin e romfs.bin (de uma dump descriptografada) para\n"
           "  sdmc:/switch/oot3dre/import/\n"
           "e abra o jogo de novo.\n\n"
           "Dumps aceitas:\n" + ri::SupportedRevisionsText() +
           "\nDumps criptografadas e arquivos .cia não funcionam.\n";
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
    std::error_code error;
    if (fs::exists(kDisabled, error))
        return true;
    const fs::path game = kGame;
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

    SwitchConsoleScreen& screen = StartupScreen("Ocarina of Time 3D - importação dos dados do jogo\nPort para Switch por Gustavo Henrique Rocha");
    if (sources.empty()) {
        screen.Show("Os dados do jogo não foram encontrados.\n\n" + Where() + kQuitLine);
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    // The first dump that matches a supported revision.
    std::optional<ri::Plan> plan;
    std::string rejected;
    for (const ri::Source& source : sources) {
        screen.Show("Conferindo " + source.Path.filename().string() + "...\n");
        try {
            plan = ri::Identify(source);
            break;
        } catch (const std::exception& ex) {
            rejected += source.Path.filename().string() + ": " + ex.what() + "\n\n";
        }
    }
    if (!plan) {
        Log("rejected: " + rejected);
        const std::string message = "Esta dump não pode ser usada:\n\n" + rejected + Where();
        if (haveGame) {
            screen.Show(message + "\nAperte A para jogar com os dados que já existem, + para sair.\n");
            const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_Plus);
            return button && (*button & HidNpadButton_A);
        }
        screen.Show(message + kQuitLine);
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    const std::string header = "Preparando o jogo a partir de " + plan->Input.Path.filename().string() +
                               "\n(" + plan->Recipe->Name + ")\n\n"
                               "Isso acontece uma vez só e leva cerca de um minuto.\n"
                               "Não tire o cartão SD nem feche o app.\n\n";
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
        const std::string message = "A importação falhou:\n\n" + failure + "\n\n" + Where();
        if (haveGameNow) {
            screen.Show(message + "\nAperte A para jogar com os dados que já existem, + para sair.\n");
            const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_Plus);
            return button && (*button & HidNpadButton_A);
        }
        screen.Show(message + kQuitLine);
        (void)screen.Wait(HidNpadButton_Plus);
        return false;
    }

    // Keeping the dump is the default; deleting it is the player's choice.
    const bool cartridge = plan->Input.Kind == ri::SourceKind::Cartridge;
    screen.Show(header + "Pronto. Os dados do jogo estão em sdmc:/switch/oot3dre/game/.\n\n" +
                (cartridge ? "Manter " + plan->Input.Path.filename().string() + "?\n"
                           : std::string("Manter os arquivos em import/?\n")) +
                "  A: manter e abrir o jogo\n"
                "  Y: apagar para liberar espaço e abrir o jogo\n");
    const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus | HidNpadButton_Y);
    if (!button)
        return false;
    if (*button & HidNpadButton_Y) {
        try {
            ri::RemoveSource(plan->Input);
            Log("deleted " + plan->Input.Path.string());
        } catch (const std::exception& ex) {
            Log(std::string("delete failed: ") + ex.what());
            screen.Show(std::string("Não foi possível apagar a dump: ") + ex.what() +
                        "\n\nAperte A para abrir o jogo.\n");
            if (!screen.Wait(HidNpadButton_A))
                return false;
        }
    }
    return true;
}

} // namespace Oot3dNativeGame

#endif
