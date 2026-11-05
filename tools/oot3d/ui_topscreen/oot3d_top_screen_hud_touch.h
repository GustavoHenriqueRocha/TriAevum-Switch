#pragma once

// Touchscreen taps on the TopScreen gameplay HUD (Switch handheld).
//
// The 3DS lets the player tap the lower-screen item buttons; TopScreen moves
// those controls into the top-screen HUD, which the host redraws on a
// 400x240 canvas. A tap on one of the pictured controls produces exactly what
// the physical control it stands for produces (X, Y, Item I = ZR,
// Item II = ZL, B, or a D-pad direction for the action icons around the
// cross), so every existing consumer decides what happens.
//
// Each finger gets an owner when it lands and keeps it until it lifts: the
// native page touch (title, file select, pause pages), one HUD control, or
// nothing. A finger that lands outside a control never reaches gameplay and
// never becomes a control by sliding onto one.

#include "oot3d_ui/ui_primitives.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

enum class TopScreenHudTouchControl : std::uint8_t {
  None,
  X,
  Y,
  ItemI,
  ItemII,
  B,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
};

// The 3DS pad bits (ThreeDsRecomp::Input::Button) of the physical control a
// HUD control pictures. ZR and ZL are the extra bits that TopScreen reads as
// Item I and Item II.
[[nodiscard]] std::uint32_t
TopScreenHudTouchButtons(TopScreenHudTouchControl control) noexcept;

// A rectangle of the 400x240 TopScreen canvas in window pixels, through the
// viewport the N64 UI renderer fits the canvas into
// (FitUiPresentationViewport).
[[nodiscard]] oot3d::ui::UiRect
MapTopScreenCanvasRectToWindow(const oot3d::ui::UiRect &canvas,
                               std::uint32_t windowWidth,
                               std::uint32_t windowHeight) noexcept;

// Hit areas of the drawn HUD controls, in window pixels.
struct TopScreenHudTouchTargets {
  // X, Y, Item I, Item II and B: the touch-cluster circles (source quads 0-4).
  std::array<oot3d::ui::UiRect, 5> Buttons{};
  std::array<bool, 5> ButtonVisible{};
  // The cross together with the action icons around it.
  oot3d::ui::UiRect Dpad{};
  float DpadCenterX = 0.0F;
  float DpadCenterY = 0.0F;
  // Taps nearer the centre than this pick no direction.
  float DpadDeadZone = 0.0F;
  bool DpadVisible = false;
};

// Reads the controls out of the gameplay HUD primitives the host drew last
// (lastTopScreenPrimitivesBySubsystem[GameplayHud], after HUD scale, margins
// and clipping). Controls that are hidden or faded out are left out.
[[nodiscard]] TopScreenHudTouchTargets ResolveTopScreenHudTouchTargets(
    std::span<const oot3d::ui::UiPrimitive> hud, std::uint32_t windowWidth,
    std::uint32_t windowHeight) noexcept;

// The control under a window point, or None.
[[nodiscard]] TopScreenHudTouchControl
HitTestTopScreenHudTouch(const TopScreenHudTouchTargets &targets, float windowX,
                         float windowY) noexcept;

enum class TopScreenTouchOwner : std::uint8_t {
  Unassigned,
  Page,
  Hud,
  Ignored,
};

// One contact of one touchscreen sample. X and Y are normalised to the panel
// (0..1). `End` marks the last sample of a contact.
struct TopScreenTouchContact {
  std::uint32_t FingerId = 0;
  float X = 0.0F;
  float Y = 0.0F;
  bool End = false;
};

struct TopScreenTouchFinger {
  bool Active = false;
  bool Held = false;
  std::uint32_t Id = 0;
  // Consuming polls the press still owes the guest after the finger lifted,
  // so a tap shorter than one poll is still seen.
  std::uint8_t PendingPolls = 0;
  float DownX = 0.0F;
  float DownY = 0.0F;
  float X = 0.0F;
  float Y = 0.0F;
  TopScreenTouchOwner Owner = TopScreenTouchOwner::Unassigned;
  TopScreenHudTouchControl Control = TopScreenHudTouchControl::None;
};

// Fingers tracked across touchscreen samples. Fed every new sample in order,
// so a tap that lands and lifts between two polls is latched, not missed.
class TopScreenTouchTracker {
public:
  static constexpr std::size_t kMaxFingers = 16;

  // One sample: every contact on the panel at that time. A held finger
  // absent from the sample has lifted.
  void ObserveSample(std::span<const TopScreenTouchContact> contacts) noexcept;
  void Reset() noexcept { mFingers = {}; }
  [[nodiscard]] std::span<TopScreenTouchFinger> Fingers() noexcept {
    return mFingers;
  }

private:
  std::array<TopScreenTouchFinger, kMaxFingers> mFingers{};
};

struct TopScreenTouchPollContext {
  // A native touch page is open (title, file select, a pause page).
  bool PageTouchEnabled = false;
  // Host UI (ImGui, the debug menu) owns input.
  bool HostUiVisible = false;
  // Empty outside TopScreen gameplay.
  const TopScreenHudTouchTargets *Targets = nullptr;
  std::uint32_t WindowWidth = 0;
  std::uint32_t WindowHeight = 0;
};

struct TopScreenTouchPollResult {
  // The page pointer: the first page-owned finger, as before (one pointer).
  bool PagePressed = false;
  float PageX = 0.0F;
  float PageY = 0.0F;
  // Pad bits of every HUD control held by a finger, outside pages and host UI.
  std::uint32_t HudButtons = 0;
};

// A HUD press lasts at least this many consuming polls (guest refreshes):
// the game reads some buttons at 30 Hz.
inline constexpr std::uint8_t kTopScreenHudMinimumPressPolls = 2;

// Gives new fingers their owner, reports what the fingers hold this poll and,
// on a poll a guest refresh consumes, retires presses already delivered.
[[nodiscard]] TopScreenTouchPollResult
ResolveTopScreenTouchPoll(TopScreenTouchTracker &tracker,
                          const TopScreenTouchPollContext &context,
                          bool guestRefreshWillConsume) noexcept;

} // namespace Oot3dNativeGame
