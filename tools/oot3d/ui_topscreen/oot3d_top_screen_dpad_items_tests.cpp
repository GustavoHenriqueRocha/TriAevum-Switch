#include "oot3d_top_screen_config.h"
#include "oot3d_top_screen_controls.h"
#include "oot3d_top_screen_dpad_items.h"
#include "oot3d_top_screen_dpad_presentation.h"
#include "oot3d_top_screen_item_dispatch.h"

#include <algorithm>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using namespace Oot3dNativeGame;

void Require(bool value, const char *message) {
  if (!value) {
    std::cerr << "dpad items: " << message << '\n';
    std::exit(1);
  }
}

constexpr std::uint8_t kNut = 0x01U;
constexpr std::uint8_t kBow = 0x03U;
constexpr std::uint8_t kHookshot = 0x0AU;
constexpr std::uint8_t kLens = 0x0FU;
constexpr std::uint8_t kHammer = 0x11U;
constexpr std::uint8_t kEmptyBottle = 0x14U;
constexpr std::uint8_t kRedPotion = 0x15U;
constexpr std::uint8_t kFairy = 0x18U;
constexpr std::uint8_t kMilk = 0x1AU;
constexpr std::uint8_t kHalfMilk = 0x1FU;
constexpr std::uint8_t kKeatonMask = 0x23U;
constexpr std::uint8_t kSoldOut = 0x2CU;

// A child in a field: nothing restricted, buttons enabled, Link standing.
TopScreenDpadItemGuestState Field() {
  TopScreenDpadItemGuestState state;
  state.Age = 1U;
  state.ItemAge.fill(9U);
  state.ItemAge[kHammer] = 0U;
  state.ItemAge[kHookshot] = 0U;
  state.HasPlay = true;
  state.PlayStateType = 3U;
  state.PlayStateSubtype = 2U;
  state.PauseState = 2U;
  state.HasPlayer = true;
  state.ButtonItems = {kBow, kNut, 0xFFU, 0xFFU};
  return state;
}

