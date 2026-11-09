#include "oot3d_roll_dash_runtime.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace Oot3dNativeGame {
namespace {
// 3.0 0x005D8BA8 inputs.
constexpr std::uint32_t kPlayStatePointer = 0x005043D4U + 0x0CU;
constexpr std::uint32_t kSaveContext = 0x00587958U;
constexpr std::uint32_t kSaveLinkAge = kSaveContext + 0x04U;      // 0 adult, 1 child
constexpr std::uint32_t kSaveDashEnabled = kSaveContext + 0x44U;  // s16 health: alive
constexpr std::uint32_t kPlayStateMode = 0x100U;                  // 3 = gameplay
constexpr std::uint32_t kPlayStateSubMode = 0x101U;               // 2 = running
// GameState input[0].cur.button; bit 0 is A, so the query runs while A is held.
constexpr std::uint32_t kPlayStateButtons = 0x14U;
constexpr std::uint32_t kPlayStatePlayer = 0x20ACU;
constexpr std::uint32_t kDashBlockedQuery = 0x0037577CU; // FUN_0037577c(play)
constexpr std::uint32_t kPlayerAnimationAge = 0x1B3U;
constexpr std::uint32_t kPlayerRollAction = 0x284U;
constexpr std::uint32_t kRollActionTable = 0x0014E9F0U * 4U; // indexed by +0x1B3
constexpr std::uint32_t kPlayerStateFlags = 0x1710U;
constexpr std::uint32_t kDashCancelFlags = 0x08A02000U;
constexpr std::uint32_t kPlayerGroundFlags = 0x90U; // bit 0: on ground
constexpr std::uint32_t kPlayerHeavy = 0x1A7U;
constexpr std::uint32_t kPlayerArchive = 0x258U;
constexpr std::uint32_t kLinkArchiveAnimations = 0x246U;
constexpr std::uint32_t kArchiveGroup = 0x20U;
constexpr std::uint32_t kArchiveGroupTable = 0x0CU;
constexpr std::uint32_t kArchiveAnimationTable = 0x50U;
constexpr std::uint32_t kDashIdleObject = 0x0051B2F4U;
constexpr std::uint32_t kDashIdleWord = 0x60U;
constexpr std::uint16_t kIdleWordNormal = 0x1B2U;
constexpr std::uint16_t kIdleWordDash = 0x1C9U;
// 3.0 literal pool 0x005D9848..0x005D9860.
constexpr float kRunAdult = 1.425F, kRunChild = 1.35F, kRunHeavy = 1.25F, kRunAirborne = 1.18F;
constexpr float kRollNoAnimation = 1.425F, kRollChild = 1.0F, kRollAdult = 0.8F,
                kRollHeavy = 0.685F;

// Native continuations of the dash sites.
constexpr std::uint32_t kTurnSpeedNear = 0x00251B80U;
constexpr std::uint32_t kTurnSpeedFar = 0x00251B64U;
constexpr std::uint32_t kMoveSpeedReturn = 0x00251D4CU;
constexpr std::uint32_t kMoveSpeedCall = 0x0035FB14U; // Actor_UpdateVelocityXZGravity
constexpr std::uint32_t kRollSpeedReturn = 0x002BE8BCU;
constexpr std::uint32_t kRollSpeedCall = 0x002DD8B0U;
// Navi's "in the hat" block: state 8, then the native hide call.
constexpr std::uint32_t kNaviInHat = 0x00129680U;

// 3.1 dash cross-fade (0x005D11A8, literals 0x005D14EC..0x005D151C).
constexpr std::uint32_t kPlayerSkeleton = 0x254U;
constexpr std::uint32_t kPlayStateFrames = 0x5BF4U;
constexpr std::uint32_t kPlayStateAnimationContext = 0x3410U;
constexpr std::uint32_t kAnimationContextEntries = 0x3414U;
constexpr std::uint32_t kAnimationContextLimit = 0x28U;
constexpr std::uint32_t kPlayerLimbCount = 0x2C8U;
constexpr std::uint32_t kPlayerJointTable = 0x2CCU;
constexpr std::uint32_t kPlayerFrameTable = 0x27CU;
constexpr std::uint32_t kPlayerAnimationFlags = 0x2D5U;
constexpr std::uint32_t kPlayerRunFrame = 0x2254U;
constexpr std::uint32_t kBlendLimbLimit = 0x19U;
constexpr std::uint32_t kDashFrameRateObject = 0x0051B2F4U; // (s16)+0x110 update rate
constexpr std::uint32_t kDashFrameRate = 0x110U;
constexpr std::uint32_t kAnimationContextAdd = 0x00324154U;   // AnimationContext_AddEntry
constexpr std::uint32_t kAnimationDecodeFrame = 0x002BD9ECU;  // AnimationEntry_DecodeFrame
constexpr std::uint32_t kAnimationInterpolate = 0x002C3814U;  // AnimationEntry_InitInterpAll
constexpr float kBlendRateScale = 0.5F, kBlendStep = 1.0F / 6.0F, kBlendMinSpeed = 1.0e-4F,
                kBlendMinRatio = 0.5F, kBlendMaxRatio = 2.0F, kBlendRunFrames = 1.0F / 29.0F;

void Branch(oot3d::recomp::a32::GuestState &state, std::uint32_t pc, std::uint32_t target,
            oot3d::recomp::a32::ExecutionResult *result, std::uint32_t *blocksConsumed) {
  state.r[15] = target;
  *result = {oot3d::recomp::a32::ExitKind::Branch, target,
             oot3d::recomp::a32::FallbackReason::None, pc};
  if (blocksConsumed != nullptr) *blocksConsumed = 1U;
}

float Float(std::uint32_t bits) noexcept { return std::bit_cast<float>(bits); }
std::uint32_t Bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
} // namespace

