#pragma once

// L3+R3 > Game > Gameplay > "Damage taken" (config/damage_multiplier: "2" to
// "10" or "ohko"; no file = normal).
//
// Every change of Link's health goes through Health_ChangeBy (EUR 0x00352DBC,
// r1 = the change as a sign-extended s16, negative for damage; health is in
// sixteenths of a heart at save context +0x44). After its entry the game
// doubles damage in Master Quest (save context +0x0E) and then halves it with
// the double defence (+0x51). The hook at the entry multiplies a negative
// change before both, as "OoT3D: Single Screen Experience" 3.2.2 by M-1 does
// (2x to 10x, or one hit): Master Quest still doubles the result and the
// double defence still halves it. Healing is never changed. The rule is plain
// code, tested on the host (oot3d_damage_multiplier_tests).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kDamageMultiplierHookPc = 0x00352DBCU; // Health_ChangeBy entry

inline constexpr std::uint8_t kDamageMultiplierNormal = 1U;
inline constexpr std::uint8_t kDamageMultiplierMaximum = 10U;
inline constexpr std::uint8_t kDamageMultiplierOneHit = 0xFFU;
// More than the most health (20 hearts, 320) even after the double defence
// halves it.
inline constexpr std::int32_t kDamageOneHitChange = -1025;
// Master Quest's doubling must still fit in the s16 the game stores.
inline constexpr std::int32_t kDamageLargestChange = -0x3FFF;

// The change Health_ChangeBy applies, from the one it was given.
constexpr std::int32_t ScaleHealthChange(std::int32_t change, std::uint8_t multiplier) noexcept {
  if (change >= 0 || multiplier <= kDamageMultiplierNormal) return change;
  if (multiplier == kDamageMultiplierOneHit) return kDamageOneHitChange;
  return std::max(change * std::min(multiplier, kDamageMultiplierMaximum), kDamageLargestChange);
}

// Read by the hook at every health change, written by the panel.
inline std::atomic<std::uint8_t> &DamageMultiplier() noexcept {
  static std::atomic<std::uint8_t> multiplier{kDamageMultiplierNormal};
  return multiplier;
}

// The panel's choices, in order.
inline constexpr std::array<std::uint8_t, 11> kDamageMultiplierChoices{
    kDamageMultiplierNormal, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, kDamageMultiplierMaximum,
    kDamageMultiplierOneHit};

// The panel's and the game log's name: "normal", "2x" to "10x", "one hit".
inline std::string DamageMultiplierName(std::uint8_t multiplier) {
  if (multiplier == kDamageMultiplierOneHit) return "one hit";
  if (multiplier <= kDamageMultiplierNormal || multiplier > kDamageMultiplierMaximum) return "normal";
  return std::to_string(multiplier) + "x";
}

inline constexpr const char *kDamageMultiplierConfigName = "damage_multiplier";

// The file's first word: 2 to 10, or "ohko"; anything else is normal.
inline std::uint8_t ParseDamageMultiplier(std::string_view text) noexcept {
  const auto start = text.find_first_not_of(" \t\r\n");
  if (start == std::string_view::npos) return kDamageMultiplierNormal;
  text = text.substr(start, text.find_first_of(" \t\r\n", start) - start);
  if (text == "ohko") return kDamageMultiplierOneHit;
  if (text.empty() || text.size() > 2U) return kDamageMultiplierNormal;
  unsigned value = 0U;
  for (const char c : text) {
    if (c < '0' || c > '9') return kDamageMultiplierNormal;
    value = value * 10U + static_cast<unsigned>(c - '0');
  }
  return value >= 2U && value <= kDamageMultiplierMaximum ? static_cast<std::uint8_t>(value)
                                                          : kDamageMultiplierNormal;
}

inline std::uint8_t ReadDamageMultiplier(const std::filesystem::path &config) {
  std::ifstream file(config / kDamageMultiplierConfigName);
  std::string word;
  if (!(file >> word)) return kDamageMultiplierNormal;
  return ParseDamageMultiplier(word);
}

// Normal removes the file; false if writing it failed.
inline bool SaveDamageMultiplier(const std::filesystem::path &config, std::uint8_t multiplier) {
  std::error_code error;
  if (multiplier <= kDamageMultiplierNormal) {
    std::filesystem::remove(config / kDamageMultiplierConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kDamageMultiplierConfigName, std::ios::trunc);
  file << (multiplier == kDamageMultiplierOneHit ? std::string("ohko") : std::to_string(multiplier)) << '\n';
  return static_cast<bool>(file);
}

} // namespace Oot3dNativeGame
