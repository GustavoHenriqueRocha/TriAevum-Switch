#pragma once

// L3+R3 > Game > Gameplay > "Landing roll (N64)" (config/no_landing_roll,
// present = off; on by default).
//
// On N64 a landing with the stick held forward after a fall of 80-800 units
// ends in a roll, with no fall damage and no stun (zeldaret/oot z_player.c,
// Player_Action_8084411C). OoT3D still has that branch (EUR 0x004BC0E8..
// 0x004BC160 in the fall action FUN_004bba4c) but dropped the mid-air switch
// to the landing animation (N64 z_player.c:9604; OoT3D keeps only the
// actionVar2 = -1 store at 0x004BBE10). A run jump therefore keeps its
// animation (0x6B) until the ground, and the run-jump-end check at 0x004BC080
// wins over the roll: Link rolls after walking off a ledge, never after
// jumping off one.
//
// The hook at 0x004BC080 (a block-entry hook of the runtime, no generated
// code changes) sends exactly the landings N64 would have rolled to the
// game's own roll setup at 0x004BC120 (Player_SetupAction(Player_Action_Roll)
// + the landing-roll animation, then the action's exit; fall damage
// FUN_002bc420 is skipped as on N64). Every other landing runs the game's own
// code. The decision and the field reads are plain code, tested on the host
// (oot3d_landing_roll_tests).
//
// Reference only: the rule (thresholds 80/800, stick forward, not carrying,
// lock-on first) and the identifiers in the comments come from reading the
// zeldaret/oot decompilation (https://github.com/zeldaret/oot, z_player.c).
// That repository carries no license, so no code is copied: the decision
// below is written from the behaviour and checked against the OoT3D code.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame {

// Fall action FUN_004bba4c, landing branch (bgCheckFlags & GROUND, not the
// stateFlags2 0x80000 jump-attack path): `ldr r0,[r4,#0x284]; cmp r0,#0x6B`.
inline constexpr std::uint32_t kLandingRollHookPc = 0x004BC080U;
// Same branch: the game's roll setup, reached on OoT3D only without a run jump.
inline constexpr std::uint32_t kLandingRollSetupPc = 0x004BC120U;

// Player fields (EUR), read from r4 at the hook.
inline constexpr std::uint32_t kLandingRollAnimationOffset = 0x0284U;      // skelAnime.animation (id)
inline constexpr std::uint32_t kLandingRollStateFlags1Offset = 0x1710U;   // stateFlags1
inline constexpr std::uint32_t kLandingRollStickIndexOffset = 0x222AU;    // controlStickDataIndex (u8)
inline constexpr std::uint32_t kLandingRollStickDirectionsOffset = 0x2231U;  // controlStickDirections (s8[4])
inline constexpr std::uint32_t kLandingRollActionVar2Offset = 0x2238U;    // av2 (s16)
inline constexpr std::uint32_t kLandingRollFallDistanceOffset = 0x2282U;  // fallDistance (s16)

inline constexpr std::uint32_t kLandingRollRunJumpAnimation = 0x6BU;   // gPlayerAnim_link_normal_run_jump
inline constexpr std::uint32_t kLandingRollStateHostileLockOn = 0x10U;  // Player_CheckHostileLockOn (0x003518CC)
inline constexpr std::uint32_t kLandingRollStateCarrying = 0x800U;      // PLAYER_STATE1_CARRYING_ACTOR
inline constexpr std::uint8_t kLandingRollStickForward = 0U;            // PLAYER_STICK_DIR_FORWARD

struct LandingRollInputs {
  std::uint32_t Animation = 0U;
  std::uint32_t StateFlags1 = 0U;
  std::int16_t ActionVar2 = 0;
  std::int16_t FallDistance = 0;
  std::uint8_t StickDirection = 0xFFU;
};

enum class LandingRollChoice : std::uint8_t { GameLanding, Roll };

// N64's landing order for the run-jump case OoT3D lost. actionVar2 < 0 means
// the fall passed the point where N64 replaced the run-jump animation, so N64
// would not have taken the run-jump-end branch; a run jump that lands before
// that point (actionVar2 >= 0) keeps it on both. Then, as on N64: lock-on
// landing first, short landing at <= 80, roll below 800 with the stick
// forward and nothing carried. Landings without the run-jump animation already
// reach the game's own roll check.
constexpr LandingRollChoice DecideLandingRoll(const LandingRollInputs &in) noexcept {
  if (in.Animation != kLandingRollRunJumpAnimation || in.ActionVar2 >= 0)
    return LandingRollChoice::GameLanding;
  if ((in.StateFlags1 & kLandingRollStateHostileLockOn) != 0U)
    return LandingRollChoice::GameLanding;
  if (in.FallDistance <= 80 || in.FallDistance >= 800)
    return LandingRollChoice::GameLanding;
  if (in.StickDirection != kLandingRollStickForward ||
      (in.StateFlags1 & kLandingRollStateCarrying) != 0U)
    return LandingRollChoice::GameLanding;
  return LandingRollChoice::Roll;
}

// Reads the fields the decision needs from the player at `player` (any bus
// with Read8/Read16/Read32 like the a32 MemoryBus); false if a read failed.
template <class Bus>
bool ReadLandingRollInputs(Bus &memory, std::uint32_t player, LandingRollInputs *out) {
  std::uint16_t actionVar2 = 0U, fallDistance = 0U;
  std::uint8_t stickIndex = 0U;
  if (out == nullptr || player == 0U ||
      !memory.Read32(player + kLandingRollAnimationOffset, &out->Animation) ||
      !memory.Read32(player + kLandingRollStateFlags1Offset, &out->StateFlags1) ||
      !memory.Read16(player + kLandingRollActionVar2Offset, &actionVar2) ||
      !memory.Read16(player + kLandingRollFallDistanceOffset, &fallDistance) ||
      !memory.Read8(player + kLandingRollStickIndexOffset, &stickIndex) ||
      !memory.Read8(player + kLandingRollStickDirectionsOffset + stickIndex, &out->StickDirection))
    return false;
  out->ActionVar2 = static_cast<std::int16_t>(actionVar2);
  out->FallDistance = static_cast<std::int16_t>(fallDistance);
  return true;
}

// Read by the hook every landing, written by the panel.
inline std::atomic<bool> &LandingRollEnabled() noexcept {
  static std::atomic<bool> enabled{true};
  return enabled;
}

inline constexpr const char *kLandingRollOffConfigName = "no_landing_roll";

inline bool ReadLandingRollEnabled(const std::filesystem::path &config) {
  std::error_code error;
  return !std::filesystem::exists(config / kLandingRollOffConfigName, error);
}

// Removes (on) or creates (off) the file; false if that failed.
inline bool SaveLandingRollEnabled(const std::filesystem::path &config, bool enabled) {
  std::error_code error;
  if (enabled) {
    std::filesystem::remove(config / kLandingRollOffConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kLandingRollOffConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

} // namespace Oot3dNativeGame
