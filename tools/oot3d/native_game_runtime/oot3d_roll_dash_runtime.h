#pragma once

// Roll dash (oot3d_roll_dash.h): SingleScreen 3.0's Roll Dash with the 3.1
// cross-fade, ported from coccofresco/TriAevum's TopScreenGameplayRuntime
// (5acf8138, 54fe8edf, 5b34bd61, 3c2ed0f3, a3eeb1e2) without its other
// options. Each hook issues only native OoT3D calls or the register/memory
// changes the 3.0/3.1 payload makes; no payload code runs.
//
// The window's block-entry policy (oot3d_native_a32_projection_policy.cpp)
// calls Execute at the hook PCs while whole-AOT code runs; when it returns
// true, the policy leaves the compiled graph at result->pc with `state`
// (Oot3dWholeAotExitAt). A native call issued here sets r14 to the hook PC,
// so its return re-enters the hook, which finishes the step. When it returns
// false the block runs natively with `state` (unchanged, or the caller's
// registers restored after such a call).

#include "oot3d_native_a32_memory.h"
#include "oot3d_roll_dash.h"
#include "oot3d_top_screen_link_ext.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace Oot3dNativeGame {

// Host-reserved guest region for zelda_link_ext.zar, outside every CTR
// mapping: the image ends below 0x00600000 and the heap starts at 0x08000000.
inline constexpr std::uint32_t kRollDashLinkExtBase = 0x06000000U;
// SingleScreen 3.1 cross-fades Link between the dash and the normal run
// (0x002BE8B8 -> 0x005D11A8): the previous animation's frame is decoded into
// a joint buffer the native animation context reads later in the frame.
// One host-reserved page; 25 joints of 16 bytes.
inline constexpr std::uint32_t kRollDashBlendBase = 0x06210000U;
inline constexpr std::uint32_t kRollDashBlendSize = 0x1000U;

class RollDashRuntime final {
public:
  // `linkAnimations`: config/zelda_link_ext.zar (it may be missing).
  RollDashRuntime(NativeA32Memory &memory, std::filesystem::path linkAnimations = {})
      : mMemory(memory), mLinkExtPath(std::move(linkAnimations)) {}