bool RollDashRuntime::ShouldObserve(std::uint32_t pc) const noexcept {
  switch (pc) {
  case kRollDashStateEntry:
    // Turned off mid-dash: the next update still ends the dash state.
    return mEnabled || mDashQueryPending || mIdleShifted || mInRoll || mAnswerDash;
  case kRollDashTurnSpeedSite:
  case kRollDashMoveSpeedSite:
    return mEnabled && mRunScale != 1.0F;
  case kRollDashRollSite:
    // The cross-fade watches every call to see the dash answer switch.
    return mEnabled || mRollStep != RollStep::None;
  case kRollDashNaviSite:
    return mEnabled && mInRoll;
  case kRollDashCsabQuery:
    return mAnswerDash;
  default:
    return false;
  }
}

void RollDashRuntime::Reset() noexcept {
  mDashQueryPending = false;
  mDashCaller = {};
  mInRoll = false;
  mAirborneFrames = 0;
  mRunScale = 1.0F;
  mRollScale = 1.0F;
  mBlendFrame = mBlendArchive = mBlendIndex = 0U;
  mBlendDash = mBlendFromDash = false;
  mBlendWeight = mBlendSpeed = 0.0F;
  mBlendRatio = 1.0F;
  mRollStep = RollStep::None;
  mRollCaller = {};
  mRollSource = mRollEntry = 0U;
  // The loaded memory decides whether the regions are ours.
  mRegionReady = false;
  mBlendReady = false;
  mLinkArchive = 0U;
  mAnswerDash = false;
}

std::uint32_t RollDashRuntime::Read32(std::uint32_t address) const {
  std::uint32_t value = 0U;
  if (!const_cast<NativeA32Memory &>(mMemory).Read32(address, &value))
    throw std::runtime_error("Roll dash cannot read guest memory");
  return value;
}

void RollDashRuntime::Prepare(bool enabled) {
  mEnabled = enabled;
  if (!enabled) return;
  PrepareLinkExtRegion();
  PrepareBlendRegion();
}

