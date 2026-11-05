// Gameplay HUD taps (oot3d_top_screen_hud_touch.h): the controls are read
// from the primitives the real HUD emitters produce at the Switch settings.
#include "oot3d_top_screen_hud_touch.h"

#include "oot3d_top_screen_dpad_presentation.h"
#include "oot3d_top_screen_mod_profile.h"
#include "three_ds_input.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
using Control = TopScreenHudTouchControl;
using ThreeDsRecomp::Input::Button;
using ThreeDsRecomp::Input::ButtonMask;

constexpr std::uint32_t kWidth = 1280;
constexpr std::uint32_t kHeight = 720;

void Require(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool Near(float a, float b) { return std::abs(a - b) < 0.01F; }

// config/topscreen_ui.switch.json: normal layout, scale 0.9, margins 4/1,
// D-pad icons on (adult: view, ocarina, iron boots, hover boots).
TopScreenUiConfig SwitchConfig() {
  TopScreenUiConfig config;
  config.HudLayout = TopScreenHudLayout::Normal;
  config.HudScale = 0.9F;
  config.HudMarginX = 4;
  config.HudMarginY = 1;
  return config;
}

std::vector<oot3d::ui::UiPrimitive> BuildHud(const TopScreenUiConfig &config,
                                             float alpha) {
  oot3d::ui::UiTextureIdentity items;
  oot3d::ui::UiTextureIdentity page;
  items.semantic_name = "native-items";
  page.semantic_name = "native-pause";
  std::vector<oot3d::ui::UiPrimitive> hud;
  AppendTopScreenTouchClusterPresentation(
      BuildTopScreenTouchClusterGeometry({0, 0, 0, 0}, alpha, config.HudLayout),
      page, hud);
  TopScreenDpadPresentationState dpad;
  dpad.OwnedEquipment = 0x7777;
  dpad.EquippedEquipment = 0x1122;
  dpad.Ocarina = 8;
  AppendTopScreenDpadPresentation(config, dpad, {}, alpha, items, page, hud);
  ApplyTopScreenHudScale(hud, config);
  ApplyTopScreenGameplayCanvas(hud);
  return hud;
}

float CenterX(const oot3d::ui::UiRect &rect) { return rect.x + rect.width / 2; }
float CenterY(const oot3d::ui::UiRect &rect) { return rect.y + rect.height / 2; }

void TestCanvasMapping() {
  // Handheld: the 5:3 canvas is fitted at x3 between 40 px side bars.
  const auto handheld =
      MapTopScreenCanvasRectToWindow({0, 0, 400, 240}, kWidth, kHeight);
  Require(Near(handheld.x, 40) && Near(handheld.y, 0) &&
              Near(handheld.width, 1200) && Near(handheld.height, 720),
          "handheld canvas must fill x 40..1240");
  const auto point = MapTopScreenCanvasRectToWindow({100, 60, 10, 20}, kWidth,
                                                    kHeight);
  Require(Near(point.x, 340) && Near(point.y, 180) && Near(point.width, 30) &&
              Near(point.height, 60),
          "canvas units are three handheld pixels");
  const auto docked =
      MapTopScreenCanvasRectToWindow({0, 0, 400, 240}, 1920, 1080);
  Require(Near(docked.x, 60) && Near(docked.width, 1800),
          "1080p canvas must fill x 60..1860");
  const auto none = MapTopScreenCanvasRectToWindow({0, 0, 400, 240}, 0, 0);
  Require(none.width == 0 && none.height == 0, "no window maps to nothing");
}

void TestButtonsUnderTheirCircles() {
  const auto hud = BuildHud(SwitchConfig(), 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(hud, kWidth, kHeight);
  constexpr std::array<Control, 5> kOrder{Control::X, Control::Y,
                                          Control::ItemI, Control::ItemII,
                                          Control::B};
  for (std::size_t index = 0; index < kOrder.size(); ++index) {
    Require(targets.ButtonVisible[index], "every cluster circle is a target");
    const auto &rect = targets.Buttons[index];
    Require(HitTestTopScreenHudTouch(targets, CenterX(rect), CenterY(rect)) ==
                kOrder[index],
            "a circle's centre hits its control");
    // Just outside the drawn edge, toward the screen edge, still counts.
    Require(HitTestTopScreenHudTouch(targets, CenterX(rect),
                                     rect.y + rect.height + 4) != Control::None,
            "a near miss below a circle still hits a control");
  }
  // X: canvas centre (358,20) scaled 0.9 about (400,0), margins -4/+1.
  const auto &x = targets.Buttons[0];
  Require(Near(CenterX(x), 40 + 3 * 358.2F) && Near(CenterY(x), 3 * 19.0F),
          "X sits where the renderer draws it");
  Require(Near(x.width, 3 * 28.8F), "X is 86 pixels wide at 0.9");
  Require(HitTestTopScreenHudTouch(targets, 640, 360) == Control::None,
          "the middle of the screen is no control");
  Require(HitTestTopScreenHudTouch(targets, 20, 700) == Control::None,
          "the side bar is no control");
  Require(TopScreenHudTouchButtons(Control::X) == ButtonMask(Button::X) &&
              TopScreenHudTouchButtons(Control::Y) == ButtonMask(Button::Y) &&
              TopScreenHudTouchButtons(Control::ItemI) ==
                  ButtonMask(Button::Zr) &&
              TopScreenHudTouchButtons(Control::ItemII) ==
                  ButtonMask(Button::Zl) &&
              TopScreenHudTouchButtons(Control::B) == ButtonMask(Button::B) &&
              TopScreenHudTouchButtons(Control::None) == 0U,
          "controls press their physical buttons");
}

void TestDpadIcons() {
  const auto config = SwitchConfig();
  const auto hud = BuildHud(config, 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(hud, kWidth, kHeight);
  Require(targets.DpadVisible, "the D-pad cross is a target");
  std::size_t icons = 0;
  for (const auto &primitive : hud) {
    if (primitive.owner_address != 0x005D4A8CU || primitive.layer != 13U)
      continue;
    // Icon slots: 0..3 by direction; View uses 38.. in direction order.
    const auto rect =
        MapTopScreenCanvasRectToWindow(primitive.destination, kWidth, kHeight);
    const auto hit = HitTestTopScreenHudTouch(targets, CenterX(rect),
                                              CenterY(rect));
    const auto direction = primitive.source_quad >= 38U
                               ? 0U // View is the first adult lane: Up
                               : primitive.source_quad;
    constexpr std::array<Control, 4> kDirections{
        Control::DpadUp, Control::DpadDown, Control::DpadLeft,
        Control::DpadRight};
    Require(hit == kDirections[direction],
            "an action icon presses its D-pad direction");
    ++icons;
  }
  Require(icons == 4, "adult defaults draw view, ocarina and both boots");
  Require(HitTestTopScreenHudTouch(targets, targets.DpadCenterX,
                                   targets.DpadCenterY) == Control::None,
          "the cross centre picks no direction");
  Require(TopScreenHudTouchButtons(Control::DpadDown) ==
              ButtonMask(Button::DpadDown),
          "the ocarina lane is D-pad down");

  // With render_dpad_icons off the bridge gives the cross alpha 0, so the
  // emitter drops it: no D-pad zone, even where an eye is still drawn.
  auto noCross = hud;
  std::erase_if(noCross, [](const oot3d::ui::UiPrimitive &primitive) {
    return primitive.owner_address == 0x005C9940U && primitive.layer == 12U &&
           primitive.source_quad == 5U;
  });
  const auto noDpad = ResolveTopScreenHudTouchTargets(noCross, kWidth, kHeight);
  Require(!noDpad.DpadVisible &&
              HitTestTopScreenHudTouch(noDpad, targets.DpadCenterX,
                                       targets.DpadCenterY +
                                           targets.DpadDeadZone * 3) ==
                  Control::None,
          "no cross, no D-pad taps");
}

void TestHiddenControls() {
  auto restoration = SwitchConfig();
  restoration.HudLayout = TopScreenHudLayout::Restoration;
  const auto normalTargets =
      ResolveTopScreenHudTouchTargets(BuildHud(SwitchConfig(), 1.0F), kWidth,
                                      kHeight);
  const auto targets = ResolveTopScreenHudTouchTargets(
      BuildHud(restoration, 1.0F), kWidth, kHeight);
  Require(!targets.ButtonVisible[2] && !targets.ButtonVisible[3],
          "Restoration draws no Item I/II circles");
  const auto &itemI = normalTargets.Buttons[2];
  const auto hit =
      HitTestTopScreenHudTouch(targets, CenterX(itemI), CenterY(itemI));
  Require(hit != Control::ItemI && hit != Control::ItemII,
          "Restoration never presses ZR/ZL from a tap");

  const auto faded = ResolveTopScreenHudTouchTargets(
      BuildHud(SwitchConfig(), 0.25F), kWidth, kHeight);
  for (std::size_t index = 0; index < 4; ++index)
    Require(!faded.ButtonVisible[index], "a faded HUD takes no taps");
  Require(!faded.DpadVisible, "a faded D-pad takes no taps");

  const auto empty = ResolveTopScreenHudTouchTargets({}, kWidth, kHeight);
  Require(HitTestTopScreenHudTouch(empty, CenterX(itemI), CenterY(itemI)) ==
              Control::None,
          "no HUD, no controls");
}

TopScreenTouchContact At(std::uint32_t id, float windowX, float windowY,
                         bool end = false) {
  return {id, windowX / kWidth, windowY / kHeight, end};
}

void TestTracker() {
  const auto hud = BuildHud(SwitchConfig(), 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(hud, kWidth, kHeight);
  const TopScreenTouchPollContext gameplay{false, false, &targets, kWidth,
                                           kHeight};
  const auto &x = targets.Buttons[0];
  const auto &itemI = targets.Buttons[2];
  const float downX = targets.DpadCenterX;
  const float downY = targets.DpadCenterY + targets.DpadDeadZone * 3;

  // A tap that lands and lifts between two polls is still delivered, for
  // at least two consuming polls.
  TopScreenTouchTracker tracker;
  const std::array tap{At(0, CenterX(x), CenterY(x))};
  tracker.ObserveSample(tap);
  tracker.ObserveSample({});
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, false).HudButtons ==
              ButtonMask(Button::X),
          "a quick tap holds X");
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
              ButtonMask(Button::X),
          "a quick tap reaches the first refresh");
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
              ButtonMask(Button::X),
          "a quick tap lasts two refreshes");
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons == 0U,
          "a delivered tap is released");

  // Two fingers hit-test independently; each holds until it lifts.
  tracker.Reset();
  const std::array both{At(3, CenterX(itemI), CenterY(itemI)),
                        At(4, downX, downY)};
  tracker.ObserveSample(both);
  for (int poll = 0; poll < 4; ++poll)
    Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
                (ButtonMask(Button::Zr) | ButtonMask(Button::DpadDown)),
            "held fingers keep Item I and D-pad down");
  const std::array onlyDown{At(4, downX, downY)};
  tracker.ObserveSample(onlyDown);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
              ButtonMask(Button::DpadDown),
          "lifting Item I releases ZR only");
  const std::array endDown{At(4, downX, downY, true)};
  tracker.ObserveSample(endDown);
  tracker.ObserveSample(endDown);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons == 0U,
          "an ended contact is released and not reborn");

  // A finger keeps the owner it landed with.
  tracker.Reset();
  const std::array empty{At(5, 640, 360)};
  tracker.ObserveSample(empty);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons == 0U,
          "a touch on empty screen does nothing");
  const std::array slid{At(5, CenterX(x), CenterY(x))};
  tracker.ObserveSample(slid);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons == 0U,
          "sliding onto a control does not press it");
  tracker.ObserveSample({});
  (void)ResolveTopScreenTouchPoll(tracker, gameplay, true);

  // Pages keep the single pointer of before.
  const TopScreenTouchPollContext page{true, false, &targets, kWidth, kHeight};
  tracker.Reset();
  const std::array first{At(6, 640, 360)};
  tracker.ObserveSample(first);
  auto result = ResolveTopScreenTouchPoll(tracker, page, true);
  Require(result.PagePressed && Near(result.PageX, 0.5F) &&
              Near(result.PageY, 0.5F) && result.HudButtons == 0U,
          "the first finger is the page pointer");
  const std::array second{At(6, 640, 360), At(7, CenterX(x), CenterY(x))};
  tracker.ObserveSample(second);
  result = ResolveTopScreenTouchPoll(tracker, page, true);
  Require(result.PagePressed && Near(result.PageX, 0.5F) &&
              result.HudButtons == 0U,
          "a second finger does not move the pointer");
  // A quick page tap is delivered once, as the old SDL latch did.
  tracker.Reset();
  tracker.ObserveSample(first);
  tracker.ObserveSample({});
  Require(ResolveTopScreenTouchPoll(tracker, page, false).PagePressed,
          "a page tap waits for a refresh");
  Require(ResolveTopScreenTouchPoll(tracker, page, true).PagePressed,
          "a page tap reaches one refresh");
  Require(!ResolveTopScreenTouchPoll(tracker, page, true).PagePressed,
          "a page tap is delivered once");

  // A page-owned finger never becomes a HUD press when the page closes, and
  // a HUD press stops while a page is open.
  tracker.Reset();
  tracker.ObserveSample(slid);
  Require(ResolveTopScreenTouchPoll(tracker, page, true).PagePressed,
          "a finger landing on a page belongs to it");
  result = ResolveTopScreenTouchPoll(tracker, gameplay, true);
  Require(!result.PagePressed && result.HudButtons == 0U,
          "a page finger does nothing in gameplay");
  tracker.Reset();
  const std::array hold{At(8, CenterX(itemI), CenterY(itemI))};
  tracker.ObserveSample(hold);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
              ButtonMask(Button::Zr),
          "Item I held");
  result = ResolveTopScreenTouchPoll(tracker, page, true);
  Require(!result.PagePressed && result.HudButtons == 0U,
          "a HUD finger does nothing on a page");

  // Host UI owns every finger that lands while it is open.
  const TopScreenTouchPollContext hostUi{false, true, &targets, kWidth,
                                         kHeight};
  tracker.Reset();
  tracker.ObserveSample(slid);
  Require(ResolveTopScreenTouchPoll(tracker, hostUi, true).HudButtons == 0U,
          "host UI blocks HUD taps");
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons == 0U,
          "a finger that landed on host UI stays ignored");

  // A finger id reused by a new touch right after a tap is a new finger.
  tracker.Reset();
  tracker.ObserveSample(tap);
  tracker.ObserveSample({});
  const std::array again{At(0, CenterX(itemI), CenterY(itemI))};
  tracker.ObserveSample(again);
  Require(ResolveTopScreenTouchPoll(tracker, gameplay, true).HudButtons ==
              (ButtonMask(Button::X) | ButtonMask(Button::Zr)),
          "a reused finger id starts a new touch");
}

} // namespace

int main() {
  TestCanvasMapping();
  TestButtonsUnderTheirCircles();
  TestDpadIcons();
  TestHiddenControls();
  TestTracker();
  std::cout << "oot3d_top_screen_hud_touch_tests: ok\n";
  return 0;
}
