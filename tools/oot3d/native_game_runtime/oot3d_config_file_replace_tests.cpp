// Host tests for replacing a saved file on the Switch (ship/config/FileReplace.h).
//
// Horizon's FS (the Switch SD card) refuses to rename over an existing file
// with EEXIST, where POSIX and Windows replace it. Saving a setting a second
// time therefore failed on the console with "cannot replace TopScreen UI
// config: File exists". This binary defines its own rename(), which
// libstdc++'s std::filesystem::rename calls, so the writers below run against
// Horizon's semantics on the PC: in Horizon mode the move refuses an existing
// destination (renameat2 RENAME_NOREPLACE: EEXIST, atomically), and a chosen
// call can be made to fail.
#include "oot3d_native_control_config.h"
#include "oot3d_top_screen_config.h"

#include "fast/renderer/cache_file.h"
#include "ship/config/ConfigPersistence.h"
#include "ship/config/FileReplace.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0U)
#endif

namespace {
bool gHorizon = false;   // rename refuses an existing destination
int gRenameCalls = 0;    // renames since the last Reset
int gFailRenameCall = 0; // that call fails with EIO (0: none)
} // namespace

// Interposes libc's rename for this process (libstdc++ included).
extern "C" __attribute__((visibility("default"))) int rename(const char *from,
                                                             const char *to) __THROW {
  ++gRenameCalls;
  if (gRenameCalls == gFailRenameCall) {
    errno = EIO;
    return -1;
  }
  const unsigned flags = gHorizon ? RENAME_NOREPLACE : 0U;
  return static_cast<int>(syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, flags));
}

