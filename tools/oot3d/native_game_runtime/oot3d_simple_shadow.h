#pragma once

// L3+R3 > Game > Gameplay > "Simple shadow" (config/simple_shadow, present =
// on; off by default; deko3d only).
//
// Link's shadow is Player_DrawFeetShadow (EUR 0x00297E04), called by
// Actor_Draw through shape.shadowDraw (0x002D6498, back at 0x002D649C): unless
// he rides Epona (stateFlags1 0x00800000) it tail-calls ActorShadow_DrawFeet
// (0x001D04F4), the two foot spots and, high up, the round shadow. "OoT3D:
// Single Screen Experience" 3.2.2 by M-1 replaces that branch: on a floor no
// steeper than a normal of 0.3, from 50 units under it to 1500 over it, it
// draws his whole body flattened onto the floor and runs the foot shadows at
// shadow alpha 0, which keeps their floor bookkeeping (feetFloorFlags). Here
// the hook at the entry decides the same, writes the alpha 0 and points r14 at
// itself, so ActorShadow_DrawFeet returns into the hook, which puts the alpha
// back and leaves the compiled graph at the caller. The body is drawn by the
// deko3d backend from the frame this hook decided (fast/oot3d/player_shadow.h).
// The rules are plain code, tested on the host (oot3d_simple_shadow_tests).

#include "fast/oot3d/player_shadow.h"
#include "recomp/a32_runtime.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>

namespace Oot3dNativeGame {

inline constexpr std::uint32_t kSimpleShadowHookPc = 0x00297E04U; // Player_DrawFeetShadow

inline constexpr std::uint32_t kSimpleShadowIdOffset = 0x0000U;          // s16, ACTOR_PLAYER = 0
inline constexpr std::uint32_t kSimpleShadowPositionOffset = 0x0028U;    // world.pos (Vec3f)
inline constexpr std::uint32_t kSimpleShadowFloorPolyOffset = 0x007CU;   // floorPoly
inline constexpr std::uint32_t kSimpleShadowFloorHeightOffset = 0x0084U; // floorHeight (f32)
inline constexpr std::uint32_t kSimpleShadowAlphaOffset = 0x00D0U;       // shape.shadowAlpha (u8)
inline constexpr std::uint32_t kSimpleShadowStateFlags1Offset = 0x1710U;
inline constexpr std::uint32_t kSimpleShadowStateFlags2Offset = 0x1714U;
inline constexpr std::uint32_t kSimpleShadowRiding = 0x00800000U;  // stateFlags1: on Epona
inline constexpr std::uint32_t kSimpleShadowHidden = 0x20000000U;  // stateFlags2: the mod skips him
// CollisionPoly normal (s16 x, y, z, 1/32767 units).
inline constexpr std::uint32_t kSimpleShadowPolyNormalOffset = 0x000AU;
inline constexpr float kSimpleShadowNormalScale = 1.0F / 32767.0F;
inline constexpr float kSimpleShadowMinimumFloorNormal = 0.3F;
inline constexpr float kSimpleShadowLowest = -50.0F;
inline constexpr float kSimpleShadowHighest = 1500.0F;

// Read by the hook, written by the panel.
inline std::atomic<bool> &SimpleShadowEnabled() noexcept {
  static std::atomic<bool> enabled{false};
  return enabled;
}

inline constexpr const char *kSimpleShadowConfigName = "simple_shadow";

inline bool ReadSimpleShadowEnabled(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kSimpleShadowConfigName, error);
}

