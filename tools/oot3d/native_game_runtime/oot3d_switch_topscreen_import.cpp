#include "oot3d_switch_topscreen_import.h"

#if defined(__SWITCH__)

#include "oot3d_rom_import.h"
#include "oot3d_switch_console_screen.h"
#include "oot3d_switch_network.h"
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
    return std::string("The HUD and menu art of this port comes from the mod\n  ") + ts::kModName + " " +
           archive.Version + " by " + ts::kModAuthor + "\n  " + ts::kModPage +
           "\nIts license (" + ts::kModLicense + ") lets you download and install\n"
           "it, but not share it, so it is not included with this game.\n\n";
}

// `upgrade`: the pack exists but was built before the HD UI atlas was.
std::string Offer(bool upgrade) {
    const ts::Archive& archive = ts::DownloadArchive();
    const std::string size = std::to_string((archive.Bytes + 500'000) / 1'000'000) + " MB";
    const std::string what =
        upgrade ? "Your HUD textures were built by an earlier version. This one also\n"
                  "builds the mod's HD HUD art (sharp buttons, D-pad and labels at\n"
                  "720p), which needs the mod again: the console can download it\n"
                  "from GameBanana (" + size + ") and rebuild the textures from your own\n"
                  "game data. Nothing else from the mod is installed, and the download\n"
                  "is deleted afterwards. Without it, the HUD keeps its current art.\n\n"
                : "The console can download it from GameBanana (" + size + ") and build the\n"
                  "textures, the HD HUD art and the HD text font from your own game data.\n"
                  "Nothing else from the mod is installed, and the download is deleted\n"
                  "afterwards. Without them, parts of the HUD may be missing.\n\n";
    return Attribution(archive) + what +
           "  A  Download (" + size + ") and start the game\n"
           "  B  Not now (ask again at the next start)\n"
           "  X  Never ask again (L3+R3 > Updates can turn this back on)\n";
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
        finish(false, "No network: connect the console to the internet and try again.");
        return;
    }
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> out(std::fopen(path.c_str(), "wb"), &std::fclose);
    if (!out) {
        finish(false, "Cannot write " + path.string() + ".");
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
        finish(false, "Download cancelled.");
    } else if (result == SwitchNetwork::GetResult::Failed) {
        finish(false, "The download from GameBanana failed: " + error + ".");
    } else if (!closed) {
        finish(false, "Cannot write " + path.string() + " (SD card full?).");
    } else if (transfer.Received != archive.Bytes || digest != archive.Sha256) {
        finish(false, "GameBanana sent a different file than the " + std::string(archive.Version) +
                          " release this\ngame knows (" + std::to_string(transfer.Received) +
                          " bytes, SHA-256\n" + digest + ").\n"
                          "The mod may have been updated; a later version of this game will\n"
                          "support it. Nothing was installed.");
    } else {
        finish(true, {});
    }
}

enum class Outcome { Installed, Failed, Cancelled, Left };

// Builds and installs from a verified archive.
Outcome BuildFrom(SwitchConsoleScreen& screen, const fs::path& archivePath, const ts::Archive& archive,
                  const std::string& header, std::string* failure) {
    screen.Show(header + "Building the textures from game/romfs.bin...\n");
    try {
        const ts::Outputs outputs = ts::Build(archivePath, archive, Root() / "game" / "romfs.bin");
        ts::Install(outputs, Config());
        Log("installed " + std::string(ts::kPackName) + " (" + std::to_string(outputs.Pack.size()) +
            " bytes), " + ts::kFontPackName + " (" + std::to_string(outputs.Fonts.size()) + " bytes) and " +
            (outputs.UiAtlasHd.empty() ? std::string("no HD UI atlas")
                                       : std::string(ts::kUiAtlasHdName) + " (" +
                                             std::to_string(outputs.UiAtlasHd.size()) + " bytes)") +
            " from " + archivePath.filename().string() + " " + archive.Version);
        return Outcome::Installed;
    } catch (const std::exception& error) {
        *failure = std::string("Could not build the textures: ") + error.what() + ".";
        return Outcome::Failed;
    }
}

Outcome DownloadAndBuild(SwitchConsoleScreen& screen, std::string* failure) {
    const ts::Archive& archive = ts::DownloadArchive();
    const std::string header = Attribution(archive);
    std::error_code error;
    const auto space = fs::space(Root(), error);
    if (!error && space.available < archive.Bytes + kSpareBytes) {
        *failure = "Not enough space on the SD card: " + Megabytes(archive.Bytes + kSpareBytes) + " needed, " +
                   Megabytes(space.available) + " free.";
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
            screen.Show(header + "Downloading " + archive.FileName + " from GameBanana...\n" +
                        Bar(done, archive.Bytes) + "  " + Megabytes(done) + " / " + Megabytes(archive.Bytes) +
                        (cancelling ? "\n\nCancelling...\n" : "\n\n  B  Cancel\n"));
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
// release its name stands for (IdentifyArchiveFile checks it).
std::optional<std::pair<fs::path, const ts::Archive*>> LocalArchive() {
    for (const ts::Archive& archive : ts::KnownArchives()) {
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
    const bool hdAtlasExists = fs::is_regular_file(Config() / ts::kUiAtlasHdName, error);
    const bool romfsExists = fs::is_regular_file(Root() / "game" / "romfs.bin", error);
    if ((packExists && hdAtlasExists) || !romfsExists) return true;
    // A download interrupted by closing the app.
    for (const ts::Archive& archive : ts::KnownArchives()) {
        fs::path partial = Root() / archive.FileName;
        partial += ".partial";
        fs::remove(partial, error);
    }
    const auto setting = ts::ParseOfferSetting(ReadText(Config() / ts::kOfferSettingName));
    auto local = LocalArchive();
    if (local && !ts::ShouldBuildFromLocalArchive(packExists, hdAtlasExists, *local->second)) local.reset();
    if (!local && !ts::ShouldOffer(packExists, hdAtlasExists, romfsExists, setting)) return true;
    if (packExists) Log("the pack has no HD UI atlas (" + std::string(ts::kUiAtlasHdName) + ")");

    SwitchConsoleScreen& screen = StartupScreen("Ocarina of Time 3D - HUD textures");
    std::string notice;
    if (local) {
        const fs::path& localPath = local->first;
        // The player copied the archive here: build from it without asking.
        screen.Show("Checking " + localPath.filename().string() + "...\n");
        std::string reason;
        const ts::Archive* archive = ts::IdentifyArchiveFile(localPath, &reason, [&](std::uint64_t done, std::uint64_t total) {
            if (done % (8U << 20U) < (1U << 20U) || done == total)
                screen.Show("Checking " + localPath.filename().string() + "...\n" + Bar(done, total) + "\n");
        });
        if (archive) {
            std::string failure;
            if (BuildFrom(screen, localPath, *archive, Attribution(*archive), &failure) == Outcome::Installed)
                return true;
            notice = failure + "\n\n";
        } else {
            Log("rejected " + localPath.string() + ": " + reason);
            notice = reason + ".\nIt was left on the SD card untouched.\n\n";
        }
        if (setting == ts::OfferSetting::Never) {
            screen.Show(notice + "Press A to start the game.\n");
            return screen.Wait(HidNpadButton_A).has_value();
        }
    }

    for (;;) {
        screen.Show(notice + Offer(packExists));
        const auto button = screen.Wait(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X);
        if (!button) return false;
        if (*button & HidNpadButton_B) {
            Log("offer: not now");
            return true;
        }
        if (*button & HidNpadButton_X) {
            Log("offer: never");
            if (!SaveOfferSetting(ts::OfferSetting::Never)) {
                screen.Show("Could not write config/" + std::string(ts::kOfferSettingName) +
                            ".\n\nPress A to start the game.\n");
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
            notice = "Download cancelled.\n\n";
            continue;
        case Outcome::Failed:
            break;
        }
        Log("failed: " + failure);
        screen.Show(failure + "\n\nYou can also download " + ts::DownloadArchive().FileName + " from\n" +
                    ts::kModPage + " on a PC and copy it to\nsdmc:/switch/oot3dre/; the next start builds from it.\n\n"
                    "  A  Try again\n"
                    "  B  Start the game without them (ask again next time)\n"
                    "  X  Never ask again\n");
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