void RollDashRuntime::PrepareLinkExtRegion() {
  if (!mLinkExtAttempted) {
    mLinkExtAttempted = true;
    std::error_code missing;
    if (mLinkExtPath.empty() || !std::filesystem::is_regular_file(mLinkExtPath, missing)) {
      mLinkExtError = std::string(kRollDashAnimationsName) + " is not installed";
    } else {
      std::ifstream file(mLinkExtPath, std::ios::binary);
      const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
      TopScreenLinkExtImage image;
      if (bytes.empty())
        mLinkExtError = "cannot read " + mLinkExtPath.string();
      else if (BuildTopScreenLinkExtImage(bytes, kRollDashLinkExtBase, &image, &mLinkExtError))
        mLinkExt = std::move(image);
    }
  }
  if (!mLinkExt.has_value() || mRegionReady) return;
  const auto &bytes = mLinkExt->Bytes;
  if (!mMemory.IsMapped(kRollDashLinkExtBase, bytes.size())) {
    if (mMemory.IsMapped(kRollDashLinkExtBase, 1U) ||
        !mMemory.MapRegion({"roll_dash_link_ext", kRollDashLinkExtBase, bytes.size(), true, false,
                            bytes},
                           &mLinkExtError)) {
      if (mLinkExtError.empty()) mLinkExtError = "its guest region is taken";
      mLinkExtError = "cannot map " + std::string(kRollDashAnimationsName) + ": " + mLinkExtError;
      mLinkExt.reset();
      return;
    }
  } else if (Read32(kRollDashLinkExtBase) != TopScreenLinkExtImage::kMagic ||
             Read32(kRollDashLinkExtBase + 4U) != mLinkExt->ContentHash) {
    // A state saved with another image (or none) in the region.
    if (!mMemory.WriteBytes(kRollDashLinkExtBase, bytes))
      throw std::runtime_error("Roll dash cannot refresh the Link animation region");
  }
  mRegionReady = true;
}

void RollDashRuntime::PrepareBlendRegion() {
  if (mBlendReady || mBlendRegionFailed) return;
  if (mMemory.IsWritable(kRollDashBlendBase, kRollDashBlendSize) &&
      !mMemory.IsExecutable(kRollDashBlendBase, kRollDashBlendSize)) {
    mBlendReady = true;
    return;
  }
  std::string error;
  if (!mMemory.IsMapped(kRollDashBlendBase, 1U) &&
      mMemory.MapRegion({"roll_dash_blend", kRollDashBlendBase, kRollDashBlendSize, true, false, {}},
                        &error)) {
    mBlendReady = true;
    return;
  }
  mBlendRegionFailed = true;
}

std::optional<std::string> RollDashRuntime::TakeSettingsLine(bool landingRoll) {
  const std::uint8_t animations = !mLinkExtAttempted ? 0U : AnimationsReady() ? 1U : 2U;
  const std::array<std::uint8_t, 3> now{static_cast<std::uint8_t>(landingRoll),
                                        static_cast<std::uint8_t>(mEnabled), animations};
  if (mLoggedSettings == now) return std::nullopt;
  mLoggedSettings = now;
  std::string line = std::string("gameplay: landing_roll=") + (landingRoll ? "on" : "off") +
                     " roll_dash=" + (mEnabled ? "on" : "off") + " animations=";
  line += animations == 0U ? "-" : animations == 1U ? "ready" : "unavailable (" + mLinkExtError + ")";
  line += " (L3+R3 > Game > Gameplay, config/no_landing_roll, config/roll_dash)";
  return line;
}

std::string RollDashRuntime::TakePerfLine() {
  const std::array<std::uint64_t, 7> now{mDashQueries,   mDashFrames, mLinkExtAnswers,
                                         mDashSpeedApplications, mDashBlends, mDashBlendPoses,
                                         mDashNaviHides};
  std::array<std::uint64_t, 7> delta{};
  for (std::size_t i = 0; i < now.size(); ++i) delta[i] = now[i] - mPerfStart[i];
  mPerfStart = now;
  if (!mEnabled) return {};
  char line[192];
  std::snprintf(line, sizeof(line),
                "roll_dash: queries=%llu frames=%llu answers=%llu speed=%llu blends=%llu "
                "poses=%llu navi=%llu animations=%s\n",
                static_cast<unsigned long long>(delta[0]), static_cast<unsigned long long>(delta[1]),
                static_cast<unsigned long long>(delta[2]), static_cast<unsigned long long>(delta[3]),
                static_cast<unsigned long long>(delta[4]), static_cast<unsigned long long>(delta[5]),
                static_cast<unsigned long long>(delta[6]), AnimationsReady() ? "ready" : "off");
  return line;
}

