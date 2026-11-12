#pragma once

// L3+R3 > Game > Gameplay > "Unmirrored Master Quest" (config/master_quest_unmirrored,
// present = on; off by default).
//
// Master Quest is save context +0x0E (gSaveContext.masterQuestFlag). The game
// reads it in about 90 places: most choose Master Quest's dungeons, enemies
// and switches; the others mirror the world (the frame's and the scene
// uniforms' setup, the cull mode, the camera, Link's turn, cutscene
// transforms, three textbox paths, the pause maps, markers, cursors and icons,
// the motion sensors). "OoT3D: Single Screen Experience" 3.2.2 by M-1 makes 25
// of them read its own copy of the flag, taken where
// FileSelect_CopyActiveSaveBuffer reads it (a file load, the pause menu's
// close); the table adds 8 it leaves (below). Here a hook at the instruction
// after each read clears the register it loaded, so they see a normal world
// while everything else stays Master Quest, and the setting applies at that
// same save-buffer read. The reads that depend on it run every frame or every
// use (the cull mode at 0x0054CC30 included, set by the per-frame
// GameRuntime_InitializeRenderSubsystemState), so a change at the pause
// menu's close takes effect whole. Left as the mod leaves them: the analog
// stick's x negation (0x0041AB34, Input_UpdateGlobalStateAndCopySnapshot's
// stick pass) and the mirror of a view built on Gameplay_Draw's stack
// (Gameplay_UpdateViewProjectionState, 0x0046406C). Hooks reach only
// whole-AOT block starts: the 33 hook pcs are in whole_aot_observed_sites.json.
// The table is checked against the EUR code.bin in SWITCH_PORT.md
// "Unmirrored Master Quest".

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kSaveContextMasterQuest = 0x00587966U; // u8

// A mirror read `ldrb Register, [base, #0x0E]` ends at HookPc - 4.
struct MasterQuestMirrorRead {
  std::uint32_t HookPc = 0U;
  std::uint8_t Register = 0U;
};

// Sorted by HookPc. "+" marks the 8 the mod leaves: the motion sensors'
// inversion (Graph_ThreadEntry stores the flag for
// HidCalibration_UpdateMotionState, which negates two of their axes) and
// seven pause reads that run the same mirroring as the mod's read of their
// function on another branch (width - x, 320 - x, 116 - x, a left/right swap).
inline constexpr std::array<MasterQuestMirrorRead, 33> kMasterQuestMirrorReads{{
    {0x001C2538U, 0U}, // textbox (MessageContext reset, 001C23A4)
    {0x00228000U, 1U}, // EffectBlure_Draw
    {0x00230B60U, 1U}, // textbox (MessageContext reset, 00230A54)
    {0x002C05A4U, 0U}, // Player_UpdateTargetTurnAndInteractionState
    {0x002D8D24U, 0U}, // Camera_Update
    {0x002EF028U, 0U}, // PauseTouchButtons_UpdateActionGridGeometry
    {0x002EF12CU, 0U}, // + PauseTouchButtons_UpdateActionGridGeometry
    {0x002EF1B8U, 2U}, // + PauseTouchButtons_UpdateActionGridGeometry
    {0x002F06F4U, 0U}, // PauseMap_UpdateWorldDestinationCursor
    {0x002F0A9CU, 0U}, // PauseMapAreaTexture_Update
    {0x002F0F44U, 1U}, // + PauseRenderGroup_DrawAnimatedMask
    {0x002F1584U, 0U}, // PauseDetail_UpdateDungeonMapMarkers
    {0x002F16C4U, 0U}, // PauseDetail_UpdateDungeonMapMarkers
    {0x002F182CU, 1U}, // + PauseDetail_UpdateDungeonMapMarkers
    {0x002F19ACU, 2U}, // + PauseDetail_UpdateDungeonMapMarkers
    {0x002F2280U, 0U}, // PauseDetail_LoadDungeonMapResources
    {0x002FC37CU, 0U}, // PicaMaterialState_UploadSceneUniforms
    {0x002FFC9CU, 0U}, // GlobalCutsceneState_InitializeTransformState
    {0x00340B60U, 2U}, // ActorSlot_TryApplyIndexedEffect
    {0x00381278U, 1U}, // textbox (Message_StartTextboxResetState, 003811DC)
    {0x00416370U, 0U}, // + Graph_ThreadEntry (the motion sensors)
    {0x00419020U, 0U}, // GraphRenderer_DrawFrame
    {0x00419904U, 0U}, // GameRuntime_InitializeRenderSubsystemState (cull mode, every frame)
    {0x0042AE20U, 0U}, // PauseQuestPage_UpdateCursorGeometry
    {0x0042B014U, 0U}, // + PauseQuestPage_UpdateCursorGeometry
    {0x0042CF04U, 1U}, // PauseDungeonItemPanel_SubmitModels
    {0x0042CF60U, 1U}, // + PauseDungeonItemPanel_SubmitModels
    {0x0042DB18U, 2U}, // PauseInput_UpdateRepeatState
    {0x0042F7DCU, 0U}, // PauseQuestSingleModel_SubmitModels
    {0x0043920CU, 0U}, // PauseTouchButtons_UpdateMessageGeometry
    {0x004419C0U, 0U}, // PauseDetail_LoadStaticIcon
    {0x00441E38U, 0U}, // PauseDetail_LoadItemIcon
    {0x004790ACU, 0U}, // RendererObjectRef_UpdateWithFeedback
}};

