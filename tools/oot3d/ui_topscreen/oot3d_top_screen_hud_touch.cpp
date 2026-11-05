#include "oot3d_top_screen_hud_touch.h"

#include "oot3d_ui/ui_contract_types.h"
#include "three_ds_input.h"
#include "oot3d/renderer/ui_presentation_layout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Oot3dNativeGame {
namespace {

using oot3d::ui::UiPrimitive;
using oot3d::ui::UiPrimitiveRole;
using oot3d::ui::UiRect;

constexpr float kCanvasWidth = 400.0F;
constexpr float kCanvasHeight = 240.0F;
// Touch cluster of payload 0x005C9940 (BuildTopScreenTouchClusterGeometry):
// layer 12, source quads 0-3 X, Y, Item I, Item II; 4 B; 5 the D-pad cross.
// Its labels and auxiliary quads share the owner on layer 13.
constexpr std::uint32_t kTouchClusterOwner = 0x005C9940U;
constexpr std::uint32_t kTouchClusterLayer = 12U;
constexpr std::uint32_t kDpadCrossQuad = 5U;
// D-pad action icons, View/Navi eyes and cycle marks
// (AppendTopScreenDpadPresentation).
constexpr std::uint32_t kDpadIconOwner = 0x005D4A8CU;
// Below this the HUD is fading out (cutscenes, menus) and takes no taps.
constexpr float kMinimumAlpha = 0.5F;
// Hit areas grow past the drawn circle by this share of its size on each
// side: a 28.8-unit circle at the default 0.9 scale is 86 pixels wide.
constexpr float kButtonPadding = 0.2F;
constexpr float kDpadPadding = 0.1F;
constexpr float kDpadDeadZone = 0.2F;

bool Takes(const UiPrimitive &primitive) noexcept {
  return primitive.visible && primitive.color.alpha >= kMinimumAlpha &&
         primitive.destination.width > 0.0F &&
         primitive.destination.height > 0.0F;
}

UiRect Union(const UiRect &a, const UiRect &b) noexcept {
  const float left = std::min(a.x, b.x);
  const float top = std::min(a.y, b.y);
  const float right = std::max(a.x + a.width, b.x + b.width);
  const float bottom = std::max(a.y + a.height, b.y + b.height);
  return {left, top, right - left, bottom - top};
}

UiRect Grow(const UiRect &rect, float share) noexcept {
  const float dx = rect.width * share;
  const float dy = rect.height * share;
  return {rect.x - dx, rect.y - dy, rect.width + 2.0F * dx,
          rect.height + 2.0F * dy};
}

bool Contains(const UiRect &rect, float x, float y) noexcept {
  return x >= rect.x && y >= rect.y && x < rect.x + rect.width &&
         y < rect.y + rect.height;
}

} // namespace

std::uint32_t
TopScreenHudTouchButtons(TopScreenHudTouchControl control) noexcept {
  using ThreeDsRecomp::Input::Button;
  using ThreeDsRecomp::Input::ButtonMask;
  switch (control) {
  case TopScreenHudTouchControl::X:
    return ButtonMask(Button::X);
  case TopScreenHudTouchControl::Y:
    return ButtonMask(Button::Y);
  case TopScreenHudTouchControl::ItemI:
    return ButtonMask(Button::Zr);
  case TopScreenHudTouchControl::ItemII:
    return ButtonMask(Button::Zl);
  case TopScreenHudTouchControl::B:
    return ButtonMask(Button::B);
  case TopScreenHudTouchControl::DpadUp:
    return ButtonMask(Button::DpadUp);
  case TopScreenHudTouchControl::DpadDown:
    return ButtonMask(Button::DpadDown);
  case TopScreenHudTouchControl::DpadLeft:
    return ButtonMask(Button::DpadLeft);
  case TopScreenHudTouchControl::DpadRight:
    return ButtonMask(Button::DpadRight);
  case TopScreenHudTouchControl::None:
    break;
  }
  return 0U;
}

UiRect MapTopScreenCanvasRectToWindow(const UiRect &canvas,
                                      std::uint32_t windowWidth,
                                      std::uint32_t windowHeight) noexcept {
  const auto viewport = Oot3d::Renderer::FitUiPresentationViewport(
      windowWidth, windowHeight, kCanvasWidth, kCanvasHeight);
  if (!viewport.Valid()) {
    return {};
  }
  const float scaleX = static_cast<float>(viewport.Width) / kCanvasWidth;
  const float scaleY = static_cast<float>(viewport.Height) / kCanvasHeight;
  return {static_cast<float>(viewport.X) + canvas.x * scaleX,
          static_cast<float>(viewport.Y) + canvas.y * scaleY,
          canvas.width * scaleX, canvas.height * scaleY};
}

