#pragma once

#include "oot3d_top_screen_dpad_items.h"
#include "oot3d_top_screen_item_guest.h"
#include <vector>

namespace Oot3dNativeGame {

inline constexpr uint32_t kTopScreenSlotItemEntry = 0x002C3970U;
inline constexpr uint32_t kTopScreenItemsUpdateCall = 0x00433B68U;
inline constexpr uint32_t kTopScreenItemsUpdateEntry = 0x002EC3E4U;
inline constexpr uint32_t kTopScreenItemsUpdateReturn = 0x00433B6CU;

struct TopScreenItemQueryTrace {
    uint32_t Entry = 0;
    uint32_t ReturnPc = 0;
    uint32_t SuppressionFlags = 0;
    bool NativeResult = false;
    bool Result = false;
    TopScreenExtendedInputFrame Input;
};

struct TopScreenItemDispatchRuntime {
    // Published by the gameplay-action owner, independent of HID sampling.
    uint8_t DirectItemId = 0;
    // Slots of the Item directions of each age (0xFF elsewhere), published
    // from the TopScreen config each refresh.
    std::array<uint8_t, 4> ChildDpadItemSlots{0xFFU, 0xFFU, 0xFFU, 0xFFU};
    std::array<uint8_t, 4> AdultDpadItemSlots{0xFFU, 0xFFU, 0xFFU, 0xFFU};
    // D-pad item button a scan exit chose, for the slot resolver call that
    // directly follows it; the native scans never produce 6..9.
    uint32_t PendingDpadItemButton = 0;
    uint64_t DpadItemButtons = 0;
    uint64_t DpadItemResolves = 0;
    uint64_t DpadItemsKept = 0;
    uint64_t DpadBottleUpdates = 0;
    uint32_t PendingSelection = 0;
    uint64_t QueryEntries = 0;
    std::array<uint64_t, 4> QueryCalls{};
    std::array<uint64_t, 4> QueryTrueResults{};
    uint64_t SlotEntries = 0;
    uint64_t SlotAttempts = 0;
    uint64_t SlotOverrides = 0;
    uint64_t SlotNativeFallbacks = 0;
    uint64_t SelectionBegins = 0;
    uint64_t SelectionCompletions = 0;
    uint64_t SelectionUpdates = 0;
    uint32_t PreviousSelectionButtons = 0;
    uint32_t PreviousSelectionPressed = 0;
    bool TraceEnabled = false;
    std::vector<TopScreenItemQueryTrace> QueryTrace;
};

// One registry for compiled-code observation and native callback routing.
std::span<const uint32_t> TopScreenItemDispatchEntries();
bool IsTopScreenItemDispatchEntry(uint32_t pc);

// Read-only eligibility: ordinary calls remain inside compiled guest code.
bool ShouldObserveTopScreenItemDispatch(
    uint32_t pc, NativeA32Memory& memory,
    const oot3d::recomp::a32::GuestState& state,
    const TopScreenExtendedInputFrame& input,
    const TopScreenItemDispatchRuntime& runtime);

// False leaves registers and memory untouched; the dispatcher resumes native
// execution once. Queries read a tick-wide pressed snapshot, not a consumable
// action queue. This module does not extend input lifetime across guest ticks.
bool ExecuteTopScreenItemDispatch(
    uint32_t pc, NativeA32Memory& memory,
    oot3d::recomp::a32::GuestState& state,
    const TopScreenExtendedInputFrame& input,
    TopScreenItemDispatchRuntime& runtime);

} // namespace Oot3dNativeGame
