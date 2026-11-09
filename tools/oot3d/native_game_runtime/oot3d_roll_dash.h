#pragma once

// L3+R3 > Game > Gameplay > "Roll dash" (config/roll_dash, present = on; off
// by default).
//
// SingleScreen 3.0's Roll Dash ("OoT3D: Single Screen Experience" by M-1):
// keep A held after a roll and Link dashes out of it, faster than his run,
// until A is released, he stops on the ground or something cancels the run.
// Ported from coccofresco/TriAevum's typed host port of the 3.0/3.1 payload
// (5acf8138 the dash, 54fe8edf the 3.1 cross-fade, 5b34bd61/3c2ed0f3 the Navi
// hat redirect, a3eeb1e2 the scene view while A is held); no payload code
// runs. The runtime is oot3d_roll_dash_runtime.h; this header holds what the
// hook list, the menu and the start-up import need without it.
//
// The dash animations come from the mod's romfs/actor/zelda_link_ext.zar,
// which the TopScreen import (oot3d_topscreen_import.h) extracts beside the
// HUD textures as config/zelda_link_ext.zar. Without it Link dashes with his
// normal run animation, played faster.

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame {

// Block entries of the dash (EUR), every one a block-entry hook of the window
// (oot3d_native_block_entry_hooks.cpp, so a whole_aot_hook_gates.json gate).
//
// Player_Update: 3.0 computes the dash state once per frame (0x005D8BA8) and
// the player update consumes it; here it is computed at the Player_Update
// entry, which sees the same player fields.
inline constexpr std::uint32_t kRollDashStateEntry = 0x001E1B54U;
// Navi's follow action (FUN_00129474, r5 the fairy, r4 fairy + 0x900, r7 the
// player + 0x1000). Returning to the hat (state 7) she has a reach timer
// (+0x9AE, 45 frames); natively its expiry gives up: state 0 and player
// +0x1714 |= 0x100000 (Navi out). At the dash speed she never reaches Link,
// so 3.0/3.1 (0x005F40AC / 0x005F4510) send the expiry to the "in the hat"
// block 0x00129680 (state 8) while dashing; without the dash the block stays
// native (upstream issues #91 #95 #99).
inline constexpr std::uint32_t kRollDashNaviSite = 0x0012966CU;
// oot3d_player_action_turn_in_place 0x00251B48 / 0x00251D34 speed loads
// (3.0 0x005F4088 / 0x005F4054) and FUN_002be660's roll animation call
// 0x002BE8B8 (3.0 0x005F4070, 3.1 0x005D11A8); hooked at their block entries.
inline constexpr std::uint32_t kRollDashTurnSpeedSite = 0x00251B40U;
inline constexpr std::uint32_t kRollDashMoveSpeedSite = 0x00251D34U;
inline constexpr std::uint32_t kRollDashRollSite = 0x002BE8B0U;
// ZAR_GetCSABByIndex (r0 archive, r1 index), the leaf 3.0 redirects
// (0x0034807C -> 0x005D10A8). Link's archive table stays native: the archive
// destructor (0x002F7188) frees every table entry through the allocator.
inline constexpr std::uint32_t kRollDashCsabQuery = 0x0034807CU;

// Sorted.
inline constexpr std::array<std::uint32_t, 6> kRollDashHookPcs{
    kRollDashNaviSite,      kRollDashStateEntry, kRollDashTurnSpeedSite,
    kRollDashMoveSpeedSite, kRollDashRollSite,   kRollDashCsabQuery};

// Read by the runtime at each host frame, written by the panel.
inline std::atomic<bool> &RollDashEnabled() noexcept {
  static std::atomic<bool> enabled{false};
  return enabled;
}

inline constexpr const char *kRollDashConfigName = "roll_dash";
// The mod's Link animation archive, installed by the TopScreen import.
inline constexpr const char *kRollDashAnimationsName = "zelda_link_ext.zar";

inline bool ReadRollDashEnabled(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kRollDashConfigName, error);
}

// Creates (on) or removes (off) the file; false if that failed.
inline bool SaveRollDashEnabled(const std::filesystem::path &config, bool enabled) {
  std::error_code error;
  if (!enabled) {
    std::filesystem::remove(config / kRollDashConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kRollDashConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

inline bool RollDashAnimationsInstalled(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::is_regular_file(config / kRollDashAnimationsName, error);
}

} // namespace Oot3dNativeGame