  static std::span<const std::uint32_t> HookPcs() noexcept { return kRollDashHookPcs; }
  // Outside guest code, at each host frame and after a state load: takes the
  // option (RollDashEnabled) and, while it is on, maps the animation image
  // and the cross-fade page (a mapping rebuilds the page table, which guest
  // code must not be running for).
  void Prepare(bool enabled);
  [[nodiscard]] bool Enabled() const noexcept { return mEnabled; }
  // Whether the hook at `pc` has anything to do: the option is on, or one of
  // its native calls or the idle word is still to be finished.
  [[nodiscard]] bool ShouldObserve(std::uint32_t pc) const noexcept;
  bool Execute(std::uint32_t pc, oot3d::recomp::a32::GuestState &state,
               oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  // A state load: host state goes; the loaded memory decides whether the
  // regions are ours (checked again by the next Prepare).
  void Reset() noexcept;
  // A native call issued by a hook is in flight (its caller is host-held).
  [[nodiscard]] bool Pending() const noexcept {
    return mDashQueryPending || mRollStep != RollStep::None;
  }
  // FUN_0037577c is in flight: the next Player_Update entry is its return,
  // with the query's registers, not the title's call (a3eeb1e2).
  [[nodiscard]] bool DashQueryPending() const noexcept { return mDashQueryPending; }
  // The image is mapped and holds the dash animations.
  [[nodiscard]] bool AnimationsReady() const noexcept {
    return mRegionReady && mLinkExt.has_value();
  }
  // Empty when the animations are available or not yet requested.
  const std::string &LinkExtError() const noexcept { return mLinkExtError; }

  std::uint64_t DashQueries() const noexcept { return mDashQueries; }
  std::uint64_t DashFrames() const noexcept { return mDashFrames; }
  std::uint64_t DashNaviHides() const noexcept { return mDashNaviHides; }
  std::uint64_t LinkExtAnswers() const noexcept { return mLinkExtAnswers; }
  std::uint64_t DashSpeedApplications() const noexcept { return mDashSpeedApplications; }
  std::uint64_t DashBlends() const noexcept { return mDashBlends; }
  std::uint64_t DashBlendPoses() const noexcept { return mDashBlendPoses; }
  // The game log line of L3+R3 > Game > Gameplay ("gameplay: landing_roll=..
  // roll_dash=.. animations=.."), when it differs from the last one written
  // (always the first time); nothing otherwise.
  [[nodiscard]] std::optional<std::string> TakeSettingsLine(bool landingRoll);
  // The perf line of the dash counters since the last call ("roll_dash:
  // queries=.. frames=.. ..\n"); empty while the option is off.
  [[nodiscard]] std::string TakePerfLine();

private:
  enum class RollStep : std::uint8_t { None, Animate, DecodeEntry, Decode, InterpEntry };

  bool DashStateEntry(oot3d::recomp::a32::GuestState &state,
                      oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  bool DashSpeedSite(std::uint32_t pc, oot3d::recomp::a32::GuestState &state,
                     oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  // FUN_002be660's animation call with 3.1's roll scale and cross-fade.
  bool DashRollSite(oot3d::recomp::a32::GuestState &state,
                    oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  bool DashRollStep(oot3d::recomp::a32::GuestState &state,
                    oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  // `eligible`: the 3.0 dash preconditions held, FUN_0037577c(play) included.
  void UpdateDashState(bool eligible);
  bool LinkCsabQuery(oot3d::recomp::a32::GuestState &state,
                     oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed);
  void PrepareLinkExtRegion();
  void PrepareBlendRegion();
  std::uint32_t Read32(std::uint32_t address) const;

  NativeA32Memory &mMemory;
  bool mEnabled = false;
  // FUN_0037577c returns to Player_Update's entry with this state.
  bool mDashQueryPending = false;
  oot3d::recomp::a32::GuestState mDashCaller{};
  // 3.0 0x0060FCB8 (rolling or dashing), 0x0060FCB9 (airborne frames),
  // 0x0060FCBB (dash idle word shifted).
  bool mInRoll = false;
  std::uint8_t mAirborneFrames = 0;
  bool mIdleShifted = false;
  // 3.0 0x0060D2C0 (run) and 0x0060D2C4 (roll) speed scales.
  float mRunScale = 1.0F;
  float mRollScale = 1.0F;
  // 3.1 cross-fade (0x0060F4D4..0x0060F4E8, ratio 0x0060D8BC): the last
  // call's frame, archive, animation index and dash answer, the animation
  // faded from, the weight, the last speed and the speed ratio at the switch.
  std::uint32_t mBlendFrame = 0;
  std::uint32_t mBlendArchive = 0;
  std::uint32_t mBlendIndex = 0;
  bool mBlendDash = false;
  bool mBlendFromDash = false;
  float mBlendWeight = 0.0F;
  float mBlendSpeed = 0.0F;
  float mBlendRatio = 1.0F;
  bool mBlendReady = false;
  bool mBlendRegionFailed = false;
  // The animation call and the pose fade's native calls return to the site.
  RollStep mRollStep = RollStep::None;
  oot3d::recomp::a32::GuestState mRollCaller{};
  std::uint32_t mRollSource = 0; // the faded-from animation entry
  std::uint32_t mRollEntry = 0;  // the animation context entry
  // zelda_link_ext.zar image, read once; the region is checked again after
  // a state load.
  std::filesystem::path mLinkExtPath;
  bool mLinkExtAttempted = false;
  std::optional<TopScreenLinkExtImage> mLinkExt;
  std::string mLinkExtError;
  bool mRegionReady = false;
  // This frame's Link archive; the query answers with slots while dashing.
  std::uint32_t mLinkArchive = 0;
  std::uint32_t mLinkAge = 0;
  bool mAnswerDash = false;
  std::uint64_t mDashQueries = 0;
  std::uint64_t mDashFrames = 0;
  std::uint64_t mDashNaviHides = 0;
  std::uint64_t mLinkExtAnswers = 0;
  std::uint64_t mDashSpeedApplications = 0;
  std::uint64_t mDashBlends = 0;
  std::uint64_t mDashBlendPoses = 0;
  // The settings last logged (Landing roll, Roll dash, animations: 0 not
  // requested, 1 ready, 2 unavailable); none before the first line.
  std::optional<std::array<std::uint8_t, 3>> mLoggedSettings;
  // The counters at the last perf line.
  std::array<std::uint64_t, 7> mPerfStart{};
};

} // namespace Oot3dNativeGame
