#pragma once

// L3+R3 > Video > "Draw distance" (config/draw_distance, one word: far,
// farther or maximum; no file = kDefaultActorDrawDistance). Applied at once:
// the panel publishes the choice, the window session copies it into the
// actor visibility state every host frame (oot3d_native_a32_actor_visibility.h).
// config/draw_distance_native_groups (present = on, read at start): keep the
// tree and bush groups' offspring at the title's spawn range (A/B switch).
// Header-only; tested in oot3d_actor_visibility_policy_tests.

#include "oot3d_actor_visibility_policy.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr const char *kDrawDistanceNativeGroupsConfigName = "draw_distance_native_groups";
inline constexpr const char *kDrawDistanceConfigName = "draw_distance";
inline constexpr ActorDrawDistance kDefaultActorDrawDistance = ActorDrawDistance::Original;

inline ActorDrawDistance ReadActorDrawDistance(const std::filesystem::path &config) {
  std::ifstream file(config / kDrawDistanceConfigName);
  if (!file) return kDefaultActorDrawDistance;
  std::string word;
  file >> word;
  return ParseActorDrawDistance(word.c_str());
}

// Writes the choice (removes the file for the default); false if that failed.
inline bool SaveActorDrawDistance(const std::filesystem::path &config, ActorDrawDistance choice) {
  std::error_code error;
  if (choice == kDefaultActorDrawDistance) {
    std::filesystem::remove(config / kDrawDistanceConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kDrawDistanceConfigName, std::ios::trunc);
  file << ActorDrawDistanceWord(choice) << '\n';
  return static_cast<bool>(file);
}

// The choice in use; the Video panel stores it, the game thread reads it.
inline std::atomic<ActorDrawDistance> &LiveActorDrawDistance() {
  static std::atomic<ActorDrawDistance> choice{kDefaultActorDrawDistance};
  return choice;
}

inline bool ReadDrawDistanceNativeGroups(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kDrawDistanceNativeGroupsConfigName, error);
}

inline std::atomic<bool> &LiveDrawDistanceNativeGroups() {
  static std::atomic<bool> native{false};
  return native;
}

} // namespace Oot3dNativeGame
