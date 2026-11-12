#pragma once

// L3+R3 > Controls > Camera > Free camera > "Smoothing" (config/free_camera_smoothing:
// "off"; no file = on). A file of its own rather than a topscreen_ui.json field:
// builds up to 0.2.0 refuse to start on a topscreen_ui.json with a field they
// don't know, which would break rolling back to oot3d_native_game.nro.old.
// Applied at once: the Controls panel publishes it and the free camera reads it
// on its next update (oot3d_native_a32_candidate_dispatch.cpp). Header-only;
// tested in oot3d_top_screen_mod_profile_tests.

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr const char *kFreeCameraSmoothingConfigName = "free_camera_smoothing";

inline bool ParseFreeCameraSmoothing(const std::string &word) noexcept {
  return word != "off" && word != "0" && word != "false";
}

inline bool ReadFreeCameraSmoothing(const std::filesystem::path &config) {
  std::ifstream file(config / kFreeCameraSmoothingConfigName);
  if (!file) return true;
  std::string word;
  file >> word;
  return ParseFreeCameraSmoothing(word);
}

// Writes the choice (removes the file for the default); false if that failed.
inline bool SaveFreeCameraSmoothing(const std::filesystem::path &config, bool smoothing) {
  std::error_code error;
  if (smoothing) {
    std::filesystem::remove(config / kFreeCameraSmoothingConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kFreeCameraSmoothingConfigName, std::ios::trunc);
  file << "off\n";
  return static_cast<bool>(file);
}

// The choice in use; the Controls panel stores it, the game thread reads it.
inline std::atomic<bool> &LiveFreeCameraSmoothing() {
  static std::atomic<bool> smoothing{true};
  return smoothing;
}

} // namespace Oot3dNativeGame
