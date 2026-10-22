#pragma once

// Resolution and frame rate of the console, as the boot reads them from
// config/: resolution_scale_docked (dock, up to 4.5), resolution_scale
// (handheld, and the dock without its own file, up to 3; default 2) and
// fps60 (present: logic at 30, interpolated to 60). The debug menu's video
// tab (oot3d_video_settings_panel.cpp) writes the same files; like any edit
// of them, the change applies on the next start of the game.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace Oot3dNativeGame {

struct VideoSettings {
  static constexpr float kDefaultScale = 2.0F;
  static constexpr float kMaxHandheldScale = 3.0F;
  static constexpr float kMaxDockedScale = 4.5F;

  float DockedScale = kDefaultScale;
  float HandheldScale = kDefaultScale;
  bool Fps60 = false;

  bool operator==(const VideoSettings &) const = default;

  // Same rule as PicaGlScale::ParseResolutionScale: a multiple of 1/4 in
  // [1, maxScale]; "," is accepted for ".".
  static float ParseScale(std::string text, float maxScale) {
    std::replace(text.begin(), text.end(), ',', '.');
    const float value = std::strtof(text.c_str(), nullptr);
    if (!(value >= 1.0F)) return 1.0F;
    return std::clamp(std::round(value * 4.0F) / 4.0F, 1.0F, maxScale);
  }

  static VideoSettings Load(const std::filesystem::path &config) {
    const auto firstLine = [&](const char *name, std::string &text) {
      std::ifstream file(config / name);
      return file && std::getline(file, text);
    };
    VideoSettings result;
    std::string common, docked;
    if (firstLine("resolution_scale", common)) {
      result.HandheldScale = ParseScale(common, kMaxHandheldScale);
    }
    result.DockedScale = firstLine("resolution_scale_docked", docked)
                             ? ParseScale(docked, kMaxDockedScale)
                             : result.HandheldScale;
    std::error_code error;
    result.Fps60 = std::filesystem::exists(config / "fps60", error);
    return result;
  }

  // Writes both scale files and creates or removes fps60. Empty on success,
  // otherwise what failed.
  std::string Save(const std::filesystem::path &config) const {
    const auto write = [&](const char *name, float value) {
      std::ofstream file(config / name, std::ios::trunc);
      char text[16];
      std::snprintf(text, sizeof(text), "%.2f\n", value);
      file << text;
      return static_cast<bool>(file);
    };
    std::error_code error;
    std::filesystem::create_directories(config, error);
    if (!write("resolution_scale", HandheldScale)) return "resolution_scale";
    if (!write("resolution_scale_docked", DockedScale)) return "resolution_scale_docked";
    if (Fps60) {
      if (!std::ofstream(config / "fps60")) return "fps60";
    } else {
      std::filesystem::remove(config / "fps60", error);
      if (error) return "fps60";
    }
    return {};
  }
};

} // namespace Oot3dNativeGame
