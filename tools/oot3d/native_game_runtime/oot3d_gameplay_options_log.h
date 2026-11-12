#pragma once

// Game log line of the damage multiplier, quick put away, unmirrored Master
// Quest (L3+R3 > Game > Gameplay) and simple shadow (L3+R3 > Video) at start
// and on each change, written beside Roll dash's line
// (NativeA32WindowSession::UpdateRollDash).

#include "oot3d_damage_multiplier.h"
#include "oot3d_master_quest_mirror.h"
#include "oot3d_quick_put_away.h"
#include "oot3d_simple_shadow.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

namespace Oot3dNativeGame {

class GameplayOptionsLog final {
public:
  // The line if an option changed since the last one.
  [[nodiscard]] std::optional<std::string> Take(bool quickPutAway, std::uint8_t damage,
                                                bool unmirrored, bool unmirrorApplied,
                                                bool simpleShadow, Fast::Oot3d::PlayerShadowMethod shadowMethod,
                                                bool shadowCutscenes) {
    const std::array<std::uint8_t, 7> now{static_cast<std::uint8_t>(quickPutAway), damage,
                                          static_cast<std::uint8_t>(unmirrored),
                                          static_cast<std::uint8_t>(unmirrorApplied),
                                          static_cast<std::uint8_t>(simpleShadow),
                                          static_cast<std::uint8_t>(shadowMethod),
                                          static_cast<std::uint8_t>(shadowCutscenes)};
    if (mLogged == now) return std::nullopt;
    mLogged = now;
    std::string line = std::string("gameplay: quick_put_away=") + (quickPutAway ? "on" : "off") +
                       " damage=" + DamageMultiplierName(damage) +
                       " master_quest_unmirrored=" + (unmirrored ? "on" : "off");
    if (unmirrored != unmirrorApplied)
      line += " (applies at the next file load or pause menu close)";
    line += std::string(" simple_shadow=") + (simpleShadow ? "on" : "off") + " shadow_method=" +
            std::string(Fast::Oot3d::PlayerShadowMethodWord(shadowMethod)) +
            " shadow_cutscenes=" + (shadowCutscenes ? "on" : "off");
    line += " (L3+R3 > Game > Gameplay: config/quick_put_away, config/damage_multiplier, "
            "config/master_quest_unmirrored; L3+R3 > Video: config/simple_shadow, "
            "config/simple_shadow_method, config/simple_shadow_cutscenes)";
    return line;
  }

  // The options the hooks read now.
  [[nodiscard]] std::optional<std::string> Take() {
    return Take(QuickPutAwayEnabled().load(std::memory_order_relaxed),
                DamageMultiplier().load(std::memory_order_relaxed),
                MasterQuestUnmirrored().load(std::memory_order_relaxed),
                MasterQuestUnmirrorApplied().load(std::memory_order_relaxed),
                SimpleShadowEnabled().load(std::memory_order_relaxed),
                Fast::Oot3d::PlayerShadowBridge::Instance().Method(),
                SimpleShadowCutscenes().load(std::memory_order_relaxed));
  }

private:
  std::optional<std::array<std::uint8_t, 7>> mLogged;
};

} // namespace Oot3dNativeGame
