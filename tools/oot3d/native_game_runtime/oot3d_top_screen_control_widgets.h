#pragma once

#include "oot3d_top_screen_config.h"
#include "oot3d_control_settings_widgets.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <iterator>

namespace Oot3dNativeGame {

// Controls owns these fields, not the HUD layout stored in the same JSON file.
inline void CopyTopScreenControlSettings(const TopScreenUiConfig& source, TopScreenUiConfig& target) {
    target.SelectAction = source.SelectAction;
    target.ExitItemsToSaveScreen = source.ExitItemsToSaveScreen;
    target.CameraZoomPercent = source.CameraZoomPercent;
    target.CameraFovPercent = source.CameraFovPercent;
    target.ChildDpad = source.ChildDpad;
    target.AdultDpad = source.AdultDpad;
    target.ChildDpadItemSlots = source.ChildDpadItemSlots;
    target.AdultDpadItemSlots = source.AdultDpadItemSlots;
    target.FreeCameraEnabled = source.FreeCameraEnabled;
    target.FreeCameraSpeedLevel = source.FreeCameraSpeedLevel;
    target.FreeCameraSmoothing = source.FreeCameraSmoothing;
    target.FreeCameraInvertX = source.FreeCameraInvertX;
    target.FreeCameraInvertY = source.FreeCameraInvertY;
    target.CStickAimSpeedLevel = source.CStickAimSpeedLevel;
    target.CStickAimInvertX = source.CStickAimInvertX;
    target.CStickAimInvertY = source.CStickAimInvertY;
}

inline bool DrawTopScreenCameraBehavior(TopScreenUiConfig& config) {
    bool changed = ImGui::Checkbox("Enabled##freecam", &config.FreeCameraEnabled);
    int speed = config.FreeCameraSpeedLevel;
    if (ControlWidgets::SliderInt("Speed##freecam", &speed, 0, 6)) {
        config.FreeCameraSpeedLevel = static_cast<uint8_t>(speed);
        changed = true;
    }
    changed |= ImGui::Checkbox("Invert X##freecam", &config.FreeCameraInvertX);
    changed |= ImGui::Checkbox("Invert Y##freecam", &config.FreeCameraInvertY);
    int zoom = config.CameraZoomPercent, fov = config.CameraFovPercent;
    if (ControlWidgets::SliderInt("Camera zoom", &zoom, kTopScreenCameraZoomMinimum, kTopScreenCameraZoomMaximum, "%d%%")) {
        config.CameraZoomPercent = static_cast<uint8_t>(zoom / 5 * 5);
        changed = true;
    }
    if (ControlWidgets::SliderInt("Field of view", &fov, kTopScreenCameraFovMinimum, kTopScreenCameraFovMaximum, "%d%%")) {
        config.CameraFovPercent = static_cast<uint8_t>(fov / 5 * 5);
        changed = true;
    }
    return changed;
}

inline bool DrawTopScreenStickAiming(TopScreenUiConfig& config) {
    ImGui::SeparatorText("C-stick aiming");
    bool changed = false;
    int speed = config.CStickAimSpeedLevel;
    if (ControlWidgets::SliderInt("Speed##aim", &speed, 0, 6)) {
        config.CStickAimSpeedLevel = static_cast<uint8_t>(speed);
        changed = true;
    }
    constexpr const char* smoothingLabels[]{"Off", "Light", "Medium", "Default", "Heavy"};
    int smoothing = static_cast<int>(config.FreeCameraSmoothing);
    if (ControlWidgets::Combo("Smoothing##aim", &smoothing, smoothingLabels, 5)) {
        config.FreeCameraSmoothing = static_cast<TopScreenFreeCameraSmoothing>(smoothing);
        changed = true;
    }
    changed |= ImGui::Checkbox("Invert X##aim", &config.CStickAimInvertX);
    changed |= ImGui::Checkbox("Invert Y##aim", &config.CStickAimInvertY);
    return changed;
}

inline bool DrawTopScreenActionBindings(TopScreenUiConfig& config) {
    constexpr const char* labels[]{"None", "View", "Ocarina", "Iron Boots", "Hover Boots",
        "Item ZR", "Item ZL", "Sword toggle", "All boots", "Minimap toggle", "Boomerang",
        "Slingshot", "Tunic toggle", "Shield toggle", "Item"};
    constexpr const char* directions[]{"Up", "Down", "Left", "Right"};
    constexpr const char* itemDirections[]{"Up item", "Down item", "Left item", "Right item"};
    // None, then kTopScreenDpadItemSlots in order.
    constexpr const char* itemLabels[]{"None", "Deku Sticks", "Deku Nuts", "Bombs", "Bow",
        "Din's Fire", "Slingshot", "Ocarina", "Bombchus", "Hookshot", "Farore's Wind",
        "Boomerang", "Lens of Truth", "Magic Beans", "Megaton Hammer", "Nayru's Love",
        "Bottle 1", "Bottle 2", "Bottle 3", "Bottle 4", "Adult trade item",
        "Child trade item or mask"};
    static_assert(std::size(itemLabels) == kTopScreenDpadItemSlots.size() + 1);
    bool changed = false;
    const auto draw = [&](const char* id, auto& bindings, auto& slots) {
        ImGui::SeparatorText(id);
        ImGui::PushID(id);
        for (size_t i = 0; i < bindings.size(); ++i) {
            int action = static_cast<int>(bindings[i]);
            if (ControlWidgets::Combo(directions[i], &action, labels,
                                      static_cast<int>(std::size(labels)))) {
                bindings[i] = static_cast<TopScreenDpadAction>(action);
                if (bindings[i] != TopScreenDpadAction::Item)
                    slots[i] = kTopScreenDpadItemSlotNone;
                changed = true;
            }
            if (bindings[i] != TopScreenDpadAction::Item) continue;
            const auto found = std::find(kTopScreenDpadItemSlots.begin(),
                                         kTopScreenDpadItemSlots.end(), slots[i]);
            int item = found == kTopScreenDpadItemSlots.end()
                ? 0 : static_cast<int>(found - kTopScreenDpadItemSlots.begin()) + 1;
            if (ControlWidgets::Combo(itemDirections[i], &item, itemLabels,
                                      static_cast<int>(std::size(itemLabels)))) {
                slots[i] = item == 0 ? kTopScreenDpadItemSlotNone
                                     : kTopScreenDpadItemSlots[static_cast<size_t>(item - 1)];
                changed = true;
            }
        }
        ImGui::PopID();
    };
    draw("Child D-pad", config.ChildDpad, config.ChildDpadItemSlots);
    draw("Adult D-pad", config.AdultDpad, config.AdultDpadItemSlots);
    int select = static_cast<int>(config.SelectAction);
    constexpr const char* actions[]{"Open Save screen", "Toggle minimap"};
    if (ControlWidgets::Combo("SELECT action", &select, actions, 2)) {
        config.SelectAction = static_cast<TopScreenSelectAction>(select);
        changed = true;
    }
    changed |= ImGui::Checkbox("B exits Items to Save screen", &config.ExitItemsToSaveScreen);
    return changed;
}

} // namespace Oot3dNativeGame
