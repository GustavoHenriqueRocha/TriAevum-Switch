#pragma once
#include "fast/oot3d/graphics_settings_window.h"
#include <filesystem>
#include <memory>
namespace Oot3dNativeGame {
// Debug menu tab "Video": resolution (dock and handheld) and frame rate,
// saved to `config` (oot3d_video_settings.h) for the next start, the
// performance overlay switch (config/fps_counter, applies at once), the
// GPU clock (config/gpu_performance, oot3d_switch_gpu_performance.h) and the
// actor draw distance (config/draw_distance, oot3d_draw_distance_config.h).
// Loads the overlay flag and the draw distance and applies the GPU clock when
// created.
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config);
}
