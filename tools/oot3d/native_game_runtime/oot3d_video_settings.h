#pragma once

// Resolution and frame rate of the console, as the boot reads them from
// config/: resolution_scale_docked (dock, up to 4.5), resolution_scale
// (handheld, and the dock without its own file, up to 3; default 2) and
// fps60 (present: logic at 30, interpolated to 60); upscaler and sharpen
// (FSR EASU + RCAS 0.2 without them) and anisotropy (4x without it,
// gfx_opengl_pica_post.h). The debug menu's video
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

  enum class Upscale { FsrSharpen, Fsr, Linear };

  float DockedScale = kDefaultScale;
  float HandheldScale = kDefaultScale;
  bool Fps60 = false;
  Upscale Upscaler = Upscale::FsrSharpen;
  bool Anisotropy = true;

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
    std::string upscaler, sharpen, anisotropy;
    if (firstLine("upscaler", upscaler) && upscaler.rfind("fsr", 0) != 0 &&
        upscaler.rfind("FSR", 0) != 0) {
      result.Upscaler = Upscale::Linear;
    } else if (firstLine("sharpen", sharpen) && std::strtof(sharpen.c_str(), nullptr) <= 0.0F) {
      result.Upscaler = Upscale::Fsr;
    }
    // Only 2, 4, 8 and 16 filter; anything else ("1") turns it off.
    if (firstLine("anisotropy", anisotropy)) {
      result.Anisotropy = anisotropy == "2" || anisotropy == "4" || anisotropy == "8" ||
                          anisotropy == "16";
    }
    return result;
  }

  // Writes both scale files and creates or removes fps60; the upscaler,
  // sharpen and anisotropy files exist only away from the default. Empty on
  // success, otherwise what failed.
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
    const auto present = [&](const char *name, bool keep, const char *text) {
      if (keep) {
        std::ofstream file(config / name, std::ios::trunc);
        file << text;
        return static_cast<bool>(file);
      }
      std::filesystem::remove(config / name, error);
      return !error;
    };
    if (!present("fps60", Fps60, "")) return "fps60";
    if (!present("upscaler", Upscaler == Upscale::Linear, "linear\n")) return "upscaler";
    if (!present("sharpen", Upscaler != Upscale::FsrSharpen, "0\n")) return "sharpen";
    if (!present("anisotropy", !Anisotropy, "1\n")) return "anisotropy";
    return {};
  }
};

} // namespace Oot3dNativeGame