TopScreenHudTouchTargets
ResolveTopScreenHudTouchTargets(std::span<const UiPrimitive> hud,
                                std::uint32_t windowWidth,
                                std::uint32_t windowHeight) noexcept {
  TopScreenHudTouchTargets targets;
  UiRect cross{};
  bool crossVisible = false;
  for (const auto &primitive : hud) {
    if (primitive.subsystem != oot3d::ui::UiSubsystem::GameplayHud ||
        primitive.role != UiPrimitiveRole::TouchControl ||
        primitive.owner_address != kTouchClusterOwner ||
        primitive.layer != kTouchClusterLayer || !Takes(primitive)) {
      continue;
    }
    if (primitive.source_quad < targets.Buttons.size()) {
      targets.Buttons[primitive.source_quad] = MapTopScreenCanvasRectToWindow(
          primitive.destination, windowWidth, windowHeight);
      targets.ButtonVisible[primitive.source_quad] = true;
    } else if (primitive.source_quad == kDpadCrossQuad) {
      cross = primitive.destination;
      crossVisible = true;
    }
  }
  if (!crossVisible) {
    return targets;
  }
  // The icons sit around the cross, outside it; the zone covers them all.
  UiRect zone = cross;
  for (const auto &primitive : hud) {
    if (primitive.subsystem == oot3d::ui::UiSubsystem::GameplayHud &&
        primitive.role == UiPrimitiveRole::ActionButton &&
        primitive.owner_address == kDpadIconOwner && primitive.visible &&
        primitive.color.alpha > 0.0F) {
      zone = Union(zone, primitive.destination);
    }
  }
  const auto windowCross =
      MapTopScreenCanvasRectToWindow(cross, windowWidth, windowHeight);
  targets.Dpad = Grow(
      MapTopScreenCanvasRectToWindow(zone, windowWidth, windowHeight),
      kDpadPadding);
  targets.DpadCenterX = windowCross.x + windowCross.width * 0.5F;
  targets.DpadCenterY = windowCross.y + windowCross.height * 0.5F;
  targets.DpadDeadZone =
      std::min(windowCross.width, windowCross.height) * kDpadDeadZone;
  targets.DpadVisible = windowCross.width > 0.0F;
  return targets;
}

TopScreenHudTouchControl
HitTestTopScreenHudTouch(const TopScreenHudTouchTargets &targets,
                         float windowX, float windowY) noexcept {
  constexpr std::array<TopScreenHudTouchControl, 5> kButtons{
      TopScreenHudTouchControl::X, TopScreenHudTouchControl::Y,
      TopScreenHudTouchControl::ItemI, TopScreenHudTouchControl::ItemII,
      TopScreenHudTouchControl::B};
  if (!std::isfinite(windowX) || !std::isfinite(windowY)) {
    return TopScreenHudTouchControl::None;
  }
  // The padded circles of the diamond overlap; the nearest centre wins.
  auto hit = TopScreenHudTouchControl::None;
  float nearest = std::numeric_limits<float>::max();
  for (std::size_t index = 0; index < kButtons.size(); ++index) {
    const auto &rect = targets.Buttons[index];
    if (!targets.ButtonVisible[index] ||
        !Contains(Grow(rect, kButtonPadding), windowX, windowY)) {
      continue;
    }
    const float dx = windowX - (rect.x + rect.width * 0.5F);
    const float dy = windowY - (rect.y + rect.height * 0.5F);
    const float distance = dx * dx + dy * dy;
    if (distance < nearest) {
      nearest = distance;
      hit = kButtons[index];
    }
  }
  if (hit != TopScreenHudTouchControl::None || !targets.DpadVisible ||
      !Contains(targets.Dpad, windowX, windowY)) {
    return hit;
  }
  const float dx = windowX - targets.DpadCenterX;
  const float dy = windowY - targets.DpadCenterY;
  if (std::max(std::abs(dx), std::abs(dy)) < targets.DpadDeadZone) {
    return TopScreenHudTouchControl::None;
  }
  if (std::abs(dx) > std::abs(dy)) {
    return dx < 0.0F ? TopScreenHudTouchControl::DpadLeft
                     : TopScreenHudTouchControl::DpadRight;
  }
  return dy < 0.0F ? TopScreenHudTouchControl::DpadUp
                   : TopScreenHudTouchControl::DpadDown;
}

