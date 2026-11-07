// Gameplay HUD taps (oot3d_top_screen_hud_touch.h): the controls are read
// from the primitives the real HUD emitters produce at the Switch settings.
#include "oot3d_top_screen_hud_touch.h"

#include "oot3d_native_a32_memory.h"
#include "oot3d_top_screen_dpad_presentation.h"
#include "oot3d_top_screen_mod_profile.h"
#include "three_ds_input.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
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

bool Same(const oot3d::ui::UiRect &a, const oot3d::ui::UiRect &b) {
  return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.width, b.width) &&
         Near(a.height, b.height);
}

TopScreenHudTouchTargets Targets(const TopScreenUiConfig &config, float alpha) {
  return ResolveTopScreenHudTouchTargets(BuildHud(config, alpha),
                                         config.HudLayout, kWidth, kHeight);
}

bool IsClusterCircle(const oot3d::ui::UiPrimitive &primitive) {
  return primitive.owner_address == 0x005C9940U && primitive.layer == 12U &&
         primitive.role == oot3d::ui::UiPrimitiveRole::TouchControl &&
         primitive.source_quad < 5U && primitive.visible &&
         primitive.color.alpha > 0.0F;
}

const oot3d::ui::UiPrimitive *FindCircle(
    const std::vector<oot3d::ui::UiPrimitive> &hud, std::uint32_t quad) {
  for (const auto &primitive : hud)
    if (IsClusterCircle(primitive) && primitive.source_quad == quad)
      return &primitive;
  return nullptr;
}

template <typename T> std::span<const std::uint8_t> Bytes(const T &values) {
  return {reinterpret_cast<const std::uint8_t *>(values.data()),
          values.size() * sizeof(values[0])};
}