bool RollDashRuntime::Execute(std::uint32_t pc, oot3d::recomp::a32::GuestState &state,
                              oot3d::recomp::a32::ExecutionResult *result,
                              std::uint32_t *blocksConsumed) {
  if (!ShouldObserve(pc)) return false;
  switch (pc) {
  case kRollDashStateEntry:
    return DashStateEntry(state, result, blocksConsumed);
  case kRollDashRollSite:
    return DashRollSite(state, result, blocksConsumed);
  case kRollDashTurnSpeedSite:
  case kRollDashMoveSpeedSite:
  case kRollDashNaviSite:
    return DashSpeedSite(pc, state, result, blocksConsumed);
  case kRollDashCsabQuery:
    return LinkCsabQuery(state, result, blocksConsumed);
  default:
    return false;
  }
}

bool RollDashRuntime::DashStateEntry(oot3d::recomp::a32::GuestState &state,
                                     oot3d::recomp::a32::ExecutionResult *result,
                                     std::uint32_t *blocksConsumed) {
  if (mDashQueryPending) {
    // FUN_0037577c returned; Player_Update now runs natively.
    const bool blocked = state.r[0] != 0U;
    state = mDashCaller;
    mDashQueryPending = false;
    ++mDashQueries;
    UpdateDashState(!blocked);
    return false;
  }
  // 0x005D9420..0x005D947C: the cheap preconditions before the native query.
  const std::uint32_t play = Read32(kPlayStatePointer);
  bool eligible = mEnabled && play != 0U;
  if (eligible) {
    std::uint8_t mode = 0, subMode = 0;
    std::uint16_t dashEnabled = 0;
    eligible = mMemory.Read8(play + kPlayStateMode, &mode) && mode == 3U &&
               mMemory.Read8(play + kPlayStateSubMode, &subMode) && subMode == 2U &&
               Read32(play + kPlayStatePlayer) != 0U &&
               mMemory.Read16(kSaveDashEnabled, &dashEnabled) && dashEnabled != 0U &&
               (Read32(play + kPlayStateButtons) & 1U) != 0U;
  }
  if (!eligible) {
    UpdateDashState(false);
    return false;
  }
  mDashCaller = state;
  mDashQueryPending = true;
  state.r[0] = play;
  state.r[14] = kRollDashStateEntry;
  Branch(state, kRollDashStateEntry, kDashBlockedQuery, result, blocksConsumed);
  return true;
}

