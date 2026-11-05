#pragma once

// The host menu finger (L3+R3 or another ImGui window) as ImGui input on the
// Switch touchscreen, which has no ImGui platform backend on deko3d.
//
// ImGui gets a touch-screen mouse. The pointer goes to where the finger
// landed and, two polls later (once ImGui has drawn a frame with it, so its
// hover test is current), the gesture either presses there (an item is under
// the finger: buttons, checkboxes, tabs, combo items, sliders, scrollbars)
// or treats the finger as a pan of the window under it.
//
// A tap is a finger that lifts before it moves more than the tap slop. A tap
// always clicks where it landed, however short it is and whatever was
// hovered: the pressed pointer stays still until the finger leaves the slop,
// so the finger rolling as it lifts can't carry the release off a small
// button (which would cancel the click). Past the slop a pressed item is
// dragged (sliders, scrollbars) and empty space scrolls with the finger.
//
// A finger that lands near a snap target (the title-bar close buttons: ImGui
// draws them a font height wide, under 2 mm on a Switch Lite) lands on its
// centre. When the finger lifts the pointer leaves the screen, so no hover
// or held state is left behind for controller navigation.

#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

// ImGui display coordinates.
struct HostMenuTouchRect {
  float MinX = 0.0F;
  float MinY = 0.0F;
  float MaxX = 0.0F;
  float MaxY = 0.0F;
};

struct HostMenuTouchPoint {
  float X = 0.0F;
  float Y = 0.0F;
};

// The centre of the target nearest to (x, y) within radius (0 inside it),
// else (x, y). Ties go to the earlier target.
[[nodiscard]] HostMenuTouchPoint
SnapHostMenuTouchPoint(float x, float y,
                       std::span<const HostMenuTouchRect> targets,
                       float radius) noexcept;

struct HostMenuTouchSample {
  bool Pressed = false;
  // ImGui display coordinates.
  float X = 0.0F;
  float Y = 0.0F;
  // ImGui::IsAnyItemHovered() for the pointer sent so far.
  bool ItemHovered = false;
  // Display pixels a finger may move and still tap.
  float TapSlop = 16.0F;
  // Read when the finger lands.
  std::span<const HostMenuTouchRect> SnapTargets{};
  float SnapRadius = 0.0F;
};

// Applied in this order: pointer, press, release, clear, scroll.
struct HostMenuTouchEvents {
  bool MovePointer = false;
  float PointerX = 0.0F;
  float PointerY = 0.0F;
  bool Press = false;
  bool Release = false;
  // Pointer off-screen (AddMousePosEvent(-FLT_MAX, -FLT_MAX)).
  bool ClearPointer = false;
  // Display pixels the content under the finger follows (down positive).
  float ScrollY = 0.0F;
};

class HostMenuTouchGesture {
public:
  // Polls between sending the pointer and reading ImGui's hover for it.
  static constexpr std::uint8_t kHoverSettlePolls = 2;

  [[nodiscard]] HostMenuTouchEvents
  Update(const HostMenuTouchSample &sample) noexcept;
  [[nodiscard]] bool Idle() const noexcept { return mPhase == Phase::Idle; }

private:
  enum class Phase : std::uint8_t { Idle, Settling, Pressing, Panning };
  Phase mPhase = Phase::Idle;
  std::uint8_t mSettle = 0;
  // The finger left the tap slop (sticky until it lifts).
  bool mMoved = false;
  // Where the finger landed, and where the pointer went (snapped).
  float mDownX = 0.0F;
  float mDownY = 0.0F;
  float mPointX = 0.0F;
  float mPointY = 0.0F;
  // The last finger position sent (pointer moves, scrolling).
  float mLastX = 0.0F;
  float mLastY = 0.0F;
};

} // namespace Oot3dNativeGame