void TopScreenTouchTracker::ObserveSample(
    std::span<const TopScreenTouchContact> contacts) noexcept {
  std::array<bool, kMaxFingers> seen{};
  for (const auto &contact : contacts) {
    const float x = std::clamp(contact.X, 0.0F, 1.0F);
    const float y = std::clamp(contact.Y, 0.0F, 1.0F);
    auto finger = std::find_if(mFingers.begin(), mFingers.end(),
                               [&](const TopScreenTouchFinger &candidate) {
                                 return candidate.Active && candidate.Held &&
                                        candidate.Id == contact.FingerId;
                               });
    if (finger == mFingers.end()) {
      // The closing sample of a contact already lifted is not a new touch.
      if (contact.End) {
        continue;
      }
      finger = std::find_if(
          mFingers.begin(), mFingers.end(),
          [](const TopScreenTouchFinger &candidate) { return !candidate.Active; });
      if (finger == mFingers.end()) {
        continue;
      }
      *finger = {};
      finger->Active = true;
      finger->Held = true;
      finger->Id = contact.FingerId;
      finger->PendingPolls = 1;
      finger->DownX = x;
      finger->DownY = y;
    }
    finger->X = x;
    finger->Y = y;
    if (contact.End) {
      finger->Held = false;
    }
    seen[static_cast<std::size_t>(finger - mFingers.begin())] = true;
  }
  for (std::size_t index = 0; index < mFingers.size(); ++index) {
    if (!seen[index]) {
      mFingers[index].Held = false;
    }
  }
}

TopScreenTouchPollResult
ResolveTopScreenTouchPoll(TopScreenTouchTracker &tracker,
                          const TopScreenTouchPollContext &context,
                          bool guestRefreshWillConsume) noexcept {
  auto fingers = tracker.Fingers();
  bool pageOwned = std::any_of(
      fingers.begin(), fingers.end(), [](const TopScreenTouchFinger &finger) {
        return finger.Active && finger.Owner == TopScreenTouchOwner::Page;
      });
  for (auto &finger : fingers) {
    if (!finger.Active || finger.Owner != TopScreenTouchOwner::Unassigned) {
      continue;
    }
    finger.Owner = TopScreenTouchOwner::Ignored;
    if (context.HostUiVisible) {
      continue;
    }
    if (context.PageTouchEnabled) {
      // One pointer, as the 3DS touchscreen has: later fingers are ignored.
      if (!pageOwned) {
        finger.Owner = TopScreenTouchOwner::Page;
        pageOwned = true;
      }
      continue;
    }
    if (context.Targets == nullptr) {
      continue;
    }
    finger.Control = HitTestTopScreenHudTouch(
        *context.Targets,
        finger.DownX * static_cast<float>(context.WindowWidth),
        finger.DownY * static_cast<float>(context.WindowHeight));
    if (finger.Control != TopScreenHudTouchControl::None) {
      finger.Owner = TopScreenTouchOwner::Hud;
      finger.PendingPolls =
          std::max(finger.PendingPolls, kTopScreenHudMinimumPressPolls);
    }
  }

  TopScreenTouchPollResult result;
  for (const auto &finger : fingers) {
    if (!finger.Active || (!finger.Held && finger.PendingPolls == 0U)) {
      continue;
    }
    if (finger.Owner == TopScreenTouchOwner::Page && !result.PagePressed &&
        context.PageTouchEnabled && !context.HostUiVisible) {
      result.PagePressed = true;
      result.PageX = finger.X;
      result.PageY = finger.Y;
    } else if (finger.Owner == TopScreenTouchOwner::Hud &&
               !context.PageTouchEnabled && !context.HostUiVisible) {
      result.HudButtons |= TopScreenHudTouchButtons(finger.Control);
    }
  }

  for (auto &finger : fingers) {
    if (!finger.Active) {
      continue;
    }
    if (guestRefreshWillConsume && finger.PendingPolls != 0U) {
      --finger.PendingPolls;
    }
    if (!finger.Held && finger.PendingPolls == 0U) {
      finger = {};
    }
  }
  return result;
}

} // namespace Oot3dNativeGame