void RollDashRuntime::UpdateDashState(bool eligible) {
  // 0x005D8BC8..0x005D95C0, without the payload's heap allocation.
  mRunScale = 1.0F;
  mRollScale = 1.0F;
  bool dash = false;
  std::uint32_t archive = 0U, age = 0U;
  const std::uint32_t play = Read32(kPlayStatePointer);
  std::uint32_t player = 0U;
  std::uint8_t mode = 0;
  if (play != 0U && mMemory.Read8(play + kPlayStateMode, &mode) && mode == 3U)
    player = Read32(play + kPlayStatePlayer);
  if (player != 0U) {
    archive = Read32(player + kPlayerArchive);
    const std::uint32_t group = archive != 0U ? Read32(archive + kArchiveGroup) : 0xFFFFFFFFU;
    if (group == 0xFFFFFFFFU ||
        Read32(Read32(archive + kArchiveGroupTable) + group * 0x10U) != kLinkArchiveAnimations)
      archive = 0U;
    else
      age = Read32(kSaveLinkAge) != 0U ? 1U : 0U;
  }

  if (!eligible) {
    mInRoll = false;
  } else {
    std::uint8_t animationAge = 0;
    if (!mMemory.Read8(player + kPlayerAnimationAge, &animationAge))
      throw std::runtime_error("Roll dash cannot read the player");
    const bool rolling = animationAge < 6U && Read32(player + kPlayerRollAction) ==
                                                  Read32(kRollActionTable + animationAge * 4U);
    if (rolling) mInRoll = true;
    if (mInRoll && (Read32(player + kPlayerStateFlags) & kDashCancelFlags) != 0U) {
      mInRoll = false;
    } else if (mInRoll) {
      std::uint8_t heavy = 0;
      mMemory.Read8(player + kPlayerHeavy, &heavy);
      const bool child = Read32(kSaveLinkAge) != 0U;
      dash = archive != 0U && AnimationsReady();
      mRollScale = !dash ? kRollNoAnimation : child ? kRollChild
                                            : heavy == 1U ? kRollHeavy
                                                          : kRollAdult;
      if (!rolling) {
        std::uint16_t ground = 0;
        mMemory.Read16(player + kPlayerGroundFlags, &ground);
        bool airborne = false;
        if ((ground & 1U) != 0U) {
          mAirborneFrames = 0U;
        } else if (mAirborneFrames == 0xFFU) {
          airborne = true;
        } else {
          ++mAirborneFrames;
          airborne = mAirborneFrames > 2U;
        }
        mRunScale = airborne ? kRunAirborne : child ? kRunChild
                                            : heavy == 1U ? kRunHeavy
                                                          : kRunAdult;
      }
    }
  }
  if (mInRoll) ++mDashFrames;

  // 0x005D95A0..0x005D95C0: the dash swaps the idle word while it lasts.
  const std::uint32_t idleObject = Read32(kDashIdleObject);
  if (idleObject != 0U) {
    std::uint16_t word = 0;
    mMemory.Read16(idleObject + kDashIdleWord, &word);
    if (mInRoll) {
      if (word == kIdleWordNormal) {
        mMemory.Write16(idleObject + kDashIdleWord, kIdleWordDash);
        mIdleShifted = true;
      }
    } else if (mIdleShifted) {
      if (word == kIdleWordDash) mMemory.Write16(idleObject + kDashIdleWord, kIdleWordNormal);
      mIdleShifted = false;
    }
  }
  const bool available = archive != 0U && AnimationsReady();
  mLinkArchive = available ? archive : 0U;
  mLinkAge = age;
  mAnswerDash = available && dash;
}

bool RollDashRuntime::LinkCsabQuery(oot3d::recomp::a32::GuestState &state,
                                    oot3d::recomp::a32::ExecutionResult *result,
                                    std::uint32_t *blocksConsumed) {
  // 0x005D10A8: while dashing, Link's archive answers a dash animation with
  // its slot, which holds the CSAB pointer as a native entry does.
  const std::uint32_t archive = state.r[0];
  const std::uint32_t index = state.r[1];
  if (archive != mLinkArchive || !mAnswerDash || !AnimationsReady()) return false;
  std::size_t i = 0;
  while (i < kTopScreenLinkExtAnimations.size() &&
         kTopScreenLinkExtAnimations[i].NativeIndex != index)
    ++i;
  if (i == kTopScreenLinkExtAnimations.size() || !mLinkExt->Present[mLinkAge][i]) return false;
  // The archive may have died since the update: answer only Link's group.
  const std::uint32_t group = Read32(archive + kArchiveGroup);
  if (group == 0xFFFFFFFFU ||
      Read32(Read32(archive + kArchiveGroupTable) + group * 0x10U) != kLinkArchiveAnimations)
    return false;
  const std::uint32_t slot =
      TopScreenLinkExtImage::SlotAddress(kRollDashLinkExtBase, mLinkAge, i);
  state.r[0] = slot;
  state.r[3] = slot;
  ++mLinkExtAnswers;
  Branch(state, kRollDashCsabQuery, state.r[14], result, blocksConsumed);
  return true;
}

