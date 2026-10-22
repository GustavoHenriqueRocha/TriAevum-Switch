#pragma once
#include "fast/oot3d/graphics_settings_window.h"
#include <filesystem>
#include <memory>
namespace Oot3dNativeGame {
// Debug menu tab "Video": resolution (dock and handheld) and frame rate,
// saved to `config` (oot3d_video_settings.h) for the next start.
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config);
}