// The native item renderer read by AppendTopScreenNativeItemIconCopies, with
// one 48-unit icon in each lower-screen item slot of payload table 0x005D33C8
// (sources 0-3: Item I, X, Y, Item II, at their slot corners) and source 4 in
// the D-pad lane, which the bridge leaves out (render_dpad_icons off).
// itemIAlpha is the native alpha of source 0 (0: the game draws no Item I).
std::vector<oot3d::ui::UiPrimitive>
NativeItemIcons(float alpha, float itemIAlpha = 1.0F,
                TopScreenNativeItemOpacity *opacity = nullptr,
                TopScreenHudLayout layout = TopScreenHudLayout::Normal,
                bool zrHeld = false) {
  NativeA32Memory memory;
  std::string error;
  Require(memory.MapRegion({"hud-item-icons", 0x00500000U, 0x00040000U, true,
                            false, {}},
                           &error),
          "could not map the item renderer fixture");
  constexpr std::uint32_t kPlayState = 0x00522000U;
  constexpr std::uint32_t kRenderer = 0x00523000U;
  constexpr std::uint32_t kPositions = 0x00524000U;
  constexpr std::uint32_t kUvs = 0x00525000U;
  constexpr std::uint32_t kColors = 0x00526000U;
  constexpr std::uint32_t kTranslations = 0x00527000U;
  Require(memory.Write32(0x0050AF34U + 0x14U, kPlayState) &&
              memory.Write32(kPlayState + 0x10CU, kRenderer) &&
              memory.Write32(kRenderer + 0x0CU, kPositions) &&
              memory.Write32(kRenderer + 0x14U, kUvs) &&
              memory.Write32(kRenderer + 0x18U, kColors) &&
              memory.Write32(kRenderer + 0x1CU, kTranslations),
          "could not seed the item renderer");
  constexpr std::array<float, 12> kQuad{0, 0, 0, 48, 0, 0, 0, 48, 0, 48, 48, 0};
  constexpr std::array<float, 8> kUv{0.0F, 0.8F, 0.1F, 0.8F,
                                     0.0F, 0.6F, 0.1F, 0.6F};
  std::array<float, 16> white{};
  white.fill(1.0F);
  auto itemI = white;
  for (std::size_t vertex = 0; vertex < 4; ++vertex)
    itemI[vertex * 4U + 3U] = itemIAlpha;
  constexpr std::array<std::array<float, 2>, 5> kSlots{
      {{266, 2}, {282, 65}, {264, 107}, {266, 186}, {2, 186}}};
  for (std::uint32_t source = 0; source < kSlots.size(); ++source)
    Require(memory.WriteBytes(kPositions + source * 0x30U, Bytes(kQuad)) &&
                memory.WriteBytes(kUvs + source * 0x20U, Bytes(kUv)) &&
                memory.WriteBytes(kColors + source * 0x40U,
                                  Bytes(source == 0 ? itemI : white)) &&
                memory.WriteBytes(kTranslations + source * 0x08U,
                                  Bytes(kSlots[source])),
            "could not seed an item icon");
  oot3d::ui::UiTextureIdentity items;
  items.semantic_name = "native-items";
  std::vector<oot3d::ui::UiPrimitive> icons;
  Require(AppendTopScreenNativeItemIconCopies(memory, {0, 0, 0, 0}, alpha,
                                              items, icons, &error, false,
                                              opacity, layout, zrHeld),
          "the item icons were not copied");
  return icons;
}

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
  const auto targets = ResolveTopScreenHudTouchTargets(
      hud, TopScreenHudLayout::Normal, kWidth, kHeight);
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
  const auto targets =
      ResolveTopScreenHudTouchTargets(hud, config.HudLayout, kWidth, kHeight);
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
  const auto noDpad = ResolveTopScreenHudTouchTargets(
      noCross, TopScreenHudLayout::Normal, kWidth, kHeight);
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
  const auto normalHud = BuildHud(SwitchConfig(), 1.0F);
  const auto restorationHud = BuildHud(restoration, 1.0F);
  const auto normalTargets = Targets(SwitchConfig(), 1.0F);
  const auto targets = Targets(restoration, 1.0F);

  // Item I/II have no lane in Restoration (they show in the X/Y lanes while
  // ZR is held): no circle in lanes 2/3; B's circle (quad 4) takes lane 3.
  Require(FindCircle(normalHud, 2U) != nullptr &&
              FindCircle(restorationHud, 2U) == nullptr,
          "Restoration draws no Item I circle");
  const auto *itemIICircle = FindCircle(normalHud, 3U);
  const auto *aCircle = FindCircle(restorationHud, 4U);
  Require(FindCircle(restorationHud, 3U) == nullptr &&
              itemIICircle != nullptr && aCircle != nullptr &&
              Same(aCircle->destination, itemIICircle->destination),
          "Restoration puts B's circle alone in the Item II lane");

  // The layout, not the drawing, decides: Item I/II are ZR+X/ZR+Y chords.
  Require(!targets.ButtonVisible[2] && !targets.ButtonVisible[3],
          "Restoration Item I/II lanes take no taps");
  const auto normalUnderRestoration = ResolveTopScreenHudTouchTargets(
      normalHud, TopScreenHudLayout::Restoration, kWidth, kHeight);
  Require(!normalUnderRestoration.ButtonVisible[2] &&
              !normalUnderRestoration.ButtonVisible[3] &&
              normalTargets.ButtonVisible[2],
          "the layout decides: a drawn Item I circle takes no tap under Restoration");
  for (const auto &primitive : restorationHud) {
    if (!IsClusterCircle(primitive))
      continue;
    const auto rect = MapTopScreenCanvasRectToWindow(primitive.destination,
                                                     kWidth, kHeight);
    const auto hit =
        HitTestTopScreenHudTouch(targets, CenterX(rect), CenterY(rect));
    Require(hit != Control::ItemI && hit != Control::ItemII,
            "Restoration never presses ZR/ZL from a tap");
  }
  const auto &itemI = normalTargets.Buttons[2];
  const auto &itemII = normalTargets.Buttons[3];
  for (const auto *rect : {&itemI, &itemII}) {
    const auto hit =
        HitTestTopScreenHudTouch(targets, CenterX(*rect), CenterY(*rect));
    Require(hit != Control::ItemI && hit != Control::ItemII,
            "Restoration never presses ZR/ZL from a tap");
  }

  const auto faded = Targets(SwitchConfig(), 0.25F);
  for (std::size_t index = 0; index < 4; ++index)
    Require(!faded.ButtonVisible[index], "a faded HUD takes no taps");
  Require(!faded.DpadVisible, "a faded D-pad takes no taps");

  const auto empty = ResolveTopScreenHudTouchTargets(
      {}, TopScreenHudLayout::Normal, kWidth, kHeight);
  Require(HitTestTopScreenHudTouch(empty, CenterX(itemI), CenterY(itemI)) ==
              Control::None,
          "no HUD, no controls");
}