bool RollDashRuntime::DashSpeedSite(std::uint32_t pc, oot3d::recomp::a32::GuestState &state,
                                    oot3d::recomp::a32::ExecutionResult *result,
                                    std::uint32_t *blocksConsumed) {
  switch (pc) {
  case kRollDashTurnSpeedSite: {
    // ldrsh r9, [r7, #0x20]; ldrh r0, [r4, #0x36]; vldr s23, [r6, #0x21C]
    // (x run scale); sub; sxth; abs; cmp r0, #0x6000; ble 0x00251B80.
    std::uint16_t target = 0, facing = 0;
    if (!mMemory.Read16(state.r[7] + 0x20U, &target) ||
        !mMemory.Read16(state.r[4] + 0x36U, &facing))
      throw std::runtime_error("Roll dash cannot read the turn");
    state.r[9] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
        std::bit_cast<std::int16_t>(target)));
    state.vfp[23] = Bits(Float(Read32(state.r[6] + 0x21CU)) * mRunScale);
    std::int32_t delta = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint32_t>(facing) - state.r[9]));
    if (delta < 0) delta = -delta;
    state.r[0] = static_cast<std::uint32_t>(delta);
    const std::uint32_t difference = state.r[0] - 0x6000U;
    std::uint32_t flags = 0U;
    if ((difference & 0x80000000U) != 0U) flags |= 0x80000000U;
    if (difference == 0U) flags |= 0x40000000U;
    if (state.r[0] >= 0x6000U) flags |= 0x20000000U;
    state.cpsr = (state.cpsr & 0x0FFFFFFFU) | flags;
    ++mDashSpeedApplications;
    Branch(state, pc, delta <= 0x6000 ? kTurnSpeedNear : kTurnSpeedFar, result, blocksConsumed);
    return true;
  }
  case kRollDashMoveSpeedSite: {
    // vldr s0, [r6, #0x21C] (x run scale); vstr s0, [r4, #0x6C];
    // ldrh r0, [r7, #0x20]; strh r0, [r4, #0x36]; mov r0, r4; bl 0x0035FB14.
    const float speed = Float(Read32(state.r[6] + 0x21CU)) * mRunScale;
    std::uint16_t target = 0;
    if (!mMemory.Write32(state.r[4] + 0x6CU, Bits(speed)) ||
        !mMemory.Read16(state.r[7] + 0x20U, &target) ||
        !mMemory.Write16(state.r[4] + 0x36U, target))
      throw std::runtime_error("Roll dash cannot move the player");
    state.vfp[0] = Bits(speed);
    state.r[0] = state.r[4];
    state.r[14] = kMoveSpeedReturn;
    ++mDashSpeedApplications;
    Branch(state, pc, kMoveSpeedCall, result, blocksConsumed);
    return true;
  }
  case kRollDashNaviSite:
    // Navi's reach timer expired while Link dashes: into the hat (as 3.0
    // and 3.1 do) instead of the native give-up that leaves her out.
    ++mDashNaviHides;
    Branch(state, pc, kNaviInHat, result, blocksConsumed);
    return true;
  default:
    return false;
  }
}