// The mirror read whose hook is `pc`, or nullptr.
inline const MasterQuestMirrorRead *FindMasterQuestMirrorRead(std::uint32_t pc) noexcept {
  const auto found = std::lower_bound(kMasterQuestMirrorReads.begin(), kMasterQuestMirrorReads.end(), pc,
                                      [](const MasterQuestMirrorRead &read, std::uint32_t value) {
                                        return read.HookPc < value;
                                      });
  return found != kMasterQuestMirrorReads.end() && found->HookPc == pc ? &*found : nullptr;
}

// FileSelect_CopyActiveSaveBuffer's block that reads the flag (`ldrb r0,
// [r0, #0x0E]` at 0x002EAFC4), after its save refresh.
inline constexpr std::uint32_t kMasterQuestMirrorApplyPc = 0x002EAFBCU;

// The setting, written by the panel.
inline std::atomic<bool> &MasterQuestUnmirrored() noexcept {
  static std::atomic<bool> unmirrored{false};
  return unmirrored;
}

// The setting the game runs with: read by the hooks and by the port's own
// Master Quest consumers (the TopScreen free camera's horizontal inversion,
// the pause map's inset); taken at kMasterQuestMirrorApplyPc.
inline std::atomic<bool> &MasterQuestUnmirrorApplied() noexcept {
  static std::atomic<bool> applied{false};
  return applied;
}

inline void ApplyMasterQuestUnmirrored() noexcept {
  MasterQuestUnmirrorApplied().store(MasterQuestUnmirrored().load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
}

// Whether the world is mirrored for a Master Quest flag the game holds.
inline bool MasterQuestMirrorShown(std::uint8_t masterQuest) noexcept {
  return masterQuest != 0U && !MasterQuestUnmirrorApplied().load(std::memory_order_relaxed);
}

inline constexpr const char *kMasterQuestUnmirroredConfigName = "master_quest_unmirrored";

inline bool ReadMasterQuestUnmirrored(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kMasterQuestUnmirroredConfigName, error);
}

// Creates (on) or removes (off) the file; false if that failed.
inline bool SaveMasterQuestUnmirrored(const std::filesystem::path &config, bool unmirrored) {
  std::error_code error;
  if (!unmirrored) {
    std::filesystem::remove(config / kMasterQuestUnmirroredConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kMasterQuestUnmirroredConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

} // namespace Oot3dNativeGame
