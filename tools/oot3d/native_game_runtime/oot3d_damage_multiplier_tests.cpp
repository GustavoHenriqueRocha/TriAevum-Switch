// Damage taken: Health_ChangeBy's change at its entry (oot3d_damage_multiplier.h)
// and the config file.
#include "oot3d_damage_multiplier.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

// Health_ChangeBy after the hook: Master Quest doubles damage, the double
// defence halves it, health stays within 0 and the maximum.
std::int32_t GameHealthAfter(std::int32_t health, std::int32_t maximum, std::int32_t change, bool masterQuest,
                             bool doubleDefence) {
  if (masterQuest && change < 0) change = static_cast<std::int16_t>(change * 2);
  if (doubleDefence && change < 0) change >>= 1;
  health = static_cast<std::int16_t>(health + change);
  return health > maximum ? maximum : health < 0 ? 0 : health;
}

void TestScale() {
  Require(ScaleHealthChange(-8, kDamageMultiplierNormal) == -8, "normal: as given");
  Require(ScaleHealthChange(-8, 2U) == -16 && ScaleHealthChange(-8, 10U) == -80, "2x and 10x");
  Require(ScaleHealthChange(-8, 40U) == -80, "above 10x: 10x");
  Require(ScaleHealthChange(16, 4U) == 16 && ScaleHealthChange(0, 4U) == 0, "healing and nothing unchanged");
  Require(ScaleHealthChange(-1, kDamageMultiplierOneHit) == kDamageOneHitChange, "one hit");
  Require(ScaleHealthChange(-0x3000, 10U) == kDamageLargestChange, "clamped");
  // Master Quest doubles the clamped change without wrapping to a heal.
  Require(static_cast<std::int16_t>(kDamageLargestChange * 2) < 0, "doubled clamp stays damage");
}

void TestWithTheGame() {
  // Full 20 hearts, the strongest defence: one hit still kills.
  Require(GameHealthAfter(320, 320, ScaleHealthChange(-1, kDamageMultiplierOneHit), false, true) == 0,
          "one hit through the double defence");
  Require(GameHealthAfter(320, 320, ScaleHealthChange(-1, kDamageMultiplierOneHit), true, true) == 0,
          "one hit in Master Quest");
  // Half a heart at 4x in Master Quest: 8 * 4 * 2 = 64 (four hearts).
  Require(GameHealthAfter(320, 320, ScaleHealthChange(-8, 4U), true, false) == 256, "stacks with Master Quest");
  Require(GameHealthAfter(320, 320, ScaleHealthChange(-8, 4U), false, true) == 304, "halved by the double defence");
  Require(GameHealthAfter(100, 320, ScaleHealthChange(48, 4U), true, false) == 148, "a heal is not scaled");
}

void TestConfig() {
  Require(ParseDamageMultiplier("2") == 2U && ParseDamageMultiplier("10\n") == 10U, "2 and 10");
  Require(ParseDamageMultiplier("  ohko ") == kDamageMultiplierOneHit, "ohko");
  for (const char *text : {"", "1", "11", "0", "x2", "-2", "100", "OHKO"})
    Require(ParseDamageMultiplier(text) == kDamageMultiplierNormal, "anything else is normal");
  const auto config = std::filesystem::temp_directory_path() / "oot3d_damage_multiplier_tests";
  std::error_code ignored;
  std::filesystem::remove_all(config, ignored);
  Require(ReadDamageMultiplier(config) == kDamageMultiplierNormal, "no file: normal");
  Require(SaveDamageMultiplier(config, 4U) && ReadDamageMultiplier(config) == 4U, "4x saved");
  Require(SaveDamageMultiplier(config, kDamageMultiplierOneHit) &&
              ReadDamageMultiplier(config) == kDamageMultiplierOneHit,
          "one hit saved");
  Require(SaveDamageMultiplier(config, kDamageMultiplierNormal) &&
              !std::filesystem::exists(config / kDamageMultiplierConfigName),
          "normal removes the file");
  std::filesystem::remove_all(config, ignored);
}

} // namespace

int main() {
  TestScale();
  TestWithTheGame();
  TestConfig();
  std::puts("oot3d_damage_multiplier_tests: ok");
  return 0;
}
