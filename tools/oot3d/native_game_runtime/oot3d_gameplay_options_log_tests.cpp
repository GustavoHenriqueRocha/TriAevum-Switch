// Game log line of the Single Screen gameplay options (oot3d_gameplay_options_log.h).
#include "oot3d_gameplay_options_log.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Contains(const std::optional<std::string> &line, std::string_view text) {
  return line.has_value() && line->find(text) != std::string::npos;
}

void TestLine() {
  using Fast::Oot3d::PlayerShadowMethod;
  constexpr auto kTwoPass = PlayerShadowMethod::TwoPass;
  GameplayOptionsLog log;
  const auto start = log.Take();
  Require(Contains(start, "gameplay: quick_put_away=off damage=normal master_quest_unmirrored=off ") &&
              Contains(start, " simple_shadow=off shadow_method=cover shadow_cutscenes=off "),
          "the defaults at start");
  Require(!Contains(start, "applies at the next"), "nothing pending at start");
  Require(!log.Take().has_value(), "an unchanged setting is not logged again");
  Require(Contains(log.Take(true, 2U, false, false, false, kTwoPass, false), "quick_put_away=on damage=2x "),
          "a change");
  Require(Contains(log.Take(true, kDamageMultiplierOneHit, false, false, false, kTwoPass, false), "damage=one hit "),
          "one hit");
  const auto pending = log.Take(true, kDamageMultiplierOneHit, true, false, false, kTwoPass, false);
  Require(Contains(pending, "master_quest_unmirrored=on (applies at the next file load or pause menu close)"),
          "unmirrored before the game's read");
  const auto applied = log.Take(true, kDamageMultiplierOneHit, true, true, false, kTwoPass, false);
  Require(Contains(applied, "master_quest_unmirrored=on simple_shadow=off ") &&
              !Contains(applied, "applies at the next"),
          "logged again once applied");
  Require(Contains(log.Take(true, kDamageMultiplierOneHit, true, true, true, kTwoPass, false), "simple_shadow=on "),
          "the simple shadow");
  Require(Contains(log.Take(true, kDamageMultiplierOneHit, true, true, true, PlayerShadowMethod::Texture, false),
                   "shadow_method=texture "),
          "the shadow method");
  Require(Contains(log.Take(true, kDamageMultiplierOneHit, true, true, true, PlayerShadowMethod::Texture, true),
                   "shadow_cutscenes=on "),
          "the shadow in cutscenes");
  Require(!log.Take(true, kDamageMultiplierOneHit, true, true, true, PlayerShadowMethod::Texture, true).has_value(),
          "then quiet");
}

} // namespace

int main() {
  TestLine();
  std::puts("oot3d_gameplay_options_log_tests: ok");
  return 0;
}
