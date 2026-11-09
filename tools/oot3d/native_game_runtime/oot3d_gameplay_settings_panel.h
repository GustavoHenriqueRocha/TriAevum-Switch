#pragma once
#include "fast/oot3d/graphics_settings_window.h"
#include <filesystem>
#include <memory>
namespace Oot3dNativeGame {
// L3+R3 > Game > Gameplay: options that change the game's own rules
// (oot3d_landing_roll.h) and, on the Switch, its files (RomFS mods,
// oot3d_romfs_mods.h). `config` is the folder of the config/ files.
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateGameplaySettingsPanel(std::filesystem::path config);
}