// Creates (on) or removes (off) the file; false if that failed.
inline bool SaveSimpleShadowEnabled(const std::filesystem::path &config, bool enabled) {
  std::error_code error;
  if (!enabled) {
    std::filesystem::remove(config / kSimpleShadowConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kSimpleShadowConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

// What the decision reads of Link's actor and his floor.
struct SimpleShadowActor final {
  std::uint32_t Address = 0U;
  std::uint16_t Id = 0xFFFFU;
  std::array<float, 3> Position{};
  std::uint32_t FloorPoly = 0U;
  float FloorHeight = 0.0F;
  std::uint8_t ShadowAlpha = 0U;
  std::uint32_t StateFlags1 = 0U;
  std::uint32_t StateFlags2 = 0U;
  std::array<std::int16_t, 3> FloorNormal{};
};

// The mod's conditions; the shadow of this frame when they hold.
inline std::optional<Fast::Oot3d::PlayerShadowFrame> DecideSimpleShadow(const SimpleShadowActor &actor) noexcept {
  if (actor.Id != 0U || (actor.StateFlags1 & kSimpleShadowRiding) != 0U ||
      (actor.StateFlags2 & kSimpleShadowHidden) != 0U || actor.FloorPoly == 0U || actor.ShadowAlpha == 0U)
    return std::nullopt;
  const float height = actor.Position[1] - actor.FloorHeight;
  if (!std::isfinite(height) || !std::isfinite(actor.Position[0]) || !std::isfinite(actor.Position[2]) ||
      height < kSimpleShadowLowest || height >= kSimpleShadowHighest)
    return std::nullopt;
  const std::array<float, 3> normal{actor.FloorNormal[0] * kSimpleShadowNormalScale,
                                    actor.FloorNormal[1] * kSimpleShadowNormalScale,
                                    actor.FloorNormal[2] * kSimpleShadowNormalScale};
  if (normal[1] < kSimpleShadowMinimumFloorNormal) return std::nullopt;
  const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
  Fast::Oot3d::PlayerShadowFrame frame;
  frame.Actor = actor.Address;
  frame.FloorPoint = {actor.Position[0], actor.FloorHeight, actor.Position[2]};
  frame.FloorNormal = {normal[0] / length, normal[1] / length, normal[2] / length};
  frame.Height = height;
  frame.Opacity = Fast::Oot3d::PlayerShadowOpacity(actor.ShadowAlpha, height);
  return frame;
}

// `Memory`: Read8/Read16/Read32 as oot3d::recomp::a32::MemoryBus has them.
template <typename Memory>
std::optional<SimpleShadowActor> ReadSimpleShadowActor(Memory &memory, std::uint32_t address) {
  SimpleShadowActor actor;
  actor.Address = address;
  std::array<std::uint32_t, 3> position{};
  std::uint32_t floorHeight = 0U;
  if (address == 0U || !memory.Read16(address + kSimpleShadowIdOffset, &actor.Id) ||
      !memory.Read32(address + kSimpleShadowPositionOffset, &position[0]) ||
      !memory.Read32(address + kSimpleShadowPositionOffset + 4U, &position[1]) ||
      !memory.Read32(address + kSimpleShadowPositionOffset + 8U, &position[2]) ||
      !memory.Read32(address + kSimpleShadowFloorPolyOffset, &actor.FloorPoly) ||
      !memory.Read32(address + kSimpleShadowFloorHeightOffset, &floorHeight) ||
      !memory.Read8(address + kSimpleShadowAlphaOffset, &actor.ShadowAlpha) ||
      !memory.Read32(address + kSimpleShadowStateFlags1Offset, &actor.StateFlags1) ||
      !memory.Read32(address + kSimpleShadowStateFlags2Offset, &actor.StateFlags2))
    return std::nullopt;
  for (std::size_t axis = 0; axis < 3U; ++axis) actor.Position[axis] = std::bit_cast<float>(position[axis]);
  actor.FloorHeight = std::bit_cast<float>(floorHeight);
  if (actor.FloorPoly != 0U) {
    for (std::uint32_t axis = 0; axis < 3U; ++axis) {
      std::uint16_t component = 0U;
      if (!memory.Read16(actor.FloorPoly + kSimpleShadowPolyNormalOffset + 2U * axis, &component))
        return std::nullopt;
      actor.FloorNormal[axis] = static_cast<std::int16_t>(component);
    }
  }
  return actor;
}

// The hook at kSimpleShadowHookPc (whole-AOT execution) and the shadow it
// decided for the window to publish with the native frame.
class SimpleShadowRuntime final {
public:
  // The pc to leave the compiled graph at, or nothing to run the block as is.
  // `compiled`: called from whole-AOT code, the only place a sequence starts
  // and the graph can be left.
  template <typename Memory>
  std::optional<std::uint32_t> Execute(oot3d::recomp::a32::GuestState &state, Memory &memory, bool enabled,
                                       bool compiled) {
    if (mFeetPending) {
      // The actor's alpha back. ActorShadow_DrawFeet's own calls leave r14
      // inside it: then this is its return, and the caller is next. A new call
      // (r14 its return) means the last one never came back.
      mFeetPending = false;
      (void)memory.Write8(mActor + kSimpleShadowAlphaOffset, mAlpha);
      if (state.r[14] != mCaller) {
        state.r[14] = mCaller;
        if (compiled) return mCaller;
        // Outside compiled code the block runs again with the call's
        // arguments: the foot shadows once more, at the actor's alpha, then
        // back to the caller.
        for (std::size_t index = 0; index < mArguments.size(); ++index) state.r[index] = mArguments[index];
        return std::nullopt;
      }
    }
    if (!enabled || !compiled) return std::nullopt;
    const auto actor = ReadSimpleShadowActor(memory, state.r[0]);
    if (!actor) return std::nullopt;
    const auto frame = DecideSimpleShadow(*actor);
    if (!frame || !memory.Write8(actor->Address + kSimpleShadowAlphaOffset, 0U)) return std::nullopt;
    mFrame = frame;
    mActor = actor->Address;
    mAlpha = actor->ShadowAlpha;
    mCaller = state.r[14];
    for (std::size_t index = 0; index < mArguments.size(); ++index) mArguments[index] = state.r[index];
    mFeetPending = true;
    state.r[14] = kSimpleShadowHookPc;
    ++mShadowFrames;
    return std::nullopt; // the riding test, then ActorShadow_DrawFeet at alpha 0, back here
  }
  // ActorShadow_DrawFeet runs at alpha 0 and returns into the hook.
  [[nodiscard]] bool Pending() const noexcept { return mFeetPending; }
  // A state load: the loaded memory has the actor's own alpha.
  void Reset() noexcept {
    mFeetPending = false;
    mFrame.reset();
  }
  // The shadow decided since the last call (nothing: none).
  [[nodiscard]] std::optional<Fast::Oot3d::PlayerShadowFrame> TakeFrame() noexcept {
    return std::exchange(mFrame, std::nullopt);
  }
  [[nodiscard]] std::uint64_t ShadowFrames() const noexcept { return mShadowFrames; }

private:
  bool mFeetPending = false;
  std::uint32_t mActor = 0U;
  std::uint8_t mAlpha = 0U;
  std::uint32_t mCaller = 0U;
  std::array<std::uint32_t, 3> mArguments{}; // actor, lights, play
  std::optional<Fast::Oot3d::PlayerShadowFrame> mFrame;
  std::uint64_t mShadowFrames = 0U;
};

} // namespace Oot3dNativeGame