// Every item icon drawn sits in exactly one circle, in both layouts and with
// ZR up or held: Restoration draws Item I/II only while ZR is held, in the X/Y
// circles, and the X/Y icons step aside then.
void TestEveryItemIconHasACircle() {
  struct Case {
    TopScreenHudLayout Layout;
    bool ZrHeld;
    std::array<bool, 4> Sources; // Item I, X, Y, Item II drawn
  };
  constexpr std::array<Case, 4> kCases{
      {{TopScreenHudLayout::Normal, false, {true, true, true, true}},
       {TopScreenHudLayout::Normal, true, {true, true, true, true}},
       {TopScreenHudLayout::Restoration, false, {false, true, true, false}},
       {TopScreenHudLayout::Restoration, true, {true, false, false, true}}}};
  for (const auto &test : kCases) {
    auto config = SwitchConfig();
    config.HudLayout = test.Layout;
    oot3d::ui::UiTextureIdentity page;
    page.semantic_name = "native-pause";
    std::vector<oot3d::ui::UiPrimitive> hud;
    const auto icons =
        NativeItemIcons(1.0F, 1.0F, nullptr, test.Layout, test.ZrHeld);
    AppendTopScreenTouchClusterPresentation(
        BuildTopScreenTouchClusterGeometry({0, 0, 0, 0}, 1.0F, test.Layout),
        page, hud);
    hud.insert(hud.end(), icons.begin(), icons.end());
    ApplyTopScreenHudScale(hud, config);
    ApplyTopScreenGameplayCanvas(hud);
    std::array<bool, 4> drawn{};
    for (const auto &icon : hud) {
      if (icon.role != oot3d::ui::UiPrimitiveRole::ActionButton ||
          icon.layer != 13U || !icon.visible)
        continue;
      Require(icon.source_quad < 4U, "only the four face-button items");
      drawn[icon.source_quad] = true;
      const float x = CenterX(icon.destination);
      const float y = CenterY(icon.destination);
      std::size_t circles = 0;
      for (const auto &circle : hud) {
        const auto &rect = circle.destination;
        if (!IsClusterCircle(circle) || x < rect.x || y < rect.y ||
            x >= rect.x + rect.width || y >= rect.y + rect.height)
          continue;
        ++circles;
        // The renderer stable-sorts by layer: circle first, icon over it.
        Require(circle.layer < icon.layer && &circle < &icon,
                "the item icon is drawn above its circle");
      }
      Require(circles == 1, "an item icon sits in exactly one circle");
    }
    Require(drawn == test.Sources, "the layout draws the expected items");
  }
}