bool RollDashRuntime::DashRollSite(oot3d::recomp::a32::GuestState &state,
                                   oot3d::recomp::a32::ExecutionResult *result,
                                   std::uint32_t *blocksConsumed) {
  if (mRollStep != RollStep::None) return DashRollStep(state, result, blocksConsumed);
  if (!mEnabled) return false;
  // The block `mov r1, r9; mov r0, r4; bl 0x002DD8B0`: r4 the player, r9 the
  // PlayState, r8 the animation index, s0 the speed. 3.1 0x005D11A8 scales the
  // speed, then cross-fades when the dash answer for this animation switches.
  const std::uint32_t player = state.r[4];
  const std::uint32_t play = state.r[9];
  const std::uint32_t index = state.r[8];
  const float nativeSpeed = Float(state.vfp[0]);
  float speed = nativeSpeed * mRollScale;
  if (mRollScale != 1.0F) ++mDashSpeedApplications;
  const std::uint32_t archive = Read32(player + kPlayerArchive);
  bool fade = false;
  if (Read32(play + kPlayStatePlayer) == player && archive != 0U) {
    const std::uint32_t frame = Read32(play + kPlayStateFrames);
    // 0x005CA8A8: the current group's animation count.
    const std::uint32_t group = Read32(archive + kArchiveGroup);
    const std::uint32_t count = group == 0xFFFFFFFFU
        ? 0U : Read32(Read32(archive + kArchiveGroupTable) + group * 0x10U);
    const std::uint32_t nativeEntry =
        index < count ? Read32(Read32(archive + kArchiveAnimationTable) + index * 4U) : 0U;
    // 0x005D1224..0x005D12B8: with Link's animations ready, this dash
    // animation's slot, and whether the query answers it now.
    std::uint32_t dashEntry = 0U;
    bool dash = false;
    if (archive == mLinkArchive && mLinkArchive != 0U && AnimationsReady() &&
        count == kLinkArchiveAnimations) {
      for (std::size_t i = 0; i < kTopScreenLinkExtAnimations.size(); ++i) {
        if (kTopScreenLinkExtAnimations[i].NativeIndex != index) continue;
        if (mLinkExt->Present[mLinkAge][i]) {
          dashEntry = TopScreenLinkExtImage::SlotAddress(kRollDashLinkExtBase, mLinkAge, i);
          dash = mAnswerDash;
        }
        break;
      }
    }
    // 0x005D12BC..0x005D1438: the same animation on consecutive frames whose
    // answer switched starts a fade from the other animation.
    if (mBlendFrame + 1U == frame && mBlendArchive == archive && mBlendIndex == index) {
      if (mBlendDash != dash) {
        mBlendWeight = 1.0F - mBlendWeight;
        mBlendFromDash = mBlendDash;
        mBlendRatio = speed <= kBlendMinSpeed
            ? 1.0F : std::clamp(mBlendSpeed / speed, kBlendMinRatio, kBlendMaxRatio);
        ++mDashBlends;
      }
    } else {
      mBlendWeight = 0.0F;
    }
    mBlendArchive = archive;
    mBlendIndex = index;
    mBlendFrame = frame;
    mBlendDash = dash;
    if (mBlendWeight > 0.0F) {
      // 0x005D1318..0x005D1458: one sixth per update at 30 Hz.
      float rate = 1.0F;
      if (const std::uint32_t object = Read32(kDashFrameRateObject); object != 0U) {
        std::uint16_t updateRate = 0U;
        if (!mMemory.Read16(object + kDashFrameRate, &updateRate))
          throw std::runtime_error("Roll dash cannot read the update rate");
        rate = static_cast<float>(std::bit_cast<std::int16_t>(updateRate)) * kBlendRateScale;
      }
      const float weight = mBlendWeight - rate * kBlendStep;
      if (weight > 0.0F) {
        speed *= 1.0F + (mBlendRatio - 1.0F) * weight;
        mBlendWeight = weight;
      } else {
        mBlendWeight = 0.0F;
      }
    }
    mBlendSpeed = speed;
    fade = mBlendWeight > 0.0F;
    mRollSource = mBlendFromDash ? dashEntry : nativeEntry;
  }
  // The native block as it is: nothing to scale or fade (no exit from the
  // compiled code outside a dash).
  if (!fade && speed == nativeSpeed) return false;
  mRollCaller = state;
  state.r[0] = player;
  state.r[1] = play;
  state.vfp[0] = Bits(speed);
  // While fading, the animation call returns here to fade the pose.
  state.r[14] = fade ? kRollDashRollSite : kRollSpeedReturn;
  mRollStep = fade ? RollStep::Animate : RollStep::None;
  Branch(state, kRollDashRollSite, kRollSpeedCall, result, blocksConsumed);
  return true;
}

