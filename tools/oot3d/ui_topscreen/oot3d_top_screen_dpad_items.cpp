#include "oot3d_top_screen_dpad_items.h"

#include "oot3d_top_screen_config.h"

#include <algorithm>

namespace Oot3dNativeGame {
namespace {

constexpr std::uint32_t kSaveContext = 0x00587958U;
constexpr std::uint32_t kButtonItems = 0x005879D8U;  // +0x80, B first
constexpr std::uint32_t kButtonSlots = 0x005879DDU;  // +0x85, Y first
constexpr std::uint32_t kInventory = 0x005879E4U;    // +0x8C
constexpr std::uint32_t kButtonStatus = 0x00588EC7U; // B first
constexpr std::uint32_t kOcarinaRestriction = 0x00588ECDU;
constexpr std::uint32_t kInterfaceMode = 0x00588EE2U;
constexpr std::uint32_t kItemAgeTable = 0x00506C58U;
constexpr std::uint32_t kItemActionTable = 0x004DBD9AU;
constexpr std::uint32_t kPauseContext = 0x0050AF34U;
constexpr std::uint32_t kGameplayRoot = 0x005043D4U;
constexpr std::uint32_t kPlayerSpan = 0x2228U;

// 3.2.2 0x0060DF8A + action: the content each bottle action expects, for
// actions 30 (empty bottle) to 42 (fairy).
constexpr std::array<std::uint8_t, 13> kBottleActionItems{
    0x14U, 0x19U, 0x1CU, 0x1DU, 0x20U, 0x1EU, 0x1BU,
    0x15U, 0x17U, 0x16U, 0x1AU, 0x1FU, 0x18U};

enum RestrictionIndex : std::size_t {
  kBottles = 0,
  kTradeItems = 1,
  kHookshot = 2,
  kFaroresWind = 6,
  kDinsNayrus = 7,
  kAll = 8,
};

constexpr bool InRange(unsigned value, unsigned first, unsigned last) {
  return value >= first && value <= last;
}

// 3.2.2 0x005CAE6C.
bool UsableAtAge(const TopScreenDpadItemGuestState &state, std::uint8_t item) {
  if (item > 61U)
    return true;
  const auto age = state.ItemAge[item];
  return age == 9U || age == state.Age;
}

} // namespace

std::uint8_t TopScreenDpadSlotItem(const std::array<std::uint8_t, 24> &inventory,
                                   std::uint8_t slot) noexcept {
  if (!IsTopScreenDpadItemSlot(slot))
    return kTopScreenNoItem;
  const std::uint8_t item = inventory[slot];
  if (InRange(slot, 18U, 21U))
    return InRange(item, 0x14U, 0x20U) ? item : kTopScreenNoItem;
  if (InRange(slot, 22U, 23U))
    return item != 0x2CU && InRange(item, 0x21U, 0x37U) ? item
                                                         : kTopScreenNoItem;
  if (item > 61U || item == 0x04U || item == 0x0CU || item == 0x12U)
    return kTopScreenNoItem;
  return item;
}

bool ReadTopScreenDpadItemGuestState(
    NativeA32Memory &memory, std::uint32_t play,
    const std::array<std::uint8_t, 4> &childSlots,
    const std::array<std::uint8_t, 4> &adultSlots,
    TopScreenDpadItemGuestState *state) {
  if (state == nullptr)
    return false;
  TopScreenDpadItemGuestState result;
  std::uint32_t age = 0U;
  if (!memory.Read32(kSaveContext + 0x04U, &age))
    return false;
  result.Age = age != 0U ? 1U : 0U;
  result.Slots = result.Age != 0U ? childSlots : adultSlots;
  for (std::size_t slot = 0; slot < result.Inventory.size(); ++slot)
    if (!memory.Read8(kInventory + slot, &result.Inventory[slot]))
      return false;
  for (std::size_t item = 0; item < result.ItemAge.size(); ++item)
    if (!memory.Read8(kItemAgeTable + item, &result.ItemAge[item]))
      return false;
  for (std::size_t button = 0; button < 4U; ++button)
    if (!memory.Read8(kButtonItems + 1U + button, &result.ButtonItems[button]) ||
        !memory.Read8(kButtonStatus + 1U + button, &result.ButtonStatus[button]))
      return false;
  if (!memory.Read16(kInterfaceMode, &result.InterfaceMode) ||
      !memory.Read8(kOcarinaRestriction, &result.OcarinaRestricted) ||
      !memory.Read32(kPauseContext + 0x34U, &result.PauseState) ||
      !memory.Read32(kGameplayRoot + 0x14U, &result.GameplayRootBusy))
    return false;
  if (play != 0U) {
    std::uint16_t scene = 0U;
    std::uint8_t minigame = 0U, special = 0U;
    std::uint32_t player = 0U;
    if (!memory.Read8(play + 0x100U, &result.PlayStateType) ||
        !memory.Read8(play + 0x101U, &result.PlayStateSubtype) ||
        !memory.Read16(play + 0x104U, &scene) ||
        !memory.Read8(play + 0x5C74U, &minigame) ||
        !memory.Read8(play + 0x5C75U, &special) ||
        !memory.Read8(play + 0x4C32U, &result.PlayMode4C32) ||
        !memory.Read32(play + 0x20ACU, &player))
      return false;
    for (std::size_t index = 0; index < result.Restrictions.size(); ++index)
      if (!memory.Read8(play + 0x2E45U + index, &result.Restrictions[index]))
        return false;
    result.HasPlay = true;
    result.SceneNum = static_cast<std::int16_t>(scene);
    result.MinigameState = static_cast<std::int8_t>(minigame);
    result.SpecialState = static_cast<std::int8_t>(special);
    // The PlayState publishes its Player before the heap object is mapped
    // (oot3d_top_screen_gameplay_action_consumer.cpp): no Player until it is.
    if (player != 0U && memory.IsMapped(player, kPlayerSpan)) {
      std::uint32_t heldActor = 0U;
      if (!memory.Read32(player + 0x1710U, &result.StateFlags1) ||
          !memory.Read32(player + 0x1714U, &result.StateFlags2) ||
          !memory.Read16(player + 0x90U, &result.BgCheckFlags) ||
          !memory.Read8(player + 0x1A7U, &result.UnderwaterBoots) ||
          !memory.Read8(player + 0x1A8U, &result.HeldItemButton) ||
          !memory.Read8(player + 0x1A9U, &result.HeldItemAction) ||
          !memory.Read8(player + 0x1ACU, &result.ItemAction) ||
          !memory.Read16(player + 0x2224U, &result.WaterDepth) ||
          !memory.Read32(player + 0x1224U, &heldActor))
        return false;
      if (heldActor != 0U) {
        std::uint16_t id = 0U;
        if (!memory.Read16(heldActor, &id))
          return false;
        result.HeldActorId = static_cast<std::int16_t>(id);
      }
      result.HasPlayer = true;
    }
  }
  *state = result;
  return true;
}

bool ReadTopScreenDpadItemPickerState(NativeA32Memory &memory,
                                      TopScreenDpadItemPickerState *state) {
  if (state == nullptr)
    return false;
  TopScreenDpadItemPickerState result;
  std::uint32_t pageState = 0U, age = 0U;
  if (!memory.Read32(0x0050672CU, &pageState) ||
      !memory.Read32(0x00506748U, &result.Position) ||
      !memory.Read32(kSaveContext + 0x04U, &age))
    return false;
  result.Browsing = pageState == 2U;
  result.Age = age != 0U ? 1U : 0U;
  const std::uint32_t grid = kSaveContext + (result.Age != 0U ? 0x138AU : 0x13A2U);
  for (std::size_t position = 0; position < result.Grid.size(); ++position)
    if (!memory.Read8(grid + position, &result.Grid[position]) ||
        !memory.Read8(kInventory + position, &result.Inventory[position]))
      return false;
  for (std::size_t item = 0; item < result.ItemAge.size(); ++item)
    if (!memory.Read8(kItemAgeTable + item, &result.ItemAge[item]))
      return false;
  *state = result;
  return true;
}

std::uint8_t
ResolveTopScreenDpadItemPickerSlot(const TopScreenDpadItemPickerState &state) noexcept {
  if (!state.Browsing || state.Position >= state.Grid.size())
    return kTopScreenDpadItemSlotNone;
  const auto slot = state.Grid[state.Position];
  if (slot >= state.Inventory.size())
    return kTopScreenDpadItemSlotNone;
  const auto item = TopScreenDpadSlotItem(state.Inventory, slot);
  if (item == kTopScreenNoItem)
    return kTopScreenDpadItemSlotNone;
  const auto age = item <= 61U ? state.ItemAge[item] : 9U;
  return age == 9U || age == state.Age ? slot : kTopScreenDpadItemSlotNone;
}

bool ReadTopScreenDpadItemUsability(NativeA32Memory &memory,
                                    const std::array<std::uint8_t, 4> &childSlots,
                                    const std::array<std::uint8_t, 4> &adultSlots,
                                    std::array<bool, 4> *usable) {
  std::uint32_t play = 0U;
  TopScreenDpadItemGuestState state;
  if (usable == nullptr || !memory.Read32(kGameplayRoot + 0x0CU, &play) ||
      !ReadTopScreenDpadItemGuestState(memory, play, childSlots, adultSlots,
                                       &state))
    return false;
  for (std::size_t direction = 0; direction < usable->size(); ++direction)
    (*usable)[direction] =
        ResolveTopScreenDpadItem(state, direction) != kTopScreenNoItem;
  return true;
}

bool TopScreenDpadItemRestricted(const TopScreenDpadItemGuestState &state,
                                 std::uint8_t item) noexcept {
  if (!state.HasPlay)
    return true;
  // 0x005CAEC4: scene 73 and the minigames past state 1 take every item.
  if (state.MinigameState > 1 || state.SceneNum == 73)
    return true;
  const auto &restrictions = state.Restrictions;
  bool hookshotUnderwater = false;
  if (state.HasPlayer && state.PlayMode4C32 != 3U) {
    // 0x005CAF20: deep water, or walking on the floor in Iron Boots, leaves
    // only the Hookshot and Longshot, and only from the floor.
    if (state.WaterDepth > 45U &&
        (state.WaterDepth > 0x1C1U || state.UnderwaterBoots == 1U)) {
      if (state.UnderwaterBoots != 1U || (state.BgCheckFlags & 1U) == 0U ||
          !InRange(item, 10U, 11U))
        return true;
      hookshotUnderwater = true;
    }
    if (!hookshotUnderwater && (state.StateFlags1 & 0x08000000U) != 0U)
      return true; // swimming
  }
  if (!hookshotUnderwater) {
    if (InRange(item, 20U, 32U))
      return restrictions[kBottles] != 0U;
    if (InRange(item, 33U, 55U))
      return restrictions[kTradeItems] != 0U;
    if (InRange(item, 7U, 8U))
      return state.OcarinaRestricted != 0U;
  }
  // 0x005CAFD0: "all" spares only the Lens of Truth in scene 16 (the
  // Treasure Chest Shop); otherwise table 0x0060DB80 picks the category.
  if (restrictions[kAll] != 0U)
    return !(state.SceneNum == 16 && item == 15U);
  switch (item) {
  case 5U:
  case 19U:
    return restrictions[kDinsNayrus] != 0U;
  case 10U:
  case 11U:
    return restrictions[kHookshot] != 0U;
  case 13U:
    return restrictions[kFaroresWind] != 0U;
  default:
    return false;
  }
}

std::uint8_t ResolveTopScreenDpadItem(const TopScreenDpadItemGuestState &state,
                                      std::size_t direction) noexcept {
  if (direction >= state.Slots.size() || !state.HasPlay)
    return kTopScreenNoItem;
  std::uint8_t item =
      TopScreenDpadSlotItem(state.Inventory, state.Slots[direction]);
  if (item == kTopScreenNoItem)
    return kTopScreenNoItem;
  // As the native resolver: Bombchu Bowling puts bombchus on every button.
  if (state.SpecialState != 0)
    return state.SpecialState > 0 ? 9U : kTopScreenNoItem;
  // 0x005D17A8: climbing or hanging (flags1 0x00202000), crawling (flags2
  // 0x00040000).
  if (state.HasPlayer && ((state.StateFlags1 & 0x00202000U) != 0U ||
                          (state.StateFlags2 & 0x00040000U) != 0U))
    return kTopScreenNoItem;
  if ((state.InterfaceMode & 0x0FU) == 1U && !InRange(item, 7U, 8U))
    return kTopScreenNoItem;
  // 0x005D17E8: Y, X, I and II all disabled, and not all by the scene, means
  // Link's state disables items; the D-pad follows.
  bool inferred = false;
  bool allDisabled = true;
  for (std::size_t button = 0; button < 4U; ++button) {
    if (state.ButtonStatus[button] != 0xFFU) {
      allDisabled = false;
      break;
    }
    const auto buttonItem = state.ButtonItems[button];
    inferred = inferred || InRange(buttonItem, 0x44U, 0x46U) ||
               !TopScreenDpadItemRestricted(state, buttonItem);
  }
  if (allDisabled && inferred)
    return kTopScreenNoItem;
  if (InRange(item, 20U, 32U) && state.Restrictions[kBottles] != 0U)
    return kTopScreenNoItem;
  if (TopScreenDpadItemRestricted(state, item) || !UsableAtAge(state, item))
    return kTopScreenNoItem;
  if (state.HasPlayer) {
    if ((state.StateFlags1 & 0x00800000U) != 0U)
      return kTopScreenNoItem; // on Epona
    // 0x005D18AC: aiming from this button keeps the arrow chosen (bow
    // actions 8..11 are items 3, 56, 57, 58).
    if ((item == 3U || InRange(item, 56U, 58U)) &&
        state.HeldItemButton == kTopScreenDpadItemButtonFirst + direction &&
        InRange(state.HeldItemAction, 8U, 11U))
      item = state.HeldItemAction == 8U
                 ? 3U
                 : static_cast<std::uint8_t>(state.HeldItemAction + 47U);
  }
  return item;
}

std::uint32_t ResolveTopScreenDpadItemButton(
    const TopScreenDpadItemGuestState &state,
    const std::array<bool, 4> &lanes, bool holdScan) noexcept {
  if (!state.HasPlayer ||
      std::none_of(lanes.begin(), lanes.end(), [](bool lane) { return lane; }))
    return 0U;
  if (!holdScan) {
    // 0x005CD898: an arrow nocked on the bow; the D-pad switches arrows.
    const bool arrowNocked = state.PlayStateType == 3U &&
                             state.PlayStateSubtype == 2U &&
                             InRange(state.HeldItemAction, 8U, 11U) &&
                             state.HeldActorId == 22;
    // 0x005CDAFC / 0x005CB200: pause controller states 7..9 and past 11.
    const bool busy = state.PauseState > 11U ||
                      InRange(state.PauseState, 7U, 9U) ||
                      state.GameplayRootBusy != 0U;
    if (arrowNocked || busy)
      return 0U;
  } else {
    const std::uint32_t held = state.HeldItemButton;
    if (InRange(held, 6U, 9U) && lanes[held - 6U] &&
        ResolveTopScreenDpadItem(state, held - 6U) != kTopScreenNoItem)
      return held;
  }
  for (std::size_t direction = 0; direction < lanes.size(); ++direction)
    if (lanes[direction] &&
        ResolveTopScreenDpadItem(state, direction) != kTopScreenNoItem)
      return kTopScreenDpadItemButtonFirst +
             static_cast<std::uint32_t>(direction);
  return 0U;
}

bool TopScreenDpadItemsUseAction(NativeA32Memory &memory,
                                 const TopScreenDpadItemGuestState &state,
                                 std::uint8_t itemAction) {
  for (std::size_t direction = 0; direction < state.Slots.size();
       ++direction) {
    const auto item = ResolveTopScreenDpadItem(state, direction);
    if (item == kTopScreenNoItem)
      continue;
    // 0x005CD710: items past 61 have no action.
    std::uint8_t action = 0U;
    if (item <= 61U && !memory.Read8(kItemActionTable + item, &action))
      continue;
    if (action == itemAction)
      return true;
  }
  return false;
}

std::optional<TopScreenDpadBottleUpdate>
ResolveTopScreenDpadBottleUpdate(const TopScreenDpadItemGuestState &state,
                                 std::uint32_t button,
                                 std::uint8_t item) noexcept {
  if (!InRange(button, 6U, 9U))
    return std::nullopt;
  const auto slot = state.Slots[button - 6U];
  if (!InRange(slot, 18U, 21U))
    return std::nullopt;
  const auto current = state.Inventory[slot];
  if (!InRange(current, 0x14U, 0x20U))
    return std::nullopt;
  if (state.HasPlayer && InRange(state.ItemAction, 30U, 42U) &&
      current != kBottleActionItems[state.ItemAction - 30U])
    return std::nullopt;
  // Drinking Lon Lon Milk leaves half a bottle, as in the native update.
  if (current == 0x1AU && item == 0x14U)
    item = 0x1FU;
  return TopScreenDpadBottleUpdate{slot, item};
}

bool ApplyTopScreenDpadBottleUpdate(NativeA32Memory &memory,
                                    std::uint32_t play,
                                    const TopScreenDpadBottleUpdate &update) {
  std::array<std::uint8_t, 4> buttonSlots{};
  for (std::size_t button = 0; button < buttonSlots.size(); ++button)
    if (!memory.Read8(kButtonSlots + button, &buttonSlots[button]) ||
        !memory.IsWritable(kButtonItems + 1U + button, 1U))
      return false;
  if (!memory.IsWritable(kInventory + update.Slot, 1U) ||
      (play != 0U && !memory.IsWritable(play + 0x3106U, 2U)))
    return false;
  memory.Write8(kInventory + update.Slot, update.Item);
  if (play != 0U)
    memory.Write16(play + 0x3106U, update.Item);
  for (std::size_t button = 0; button < buttonSlots.size(); ++button)
    if (buttonSlots[button] == update.Slot)
      memory.Write8(kButtonItems + 1U + button, update.Item);
  return true;
}

} // namespace Oot3dNativeGame
