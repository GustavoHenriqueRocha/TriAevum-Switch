// Video settings files (oot3d_video_settings.h): what the boot would read,
// and that the debug menu's save round-trips through the same files.
#include "oot3d_video_settings.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Oot3dNativeGame::VideoSettings;

int gChecks = 0;
void Require(bool condition, const std::string &what) {
  ++gChecks;
  if (!condition) throw std::runtime_error(what);
}

void WriteText(const std::filesystem::path &path, const std::string &text) {
  std::ofstream(path, std::ios::trunc) << text;
}
} // namespace

int main() {
  try {
    const auto dir = std::filesystem::temp_directory_path() / "oot3d_video_settings_tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    // No files: the boot default (2x in both modes, 30 FPS).
    Require(VideoSettings::Load(dir) == VideoSettings{}, "defaults without files");

    // Without its own file the dock follows resolution_scale.
    WriteText(dir / "resolution_scale", "3\n");
    auto loaded = VideoSettings::Load(dir);
    Require(loaded.HandheldScale == 3.0F && loaded.DockedScale == 3.0F, "dock follows handheld");

    // Parser of the boot: decimal comma, quarter rounding, clamps.
    WriteText(dir / "resolution_scale_docked", "4,5");
    WriteText(dir / "resolution_scale", "7");
    loaded = VideoSettings::Load(dir);
    Require(loaded.DockedScale == 4.5F, "docked 4,5");
    Require(loaded.HandheldScale == 3.0F, "handheld clamped to 3");
    Require(VideoSettings::ParseScale("2.6", 4.5F) == 2.5F, "rounded to 1/4");
    Require(VideoSettings::ParseScale("x", 4.5F) == 1.0F, "unreadable is 1");
    Require(VideoSettings::ParseScale("6", 4.5F) == 4.5F, "docked clamped to 4.5");

    // Save and load again: same values, fps60 created and removed.
    VideoSettings saved{.DockedScale = 4.5F, .HandheldScale = 2.0F, .Fps60 = true};
    Require(saved.Save(dir).empty(), "save");
    Require(VideoSettings::Load(dir) == saved, "round trip with fps60");
    saved.Fps60 = false;
    saved.DockedScale = 3.5F;
    Require(saved.Save(dir).empty(), "save without fps60");
    Require(!std::filesystem::exists(dir / "fps60"), "fps60 removed");
    Require(VideoSettings::Load(dir) == saved, "round trip without fps60");

    std::filesystem::remove_all(dir);
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "oot3d_video_settings_tests: " << gChecks << " checks ok\n";
  return 0;
}