// The Restoration slot-to-button mapping (SingleScreen 3.2.2): with ZR held
// Item I is drawn where X is and Item II where Y is, the buttons that chord
// them (ZR+X, ZR+Y), labelled I/II; without ZR they are not drawn at all.
void TestRestorationItemLanes() {
  const auto center = [](const std::vector<oot3d::ui::UiPrimitive> &icons,
                         std::uint32_t source) {
    for (const auto &icon : icons)
      if (icon.source_quad == source && icon.visible)
        return std::array<float, 2>{CenterX(icon.destination),
                                    CenterY(icon.destination)};
    return std::array<float, 2>{-1.0F, -1.0F};
  };
  const auto normal = NativeItemIcons(1.0F);
  const auto held = NativeItemIcons(1.0F, 1.0F, nullptr,
                                    TopScreenHudLayout::Restoration, true);
  const auto released = NativeItemIcons(1.0F, 1.0F, nullptr,
                                        TopScreenHudLayout::Restoration, false);
  const auto same = [](std::array<float, 2> a, std::array<float, 2> b) {
    return Near(a[0], b[0]) && Near(a[1], b[1]);
  };
  // The move is exactly one lane: right (Item I) to top (X), bottom (Item
  // II) to left (Y), as the circles are spaced.
  const auto lanes = BuildTopScreenTouchClusterGeometry({0, 0, 0, 0}, 1.0F);
  const auto moved = [](std::array<float, 2> point, TopScreenVec2 from,
                        TopScreenVec2 to) {
    return std::array<float, 2>{point[0] + to.X - from.X,
                                point[1] + to.Y - from.Y};
  };
  Require(same(center(held, 0U), moved(center(normal, 0U), lanes.Positions[2],
                                       lanes.Positions[0])),
          "ZR held: Item I is drawn in the X lane");
  Require(same(center(held, 3U), moved(center(normal, 3U), lanes.Positions[3],
                                       lanes.Positions[1])),
          "ZR held: Item II is drawn in the Y lane");
  Require(center(held, 1U)[0] < 0 && center(held, 2U)[0] < 0,
          "ZR held: the X/Y items step aside");
  Require(center(released, 0U)[0] < 0 && center(released, 3U)[0] < 0 &&
              same(center(released, 1U), center(normal, 1U)) &&
              same(center(released, 2U), center(normal, 2U)),
          "ZR up: X/Y in their lanes, no Item I/II");
  for (std::size_t region = 0; region < 6U; ++region) {
    const auto placement = ResolveTopScreenItemIconPlacement(
        TopScreenHudLayout::Normal, true, region);
    Require(placement.Visible && placement.Offset.X == 0.0F &&
                placement.Offset.Y == 0.0F,
            "Normal places every region natively");
  }
  Require(ResolveTopScreenItemIconPlacement(TopScreenHudLayout::Restoration,
                                            true, 4U)
                  .Visible &&
              ResolveTopScreenItemIconPlacement(
                  TopScreenHudLayout::Restoration, false, 5U)
                  .Visible,
          "Restoration leaves the D-pad and action regions alone");

  // The chord uses Item I/II and X/Y give up their own items meanwhile.
  TopScreenItemQueryState state;
  state.Input.RestorationLayout = true;
  state.Input.ZrHeld = true;
  state.Input.XPressed = true;
  state.Input.XHeld = true;
  Require(ResolveTopScreenItemQuery(TopScreenItemQuery::ItemIPressed, state) &&
              !ResolveTopScreenItemQuery(TopScreenItemQuery::ItemIIPressed,
                                         state),
          "ZR+X uses Item I");
  Require(HasTopScreenSlotItemOverrideInput(state.Input),
          "ZR held routes the X/Y slot queries");
  for (const std::uint8_t slot : {1, 2})
    Require(ResolveTopScreenSlotItemId({slot, 0x0AU, false, false, false,
                                        true}) == 0xFFU,
            "ZR held: X/Y use no item of their own");
  Require(ResolveTopScreenSlotItemId({1U, 0x0AU, false, false, true, true}) ==
              0x0AU,
          "the game's special states keep the native item");
  Require(ResolveTopScreenSlotItemId({0U, 0x3BU, false, false, false, true}) ==
              0x3BU,
          "B keeps its item");
  state.Input.ZrHeld = false;
  Require(!HasTopScreenSlotItemOverrideInput(state.Input) &&
              !ResolveTopScreenItemQuery(TopScreenItemQuery::ItemIPressed,
                                         state),
          "X alone is X");
}

TopScreenTouchContact At(std::uint32_t id, float windowX, float windowY,
                         bool end = false) {
  return {id, windowX / kWidth, windowY / kHeight, end};
}

