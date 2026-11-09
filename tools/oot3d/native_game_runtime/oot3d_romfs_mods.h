#pragma once

// RomFS mods on the Switch: files the player puts in
// sdmc:/switch/oot3dre/mods/<name>/romfs/ replace the game's own RomFS files
// of the same path (e.g. a Link model: mods/vanilla_plus_link/romfs/actor/
// zelda_link_boy_new.zar). L3+R3 > Game > Gameplay > "Mods (RomFS)"
// (config/no_romfs_mods, present = off; on by default).
//
// At start-up (oot3d_native_game.cpp, after the HUD texture import) the
// composer writes an O3RO overlay over game/romfs.bin's service view
// (oot3d_native_a32_romfs_overlay.h, from upstream coccofresco/TriAevum
// 9c90abb9) and the launch gets --romfs-overlay. game/romfs.bin is never
// written: each replaced file's 16-byte offset/size field in the Level-3 file
// table points at its new payload, appended 16-byte aligned after the view.
//
// - Mods apply in folder name order; a later mod's file wins over an earlier
//   one's (reported as overridden).
// - Paths match the view the import made: a EUR dump keeps its IVFC image
//   (view at 0x1000), a USA dump became a converted view at 0 where
//   message/us and misc/us are message/eu and misc/eu (us.qm is eu.qm, and
//   three hint movies moved, oot3d_rom_import.cpp). A mod path missing from
//   the view is tried under those names.
// - Replace only: files the game's RomFS lacks, exefs/ and code.ips (the code
//   is compiled ahead of time), files outside romfs/, empty files and files
//   over 64 MiB are ignored, with a reason.
// - A receipt (game/romfs_mods.json: every file under mods/ with its size
//   and mtime, romfs.bin's size and mtime, the view) lets an unchanged start
//   reuse the overlay. No applicable file: no overlay, nothing changes.
//
// Plain code: tested on the host (oot3d_romfs_mods_tests).

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace Oot3dNativeGame {

inline constexpr const char *kRomFsModsOffConfigName = "no_romfs_mods";
// A mod file larger than this is ignored (the guest's linear heap is 64 MiB).
inline constexpr std::uint64_t kRomFsModMaximumFileBytes = 64ULL * 1024ULL * 1024ULL;

inline bool ReadRomFsModsEnabled(const std::filesystem::path &config) {
  std::error_code error;
  return !std::filesystem::exists(config / kRomFsModsOffConfigName, error);
}

// Removes (on) or creates (off) the file; false if that failed.
inline bool SaveRomFsModsEnabled(const std::filesystem::path &config, bool enabled) {
  std::error_code error;
  if (enabled) {
    std::filesystem::remove(config / kRomFsModsOffConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kRomFsModsOffConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

// The RomFS service view the runtime exposes (the process manifest's
// romfs_image_path, romfs_service_offset and romfs_service_size).
struct RomFsServiceView {
  std::filesystem::path ImagePath;
  std::uint64_t Offset = 0;
  std::uint64_t Size = 0;
};

struct RomFsModsRequest {
  std::filesystem::path ModsDirectory; // mods/<name>/romfs/...
  RomFsServiceView View;
  std::filesystem::path OverlayPath;
  std::filesystem::path ReceiptPath;
  // Called once before an overlay is (re)built (e.g. a start-up screen line).
  std::function<void()> BeforeBuild;
};

struct RomFsModFile {
  std::string Mod;
  std::string Path;   // relative to the mod folder, e.g. romfs/actor/x.zar
  std::string Target; // RomFS path it replaces, e.g. actor/x.zar
  std::uint64_t Size = 0;
};

struct RomFsModNote {
  std::string Mod;
  std::string Path;
  std::string Reason;
};

enum class RomFsModsOutcome { NoMods, Built, Reused, Failed };

struct RomFsModsReport {
  RomFsModsOutcome Outcome = RomFsModsOutcome::NoMods;
  std::vector<std::string> Mods;        // folders with at least one applied file
  std::vector<RomFsModFile> Applied;    // one per replaced RomFS file
  std::vector<RomFsModNote> Ignored;
  std::vector<RomFsModNote> Overridden; // replaced by a later mod's file
  std::string Error;                    // Failed
  std::uint64_t OverlayBytes = 0;
  std::uint64_t AppendedBytes = 0;
  double Seconds = 0.0;

  // "romfs mods: 26 files from 1 mod (vanilla_plus_link), 0 ignored"
  std::string Summary() const;
  // The summary, then the overlay line, then up to `maxNotes` notes
  // (renamed, overridden, ignored) and a count of the rest.
  std::vector<std::string> LogLines(std::size_t maxNotes = 12U) const;
};

// Scans the mods, then reuses, builds or removes the overlay and receipt.
// Never throws; Failed leaves no overlay behind.
RomFsModsReport ComposeRomFsMods(const RomFsModsRequest &request);

// The RomFS path a mod's path (relative to romfs/) has in a USA dump's
// converted view, or nullopt when the import keeps it as it is.
std::optional<std::string> UsaConvertedRomFsPath(std::string_view path);

// The start-up step around the composer, shared by the Switch's main().
struct RomFsModsStartup {
  std::filesystem::path ConfigDirectory; // config/no_romfs_mods
  std::filesystem::path ModsDirectory;
  // The view from the process manifest; called only when mods may apply.
  std::function<std::optional<RomFsServiceView>(std::string *error)> ReadView;
  std::filesystem::path OverlayPath;
  std::filesystem::path ReceiptPath;
  std::function<void()> BeforeBuild;
};

struct RomFsModsStartupResult {
  std::optional<std::filesystem::path> Overlay; // --romfs-overlay, when set
  std::vector<std::string> LogLines;
  // boot-status romfs_mods=: off, none, active:<files>, failed (the window
  // setup makes it rejected when the overlay does not load).
  std::string BootStatus = "none";
  std::string PanelLine; // L3+R3 > Game > Gameplay, under the option
};

// Off (config/no_romfs_mods) or no mods folder: no scan, no overlay, no
// argument, as before RomFS mods existed.
RomFsModsStartupResult PrepareRomFsMods(const RomFsModsStartup &startup);

// What this start did, for boot-status and the Gameplay panel (written by
// main() before the game starts, read afterwards).
struct RomFsModsState {
  std::string BootStatus = "none";
  std::string PanelLine;
};
inline RomFsModsState &RomFsModsStartupState() {
  static RomFsModsState state;
  return state;
}

} // namespace Oot3dNativeGame
