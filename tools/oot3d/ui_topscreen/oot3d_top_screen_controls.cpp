#include "oot3d_top_screen_controls.h"

#include <algorithm>

namespace Oot3dNativeGame {
namespace {

std::size_t ActionIndex(TopScreenDpadAction action) noexcept {
  return static_cast<std::size_t>(action);
}

} // namespace

bool TopScreenDpadActionState::WasPressed(
    TopScreenDpadAction action) const noexcept {
  const auto index = ActionIndex(action);
  return index < Pressed.size() && Pressed[index];
}

bool TopScreenDpadActionState::IsHeld(
    TopScreenDpadAction action) const noexcept {
  const auto index = ActionIndex(action);
  return index < Held.size() && Held[index];
}

TopScreenDpadActionState ResolveTopScreenDpadActions(
    const TopScreenUiConfig &config,
    const TopScreenDpadPhysicalState &physical) noexcept {
  TopScreenDpadActionState resolved;
  const auto &mapping =
      physical.ChildLink ? config.ChildDpad : config.AdultDpad;
  for (std::size_t direction = 0U; direction < mapping.size(); ++direction) {
    const auto action = mapping[direction];
    const auto actionIndex = ActionIndex(action);
    if (action == TopScreenDpadAction::None ||
        actionIndex >= kTopScreenDpadActionCount) {
      continue;
    }
    resolved.Pressed[actionIndex] =
        resolved.Pressed[actionIndex] || physical.Pressed[direction];
    resolved.Held[actionIndex] =
        resolved.Held[actionIndex] || physical.Held[direction];
  }
  return resolved;
}

TopScreenDpadItemLanes ResolveTopScreenDpadItemLanes(
    const TopScreenUiConfig &config,
    const TopScreenDpadPhysicalState &physical) noexcept {
  TopScreenDpadItemLanes lanes;
  const auto slots = TopScreenDpadItemSlots(config, physical.ChildLink);
  for (std::size_t direction = 0U; direction < slots.size(); ++direction) {
    lanes.Held[direction] = slots[direction] != kTopScreenDpadItemSlotNone &&
                            physical.Held[direction];
  }
  return lanes;
}

std::array<std::uint8_t, kTopScreenDpadDirectionCount>
TopScreenDpadItemSlots(const TopScreenUiConfig &config,
                       bool childLink) noexcept {
  const auto &mapping = childLink ? config.ChildDpad : config.AdultDpad;
  const auto &slots =
      childLink ? config.ChildDpadItemSlots : config.AdultDpadItemSlots;
  std::array<std::uint8_t, kTopScreenDpadDirectionCount> result{};
  for (std::size_t direction = 0U; direction < result.size(); ++direction) {
    const auto action = mapping[direction];
    result[direction] = kTopScreenDpadItemSlotNone;
    if (action == TopScreenDpadAction::Item &&
        IsTopScreenDpadItemSlot(slots[direction]))
      result[direction] = slots[direction];
    // 3.2.2 0x005D16B0: the child's Boomerang and Slingshot are D-pad item
    // buttons too (slots 12 and 6); the adult's are nothing.
    else if (childLink && action == TopScreenDpadAction::Boomerang)
      result[direction] = 12U;
    else if (childLink && action == TopScreenDpadAction::Slingshot)
      result[direction] = 6U;
  }
  return result;
}

void AssignTopScreenDpadItem(TopScreenUiConfig &config, bool childLink,
                             std::size_t direction, std::uint8_t slot) noexcept {
  auto &actions = childLink ? config.ChildDpad : config.AdultDpad;
  auto &slots =
      childLink ? config.ChildDpadItemSlots : config.AdultDpadItemSlots;
  if (direction >= actions.size() || !IsTopScreenDpadItemSlot(slot))
    return;
  for (std::size_t other = 0U; other < slots.size(); ++other) {
    if (other != direction && actions[other] == TopScreenDpadAction::Item &&
        slots[other] == slot) {
      actions[other] = TopScreenDpadAction::None;
      slots[other] = kTopScreenDpadItemSlotNone;
    }
  }
  actions[direction] = TopScreenDpadAction::Item;
  slots[direction] = slot;
}

TopScreenCStickAimPolicy
ResolveTopScreenCStickAimPolicy(const TopScreenUiConfig &config) noexcept {
  constexpr std::size_t kDefaultSpeedIndex = 3U;
  const auto index = std::min<std::size_t>(
      config.CStickAimSpeedLevel, kTopScreenCStickAimSpeeds.size() - 1U);
  return {static_cast<float>(kTopScreenCStickAimSpeeds[index]) /
              static_cast<float>(kTopScreenCStickAimSpeeds[kDefaultSpeedIndex]),
          config.CStickAimInvertX, config.CStickAimInvertY};
}

float ResolveTopScreenCStickSmoothingCoefficient(
    TopScreenFreeCameraSmoothing smoothing) noexcept {
  constexpr std::array<std::uint16_t, 5> kSmoothingFrames{0U, 60U, 120U, 190U,
                                                          300U};
  constexpr float kNativeSampleRate = 33.3333F;
  const auto index = std::min<std::size_t>(static_cast<std::size_t>(smoothing),
                                           kSmoothingFrames.size() - 1U);
  return kNativeSampleRate /
         (static_cast<float>(kSmoothingFrames[index]) + kNativeSampleRate);
}

} // namespace Oot3dNativeGame
