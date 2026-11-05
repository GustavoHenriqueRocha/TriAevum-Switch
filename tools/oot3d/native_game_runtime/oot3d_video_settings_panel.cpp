#include "oot3d_video_settings_panel.h"
#include "oot3d_video_settings.h"
#include "oot3d_fps_counter.h"
#include <imgui.h>
#include <array>
#include <cstdio>
#include <span>
#include <string>
namespace Oot3dNativeGame {
namespace {
// The scene image is the 400x240 top screen times the scale.
struct ScaleOption {
  float Scale;
  const char *Label;
};
constexpr std::array<ScaleOption, 6> kDocked{{{2.0F, "2x (800x480)"},
                                              {2.5F, "2,5x (1000x600)"},
                                              {3.0F, "3x (720p)"},
                                              {3.5F, "3,5x (1400x840)"},
                                              {4.0F, "4x (1600x960)"},
                                              {4.5F, "4,5x (1080p)"}}};
constexpr std::array<ScaleOption, 5> kHandheld{{{1.0F, "1x (400x240)"},
                                                {1.5F, "1,5x (600x360)"},
                                                {2.0F, "2x (800x480)"},
                                                {2.5F, "2,5x (1000x600)"},
                                                {3.0F, "3x (720p)"}}};

bool ScaleCombo(const char *label, float &value, std::span<const ScaleOption> options) {
  std::string current;
  for (const auto &option : options)
    if (option.Scale == value) current = option.Label;
  if (current.empty()) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2fx", value);
    current = text;
  }
  bool changed = false;
  if (ImGui::BeginCombo(label, current.c_str())) {
    for (const auto &option : options) {
      const bool selected = option.Scale == value;
      if (ImGui::Selectable(option.Label, selected)) {
        value = option.Scale;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

class VideoSettingsPanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit VideoSettingsPanel(std::filesystem::path config)
      : mConfig(std::move(config)), mBoot(VideoSettings::Load(mConfig)), mSettings(mBoot) {
    std::error_code error;
    PerfOverlayEnabled().store(std::filesystem::exists(mConfig / kPerfOverlayConfigName, error),
                               std::memory_order_relaxed);
  }
  const char *Label() const noexcept override { return "Video"; }
  void Draw() override {
    bool changed = ScaleCombo("Docked resolution", mSettings.DockedScale, kDocked);
    changed |= ScaleCombo("Handheld resolution", mSettings.HandheldScale, kHandheld);
    const char *const rates[] = {"30 FPS", "60 FPS (interpolated)"};
    int rate = mSettings.Fps60 ? 1 : 0;
    if (ImGui::Combo("FPS", &rate, rates, 2)) {
      mSettings.Fps60 = rate == 1;
      changed = true;
    }
    const char *const upscalers[] = {"FSR + sharpening (default)", "FSR without sharpening",
                                     "Linear (lightest)"};
    int upscaler = static_cast<int>(mSettings.Upscaler);
    if (ImGui::Combo("Upscaling", &upscaler, upscalers, 3)) {
      mSettings.Upscaler = static_cast<VideoSettings::Upscale>(upscaler);
      changed = true;
    }
    const char *const filters[] = {"Off", "4x (default)"};
    int anisotropy = mSettings.Anisotropy ? 1 : 0;
    if (ImGui::Combo("Anisotropic filter", &anisotropy, filters, 2)) {
      mSettings.Anisotropy = anisotropy == 1;
      changed = true;
    }
    if (changed) mError = mSettings.Save(mConfig);
    if (!mError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mError.c_str());
    else if (mSettings != mBoot)
      ImGui::TextUnformatted("Saved. Close and reopen the game to apply.");
    else
      ImGui::TextDisabled("Values in use.");
    // Applies at once (RunFrame reads the flag every frame); the file keeps
    // it for the next start.
    ImGui::Separator();
    bool overlay = PerfOverlayEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Performance overlay (FPS, speed, clocks)", &overlay)) {
      PerfOverlayEnabled().store(overlay, std::memory_order_relaxed);
      mOverlayError = SaveOverlay(overlay) ? "" : kPerfOverlayConfigName;
    }
    if (!mOverlayError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mOverlayError.c_str());
  }

private:
  // config/fps_counter: present = overlay on.
  bool SaveOverlay(bool enabled) const {
    std::error_code error;
    if (!enabled) {
      std::filesystem::remove(mConfig / kPerfOverlayConfigName, error);
      return !error;
    }
    std::filesystem::create_directories(mConfig, error);
    std::ofstream file(mConfig / kPerfOverlayConfigName, std::ios::trunc);
    return static_cast<bool>(file);
  }

  std::filesystem::path mConfig;
  VideoSettings mBoot;
  VideoSettings mSettings;
  std::string mError;
  std::string mOverlayError;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config) {
  return std::make_shared<VideoSettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