void TestConfig() {
  TopScreenUiConfig config;
  Require(config.ChildDpadItemSlots[0] == kTopScreenDpadItemSlotNone &&
              config.AdultDpadItemSlots[3] == kTopScreenDpadItemSlotNone,
          "no item is assigned by default");
  config.ChildDpad[2] = TopScreenDpadAction::Item;
  config.ChildDpadItemSlots[2] = 18U;
  config.AdultDpad[0] = TopScreenDpadAction::Item;
  config.AdultDpadItemSlots[0] = 15U;
  std::string text, itemsText, error;
  Require(SerializeTopScreenUiConfigText(config, &text, &error) &&
              SerializeTopScreenDpadItemsText(config, &itemsText, &error),
          "serializes");
  // Builds without D-pad items reject unknown fields and actions at launch.
  Require(text.find("\"item\"") == std::string::npos &&
              text.find("dpad_items") == std::string::npos &&
              text.find("bottle_1") == std::string::npos,
          "the UI config holds nothing an earlier build rejects");
  Require(itemsText.find("\"bottle_1\"") != std::string::npos &&
              itemsText.find("\"megaton_hammer\"") != std::string::npos,
          "the items file names the slots");
  TopScreenUiConfig parsed;
  Require(ParseTopScreenUiConfigText(text, &parsed, &error) &&
              parsed.ChildDpad[2] == TopScreenDpadAction::None &&
              ParseTopScreenDpadItemsText(itemsText, &parsed, &error) && parsed == config,
          "both texts together keep actions and slots");

  const std::string legacy = R"({"schema":"oot3d_topscreen_ui_v2",)"
                             R"("dpad_child":["view","ocarina","item_zr","item_zl"]})";
  Require(ParseTopScreenUiConfigText(legacy, &parsed, &error) &&
              parsed.ChildDpadItemSlots[0] == kTopScreenDpadItemSlotNone,
          "configs without D-pad items still load");
  for (const char *bad : {R"(["fire_arrow","none","none","none"])",
                          R"(["bomb","none","none"])", R"(["bomb",3,"none","none"])"}) {
    const std::string document = std::string(R"({"schema":"oot3d_topscreen_dpad_items_v1",)") +
                                 R"("adult":)" + bad + "}";
    Require(!ParseTopScreenDpadItemsText(document, &parsed, &error),
            "magic arrows, short arrays and numbers are refused");
  }
  Require(!ParseTopScreenDpadItemsText(R"({"schema":"x","child":[]})", &parsed, &error) &&
              !ParseTopScreenDpadItemsText("{", &parsed, &error),
          "another schema or broken JSON is refused");

  const auto directory =
      std::filesystem::temp_directory_path() / "oot3d_dpad_items_tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "topscreen_ui.json";
  Require(TopScreenDpadItemsPath(path) == directory / "topscreen_ui_dpad_items.json",
          "the items file sits next to the UI config");
  Require(SaveTopScreenUiConfig(path, config, &error), "saves both files");
  TopScreenUiConfig loaded;
  Require(LoadTopScreenUiConfig(path, &loaded, &error) && loaded == config,
          "loads both files");
  {
    std::ofstream broken(TopScreenDpadItemsPath(path), std::ios::trunc);
    broken << "{";
  }
  Require(LoadTopScreenUiConfig(path, &loaded, &error) &&
              loaded.ChildDpad[2] == TopScreenDpadAction::None &&
              loaded.ChildDpadItemSlots[2] == kTopScreenDpadItemSlotNone,
          "a broken items file never stops a launch");
  std::filesystem::remove_all(directory);

  const auto slots = TopScreenDpadItemSlots(config, false);
  Require(slots[0] == 15U && slots[1] == kTopScreenDpadItemSlotNone,
          "only Item directions publish their slot");
  TopScreenDpadPhysicalState physical;
  physical.Held = {true, true, false, false};
  const auto lanes = ResolveTopScreenDpadItemLanes(config, physical);
  Require(lanes.Held[0] && !lanes.Held[1], "a lane follows an Item direction only");
}

void TestSlotItems() {
  auto state = Field();
  state.Inventory.fill(0xFFU);
  state.Inventory[1] = kNut;
  state.Inventory[15] = kHammer;
  state.Inventory[18] = kFairy;
  state.Inventory[19] = kNut; // corrupt bottle slot
  state.Inventory[23] = kSoldOut;
  state.Inventory[3] = 0x04U; // fire arrow where the bow belongs
  state.Slots = {1U, 15U, 18U, 19U};
  Require(ResolveTopScreenDpadItem(state, 0) == kNut, "Deku Nuts for a child");
  Require(ResolveTopScreenDpadItem(state, 1) == kTopScreenNoItem,
          "the hammer is refused to a child (0x00506C58)");
  state.Age = 0U;
  Require(ResolveTopScreenDpadItem(state, 1) == kHammer, "the hammer for an adult");
  Require(ResolveTopScreenDpadItem(state, 2) == kFairy, "a bottle holds its content");
  Require(ResolveTopScreenDpadItem(state, 3) == kTopScreenNoItem,
          "a bottle slot needs a bottle item");
  state.Slots = {23U, 3U, 2U, kTopScreenDpadItemSlotNone};
  Require(ResolveTopScreenDpadItem(state, 0) == kTopScreenNoItem, "SOLD OUT is no item");
  state.Inventory[23] = kKeatonMask;
  Require(ResolveTopScreenDpadItem(state, 0) == kKeatonMask, "a trade item or mask");
  Require(ResolveTopScreenDpadItem(state, 1) == kTopScreenNoItem, "never a magic arrow");
  Require(ResolveTopScreenDpadItem(state, 2) == kTopScreenNoItem, "an empty slot");
  Require(ResolveTopScreenDpadItem(state, 3) == kTopScreenNoItem, "no slot");
}

