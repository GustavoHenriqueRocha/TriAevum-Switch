#include "oot3d_video_settings_panel.h"
#include "oot3d_video_settings.h"
#include "oot3d_fps_counter.h"
#include "fast/backends/gfx_deko3d_lighting.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"
#include "fast/oot3d/grass_switch_profile.h"
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
// Texture pack size cap, as a multiple of the original texture.
struct PackScaleOption {
  uint32_t Scale;
  const char *Label;
};
constexpr std::array<PackScaleOption, 5> kPackScales{{{1U, "1x (original size)"},
                                                      {2U, "2x (default)"},
                                                      {3U, "3x"},
                                                      {4U, "4x (full 1080p pack)"},
                                                      {0U, "No limit"}}};

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
      : mConfig(std::move(config)), mBoot(VideoSettings::Load(mConfig)), mSettings(mBoot),
        mPack(::Oot3d::Renderer::TexturePackSettings::Load(mConfig)),
        mLighting(Fast::Deko3d::LoadLightingMode(mConfig)) {
    std::error_code error;
    PerfOverlayEnabled().store(std::filesystem::exists(mConfig / kPerfOverlayConfigName, error),
                               std::memory_order_relaxed);
    mPackFound = PackFolderExists();
    mGrassBoot = mGrass = Fast::Oot3d::ReadGrassSwitchMode(mConfig);
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
    DrawTexturePack();
    DrawGrass();
    DrawLighting();
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

  // HD textures (config/texture_pack) and their size cap
  // (config/texture_pack_scale): saved, and applied by the deko3d renderer at
  // the start of its next frame.
  void DrawTexturePack() {
    ImGui::Separator();
    bool changed = ImGui::Checkbox("HD textures (texture pack)", &mPack.Enabled);
    const char *current = "Custom";
    for (const auto &option : kPackScales)
      if (option.Scale == mPack.MaxScale) current = option.Label;
    if (ImGui::BeginCombo("HD texture size", current)) {
      for (const auto &option : kPackScales) {
        const bool selected = option.Scale == mPack.MaxScale;
        if (ImGui::Selectable(option.Label, selected)) {
          changed |= !selected;
          mPack.MaxScale = option.Scale;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (changed) {
      mPackError = mPack.Save(mConfig);
      ::Oot3d::Renderer::LiveTexturePackSettings().Publish(mPack);
      mPackFound = PackFolderExists();
    }
    if (!mPackFound)
      ImGui::TextWrapped("No texture pack found in %s.",
                         ::Oot3d::Renderer::TexturePackDisplayFolder(mPack.Folder).c_str());
    if (!mPackError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mPackError.c_str());
    ImGui::TextDisabled("Applies at once. Larger sizes use more memory and GPU time.");
  }

  // config/grass (SWITCH_PORT.md, "Grass on deko3d"): off, measure only
  // (placements and the grass: line, nothing drawn) or drawn. Read at start,
  // so it applies on the next start, like the frame rate.
  void DrawGrass() {
    ImGui::Separator();
    using Fast::Oot3d::GrassSwitchMode;
    const char *const modes[] = {"Off (default)", "Measure only (log, not drawn)",
                                 "On (drawn)"};
    int mode = static_cast<int>(mGrass);
    if (ImGui::Combo("Grass", &mode, modes, 3)) {
      mGrass = static_cast<GrassSwitchMode>(mode);
      mGrassError = !Fast::Oot3d::SaveGrassSwitchMode(mConfig, mGrass);
    }
    if (mGrassError)
      ImGui::TextWrapped("Could not write config/grass.");
    else if (mGrass != mGrassBoot)
      ImGui::TextUnformatted("Saved. Close and reopen the game to apply.");
    ImGui::TextDisabled("deko3d renderer only. Drawn grass costs GPU time; "
                        "with 30 FPS it reaches farther.");
  }

  // config/lighting: native (no file), toon bands or trimmed per-pixel
  // lighting; saved, and applied by the deko3d renderer at the start of its
  // next frame. A shader the pack lacks keeps its native look.
  void DrawLighting() {
    ImGui::Separator();
    const char *const modes[] = {"Native (default)", "Toon bands", "Per-pixel (for 30 FPS)"};
    int mode = static_cast<int>(mLighting);
    if (ImGui::Combo("Lighting", &mode, modes, 3)) {
      mLighting = static_cast<Fast::Deko3d::LightingMode>(mode);
      mLightingError = Fast::Deko3d::SaveLightingMode(mConfig, mLighting);
      Fast::Deko3d::LiveLightingSettings().Publish(mLighting);
    }
    if (!mLightingError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mLightingError.c_str());
    ImGui::TextDisabled("Applies at once. Per-pixel costs more GPU time: use it at 30 FPS.");
  }

  bool PackFolderExists() const {
    std::error_code error;
    return std::filesystem::is_directory(mPack.Folder, error);
  }

  std::filesystem::path mConfig;
  VideoSettings mBoot;
  VideoSettings mSettings;
  std::string mError;
  std::string mOverlayError;
  ::Oot3d::Renderer::TexturePackSettings mPack;
  bool mPackFound = false;
  std::string mPackError;
  Fast::Oot3d::GrassSwitchMode mGrassBoot = Fast::Oot3d::GrassSwitchMode::Off;
  Fast::Oot3d::GrassSwitchMode mGrass = Fast::Oot3d::GrassSwitchMode::Off;
  bool mGrassError = false;
  Fast::Deko3d::LightingMode mLighting = Fast::Deko3d::LightingMode::Off;
  std::string mLightingError;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config) {
  return std::make_shared<VideoSettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
