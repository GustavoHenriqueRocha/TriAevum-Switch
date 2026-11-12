#pragma once

// L3+R3 > Game > Gameplay > "Quick put away" (config/quick_put_away, present =
// on; off by default).
//
// After Link uses an item, Player_UpdateContextActionAndSequenceState gives him
// a 30-frame wait (one second at 30 Hz) before the action to put it away is
// offered: `mov r0, #30` at EUR 0x003C49CC, stored at player +0x21B by
// 0x003C4B00, which counts it down. The hook skips that `mov` and stores one
// frame instead, as upstream TriAevum's TopScreen gameplay option does
// (coccofresco, oot3d_top_screen_gameplay_runtime, GPL-3.0-or-later) after
// "OoT3D: Single Screen Experience" 3.0 by M-1, whose 3.2.2 also stores 1.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kQuickPutAwayHookPc = 0x003C49CCU;  // mov r0, #30
inline constexpr std::uint32_t kQuickPutAwayStorePc = 0x003C4B00U; // strb r0, [r4, #0x21B]
inline constexpr std::uint32_t kQuickPutAwayFrames = 1U;

// Read by the hook, written by the panel.
inline std::atomic<bool> &QuickPutAwayEnabled() noexcept {
  static std::atomic<bool> enabled{false};
  return enabled;
}

inline constexpr const char *kQuickPutAwayConfigName = "quick_put_away";

inline bool ReadQuickPutAwayEnabled(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kQuickPutAwayConfigName, error);
}

// Creates (on) or removes (off) the file; false if that failed.
inline bool SaveQuickPutAwayEnabled(const std::filesystem::path &config, bool enabled) {
  std::error_code error;
  if (!enabled) {
    std::filesystem::remove(config / kQuickPutAwayConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kQuickPutAwayConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

} // namespace Oot3dNativeGame