void TestTracker() {
  const auto hud = BuildHud(SwitchConfig(), 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(
      hud, TopScreenHudLayout::Normal, kWidth, kHeight);
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

void TestAimOwnership() {
  const auto hud = BuildHud(SwitchConfig(), 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(
      hud, TopScreenHudLayout::Normal, kWidth, kHeight);
  const TopScreenTouchPollContext aim{false, false, &targets, kWidth, kHeight,
                                      true};
  const auto &x = targets.Buttons[0];
  const auto &itemI = targets.Buttons[2];
  TopScreenTouchTracker tracker;

  // A finger landing on empty screen aims; its motion is reported in panel
  // pixels on consuming polls only, accumulated across the others.
  const std::array land{At(1, 640, 360)};
  tracker.ObserveSample(land);
  auto result = ResolveTopScreenTouchPoll(tracker, aim, true);
  Require(result.AimDeltaX == 0 && result.AimDeltaY == 0 &&
              result.HudButtons == 0U,
          "landing does not move the aim");
  const std::array moved{At(1, 700, 340)};
  tracker.ObserveSample(moved);
  result = ResolveTopScreenTouchPoll(tracker, aim, false);
  Require(result.AimDeltaX == 0 && result.AimDeltaY == 0,
          "motion waits for a consuming poll");
  const std::array further{At(1, 740, 350)};
  tracker.ObserveSample(further);
  result = ResolveTopScreenTouchPoll(tracker, aim, true);
  Require(Near(result.AimDeltaX, 100) && Near(result.AimDeltaY, -10),
          "the drag since the last consuming poll is delivered once");
  Require(Near(ResolveTopScreenTouchPoll(tracker, aim, true).AimDeltaX, 0),
          "a still finger does not aim");

  // The aim finger never presses a control it slides over; a second finger
  // on a control still taps, one on empty screen does not aim.
  const std::array overX{At(1, CenterX(x), CenterY(x)),
                         At(2, CenterX(itemI), CenterY(itemI)),
                         At(3, 640, 600)};
  tracker.ObserveSample(overX);
  result = ResolveTopScreenTouchPoll(tracker, aim, true);
  Require(result.HudButtons == ButtonMask(Button::Zr),
          "a sliding aim finger presses nothing; a second finger taps");
  Require(Near(result.AimDeltaX, CenterX(x) - 740) &&
              Near(result.AimDeltaY, CenterY(x) - 350),
          "only the first aim finger moves the aim");

  // A lifted aim finger delivers its last motion, then a new finger aims.
  const std::array lastMove{At(1, CenterX(x) + 20, CenterY(x))};
  tracker.ObserveSample(lastMove);
  tracker.ObserveSample({});
  result = ResolveTopScreenTouchPoll(tracker, aim, false);
  Require(result.AimDeltaX == 0, "the last motion waits for a refresh");
  result = ResolveTopScreenTouchPoll(tracker, aim, true);
  // (Item I, lifted in the same sample, still owes its second refresh.)
  Require(Near(result.AimDeltaX, 20) &&
              result.HudButtons == ButtonMask(Button::Zr),
          "a lifted aim finger delivers its last motion");
  const std::array next{At(4, 300, 300)};
  tracker.ObserveSample(next);
  (void)ResolveTopScreenTouchPoll(tracker, aim, true);
  const std::array nextMoved{At(4, 290, 330)};
  tracker.ObserveSample(nextMoved);
  result = ResolveTopScreenTouchPoll(tracker, aim, true);
  Require(Near(result.AimDeltaX, -10) && Near(result.AimDeltaY, 30),
          "the next finger on empty screen aims");

  // A HUD finger never aims.
  tracker.Reset();
  const std::array tapX{At(5, CenterX(x), CenterY(x))};
  tracker.ObserveSample(tapX);
  (void)ResolveTopScreenTouchPoll(tracker, aim, true);
  const std::array dragX{At(5, CenterX(x) - 200, CenterY(x) + 100)};
  tracker.ObserveSample(dragX);
  result = ResolveTopScreenTouchPoll(tracker, aim, true);
  Require(result.AimDeltaX == 0 && result.AimDeltaY == 0 &&
              result.HudButtons == ButtonMask(Button::X),
          "a finger that landed on X keeps pressing X and never aims");

  // Drag aim off, pages and host UI: no aim finger.
  for (const auto &context :
       {TopScreenTouchPollContext{false, false, &targets, kWidth, kHeight,
                                  false},
        TopScreenTouchPollContext{true, false, &targets, kWidth, kHeight,
                                  true},
        TopScreenTouchPollContext{false, true, &targets, kWidth, kHeight,
                                  true}}) {
    tracker.Reset();
    tracker.ObserveSample(land);
    (void)ResolveTopScreenTouchPoll(tracker, context, true);
    tracker.ObserveSample(further);
    result = ResolveTopScreenTouchPoll(tracker, context, true);
    Require(result.AimDeltaX == 0 && result.AimDeltaY == 0,
            "no aim without drag aim, or under a page or host UI");
    // The finger keeps its owner after the page or host UI closes.
    tracker.ObserveSample(land);
    result = ResolveTopScreenTouchPoll(tracker, aim, true);
    Require(result.AimDeltaX == 0 && result.AimDeltaY == 0,
            "a finger that landed elsewhere never starts aiming");
  }

  // An aim finger under a page that opens mid-drag: the motion made under
  // the page is dropped, not replayed when it closes.
  const TopScreenTouchPollContext page{true, false, &targets, kWidth, kHeight,
                                       true};
  tracker.Reset();
  tracker.ObserveSample(land);
  (void)ResolveTopScreenTouchPoll(tracker, aim, true);
  tracker.ObserveSample(further);
  Require(ResolveTopScreenTouchPoll(tracker, page, true).AimDeltaX == 0,
          "a page owns the screen");
  Require(ResolveTopScreenTouchPoll(tracker, aim, true).AimDeltaX == 0,
          "motion under a page is not replayed");
}

void TestAimRates() {
  using namespace ThreeDsRecomp::Input;
  MappingConfig config;
  Require(config.TouchMotionDegreesPerPixel == 0.14F,
          "default touch sensitivity");
  // Integrated over the sample period, half the 1280-pixel screen turns
  // about 90 degrees.
  constexpr double kSeconds = 1.0 / 30.0;
  auto rates = ResolveTouchAimRates(config, 640.0F, 0.0F, kSeconds);
  Require(std::abs(rates.YawDegreesPerSecond * kSeconds + 89.6) < 0.01 &&
              rates.PitchDegreesPerSecond == 0.0F,
          "a half-screen drag right turns 89.6 degrees right");
  rates = ResolveTouchAimRates(config, 0.0F, 100.0F, kSeconds);
  Require(std::abs(rates.PitchDegreesPerSecond * kSeconds - 14.0) < 0.01,
          "a drag down looks down like the mouse");
  config.TouchMotionDegreesPerPixel = 0.28F;
  rates = ResolveTouchAimRates(config, 640.0F, 100.0F, kSeconds);
  Require(std::abs(rates.YawDegreesPerSecond * kSeconds + 179.2) < 0.02 &&
              std::abs(rates.PitchDegreesPerSecond * kSeconds - 28.0) < 0.01,
          "sensitivity scales the turn");
  config.NativeMotionInvertX = true;
  config.NativeMotionInvertY = true;
  rates = ResolveTouchAimRates(config, 640.0F, 100.0F, kSeconds);
  Require(rates.YawDegreesPerSecond > 0.0F &&
              rates.PitchDegreesPerSecond < 0.0F,
          "aim inversion applies to touch");
  rates = ResolveTouchAimRates(config, std::nanf(""), 1.0F, kSeconds);
  Require(rates.YawDegreesPerSecond == 0.0F &&
              rates.PitchDegreesPerSecond == 0.0F,
          "a broken sample does not aim");
}

void TestAimThroughMotionSources() {
  using namespace ThreeDsRecomp::Input;
  const DigitalState digital;
  MappingConfig config;
  config.NativeMotionSource = MotionSource::Automatic;
  config.MouseMotionDegreesPerPixel = config.TouchMotionDegreesPerPixel;
  PhysicalInputState physical;
  physical.SamplePeriodSeconds = 1.0 / 30.0;

  // Without a sensor, a touch drag aims exactly like the same mouse motion.
  auto touch = physical;
  touch.TouchAimDeltaX = 50.0F;
  touch.TouchAimDeltaY = -20.0F;
  auto mouse = physical;
  mouse.MouseDeltaX = 50;
  mouse.MouseDeltaY = -20;
  VirtualMotionState touchState;
  VirtualMotionState mouseState;
  const auto byTouch =
      ResolveInput(config, touch, digital, {}, nullptr, true, &touchState);
  const auto byMouse =
      ResolveInput(config, mouse, digital, {}, nullptr, true, &mouseState);
  for (std::size_t axis = 0; axis < 3; ++axis)
    Require(std::abs(byTouch.Hid.GyroscopeDegreesPerSecond[axis] -
                     byMouse.Hid.GyroscopeDegreesPerSecond[axis]) < 0.01F,
            "touch aims like the mouse at equal sensitivity");
  Require(std::abs(byTouch.Hid.GyroscopeDegreesPerSecond[1] + 210.0F) < 0.1F,
          "50 px right at 0.14 deg/px over 1/30 s is -210 deg/s of yaw");
  Require(byTouch.CStick.X == 0 && byTouch.CStick.Y == 0,
          "touch aim never moves the camera stick");

  // With the gyro owning the aim, touch adds to it and does not take over.
  auto gyro = physical;
  gyro.ControllerMotion.GyroscopeValid = true;
  gyro.ControllerMotion.AccelerometerValid = true;
  gyro.ControllerMotion.GyroscopeDegreesPerSecond = {0.0F, 30.0F, 0.0F};
  gyro.ControllerPressed = 1U;
  VirtualMotionState gyroState;
  const auto gyroOnly =
      ResolveInput(config, gyro, digital, {}, nullptr, false, &gyroState);
  auto gyroTouch = gyro;
  gyroTouch.TouchAimDeltaX = 50.0F;
  const auto combined =
      ResolveInput(config, gyroTouch, digital, {}, nullptr, true, &gyroState);
  Require(std::abs(combined.Hid.GyroscopeDegreesPerSecond[1] -
                   (gyroOnly.Hid.GyroscopeDegreesPerSecond[1] - 210.0F)) < 0.1F,
          "touch adds its yaw to the gyro");
  Require(gyroState.AutomaticOwner == VirtualMotionState::Owner::Controller,
          "a touch drag leaves the controller (gyro) as owner");

  // A physical-sensor-only source ignores touch.
  config.NativeMotionSource = MotionSource::ControllerGyroscope;
  const auto sensor = ResolveInput(config, gyro, digital);
  const auto sensorTouch = ResolveInput(config, gyroTouch, digital);
  Require(sensor.Hid.GyroscopeDegreesPerSecond ==
              sensorTouch.Hid.GyroscopeDegreesPerSecond,
          "controller_gyroscope ignores touch");
  // A virtual source adds it.
  config.NativeMotionSource = MotionSource::RightStick;
  const auto stick = ResolveInput(config, gyroTouch, digital);
  Require(std::abs(stick.Hid.GyroscopeDegreesPerSecond[1] + 210.0F) < 0.1F,
          "right_stick adds touch");
}

void TestMenuOwnership() {
  const auto hud = BuildHud(SwitchConfig(), 1.0F);
  const auto targets = ResolveTopScreenHudTouchTargets(
      hud, TopScreenHudLayout::Normal, kWidth, kHeight);
  const TopScreenTouchPollContext gameplay{false, false, &targets, kWidth,
                                           kHeight, true};
  const TopScreenTouchPollContext menu{false, true, &targets, kWidth, kHeight,
                                       true};
  const auto &x = targets.Buttons[0];
  TopScreenTouchTracker tracker;

  // A finger that lands while the menu is open is the menu's: it is the
  // pointer, even over a HUD control, and never presses or aims.
  const std::array onX{At(1, CenterX(x), CenterY(x))};
  tracker.ObserveSample(onX);
  auto result = ResolveTopScreenTouchPoll(tracker, menu, false);
  Require(result.MenuPressed && Near(result.MenuX, CenterX(x) / kWidth) &&
              result.HudButtons == 0U && result.AimDeltaX == 0,
          "a finger landing on the open menu is its pointer");
  // Still the menu's after it closes: no HUD press, no aim.
  const std::array dragged{At(1, CenterX(x) - 300, CenterY(x) + 100)};
  tracker.ObserveSample(dragged);
  result = ResolveTopScreenTouchPoll(tracker, gameplay, true);
  Require(!result.MenuPressed && result.HudButtons == 0U &&
              result.AimDeltaX == 0 && result.AimDeltaY == 0,
          "a menu finger never reaches the game");

  // One menu pointer; a quick tap reaches the menu once even when no guest
  // refresh consumes input (the game may be stopped behind the menu).
  tracker.Reset();
  const std::array two{At(2, 100, 100), At(3, 900, 500)};
  tracker.ObserveSample(two);
  result = ResolveTopScreenTouchPoll(tracker, menu, false);
  Require(result.MenuPressed && Near(result.MenuX, 100.0F / kWidth),
          "the first finger is the menu pointer");
  tracker.Reset();
  const std::array tap{At(4, 640, 360)};
  tracker.ObserveSample(tap);
  tracker.ObserveSample({});
  Require(ResolveTopScreenTouchPoll(tracker, menu, false).MenuPressed,
          "a quick tap reaches the menu");
  Require(!ResolveTopScreenTouchPoll(tracker, menu, false).MenuPressed,
          "a quick tap is released without a guest refresh");

  // A finger that landed before the menu opened stays out of it.
  tracker.Reset();
  tracker.ObserveSample(tap);
  (void)ResolveTopScreenTouchPoll(tracker, gameplay, true);
  Require(!ResolveTopScreenTouchPoll(tracker, menu, true).MenuPressed,
          "a gameplay finger is not the menu pointer");
}

} // namespace

int main() {
  TestCanvasMapping();
  TestButtonsUnderTheirCircles();
  TestDpadIcons();
  TestHiddenControls();
  TestEveryItemIconHasACircle();
  TestRestorationItemLanes();
  TestTracker();
  TestAimOwnership();
  TestAimRates();
  TestAimThroughMotionSources();
  TestMenuOwnership();
  std::cout << "oot3d_top_screen_hud_touch_tests: ok\n";
  return 0;
}
