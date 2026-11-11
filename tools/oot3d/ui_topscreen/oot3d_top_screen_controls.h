#pragma once

#include "oot3d_top_screen_config.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

enum class TopScreenDpadDirection : std::uint8_t {
  Up = 0,
  Down = 1,
  Left = 2,
  Right = 3,
};

inline constexpr std::size_t kTopScreenDpadDirectionCount = 4U;
inline constexpr std::size_t kTopScreenDpadActionCount = 15U;
inline constexpr std::array<std::uint8_t, 7> kTopScreenCStickAimSpeeds{
    2U, 3U, 4U, 6U, 8U, 12U, 16U};

struct TopScreenDpadPhysicalState {
  std::array<bool, kTopScreenDpadDirectionCount> Pressed{};
  std::array<bool, kTopScreenDpadDirectionCount> Held{};
  bool ChildLink = false;
};

struct TopScreenDpadActionState {
  std::array<bool, kTopScreenDpadActionCount> Pressed{};
  std::array<bool, kTopScreenDpadActionCount> Held{};

  [[nodiscard]] bool WasPressed(TopScreenDpadAction action) const noexcept;
  [[nodiscard]] bool IsHeld(TopScreenDpadAction action) const noexcept;
};

// Resolves host D-pad state through the exact child/adult assignment tables.
// The result contains semantic actions only; guest mutations and native calls
// remain owned by the application-side TopScreen consumer.
TopScreenDpadActionState ResolveTopScreenDpadActions(
    const TopScreenUiConfig &config,
    const TopScreenDpadPhysicalState &physical) noexcept;

// Per direction, not per action: each Item direction is its own D-pad item
// button (6 + direction), so two of them must stay distinguishable.
struct TopScreenDpadItemLanes {
  std::array<bool, kTopScreenDpadDirectionCount> Held{};
};

TopScreenDpadItemLanes ResolveTopScreenDpadItemLanes(
    const TopScreenUiConfig &config,
    const TopScreenDpadPhysicalState &physical) noexcept;

// One age's slot for each D-pad item button: Item directions, and the
// child's Boomerang and Slingshot; kTopScreenDpadItemSlotNone elsewhere.
std::array<std::uint8_t, kTopScreenDpadDirectionCount>
TopScreenDpadItemSlots(const TopScreenUiConfig &config,
                       bool childLink) noexcept;

// The Items-page picker: the direction takes the slot as an Item for that
// age; another of that age's directions holding the slot gives it up.
void AssignTopScreenDpadItem(TopScreenUiConfig &config, bool childLink,
                             std::size_t direction, std::uint8_t slot) noexcept;

struct TopScreenCStickAimPolicy {
  float SpeedMultiplier = 1.0F;
  bool InvertX = false;
  bool InvertY = false;
};

TopScreenCStickAimPolicy
ResolveTopScreenCStickAimPolicy(const TopScreenUiConfig &config) noexcept;

// The persisted 2.1.1 option is retained under its original schema name, but
// this coefficient belongs to native C-stick aiming. Free-camera ownership
// consumes raw analog or relative mouse motion through its independent path.
float ResolveTopScreenCStickSmoothingCoefficient(
    TopScreenFreeCameraSmoothing smoothing) noexcept;

} // namespace Oot3dNativeGame