void TestRules() {
  auto state = Field();
  state.Inventory[1] = kNut;
  state.Inventory[9] = kHookshot;
  state.Inventory[13] = kLens;
  state.Inventory[18] = kRedPotion;
  state.Age = 0U;
  state.Slots = {1U, 9U, 13U, 18U};
  const auto item = [&](std::size_t direction) {
    return ResolveTopScreenDpadItem(state, direction);
  };
  Require(item(0) == kNut && item(1) == kHookshot && item(3) == kRedPotion, "all usable");

  auto epona = state;
  epona.StateFlags1 = 0x00800000U;
  Require(ResolveTopScreenDpadItem(epona, 0) == kTopScreenNoItem, "nothing on Epona");
  auto climbing = state;
  climbing.StateFlags1 = 0x00200000U;
  Require(ResolveTopScreenDpadItem(climbing, 0) == kTopScreenNoItem, "nothing while climbing");
  auto crawling = state;
  crawling.StateFlags2 = 0x00040000U;
  Require(ResolveTopScreenDpadItem(crawling, 0) == kTopScreenNoItem, "nothing while crawling");

  state.Restrictions[0] = 1U; // bottles
  Require(item(3) == kTopScreenNoItem && item(0) == kNut, "bottles restricted alone");
  state.Restrictions[0] = 0U;
  state.Restrictions[2] = 1U; // hookshot
  Require(item(1) == kTopScreenNoItem && item(0) == kNut, "hookshot restricted alone");
  state.Restrictions[2] = 0U;
  state.Restrictions[8] = 1U; // all
  Require(item(0) == kTopScreenNoItem && item(2) == kTopScreenNoItem, "all restricted");
  state.SceneNum = 16;
  Require(item(2) == kLens && item(0) == kTopScreenNoItem,
          "the Treasure Chest Shop spares the Lens of Truth");
  state.SceneNum = 0;
  state.Restrictions[8] = 0U;

  auto swimming = state;
  swimming.StateFlags1 = 0x08000000U;
  Require(ResolveTopScreenDpadItem(swimming, 0) == kTopScreenNoItem, "nothing while swimming");
  auto underwater = state;
  underwater.WaterDepth = 100U;
  underwater.UnderwaterBoots = 1U;
  Require(ResolveTopScreenDpadItem(underwater, 1) == kTopScreenNoItem,
          "no hookshot off the floor under water");
  underwater.BgCheckFlags = 1U;
  Require(ResolveTopScreenDpadItem(underwater, 1) == kHookshot &&
              ResolveTopScreenDpadItem(underwater, 0) == kTopScreenNoItem,
          "only the hookshot on the floor in Iron Boots");

  auto bowling = state;
  bowling.SpecialState = 1;
  Require(ResolveTopScreenDpadItem(bowling, 0) == 9U, "bombchus in Bombchu Bowling");
  bowling.SpecialState = -1;
  Require(ResolveTopScreenDpadItem(bowling, 0) == kTopScreenNoItem, "negative special state");

  auto ocarinaOnly = state;
  ocarinaOnly.InterfaceMode = 0x0011U;
  Require(ResolveTopScreenDpadItem(ocarinaOnly, 0) == kTopScreenNoItem, "ocarina-only mode");

  auto disabled = state;
  disabled.ButtonStatus.fill(0xFFU);
  Require(ResolveTopScreenDpadItem(disabled, 0) == kTopScreenNoItem,
          "Y/X/I/II disabled by Link's state disables the D-pad");
  disabled.ButtonItems = {kHookshot, kHookshot, kHookshot, kHookshot};
  disabled.Restrictions[2] = 1U;
  Require(ResolveTopScreenDpadItem(disabled, 0) == kNut,
          "buttons disabled only by the scene leave other D-pad items");

  auto aiming = state;
  aiming.Inventory[3] = kBow;
  aiming.Slots[2] = 3U;
  aiming.HeldItemButton = 8U;
  aiming.HeldItemAction = 10U; // ice arrow chosen
  Require(ResolveTopScreenDpadItem(aiming, 2) == 57U,
          "aiming from the D-pad keeps the chosen arrow");
  aiming.HeldItemButton = 2U;
  Require(ResolveTopScreenDpadItem(aiming, 2) == kBow, "another button's bow is the bow");
}

