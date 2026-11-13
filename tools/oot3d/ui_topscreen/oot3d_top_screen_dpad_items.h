#pragma once

#include "oot3d_native_a32_memory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace Oot3dNativeGame {

// SingleScreen 3.2.2's D-pad item buttons. Player_UpdateUpperBody
// (0x0034CC78) scans buttons 0..5 (B, Y, X, I, II, 5) for a press, then for a
// hold; 3.2.2 numbers the D-pad 6 + direction (Up, Down, Left, Right) and
// patches the Player at four sites. The port hooks the same sites
// (oot3d_top_screen_item_dispatch.cpp):
//   0x0034D1EC / 0x0034D268  scan exits: no native button, try the D-pad
//                            (3.2.2 0x005F7200 / 0x005F71F8 -> 0x005D2050)
//   0x002C3970               slot resolver: the item of button 6..9 (0x005D2178)
//   0x004C1044               Inventory_UpdateBottleItem for 6..9 (0x005D05D8)
//   0x0034D104               held item kept while a D-pad item uses it
//                            (0x005F722C -> 0x005D212C)
//   0x0034CF78               mask kept while a D-pad item is that mask
//                            (0x005F7248)
inline constexpr std::uint32_t kTopScreenDpadItemPressScanExit = 0x0034D1ECU;
inline constexpr std::uint32_t kTopScreenDpadItemHoldScanExit = 0x0034D268U;
inline constexpr std::uint32_t kTopScreenDpadItemPressResolverReturn = 0x0034D1F8U;
inline constexpr std::uint32_t kTopScreenDpadItemHoldResolverReturn = 0x0034D274U;
inline constexpr std::uint32_t kTopScreenDpadItemHeldPutAway = 0x0034D104U;
inline constexpr std::uint32_t kTopScreenDpadItemHeldKept = 0x0034D120U;
inline constexpr std::uint32_t kTopScreenDpadItemMaskRemoval = 0x0034CF78U;
inline constexpr std::uint32_t kTopScreenDpadItemMaskKept = 0x0034CF7CU;
inline constexpr std::uint32_t kTopScreenBottleUpdateEntry = 0x004C1044U;

inline constexpr std::uint32_t kTopScreenDpadItemButtonFirst = 6U;
// Player_UpdateUpperBody's scans end at 6 when no native button matched.
inline constexpr std::uint32_t kTopScreenNativeScanNoButton = 6U;
inline constexpr std::uint8_t kTopScreenNoItem = 0xFFU;

struct TopScreenDpadItemGuestState {
  // Slots of the current age's Item directions (kTopScreenDpadItemSlotNone
  // for the others), from the TopScreen config.
  std::array<std::uint8_t, 4> Slots{0xFFU, 0xFFU, 0xFFU, 0xFFU};
  std::array<std::uint8_t, 24> Inventory{};
  // 0x00506C58: the age an item needs (0 adult, 1 child, 9 both).
  std::array<std::uint8_t, 62> ItemAge{};
  std::uint32_t Age = 0U; // SaveContext +0x04: 0 adult, 1 child
  // Y, X, I, II: SaveContext +0x81..+0x84, statuses 0x00588EC8..CB.
  std::array<std::uint8_t, 4> ButtonItems{};
  std::array<std::uint8_t, 4> ButtonStatus{};
  std::uint16_t InterfaceMode = 0U;   // 0x00588EE2, & 15 == 1: ocarina only
  std::uint8_t OcarinaRestricted = 0U; // 0x00588ECD
  std::uint32_t PauseState = 0U;      // 0x0050AF34 +0x34
  std::uint32_t GameplayRootBusy = 0U; // 0x005043D4 +0x14

  bool HasPlay = false;
  std::uint8_t PlayStateType = 0U;    // +0x100
  std::uint8_t PlayStateSubtype = 0U; // +0x101
  std::int16_t SceneNum = 0;          // +0x104
  std::int8_t MinigameState = 0;      // +0x5C74
  std::int8_t SpecialState = 0;       // +0x5C75 (> 0: every button is 9)
  std::uint8_t PlayMode4C32 = 0U;     // +0x4C32
  // +0x2E45..+0x2E4D: bottles, trade items, hookshot, ocarina, warp songs,
  // Sun's Song, Farore's Wind, Din's Fire / Nayru's Love, all.
  std::array<std::uint8_t, 9> Restrictions{};

  bool HasPlayer = false;             // PlayState +0x20AC
  std::uint32_t StateFlags1 = 0U;     // +0x1710
  std::uint32_t StateFlags2 = 0U;     // +0x1714
  std::uint16_t BgCheckFlags = 0U;    // +0x90
  std::uint8_t HeldItemButton = 0U;   // +0x1A8
  std::uint8_t HeldItemAction = 0U;   // +0x1A9
  std::uint8_t ItemAction = 0U;       // +0x1AC
  std::uint8_t UnderwaterBoots = 0U;  // +0x1A7 (1: walking under water)
  std::uint16_t WaterDepth = 0U;      // +0x2224
  std::int16_t HeldActorId = -1;      // heldActor (+0x1224) +0x00
};

