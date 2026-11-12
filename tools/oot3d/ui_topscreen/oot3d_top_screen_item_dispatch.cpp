#include "oot3d_top_screen_item_dispatch.h"

#include <algorithm>
#include <vector>

namespace Oot3dNativeGame {
namespace {
constexpr uint32_t kItemsPageState = 0x0050672CU;
constexpr uint32_t kActionFlags = 0x0050AF18U;
constexpr uint32_t kSelectionState = 0x0050674CU;
constexpr uint32_t kCursorBase = 0x00506700U;
constexpr std::array<uint32_t, 3> kCursorOffsets{0U, 8U, 0x10U};

TopScreenItemsSelectionPlan SelectionPlan(
    NativeA32Memory& memory, const TopScreenExtendedInputFrame& input) {
    uint32_t pageState = 0;
    if (!memory.Read32(kItemsPageState, &pageState)) return {};
    return ResolveTopScreenItemsSelectionBegin(
        {input.ZrPressed, input.ZlPressed, pageState});
}

bool DpadItemsConfigured(const TopScreenItemDispatchRuntime& runtime) {
    const auto set = [](uint8_t slot) { return slot != 0xFFU; };
    return std::any_of(runtime.ChildDpadItemSlots.begin(), runtime.ChildDpadItemSlots.end(), set) ||
        std::any_of(runtime.AdultDpadItemSlots.begin(), runtime.AdultDpadItemSlots.end(), set);
}

bool ReadDpadItems(NativeA32Memory& memory, uint32_t play,
                   const TopScreenItemDispatchRuntime& runtime,
                   TopScreenDpadItemGuestState* state) {
    return ReadTopScreenDpadItemGuestState(memory, play, runtime.ChildDpadItemSlots,
                                           runtime.AdultDpadItemSlots, state);
}

bool IsDpadItemScanExit(uint32_t pc) {
    return pc == kTopScreenDpadItemPressScanExit || pc == kTopScreenDpadItemHoldScanExit;
}

// Scan exits: Player_UpdateUpperBody keeps PlayState in r11 and ends a scan
// with r4 = 6 when no native button matched.
uint32_t ResolveDpadItemScanButton(
    uint32_t pc, NativeA32Memory& memory, const oot3d::recomp::a32::GuestState& state,
    const TopScreenExtendedInputFrame& input, const TopScreenItemDispatchRuntime& runtime) {
    const bool hold = pc == kTopScreenDpadItemHoldScanExit;
    const auto& lanes = hold ? input.DpadItemHeld : input.DpadItemPressed;
    if (state.r[4] != kTopScreenNativeScanNoButton || !DpadItemsConfigured(runtime) ||
        std::none_of(lanes.begin(), lanes.end(), [](bool lane) { return lane; }))
        return 0;
    TopScreenDpadItemGuestState dpad;
    if (!ReadDpadItems(memory, state.r[11], runtime, &dpad)) return 0;
    return ResolveTopScreenDpadItemButton(dpad, lanes, hold);
}

// Held item (0x0034D104) and mask (0x0034CF78) checks compare r0, the item
// action Link uses, with Y/X/I/II; a D-pad item with that action keeps it.
bool DpadItemKeepsAction(NativeA32Memory& memory, const oot3d::recomp::a32::GuestState& state,
                         const TopScreenItemDispatchRuntime& runtime) {
    if (!DpadItemsConfigured(runtime)) return false;
    TopScreenDpadItemGuestState dpad;
    return ReadDpadItems(memory, state.r[11], runtime, &dpad) &&
        TopScreenDpadItemsUseAction(memory, dpad, static_cast<uint8_t>(state.r[0]));
}

bool IsPendingDpadItemResolve(const oot3d::recomp::a32::GuestState& state,
                              const TopScreenItemDispatchRuntime& runtime) {
    return runtime.PendingDpadItemButton != 0 && state.r[1] == runtime.PendingDpadItemButton &&
        (state.r[14] == kTopScreenDpadItemPressResolverReturn ||
         state.r[14] == kTopScreenDpadItemHoldResolverReturn);
}

bool IsDpadItemButton(uint32_t button) {
    return button >= kTopScreenDpadItemButtonFirst && button < kTopScreenDpadItemButtonFirst + 4U;
}
} // namespace

std::span<const uint32_t> TopScreenItemDispatchEntries() {
    static const auto entries = [] {
        std::vector<uint32_t> result;
        for (const auto& query : TopScreenVerifiedItemQueryContracts())
            result.push_back(query.OriginalEntry);
        result.insert(result.end(), {kTopScreenSlotItemEntry,
            kTopScreenItemsUpdateEntry, kTopScreenItemsUpdateReturn,
            kTopScreenDpadItemPressScanExit, kTopScreenDpadItemHoldScanExit,
            kTopScreenDpadItemHeldPutAway, kTopScreenDpadItemMaskRemoval,
            kTopScreenBottleUpdateEntry});
        std::sort(result.begin(), result.end());
        return result;
    }();
    return entries;
}

bool IsTopScreenItemDispatchEntry(uint32_t pc) {
    const auto entries = TopScreenItemDispatchEntries();
    return std::binary_search(entries.begin(), entries.end(), pc);
}

bool ShouldObserveTopScreenItemDispatch(
    uint32_t pc, NativeA32Memory& memory,
    const oot3d::recomp::a32::GuestState& state,
    const TopScreenExtendedInputFrame& input,
    const TopScreenItemDispatchRuntime& runtime) {
    if (pc == kTopScreenItemsUpdateEntry)
        return state.r[14] == kTopScreenItemsUpdateReturn && runtime.PendingSelection == 0;
    if (pc == kTopScreenItemsUpdateReturn)
        return runtime.PendingSelection != 0;
    if (IsDpadItemScanExit(pc))
        return ResolveDpadItemScanButton(pc, memory, state, input, runtime) != 0;
    if (pc == kTopScreenDpadItemHeldPutAway || pc == kTopScreenDpadItemMaskRemoval)
        return DpadItemKeepsAction(memory, state, runtime);
    // Native code would index the four-entry button slots with 6..9.
    if (pc == kTopScreenBottleUpdateEntry)
        return IsDpadItemButton(state.r[2]);
    if (pc == kTopScreenSlotItemEntry && IsPendingDpadItemResolve(state, runtime))
        return true;
    if (pc == kTopScreenSlotItemEntry)
        return ResolveTopScreenDirectSlotItemGuest(
                   memory, static_cast<uint8_t>(state.r[1]), runtime.DirectItemId).has_value() ||
            (HasTopScreenSlotItemOverrideInput(input) &&
            ResolveTopScreenSlotItemOverrideGuest(
                memory, state.r[0], static_cast<uint8_t>(state.r[1]), input).has_value());
    return HasTopScreenItemQueryOverrideInput(input) &&
        ResolveTopScreenItemQueryGuest(memory, pc, input).has_value();
}

bool ExecuteTopScreenItemDispatch(
    uint32_t pc, NativeA32Memory& memory,
    oot3d::recomp::a32::GuestState& state,
    const TopScreenExtendedInputFrame& input,
    TopScreenItemDispatchRuntime& runtime) {
    if (pc == kTopScreenItemsUpdateEntry) {
        if (state.r[14] != kTopScreenItemsUpdateReturn || runtime.PendingSelection != 0)
            return false;
        ++runtime.SelectionUpdates;
        // The official assignment wrapper has its own held-to-pressed sampler.
        const uint32_t held = uint32_t(input.ZrHeld) | (uint32_t(input.ZlHeld) << 1);
        const uint32_t presses = uint32_t(input.ZrPressed) | (uint32_t(input.ZlPressed) << 1);
        const auto edges = (held & ~runtime.PreviousSelectionButtons) |
                           (presses & ~runtime.PreviousSelectionPressed);
        runtime.PreviousSelectionButtons = held;
        runtime.PreviousSelectionPressed = presses;
        auto selectionInput = input;
        selectionInput.ZrPressed = (edges & 1U) != 0;
        selectionInput.ZlPressed = (edges & 2U) != 0;
        const auto plan = SelectionPlan(memory, selectionInput);
        uint32_t flags = 0;
        if (!plan.Active || !memory.Read32(kActionFlags, &flags) ||
            !memory.Write32(kActionFlags, flags | 0x400U)) return false;
        runtime.PendingSelection = plan.Selection;
        ++runtime.SelectionBegins;
        state.r[15] = pc;
        return true;
    }
    if (pc == kTopScreenItemsUpdateReturn) {
        uint32_t flags = 0, nativeSelection = 0, selection = 0;
        uint16_t cursorY = 0;
        if (runtime.PendingSelection == 0 ||
            !memory.Read32(kActionFlags, &flags) ||
            !memory.Read32(kSelectionState, &nativeSelection) ||
            !memory.IsWritable(kActionFlags, sizeof(flags))) return false;
        const bool complete = CompleteTopScreenItemsSelection(
            runtime.PendingSelection, nativeSelection, &selection, &cursorY);
        // Validate every destination before changing native state. A declined
        // hook must not hand partially modified memory back to compiled code.
        if (complete) {
            if (!memory.IsWritable(kSelectionState, sizeof(selection))) return false;
            for (const auto offset : kCursorOffsets)
                if (!memory.IsWritable(kCursorBase + offset, 4U)) return false;
        }
        memory.Write32(kActionFlags, flags & ~0x400U);
        if (complete) {
            memory.Write32(kSelectionState, selection);
            for (const auto offset : kCursorOffsets) {
                memory.Write16(kCursorBase + offset, 0x010FU);
                memory.Write16(kCursorBase + offset + 2U, cursorY);
            }
            ++runtime.SelectionCompletions;
        }
        runtime.PendingSelection = 0;
        // Resume the original compiled return block once. pc+4 is an internal
        // NOP, not a valid entry in the precompiled module.
        state.r[15] = pc;
        return true;
    }
    if (IsDpadItemScanExit(pc)) {
        const auto button = ResolveDpadItemScanButton(pc, memory, state, input, runtime);
        if (button == 0) return false;
        // Resumed in place: the native resolver call then asks for 6..9.
        state.r[4] = button;
        runtime.PendingDpadItemButton = button;
        ++runtime.DpadItemButtons;
        state.r[15] = pc;
        return true;
    }
    if (pc == kTopScreenDpadItemHeldPutAway || pc == kTopScreenDpadItemMaskRemoval) {
        if (!DpadItemKeepsAction(memory, state, runtime)) return false;
        ++runtime.DpadItemsKept;
        state.r[15] = pc == kTopScreenDpadItemHeldPutAway ? kTopScreenDpadItemHeldKept
                                                          : kTopScreenDpadItemMaskKept;
        return true;
    }
    if (pc == kTopScreenBottleUpdateEntry) {
        if (!IsDpadItemButton(state.r[2])) return false;
        // Inventory_UpdateBottleItem(play r0, item r1, button r2). A bottle
        // that no longer matches is left as it is, as 3.2.2 does.
        TopScreenDpadItemGuestState dpad;
        if (ReadDpadItems(memory, state.r[0], runtime, &dpad)) {
            const auto update = ResolveTopScreenDpadBottleUpdate(
                dpad, state.r[2], static_cast<uint8_t>(state.r[1]));
            if (update && ApplyTopScreenDpadBottleUpdate(memory, state.r[0], *update))
                ++runtime.DpadBottleUpdates;
        }
        state.r[15] = state.r[14];
        return true;
    }
    if (pc == kTopScreenSlotItemEntry && IsPendingDpadItemResolve(state, runtime)) {
        ++runtime.SlotEntries;
        const auto button = state.r[1];
        runtime.PendingDpadItemButton = 0;
        TopScreenDpadItemGuestState dpad;
        state.r[0] = ReadDpadItems(memory, state.r[0], runtime, &dpad)
            ? ResolveTopScreenDpadItem(dpad, button - kTopScreenDpadItemButtonFirst)
            : kTopScreenNoItem;
        ++runtime.DpadItemResolves;
        state.r[15] = state.r[14];
        return true;
    }
    if (pc == kTopScreenSlotItemEntry) {
        ++runtime.SlotEntries;
        if (runtime.DirectItemId == 0 && !HasTopScreenSlotItemOverrideInput(input)) return false;
        ++runtime.SlotAttempts;
        auto resolved = ResolveTopScreenDirectSlotItemGuest(
            memory, static_cast<uint8_t>(state.r[1]), runtime.DirectItemId);
        if (!resolved) resolved = ResolveTopScreenSlotItemOverrideGuest(
            memory, state.r[0], static_cast<uint8_t>(state.r[1]), input);
        if (!resolved.has_value()) {
            ++runtime.SlotNativeFallbacks;
            return false;
        }
        ++runtime.SlotOverrides;
        state.r[0] = *resolved;
        state.r[15] = state.r[14];
        return true;
    }
    for (const auto& query : TopScreenVerifiedItemQueryContracts()) {
        if (query.OriginalEntry != pc) continue;
        ++runtime.QueryEntries;
        if (!HasTopScreenItemQueryOverrideInput(input)) return false;
        const auto index = static_cast<size_t>(query.Query);
        ++runtime.QueryCalls[index];
        TopScreenItemQueryState observed;
        const auto resolved = ResolveTopScreenItemQueryGuest(memory, pc, input,
            runtime.TraceEnabled ? &observed : nullptr);
        if (!resolved.has_value()) return false;
        if (runtime.TraceEnabled && runtime.QueryTrace.size() < 128U)
            runtime.QueryTrace.push_back({pc, state.r[14], observed.SuppressionFlags,
                observed.NativeResults[index], *resolved, input});
        runtime.QueryTrueResults[index] += *resolved ? 1U : 0U;
        state.r[0] = *resolved ? 1U : 0U;
        state.r[15] = state.r[14];
        return true;
    }
    return false;
}
} // namespace Oot3dNativeGame