void TestButtons() {
  auto state = Field();
  state.Inventory[1] = kNut;
  state.Inventory[3] = kBow;
  state.Slots = {kTopScreenDpadItemSlotNone, 1U, 3U, kTopScreenDpadItemSlotNone};
  Require(ResolveTopScreenDpadItemButton(state, {false, true, false, false}, false) == 7U,
          "Down press is button 7");
  Require(ResolveTopScreenDpadItemButton(state, {true, false, false, true}, false) == 0U,
          "a lane without an item is no button");
  auto nocked = state;
  nocked.HeldItemAction = 9U;
  nocked.HeldActorId = 22;
  Require(ResolveTopScreenDpadItemButton(nocked, {false, true, false, false}, false) == 0U,
          "the press scan leaves the D-pad to the arrow switch");
  auto paused = state;
  paused.PauseState = 8U;
  Require(ResolveTopScreenDpadItemButton(paused, {false, true, false, false}, false) == 0U,
          "pause controller states 7..9");
  auto holding = state;
  holding.HeldItemButton = 8U;
  Require(ResolveTopScreenDpadItemButton(holding, {false, true, true, false}, true) == 8U,
          "the hold scan keeps the button the held item came from");
  Require(ResolveTopScreenDpadItemButton(holding, {false, true, false, false}, true) == 7U,
          "then any held item lane");
}

void TestBottles() {
  auto state = Field();
  state.Inventory[18] = kRedPotion;
  state.Inventory[19] = kMilk;
  state.Slots = {18U, 19U, 1U, kTopScreenDpadItemSlotNone};
  state.ItemAction = 37U; // drinking a red potion
  auto update = ResolveTopScreenDpadBottleUpdate(state, 6U, kEmptyBottle);
  Require(update && update->Slot == 18U && update->Item == kEmptyBottle,
          "the D-pad bottle empties");
  Require(!ResolveTopScreenDpadBottleUpdate(state, 7U, kEmptyBottle),
          "a bottle the Player is not using stays");
  state.ItemAction = 40U; // full milk
  update = ResolveTopScreenDpadBottleUpdate(state, 7U, kEmptyBottle);
  Require(update && update->Item == kHalfMilk, "Lon Lon Milk leaves half");
  Require(!ResolveTopScreenDpadBottleUpdate(state, 8U, kEmptyBottle),
          "a direction without a bottle changes nothing");
  Require(!ResolveTopScreenDpadBottleUpdate(state, 4U, kEmptyBottle), "native buttons");
}

void TestBoomerangSlingshot() {
  TopScreenUiConfig config;
  config.ChildDpad = {TopScreenDpadAction::Boomerang, TopScreenDpadAction::Slingshot,
                      TopScreenDpadAction::View, TopScreenDpadAction::Item};
  config.AdultDpad = config.ChildDpad;
  const auto child = TopScreenDpadItemSlots(config, true);
  const auto adult = TopScreenDpadItemSlots(config, false);
  Require(child[0] == 12U && child[1] == 6U && child[2] == kTopScreenDpadItemSlotNone &&
              child[3] == kTopScreenDpadItemSlotNone,
          "the child's Boomerang and Slingshot are D-pad item buttons (3.2.2 0x005D16B0)");
  Require(adult[0] == kTopScreenDpadItemSlotNone && adult[1] == kTopScreenDpadItemSlotNone,
          "the adult's are nothing");
}