namespace {
namespace fs = std::filesystem;
using namespace Oot3dNativeGame;

int gFailures = 0;

void Require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

void Reset(bool horizon, int failCall = 0) {
  gHorizon = horizon;
  gRenameCalls = 0;
  gFailRenameCall = failCall;
}

void Write(const fs::path &path, const std::string &text) {
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

std::string Read(const fs::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

fs::path Bak(const fs::path &path) { return Ship::ReplaceBackupPath(path); }

fs::path Fresh(const fs::path &root, const char *name) {
  fs::remove_all(root / name);
  fs::create_directories(root / name);
  return root / name;
}

// The interposer is in effect: the old writers' single rename fails as on the console.
void TestHorizonRenameRefusesExistingFile(const fs::path &root) {
  const auto dir = Fresh(root, "selfcheck");
  Write(dir / "a.json", "old");
  Write(dir / "a.json.tmp", "new");
  Reset(true);
  std::error_code error;
  fs::rename(dir / "a.json.tmp", dir / "a.json", error);
  Require(gRenameCalls == 1, "std::filesystem::rename goes through the interposed rename");
  Require(error == std::errc::file_exists, "Horizon mode: rename over a file gives EEXIST");
  Require(error.message() == "File exists", "the console's message: File exists");
  Require(Read(dir / "a.json") == "old", "the refused rename keeps the target");
  Reset(false);
  fs::rename(dir / "a.json.tmp", dir / "a.json", error);
  Require(!error && Read(dir / "a.json") == "new", "POSIX mode: rename replaces");
}

void TestReplace(const fs::path &root, bool horizon) {
  const auto dir = Fresh(root, horizon ? "replace-horizon" : "replace-posix");
  const auto target = dir / "c.json", temporary = dir / "c.json.tmp";

  Reset(horizon);
  Write(temporary, "first");
  Require(!Ship::ReplaceFileWithTemporary(temporary, target), "replace creates a missing target");
  Require(Read(target) == "first" && !fs::exists(temporary) && !fs::exists(Bak(target)),
          "created: target written, no temporary, no backup");
  Require(gRenameCalls == 1, "a missing target takes one rename");

  Reset(horizon);
  Write(temporary, "second");
  Require(!Ship::ReplaceFileWithTemporary(temporary, target), "replace over an existing target");
  Require(Read(target) == "second" && !fs::exists(temporary) && !fs::exists(Bak(target)),
          "replaced: new contents, no temporary, no backup");
  Require(gRenameCalls == (horizon ? 3 : 1), "Horizon: refused, aside, into place; POSIX: one rename");

  // A backup left by an earlier completed replace (the target exists) is stale.
  Reset(horizon);
  Write(Bak(target), "stale");
  Write(temporary, "third");
  Require(!Ship::ReplaceFileWithTemporary(temporary, target), "replace with a stale backup");
  Require(Read(target) == "third", "stale backup: new contents");
  Require(fs::exists(Bak(target)) == !horizon, "Horizon drops the stale backup; POSIX never touches it");
  fs::remove(Bak(target));
}

void TestReplaceFailures(const fs::path &root) {
  const auto dir = Fresh(root, "failures");
  const auto target = dir / "c.json", temporary = dir / "c.json.tmp";
  Write(target, "old");

  // Moving the old file aside fails: nothing changes, the temporary stays.
  Reset(true, 2);
  Write(temporary, "new");
  Require(static_cast<bool>(Ship::ReplaceFileWithTemporary(temporary, target)), "a failed move aside is reported");
  Require(Read(target) == "old" && Read(temporary) == "new" && !fs::exists(Bak(target)),
          "failed move aside: target intact, temporary kept");

  // Moving the temporary into place fails: the old file comes back.
  Reset(true, 3);
  Require(static_cast<bool>(Ship::ReplaceFileWithTemporary(temporary, target)), "a failed move into place is reported");
  Require(Read(target) == "old" && Read(temporary) == "new" && !fs::exists(Bak(target)),
          "failed move into place: target restored, no backup");

  // Other errors are not taken for Horizon's refusal.
  Reset(true, 1);
  Require(static_cast<bool>(Ship::ReplaceFileWithTemporary(temporary, target)) && gRenameCalls == 1,
          "a non-EEXIST failure is reported without the fallback");
  Require(Read(target) == "old", "non-EEXIST failure: target intact");
  Reset(false);
}

// The state a power cut or a closed game leaves between the two moves: the
// target is gone, its backup holds the last complete file.
void TestRecovery(const fs::path &root) {
  const auto dir = Fresh(root, "recovery");
  const auto target = dir / "c.json";
  Reset(true);
  Write(Bak(target), "last");
  Write(dir / "c.json.tmp", "unsaved");
  Require(Ship::RestoreInterruptedReplace(target), "recovery succeeds");
  Require(Read(target) == "last" && !fs::exists(Bak(target)), "recovery brings the backup back");
  Require(Ship::RestoreInterruptedReplace(target) && Read(target) == "last", "recovery again is a no-op");

  Write(Bak(target), "stale");
  Require(Ship::RestoreInterruptedReplace(target) && Read(target) == "last",
          "a target with a backup beside it is kept");
  fs::remove(Bak(target));
  Require(Ship::RestoreInterruptedReplace(dir / "missing.json") && !fs::exists(dir / "missing.json"),
          "nothing to recover: no file appears");
  Require(Ship::RestoreInterruptedReplace({}), "an empty path is a no-op");
  Reset(false);
}

// The report: the TopScreen settings saved once, then again over the file.
void TestTopScreenConfig(const fs::path &root) {
  const auto dir = Fresh(root, "topscreen");
  const auto path = dir / "topscreen_ui.json";
  Reset(true);
  TopScreenUiConfig config;
  std::string error;
  Require(SaveTopScreenUiConfig(path, config, &error), "TopScreen: first save");
  config.FreeCameraEnabled = true;
  config.CameraZoomPercent = 110U;
  error.clear();
  Require(SaveTopScreenUiConfig(path, config, &error), "TopScreen: save over the existing file (the report)");
  Require(error.empty(), "TopScreen: no 'cannot replace TopScreen UI config: File exists'");
  TopScreenUiConfig loaded;
  Require(LoadTopScreenUiConfig(path, &loaded, &error) && loaded == config, "TopScreen: the second save is read back");
  Require(!fs::exists(dir / "topscreen_ui.json.tmp") && !fs::exists(Bak(path)), "TopScreen: no leftovers");

  fs::rename(path, Bak(path));
  Write(dir / "topscreen_ui.json.tmp", "{");
  TopScreenUiConfig recovered;
  Require(LoadTopScreenUiConfig(path, &recovered, &error) && recovered == config,
          "TopScreen: load after an interrupted save reads the last complete file");
  Require(fs::exists(path) && !fs::exists(Bak(path)), "TopScreen: the backup is back in place");
  Reset(false);
}

void TestControlConfig(const fs::path &root) {
  const auto dir = Fresh(root, "controls");
  const auto path = dir / "oot3d_controls.json";
  Reset(true);
  auto config = NativeControlPreset(NativeControlProfile::Controller);
  std::string error;
  Require(SaveNativeControlConfig(path, config, &error), "controls: first save");
  config.NativeAimInvertY = !config.NativeAimInvertY;
  Require(SaveNativeControlConfig(path, config, &error), "controls: save over the existing file");
  NativeControlConfig loaded;
  Require(LoadNativeControlConfig(path, &loaded, &error) && loaded == config, "controls: the second save is read back");
  Require(!fs::exists(Bak(path)), "controls: no backup left");

  fs::rename(path, Bak(path));
  NativeControlConfig recovered;
  Require(LoadNativeControlConfig(path, &recovered, &error) && recovered == config,
          "controls: load after an interrupted save reads the last complete file");
  Reset(false);
}

// Ship::Config, dev warps, language, save-state preferences, environments.
void TestShipConfigWriter(const fs::path &root) {
  const auto dir = Fresh(root, "ship");
  const auto path = dir / "triaevum.json";
  Reset(true);
  Require(Ship::WriteConfigFileAtomically(path, "{\"a\":1}").Success, "ship: first write");
  const auto second = Ship::WriteConfigFileAtomically(path, "{\"a\":2}");
  Require(second.Success && second.Error.empty(), "ship: write over the existing file");
  Require(Read(path) == "{\"a\":2}" && !fs::exists(Bak(path)), "ship: new contents, no backup");
  for (const auto &entry : fs::directory_iterator(dir))
    Require(entry.path().filename() == "triaevum.json", "ship: no temporary left");
  Reset(false);
}

void TestCacheFileWriter(const fs::path &root) {
  const auto dir = Fresh(root, "cache");
  const auto path = dir / "entry.bin";
  const std::uint8_t first[] = {1, 2}, second[] = {3, 4, 5};
  Reset(true);
  std::string error;
  Require(Fast::Renderer::WriteCacheFileAtomically(path, {}, first, &error), "cache: first write");
  Require(Fast::Renderer::WriteCacheFileAtomically(path, {}, second, &error), "cache: write over the existing entry");
  Require(Read(path) == std::string("\x03\x04\x05") && !fs::exists(Bak(path)), "cache: new entry, no backup");
  Reset(false);
}

} // namespace

int main() {
  const auto root = fs::temp_directory_path() / ("oot3d_config_file_replace_tests." + std::to_string(getpid()));
  fs::create_directories(root);
  TestHorizonRenameRefusesExistingFile(root);
  TestReplace(root, false);
  TestReplace(root, true);
  TestReplaceFailures(root);
  TestRecovery(root);
  TestTopScreenConfig(root);
  TestControlConfig(root);
  TestShipConfigWriter(root);
  TestCacheFileWriter(root);
  fs::remove_all(root);
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_config_file_replace_tests: ok");
  return EXIT_SUCCESS;
}