// The item an Item direction's slot holds, or kTopScreenNoItem: bottles must
// hold a bottle item, the trade slots a trade item other than SOLD OUT, the
// others an item up to 61 that is not a magic arrow (3.2.2 0x005D1124 /
// 0x005CDC3C). Shared with the HUD icons.
std::uint8_t TopScreenDpadSlotItem(const std::array<std::uint8_t, 24> &inventory,
                                   std::uint8_t slot) noexcept;

// Reads everything the rules need. play may be 0 (no PlayState): the result
// then resolves no D-pad item.
bool ReadTopScreenDpadItemGuestState(
    NativeA32Memory &memory, std::uint32_t play,
    const std::array<std::uint8_t, 4> &childSlots,
    const std::array<std::uint8_t, 4> &adultSlots,
    TopScreenDpadItemGuestState *state);

// Per direction: its D-pad item button resolves an item now (the gameplay
// rules, on the root PlayState). For the HUD, which greys the others.
bool ReadTopScreenDpadItemUsability(NativeA32Memory &memory,
                                    const std::array<std::uint8_t, 4> &childSlots,
                                    const std::array<std::uint8_t, 4> &adultSlots,
                                    std::array<bool, 4> *usable);

// SingleScreen 3.0's "Assign any item to the D-pad from the ITEMS screen
// (Hold A)". Logs41 (issue #13) showed how the game takes that A: while the
// page browses (state 2) both positions below are -1; the A press moves it
// to state 8 with 0x00506748 the position of the item it picked up (to
// rearrange it), and the D-pad then moves that item. So the picker works in
// state 8, while A is still held: a direction takes the picked item and is
// kept from the game, and the release puts the item back in place.
inline constexpr std::uint32_t kTopScreenItemsPickedUp = 8U;
struct TopScreenDpadItemPickerState {
  std::uint32_t PageState = 0U;   // Items page state 0x0050672C
  bool PickedUp = false;          // PageState == kTopScreenItemsPickedUp
  std::uint32_t Position = 0U;    // the picked-up item's grid position, 0x00506748
  // The game's other Items position, 0x0050674C (the 2.1.1 ZR/ZL selection
  // writes it): logged beside Position (issue #13) to tell which one follows
  // the cursor on the console.
  std::uint32_t OtherPosition = 0U;
  std::uint32_t Age = 0U;         // 0 adult, 1 child
  // The grid of the current age: position -> slot (SaveContext +0x138A for
  // the child, +0x13A2 for the adult, as PauseItemsPage_CanSwapSelectedSlots).
  std::array<std::uint8_t, 24> Grid{};
  std::array<std::uint8_t, 24> Inventory{};
  std::array<std::uint8_t, 62> ItemAge{};
};

bool ReadTopScreenDpadItemPickerState(NativeA32Memory &memory,
                                      TopScreenDpadItemPickerState *state);

// The slot of the item A picked up when a D-pad direction can hold it and
// the age can use it, else kTopScreenDpadItemSlotNone.
std::uint8_t
ResolveTopScreenDpadItemPickerSlot(const TopScreenDpadItemPickerState &state) noexcept;

// The game log line for a picker input (issue #13: hold A did nothing):
// what it read and why it took or refused the hovered item. `direction` is
// the D-pad direction pressed (0..3), or -1 for the A press itself.
std::string DescribeTopScreenDpadItemPicker(const TopScreenDpadItemPickerState &state,
                                            std::uint8_t slot, int direction);

// 3.2.2 0x005CAEC4: the scene, the water and the minigame refuse the item.
bool TopScreenDpadItemRestricted(const TopScreenDpadItemGuestState &state,
                                 std::uint8_t item) noexcept;

// 3.2.2 0x005D1748: the item D-pad button 6 + direction uses now, or
// kTopScreenNoItem.
std::uint8_t ResolveTopScreenDpadItem(const TopScreenDpadItemGuestState &state,
                                      std::size_t direction) noexcept;

// 3.2.2 0x005D2050 at a scan exit: 6 + direction, or 0 to keep the native
// "no button". The press scan takes fresh presses and stays out of the
// arrow switch while aiming; the hold scan keeps the button the held item
// came from.
std::uint32_t ResolveTopScreenDpadItemButton(
    const TopScreenDpadItemGuestState &state,
    const std::array<bool, 4> &lanes, bool holdScan) noexcept;

// 0x005D212C / 0x005F7248: one of the D-pad items has this item action.
bool TopScreenDpadItemsUseAction(NativeA32Memory &memory,
                                 const TopScreenDpadItemGuestState &state,
                                 std::uint8_t itemAction);

struct TopScreenDpadBottleUpdate {
  std::uint8_t Slot = 0U;
  std::uint8_t Item = 0U;
};

// 3.2.2 0x005D05D8 for button 6..9: the bottle slot of that direction gets
// the new content, or nothing changes when the slot no longer holds the
// bottle the Player is using.
std::optional<TopScreenDpadBottleUpdate>
ResolveTopScreenDpadBottleUpdate(const TopScreenDpadItemGuestState &state,
                                 std::uint32_t button,
                                 std::uint8_t item) noexcept;

// Applies an update as Inventory_UpdateBottleItem would (inventory and the
// pause cursor item), plus every Y/X/I/II button set to the same slot, so a
// stale copy of the old content cannot be used again.
bool ApplyTopScreenDpadBottleUpdate(NativeA32Memory &memory,
                                    std::uint32_t play,
                                    const TopScreenDpadBottleUpdate &update);

} // namespace Oot3dNativeGame