bool RollDashRuntime::DashRollStep(oot3d::recomp::a32::GuestState &state,
                                   oot3d::recomp::a32::ExecutionResult *result,
                                   std::uint32_t *blocksConsumed) {
  const auto &caller = mRollCaller;
  const std::uint32_t player = caller.r[4];
  const std::uint32_t play = caller.r[9];
  const std::uint32_t stack = caller.r[13] - (mRollStep == RollStep::Decode ? 0x20U : 0U);
  if (state.r[13] != stack || state.thread_pointer != caller.thread_pointer)
    throw std::runtime_error("Roll dash re-entry caller does not match");
  const auto call = [&](RollStep next, std::uint32_t target, std::uint32_t returnPc) {
    mRollStep = next;
    state.r[14] = returnPc;
    Branch(state, kRollDashRollSite, target, result, blocksConsumed);
    return true;
  };
  std::uint8_t limbs = 0U;
  if (!mMemory.Read8(player + kPlayerLimbCount, &limbs))
    throw std::runtime_error("Roll dash cannot read Link's skeleton");
  switch (mRollStep) {
  case RollStep::Animate:
    // 0x005D136C..0x005D13C0: fade the pose only from a loaded animation, a
    // skeleton the buffer holds and an animation context with room.
    if (mRollSource == 0U || Read32(mRollSource) == 0U || limbs > kBlendLimbLimit ||
        Read32(play + kAnimationContextEntries) > kAnimationContextLimit || !mBlendReady) {
      mBlendWeight = 0.0F;
      mRollStep = RollStep::None;
      Branch(state, kRollDashRollSite, kRollSpeedReturn, result, blocksConsumed);
      return true;
    }
    state.r[0] = play + kPlayStateAnimationContext;
    state.r[1] = player + kPlayerSkeleton;
    return call(RollStep::DecodeEntry, kAnimationContextAdd, kRollDashRollSite);
  case RollStep::DecodeEntry: {
    // 0x005D145C..0x005D14B8: the faded-from animation's frame at the run's
    // position, decoded into the buffer.
    mRollEntry = state.r[0];
    const std::uint32_t csab = Read32(mRollSource);
    const std::int32_t frames =
        std::bit_cast<std::int32_t>(Read32(csab + Read32(csab + 0x14U) + 0x10U));
    const float frame = (static_cast<float>(frames) + 1.0F) * kBlendRunFrames *
                        Float(Read32(player + kPlayerRunFrame));
    std::uint8_t flags = 0U;
    const std::uint32_t arguments = caller.r[13] - 0x20U;
    if (!mMemory.Read8(player + kPlayerAnimationFlags, &flags) ||
        !mMemory.Write32(arguments, kRollDashBlendBase) ||
        !mMemory.Write32(arguments + 4U, flags) || !mMemory.Write32(arguments + 8U, 0U))
      throw std::runtime_error("Roll dash cannot pass the frame decode");
    state.r[13] = arguments;
    state.r[0] = mRollEntry;
    state.r[1] = Read32(player + kPlayerFrameTable);
    state.r[2] = mRollSource;
    state.r[3] = limbs;
    state.vfp[0] = Bits(frame);
    return call(RollStep::Decode, kAnimationDecodeFrame, kRollDashRollSite);
  }
  case RollStep::Decode:
    state.r[13] = caller.r[13];
    state.r[0] = play + kPlayStateAnimationContext;
    state.r[1] = player + kPlayerSkeleton;
    return call(RollStep::InterpEntry, kAnimationContextAdd, kRollDashRollSite);
  case RollStep::InterpEntry:
    // 0x005D14CC..0x005D14E8: blend Link's joints toward it by the weight;
    // the call returns to the native continuation.
    state.r[1] = limbs;
    state.r[2] = Read32(player + kPlayerJointTable);
    state.r[3] = kRollDashBlendBase;
    state.vfp[0] = Bits(mBlendWeight);
    ++mDashBlendPoses;
    return call(RollStep::None, kAnimationInterpolate, kRollSpeedReturn);
  case RollStep::None:
    break;
  }
  return false;
}

} // namespace Oot3dNativeGame