void TestPresentation() {
  TopScreenUiConfig config;
  config.ChildDpad = {TopScreenDpadAction::Item, TopScreenDpadAction::Item,
                      TopScreenDpadAction::Item, TopScreenDpadAction::None};
  config.ChildDpadItemSlots = {2U, 15U, 1U, kTopScreenDpadItemSlotNone};
  TopScreenDpadPresentationState state;
  state.Child = true;
  state.Inventory.fill(0xFFU);
  state.Inventory[2] = 0x02U; // bombs
  state.Inventory[15] = kHammer;
  state.Inventory[1] = kNut;
  state.Ammo[2] = 25U;
  state.Ammo[1] = 0U;
  state.ItemUsable = {true, false, true, true};
  oot3d::ui::UiTextureIdentity items, atlas, glyphs;
  items.semantic_name = "native-items";
  atlas.semantic_name = "custom_ui01";
  glyphs.semantic_name = "number-glyphs";
  std::vector<oot3d::ui::UiPrimitive> icons;
  AppendTopScreenDpadPresentation(config, state, {}, 1.0F, items, atlas, icons);
  const auto icon = [&](std::uint32_t direction) {
    return std::find_if(icons.begin(), icons.end(), [&](const auto &primitive) {
      return primitive.layer == 13U && primitive.source_quad == direction;
    });
  };
  Require(icon(0) != icons.end() && icon(1) != icons.end() && icon(2) != icons.end(),
          "each Item direction draws its slot's item");
  Require(icon(0)->color.alpha == 1.0F && icon(1)->color.alpha < 0.31F,
          "a refused D-pad item is greyed");

  std::vector<oot3d::ui::UiPrimitive> ammo;
  AppendTopScreenDpadItemAmmo(config, state, {}, 1.0F, glyphs, ammo);
  Require(ammo.size() == 3U, "two digits for 25 bombs, one for no nuts, none for the hammer");
  Require(ammo[0].destination.x > ammo[1].destination.x,
          "the ones digit is drawn first, at the right");
  Require(ammo[2].color.red < 0.5F && ammo[0].color.red == 1.0F, "an empty count is grey");
  config.RenderDpadIcons = false;
  ammo.clear();
  Require(AppendTopScreenDpadItemAmmo(config, state, {}, 1.0F, glyphs, ammo) == 0U,
          "no ammo without the D-pad icons");
}

void TestPicker() {
  TopScreenDpadItemPickerState picker;
  picker.Browsing = true;
  picker.Age = 1U;
  picker.ItemAge.fill(9U);
  picker.ItemAge[kHammer] = 0U;
  picker.Inventory.fill(0xFFU);
  picker.Inventory[1] = kNut;
  picker.Inventory[4] = 0x04U; // fire arrow
  picker.Inventory[15] = kHammer;
  for (std::size_t position = 0; position < picker.Grid.size(); ++position)
    picker.Grid[position] = static_cast<std::uint8_t>(position);
  picker.Grid[2] = 15U; // the 3DS grid can be rearranged
  const auto slot = [&](std::uint32_t position) {
    picker.Position = position;
    return ResolveTopScreenDpadItemPickerSlot(picker);
  };
  Require(slot(1) == 1U, "the hovered Deku Nuts");
  Require(slot(2) == kTopScreenDpadItemSlotNone, "the hammer is refused to a child");
  picker.Age = 0U;
  Require(slot(2) == 15U, "the hovered position goes through the grid");
  Require(slot(4) == kTopScreenDpadItemSlotNone && slot(3) == kTopScreenDpadItemSlotNone &&
              slot(24) == kTopScreenDpadItemSlotNone,
          "no magic arrow, empty slot or position past the grid");
  picker.PageState = 2U;
  picker.Position = 1U;
  const auto taken = DescribeTopScreenDpadItemPicker(picker, 1U, -1);
  Require(taken.find("dpad picker: A held page_state=2 position=1 age=adult slot=1 item=0x") == 0U &&
              taken.ends_with(": takes it"),
          "the picker log line names what it read and that it took the item");
  picker.Position = 2U;
  picker.Age = 1U;
  Require(DescribeTopScreenDpadItemPicker(picker, slot(2), 3).find(
              "dpad picker: right page_state=2 position=2 age=child slot=15") == 0U &&
              DescribeTopScreenDpadItemPicker(picker, slot(2), 3)
                  .ends_with("refused: the item is for the other age"),
          "the picker log line says why it refused the item");
  picker.Age = 0U;
  picker.Browsing = false;
  picker.PageState = 8U;
  Require(slot(1) == kTopScreenDpadItemSlotNone, "only while the Items page browses");
  Require(DescribeTopScreenDpadItemPicker(picker, slot(1), 0)
              .ends_with("refused: not browsing the Items page"),
          "the picker log line says the page was not browsing");

  TopScreenUiConfig config;
  AssignTopScreenDpadItem(config, true, 2U, 1U);
  Require(config.ChildDpad[2] == TopScreenDpadAction::Item &&
              config.ChildDpadItemSlots[2] == 1U &&
              config.AdultDpad == kTopScreenAdultDpadDefaults,
          "the direction takes the item for that age only");
  AssignTopScreenDpadItem(config, true, 0U, 1U);
  Require(config.ChildDpad[0] == TopScreenDpadAction::Item &&
              config.ChildDpad[2] == TopScreenDpadAction::None &&
              config.ChildDpadItemSlots[2] == kTopScreenDpadItemSlotNone,
          "an item moves to its new direction");
  const auto before = config;
  AssignTopScreenDpadItem(config, false, 1U, 4U);
  Require(config == before, "a magic arrow slot is never assigned");
}

