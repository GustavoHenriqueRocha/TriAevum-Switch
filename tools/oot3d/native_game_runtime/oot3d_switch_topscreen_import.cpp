#include "oot3d_switch_topscreen_import.h"

#if defined(__SWITCH__)

#include "oot3d_rom_import.h"
#include "oot3d_switch_console_screen.h"
#include "oot3d_switch_network.h"
#include "oot3d_roll_dash.h"
#include "oot3d_romfs_mods.h"
#include "oot3d_topscreen_import.h"

#include <switch.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace Oot3dNativeGame {
namespace {

namespace fs = std::filesystem;
namespace ts = TopScreenImport;
namespace ri = RomImport;

constexpr const char* kRoot = "sdmc:/switch/oot3dre";
// Room left on the SD card beyond the download and the outputs.
constexpr std::uint64_t kSpareBytes = 16U << 20U;
constexpr std::size_t kWriteBuffer = 1U << 20U;

fs::path Root() { return kRoot; }
fs::path Config() { return Root() / "config"; }

// The RomFS mods the game will load (oot3d_romfs_mods.h): a texture they
// replace gets a pack entry too (the PT-BR mod redraws menu_top_parts00).
// Folder name order, the last one wins, like the overlay.
ts::RomFsOverride ModFiles() {
    if (!ReadRomFsModsEnabled(Config())) return {};
    std::vector<fs::path> folders;
    std::error_code error;
    for (fs::directory_iterator it(Root() / "mods", error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error)) folders.push_back(it->path() / "romfs");
    std::sort(folders.begin(), folders.end());
    return [folders](std::string_view path) -> std::optional<ts::Bytes> {
        for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
            std::ifstream file(*folder / std::string(path), std::ios::binary);
            if (file) return ts::Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return std::nullopt;
    };
}

void Log(const std::string& line) {
    std::error_code error;
    fs::create_directories(Root() / "logs", error);
    std::ofstream file(Root() / "logs" / "topscreen_import.txt", std::ios::app);
    const std::time_t now = std::time(nullptr);
    char stamp[32] = "";
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    file << stamp << ' ' << line << '\n';
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

bool SaveOfferSetting(ts::OfferSetting setting) {
    std::error_code error;
    fs::create_directories(Config(), error);
    std::ofstream file(Config() / ts::kOfferSettingName, std::ios::trunc);
    file << ts::OfferSettingText(setting) << '\n';
    return static_cast<bool>(file);
}

std::string Megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
    return text;
}

std::string Bar(std::uint64_t done, std::uint64_t total) {
    const int width = 50;
    const int filled = total ? static_cast<int>(std::min(done, total) * width / total) : 0;
    std::string bar = "[";
    bar.append(static_cast<std::size_t>(filled), '#');
    bar.append(static_cast<std::size_t>(width - filled), '.');
    return bar + "] " + std::to_string(total ? std::min(done, total) * 100 / total : 0) + "%";
}

std::string Attribution(const ts::Archive& archive) {
    return std::string("A arte do HUD e dos menus deste port vem do mod\n  ") + ts::kModName + " " +
           archive.Version + " de " + ts::kModAuthor + "\n  " + ts::kModPage +
           "\nA licença dele (" + ts::kModLicense + ") permite baixar e instalar,\n"
           "mas não redistribuir; por isso ele não vem junto com o jogo.\n\n";
}

// `packExists` / `hdArtExists`: what is already installed; with both, the
// offer is for Link's Roll dash animations (Roll dash on, an install from
// before 0.2.1).
std::string Offer(bool packExists, bool hdArtExists) {
    const ts::Archive& archive = ts::DownloadArchive();
    const std::string size = std::to_string((archive.Bytes + 500'000) / 1'000'000) + " MB";
    const std::string what =
        packExists && hdArtExists
            ? "O Roll dash (L3+R3 > Game > Gameplay) está ligado: o Link usa as animações\n"
              "de arrancada do mod, e suas texturas do HUD foram montadas sem elas. O\n"
              "console pode baixar o mod do GameBanana (" + size + ") e montá-las de novo\n"
              "com as animações. Nada mais do mod é instalado, e o download é apagado\n"
              "depois. Sem elas, o Link arranca com a corrida normal.\n\n"
        : packExists
            ? "Suas texturas do HUD foram montadas por uma versão anterior. Esta também\n"
              "monta a arte HD do HUD (botões, direcional, rótulos, corações e cursor\n"
              "nítidos em 720p), que precisa do mod de novo: o console pode baixá-lo\n"
              "do GameBanana (" + size + ") e remontar as texturas a partir dos seus\n"
              "dados do jogo. Nada mais do mod é instalado, e o download é apagado\n"
              "depois. Sem ele, o HUD continua com a arte atual.\n\n"
            : "O console pode baixá-lo do GameBanana (" + size + ") e montar as texturas,\n"
              "a arte HD do HUD e a fonte HD dos textos a partir dos seus dados do jogo.\n"
              "Nada mais do mod é instalado, e o download é apagado depois.\n"
              "Sem eles, partes do HUD podem faltar.\n\n";
    return Attribution(archive) + what +
           "  A  Baixar (" + size + ") e abrir o jogo\n"
           "  B  Agora não (perguntar de novo na próxima vez)\n"
           "  X  Não perguntar mais (dá para religar em L3+R3 > Atualizações)\n";
}

struct Download {
    std::atomic<bool> Cancel{false};
    std::atomic<bool> Finished{false};
    std::atomic<std::uint64_t> Done{0};
    // Set by the worker before Finished.
    bool Ok = false;
    bool Cancelled = false;
    std::string Error;
};

// Downloads the archive to `path`, checking its size and SHA-256.
void RunDownload(const ts::Archive& archive, const fs::path& path, Download& state) {
    const auto finish = [&](bool ok, std::string error) {
        state.Ok = ok;
        state.Error = std::move(error);
        state.Finished.store(true);
    };
    SwitchNetwork::NetworkSession network;
    if (!network.Ready()) {
        finish(false, "Sem rede: conecte o console à internet e tente de novo.");
        return;
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (!out) {
        finish(false, "Não foi possível gravar " + path.string() + ".");
        return;
    }
    std::vector<char> buffer(kWriteBuffer);
    std::setvbuf(out.get(), buffer.data(), _IOFBF, buffer.size());
    ri::Sha256 hash;
    SwitchNetwork::Transfer transfer;
    transfer.Limit = archive.Bytes;
    transfer.Cancel = &state.Cancel;
    transfer.Write = [&](const char* data, size_t length) {
        if (std::fwrite(data, 1, length, out.get()) != length) return false;
        hash.Update(data, length);
        state.Done.fetch_add(length, std::memory_order_relaxed);
        return true;
    };
    long status = 0;
    std::string error;
    const auto result = SwitchNetwork::HttpGet(archive.Url, {}, transfer, &status, &error);
    const bool closed = std::fclose(out.release()) == 0;
    const std::string digest = ri::Hex(hash.Finish());
    Log("download " + std::string(archive.Url) + ": status " + std::to_string(status) + ", " +
        std::to_string(transfer.Received) + " bytes, sha256 " + digest + (error.empty() ? "" : ", " + error));
    if (result == SwitchNetwork::GetResult::Cancelled) {
        state.Cancelled = true;
        finish(false, "Download cancelado.");
    } else if (result == SwitchNetwork::GetResult::Failed) {
        finish(false, "O download do GameBanana falhou: " + error + ".");
    } else if (!closed) {
        finish(false, "Não foi possível gravar " + path.string() + " (cartão SD cheio?).");
    } else if (transfer.Received != archive.Bytes || digest != archive.Sha256) {
        finish(false, "O GameBanana mandou um arquivo diferente da versão " + std::string(archive.Version) +
                          " que\neste jogo conhece (" + std::to_string(transfer.Received) +
                          " bytes, SHA-256\n" + digest + ").\n"
                          "O mod pode ter sido atualizado; uma versão futura do jogo vai aceitá-lo.\n"
                          "Nada foi instalado.");
    } else {
        finish(true, {});
    }
}

enum class Outcome { Installed, Failed, Cancelled, Left };

// Builds and installs from a verified archive.
Outcome BuildFrom(SwitchConsoleScreen& screen, const fs::path& archivePath, const ts::Archive& archive,
                  const std::string& header, std::string* failure) {
    screen.Show(header + "Montando as texturas a partir de game/romfs.bin...\n");
    try {
        const ts::Outputs outputs = ts::Build(archivePath, archive, Root() / "game" / "romfs.bin", ModFiles());
        ts::Install(outputs, Config());
        Log("installed " + std::string(ts::kPackName) + " (" + std::to_string(outputs.Pack.size()) +
            " bytes), " + ts::kFontPackName + " (" + std::to_string(outputs.Fonts.size()) + " bytes) and " +
            (outputs.UiAtlasHd.empty() ? std::string("no HD UI atlas")
                                       : std::string(ts::kUiAtlasHdName) + " (" +
                                             std::to_string(outputs.UiAtlasHd.size()) + " bytes)") +
            ", " + std::to_string(outputs.HdTextures.size()) + " HD images in " + ts::kHdTexturesDirectoryName +
            "/, " +
            (outputs.LinkAnimations.empty() ? std::string("no Link dash animations")
                                            : std::string(kRollDashAnimationsName) + " (" +
                                                  std::to_string(outputs.LinkAnimations.size()) + " bytes)") +
            " from " + archivePath.filename().string() + " " + archive.Version);
        return Outcome::Installed;
    } catch (const std::exception& error) {
        *failure = std::string("Não foi possível montar as texturas: ") + error.what() + ".";
        return Outcome::Failed;
    }
}

Outcome DownloadAndBuild(SwitchConsoleScreen& screen, std::string* failure) {
    const ts::Archive& archive = ts::DownloadArchive();
    const std::string header = Attribution(archive);
    std::error_code error;
    const auto space = fs::space(Root(), error);
    if (!error && space.available < archive.Bytes + kSpareBytes) {
        *failure = "Falta espaço no cartão SD: precisa de " + Megabytes(archive.Bytes + kSpareBytes) + ", tem " +
                   Megabytes(space.available) + " livres.";
        return Outcome::Failed;
    }
    fs::path partial = Root() / archive.FileName;
    partial += ".partial";
    // The download is removed whatever happens; only the built files stay.
    struct Remove {
        fs::path Path;
        ~Remove() {
            std::error_code ignored;
            fs::remove(Path, ignored);
        }
    } removeDownload{partial};

    Download state;
    appletSetAutoSleepDisabled(true);
    std::thread worker([&] { RunDownload(archive, partial, state); });
    bool left = false;
    std::uint64_t shownPercent = 101;
    bool shownCancel = false;
    while (!state.Finished.load()) {
        if (!appletMainLoop()) {
            left = true;
            state.Cancel.store(true);
            break;
        }
        if (screen.Poll() & HidNpadButton_B) state.Cancel.store(true);
        const std::uint64_t done = state.Done.load(std::memory_order_relaxed);
        const std::uint64_t percent = done * 100 / archive.Bytes;
        const bool cancelling = state.Cancel.load();
        if (percent != shownPercent || cancelling != shownCancel) {
            shownPercent = percent;
            shownCancel = cancelling;
            screen.Show(header + "Baixando " + archive.FileName + " do GameBanana...\n" +
                        Bar(done, archive.Bytes) + "  " + Megabytes(done) + " / " + Megabytes(archive.Bytes) +
                        (cancelling ? "\n\nCancelando...\n" : "\n\n  B  Cancelar\n"));
        }
    }
    worker.join();
    appletSetAutoSleepDisabled(false);
    if (left) return Outcome::Left;
    if (state.Cancelled) return Outcome::Cancelled;
    if (!state.Ok) {
        *failure = state.Error;
        return Outcome::Failed;
    }
    return BuildFrom(screen, partial, archive, header, failure);
}

// An official archive the player put in sdmc:/switch/oot3dre/, and the
// release its name stands for (IdentifyArchiveFile checks it); the newest
// release when there are several.
std::optional<std::pair<fs::path, const ts::Archive*>> LocalArchive() {
    const auto known = ts::KnownArchives();
    for (auto it = known.rbegin(); it != known.rend(); ++it) {
        const ts::Archive& archive = *it;
        const fs::path path = Root() / archive.FileName;
        std::error_code error;
        if (fs::is_regular_file(path, error)) return std::pair{path, &archive};
    }
    return std::nullopt;
}

} // namespace

bool RunSwitchTopScreenImport() {
    std::error_code error;
    const bool packExists = fs::is_regular_file(Config() / ts::kPackName, error);
    const bool hdArtExists = ts::HdArtInstalled(Config());
    const bool romfsExists = fs::is_regular_file(Root() / "game" / "romfs.bin", error);
    // Roll dash on (oot3d_roll_dash.h) with an install from before the import
    // extracted Link's dash animations: only then is a 0.2.0 install with
    // the pack and the HD art offered the download again.
    const bool dashAnimationsMissing = ReadRollDashEnabled(Config()) && !RollDashAnimationsInstalled(Config());
    if ((packExists && hdArtExists && !dashAnimationsMissing) || !romfsExists) return true;
    // A download interrupted by closing the app.
    for (const ts::Archive& archive : ts::KnownArchives()) {
        fs::path partial = Root() / archive.FileName;
        partial += ".partial";
        fs::remove(partial, error);
    }
    const auto setting = ts::ParseOfferSetting(ReadText(Config() / ts::kOfferSettingName));
    auto local = LocalArchive();
    if (local && !ts::ShouldBuildFromLocalArchive(packExists, hdArtExists, dashAnimationsMissing, *local->second))
        local.reset();
    if (!local && !ts::ShouldOffer(packExists, hdArtExists, dashAnimationsMissing, romfsExists, setting)) return true;
    if (packExists && !hdArtExists)
        Log("the pack is installed without all of the HD art (" + std::string(ts::kUiAtlasHdName) + ", " +
            ts::kHdTexturesDirectoryName + "/)");
    if (dashAnimationsMissing)
        Log("Roll dash is on without Link's dash animations (" + std::string(kRollDashAnimationsName) + ")");

    SwitchConsoleScreen& screen = StartupScreen("Ocarina of Time 3D - texturas do HUD");
    std::string notice;
    if (local) {
        const fs::path& localPath = local->first;
        // The player copied the archive here: build from it without asking.
        screen.Show("Conferindo " + localPath.filename().string() + "...\n");
        std::string reason;
        const ts::Archive* archive = ts::IdentifyArchiveFile(localPath, &reason, [&](std::uint64_t done, std::uint64_t total) {
            if (done % (8U << 20U) < (1U << 20U) || done == total)
                screen.Show("Conferindo " + localPath.filename().string() + "...\n" + Bar(done, total) + "\n");
        });
        if (archive) {
            std::string failure;
            if (BuildFrom(screen, localPath, *archive, Attribution(*archive), &failure) == Outcome::Installed)
                return true;
            notice = failure + "\n\n";
        } else {
            Log("rejected " + localPath.string() + ": " + reason);
            notice = reason + ".\nO arquivo ficou no cartão SD sem alteração.\n\n";
        }
        if (setting == ts::OfferSetting::Never) {
            screen.Show(notice + "Aperte A para abrir o jogo.\n");
            return screen.Wait(HidNpadButton_A).has_value();
        }
    }

    for (;;) {
        screen.Show(notice + Offer(packExists, hdArtExists));
        const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X);
        if (!button) return false;
        if (*button & HidNpadButton_B) {
            Log("offer: not now");
            return true;
        }
        if (*button & HidNpadButton_X) {
            Log("offer: never");
            if (!SaveOfferSetting(ts::OfferSetting::Never)) {
                screen.Show("Não foi possível gravar config/" + std::string(ts::kOfferSettingName) +
                            ".\n\nAperte A para abrir o jogo.\n");
                return screen.Wait(HidNpadButton_A).has_value();
            }
            return true;
        }
        std::string failure;
        switch (DownloadAndBuild(screen, &failure)) {
        case Outcome::Installed:
            return true;
        case Outcome::Left:
            return false;
        case Outcome::Cancelled:
            Log("download cancelled");
            notice = "Download cancelado.\n\n";
            continue;
        case Outcome::Failed:
            break;
        }
        Log("failed: " + failure);
        screen.Show(failure + "\n\nVocê também pode baixar " + ts::DownloadArchive().FileName + " de\n" +
                    ts::kModPage + " num PC e copiar para\nsdmc:/switch/oot3dre/; a próxima abertura monta a partir dele.\n\n"
                    "  A  Tentar de novo\n"
                    "  B  Abrir o jogo sem elas (perguntar de novo na próxima vez)\n"
                    "  X  Não perguntar mais\n");
        const auto choice = screen.Wait(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X);
        if (!choice) return false;
        if (*choice & HidNpadButton_B) return true;
        if (*choice & HidNpadButton_X) {
            SaveOfferSetting(ts::OfferSetting::Never);
            return true;
        }
        notice.clear();
    }
}

} // namespace Oot3dNativeGame

#endif