constexpr std::uint32_t kPlay = 0x30000000U;
constexpr std::uint32_t kPlayer = 0x30010000U;
constexpr std::uint32_t kSave = 0x00587958U;

void TestDispatch() {
  NativeA32Memory memory;
  Require(memory.MapRegion({"ui", 0x00500000U, 0x90000U, true, false, {}}), "map UI");
  Require(memory.MapRegion({"actions", 0x004D0000U, 0x10000U, true, false, {}}),
          "map item actions");
  Require(memory.MapRegion({"play", kPlay, 0x6000U, true, false, {}}), "map play");
  Require(memory.MapRegion({"player", kPlayer, 0x3000U, true, false, {}}), "map player");
  for (std::uint32_t item = 0; item < 62U; ++item)
    memory.Write8(0x00506C58U + item, 9U);
  memory.Write8(0x004DBD9AU + kNut, 0x20U);
  memory.Write32(kSave + 0x04U, 1U); // child
  memory.Write8(kSave + 0x8CU + 1U, kNut);
  memory.Write8(kSave + 0x8CU + 18U, kFairy);
  memory.Write8(kSave + 0x85U + 1U, 18U); // X holds bottle 1 too
  memory.Write8(kSave + 0x80U + 2U, kFairy);
  memory.Write32(0x0050AF34U + 0x34U, 2U);
  memory.Write8(kPlay + 0x100U, 3U);
  memory.Write8(kPlay + 0x101U, 2U);
  memory.Write32(kPlay + 0x20ACU, kPlayer);

  TopScreenItemDispatchRuntime runtime;
  TopScreenExtendedInputFrame input;
  oot3d::recomp::a32::GuestState state{};
  state.r[4] = kTopScreenNativeScanNoButton;
  state.r[11] = kPlay;
  input.DpadItemPressed[2] = true;
  Require(!ShouldObserveTopScreenItemDispatch(kTopScreenDpadItemPressScanExit, memory,
                                              state, input, runtime),
          "inert without D-pad items");
  runtime.ChildDpadItemSlots = {kTopScreenDpadItemSlotNone, 18U, 1U,
                                kTopScreenDpadItemSlotNone};
  Require(ShouldObserveTopScreenItemDispatch(kTopScreenDpadItemPressScanExit, memory,
                                             state, input, runtime) &&
              ExecuteTopScreenItemDispatch(kTopScreenDpadItemPressScanExit, memory,
                                           state, input, runtime),
          "a Left press at the press scan exit");
  Require(state.r[4] == 8U && state.r[15] == kTopScreenDpadItemPressScanExit &&
              runtime.PendingDpadItemButton == 8U,
          "resumes in place with button 8");
  Require(!ShouldObserveTopScreenItemDispatch(kTopScreenDpadItemPressScanExit, memory,
                                              state, input, runtime),
          "the resumed block runs natively");

  state.r[0] = kPlay;
  state.r[1] = 8U;
  state.r[14] = 0x00123456U;
  Require(!ShouldObserveTopScreenItemDispatch(kTopScreenSlotItemEntry, memory, state,
                                              input, runtime),
          "the resolver answers 6..9 only to the scans");
  state.r[14] = kTopScreenDpadItemPressResolverReturn;
  Require(ExecuteTopScreenItemDispatch(kTopScreenSlotItemEntry, memory, state, input,
                                       runtime) &&
              state.r[0] == kNut && state.r[15] == kTopScreenDpadItemPressResolverReturn &&
              runtime.PendingDpadItemButton == 0U,
          "button 8 resolves to its Deku Nuts");

  state.r[0] = 0x20U;
  Require(ExecuteTopScreenItemDispatch(kTopScreenDpadItemHeldPutAway, memory, state,
                                       input, runtime) &&
              state.r[15] == kTopScreenDpadItemHeldKept,
          "a held D-pad item is not put away");
  state.r[0] = 0x21U;
  Require(!ShouldObserveTopScreenItemDispatch(kTopScreenDpadItemHeldPutAway, memory,
                                              state, input, runtime),
          "other held items still are");

  memory.Write8(kPlayer + 0x1ACU, 42U); // releasing the fairy
  state.r[0] = kPlay;
  state.r[1] = kEmptyBottle;
  state.r[2] = 7U;
  state.r[14] = 0x0035E59CU;
  Require(ShouldObserveTopScreenItemDispatch(kTopScreenBottleUpdateEntry, memory, state,
                                             input, runtime) &&
              ExecuteTopScreenItemDispatch(kTopScreenBottleUpdateEntry, memory, state,
                                           input, runtime) &&
              state.r[15] == 0x0035E59CU,
          "Inventory_UpdateBottleItem for button 7 returns without native code");
  std::uint8_t bottle = 0U, xItem = 0U;
  memory.Read8(kSave + 0x8CU + 18U, &bottle);
  memory.Read8(kSave + 0x80U + 2U, &xItem);
  Require(bottle == kEmptyBottle && xItem == kEmptyBottle,
          "the D-pad bottle and X, set to the same bottle, are empty");
  state.r[2] = 3U;
  Require(!ShouldObserveTopScreenItemDispatch(kTopScreenBottleUpdateEntry, memory, state,
                                              input, runtime),
          "native buttons keep the native update");

  memory.Write32(0x005043D4U + 0x0CU, kPlay);
  std::array<bool, 4> usable{};
  Require(ReadTopScreenDpadItemUsability(memory, runtime.ChildDpadItemSlots,
                                         runtime.AdultDpadItemSlots, &usable) &&
              !usable[0] && usable[1] && usable[2] && !usable[3],
          "the HUD sees what the hooks resolve");
  memory.Write32(kPlayer + 0x1710U, 0x00800000U);
  Require(ReadTopScreenDpadItemUsability(memory, runtime.ChildDpadItemSlots,
                                         runtime.AdultDpadItemSlots, &usable) &&
              !usable[1] && !usable[2],
          "greyed on Epona");
}

} // namespace

void RunTopScreenDpadItemsTests() {
  TestConfig();
  TestSlotItems();
  TestRules();
  TestButtons();
  TestBottles();
  TestBoomerangSlingshot();
  TestPresentation();
  TestPicker();
  TestDispatch();
}
