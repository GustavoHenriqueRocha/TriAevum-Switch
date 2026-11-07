#include "oot3d_video_settings_panel.h"
#include "oot3d_video_settings.h"
#include "oot3d_fps_counter.h"
#include "oot3d_native_launch_frame_rate.h"
#include "oot3d_switch_gpu_performance.h"
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/oot3d/graphics_settings_runtime.h"
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
        mPack(::Oot3d::Renderer::TexturePackSettings::Load(mConfig)) {
    std::error_code error;
    PerfOverlayEnabled().store(std::filesystem::exists(mConfig / kPerfOverlayConfigName, error),
                               std::memory_order_relaxed);
    mPackFound = PackFolderExists();
    mGrassBoot = mGrass = Fast::Oot3d::ReadGrassSwitchMode(mConfig);
    mGrassDistance = Fast::Oot3d::ReadGrassSwitchDistance(mConfig);
    mGrassLod = Fast::Oot3d::ReadGrassSwitchLod(mConfig);
    mGrassFill = Fast::Oot3d::ReadGrassSwitchFill(mConfig);
    // config/gpu_performance applies at start (the panel is created once, at
    // window setup); Standard opens no apm session.
    mGpuHigh = ReadGpuPerformanceHigh(mConfig);
    if (mGpuHigh) mGpuStatus = ApplyGpuPerformance(true);
  }
  const char *Label() const noexcept override { return "Video"; }
  void Draw() override {
    bool changed = ScaleCombo("Docked resolution", mSettings.DockedScale, kDocked);
    changed |= ScaleCombo("Handheld resolution", mSettings.HandheldScale, kHandheld);
    const char *const rates[] = {"30 FPS", "60 FPS (interpolated)"};
    int rate = mSettings.Fps60 ? 1 : 0;
    bool rateChanged = false;
    if (ImGui::Combo("FPS", &rate, rates, 2)) {
      mSettings.Fps60 = rate == 1;
      changed = rateChanged = true;
    }
    const char *const drsModes[] = {"Dynamic (default)", "Full"};
    int drsMode = mSettings.FullResolution60 ? 1 : 0;
    bool drsChanged = false;
    if (ImGui::Combo("Resolution at 60 FPS", &drsMode, drsModes, 2)) {
      mSettings.FullResolution60 = drsMode == 1;
      changed = drsChanged = true;
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
    // Both rates simulate at 30 Hz: the game applies the new one at its next
    // frame (oot3d_native_launch_frame_rate.h); config/fps60 keeps it.
    if (rateChanged) LiveFrameRate().Publish(mSettings.Fps60);
    // config/drs_60 likewise: the render thread reconfigures its pacing at
    // its next frame (oot3d_native_dynamic_resolution.h).
    if (drsChanged)
      LiveDynamicResolutionSixty().Publish(mSettings.FullResolution60
                                               ? DynamicResolutionSixtyMode::Full
                                               : DynamicResolutionSixtyMode::Dynamic);
    // The rest (resolution, upscaling, filter) is read at start.
    VideoSettings atStart = mSettings;
    atStart.Fps60 = mBoot.Fps60;
    atStart.FullResolution60 = mBoot.FullResolution60;
    if (!mError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mError.c_str());
    else if (atStart != mBoot)
      ImGui::TextUnformatted("Saved. Close and reopen the game to apply the resolution, "
                             "upscaling and filter.");
    else
      ImGui::TextDisabled("Values in use.");
    ImGui::TextDisabled("FPS applies at once; the game keeps 30 steps a second either way.");
    ImGui::TextDisabled("Resolution at 60 FPS applies at once. Dynamic lowers the resolution in busy "
                        "rooms to keep 60 FPS. Full keeps the full resolution (handheld 3x, docked "
                        "the docked setting) and can drop the real frame rate in busy rooms at the "
                        "Standard GPU clock; GPU clock High helps.");
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
    DrawGpuClock();
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

  // config/gpu_performance (oot3d_switch_gpu_performance.h): the official
  // handheld configuration with GPU 460.8 MHz, applied at once and at start.
  void DrawGpuClock() {
    ImGui::Separator();
    const char *const clocks[] = {"Standard (307 MHz)", "High (460.8 MHz, more battery)"};
    int clock = mGpuHigh ? 1 : 0;
    if (ImGui::Combo("GPU clock", &clock, clocks, 2)) {
      mGpuHigh = clock == 1;
      mGpuSaveError = !SaveGpuPerformanceHigh(mConfig, mGpuHigh);
      mGpuStatus = ApplyGpuPerformance(mGpuHigh);
    }
    if (mGpuSaveError)
      ImGui::TextWrapped("Could not write config/%s.", kGpuPerformanceConfigName);
    if (mGpuStatus.Attempted && !mGpuStatus.Available)
      ImGui::TextWrapped("Unavailable: %s refused (0x%X); the clock is unchanged.",
                         mGpuStatus.FailedStep, mGpuStatus.Result);
    ImGui::TextDisabled("Handheld only (docked already runs the GPU at 768 MHz). High is Nintendo's "
                        "official handheld mode with a faster GPU: more frames in busy rooms, shorter "
                        "battery life. Applies at once; sys-clk overrides it.");
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
  // so it applies on the next start (its budgets follow the FPS at once).
  // config/grass_distance: how far it reaches, applied at once (the
  // renderer reads the published choice at its next frame).
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
    const char *const distances[] = {"Near (default)", "Far", "Whole area"};
    int distance = static_cast<int>(mGrassDistance);
    if (ImGui::Combo("Grass distance", &distance, distances, 3)) {
      mGrassDistance = static_cast<Fast::Oot3d::GrassSwitchDistance>(distance);
      mGrassDistanceError = !Fast::Oot3d::SaveGrassSwitchDistance(mConfig, mGrassDistance);
      Fast::Oot3d::PublishGrassSwitchDistance(mGrassDistance);
    }
    if (mGrassDistanceError)
      ImGui::TextWrapped("Could not write config/grass_distance.");
    ImGui::TextDisabled("Applies at once. Grass costs GPU time per tuft drawn (about 0.5 ms per 1,000 "
                        "tufts at 3.0x resolution). Near: full density to 10 m (16 m at 30 FPS), thinner "
                        "beyond, at most 8,000 tufts (22,000 at 30 FPS). Far: full density to twice that, "
                        "about twice the GPU time of Near. Whole area: every tuft of the area, no "
                        "thinning (up to 65,000 drawn): up to 30 ms per frame in large fields such as "
                        "Hyrule Field, more than a 60 FPS frame has. Use 30 FPS with Whole area.");
    // config/grass_lod (SWITCH_PORT.md "Grass on deko3d: cost per tuft"):
    // applied at once (the renderer's next cull takes the published choice).
    const char *const details[] = {"Levels (fewer, wider blades far away; default)",
                                   "Every blade (test.9)"};
    int detail = mGrassLod ? 0 : 1;
    if (ImGui::Combo("Grass detail", &detail, details, 2)) {
      mGrassLod = detail == 0;
      mGrassLodError = !Fast::Oot3d::SaveGrassSwitchLod(mConfig, mGrassLod);
      Fast::Oot3d::PublishGrassSwitchLod(mGrassLod);
    }
    if (mGrassLodError)
      ImGui::TextWrapped("Could not write config/grass_lod.");
    ImGui::TextDisabled("Applies at once. Levels: each tuft's wind, light and colour once per frame instead of "
                        "for every vertex, and far tufts drawn with 3, then 1 wider blade covering the same "
                        "ground. Every blade: the grass of test.9, for comparison.");
    // config/grass_fill (SWITCH_PORT.md "Grass on deko3d: nothing grows near
    // Link"): applied at once (the renderer's next cull takes the choice).
    const char *const fills[] = {"Fixed (nothing grows near Link; default)", "Grows in (test.9)"};
    int fill = mGrassFill ? 0 : 1;
    if (ImGui::Combo("Grass near Link", &fill, fills, 2)) {
      mGrassFill = fill == 0;
      mGrassFillError = !Fast::Oot3d::SaveGrassSwitchFill(mConfig, mGrassFill);
      Fast::Oot3d::PublishGrassSwitchFill(mGrassFill);
    }
    if (mGrassFillError)
      ImGui::TextWrapped("Could not write config/grass_fill.");
    ImGui::TextDisabled("Applies at once. Fixed: the same extra tufts around Link wherever he walks, at full "
                        "height; farther away they narrow out. Grows in: the grass of test.9, whose tufts "
                        "grew out of the ground as he came near, for comparison.");
  }

  // The lighting model (graphics settings: Modern = per-pixel, toon bands
  // or native), saved to config/lighting and applied at once; the renderer
  // and Lighting > Scene lights read the same settings. A shader the pack
  // lacks keeps its native look.
  void DrawLighting() {
    ImGui::Separator();
    auto &graphics = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
    const auto current = graphics.Snapshot();
    const bool modernAvailable =
        graphics.Capabilities().Get(Fast::Oot3d::GraphicsCapability::NativeSurfaceLighting).Available;
    const char *const modes[] = {"Native (default)", "Toon bands", "Per-pixel (Modern model)"};
    int mode = static_cast<int>(Fast::Deko3d::LightingModeOf(current));
    if (ImGui::Combo("Lighting", &mode, modes, 3)) {
      const auto chosen = static_cast<Fast::Deko3d::LightingMode>(mode);
      const auto validation = graphics.Apply(Fast::Deko3d::WithLightingMode(current, chosen), false);
      const auto applied = Fast::Deko3d::LightingModeOf(graphics.Snapshot());
      mLightingError = Fast::Deko3d::SaveLightingMode(mConfig, applied);
      if (applied != chosen && !validation.Issues.empty()) mLightingIssue = validation.Issues.front().Message;
      else mLightingIssue.clear();
    }
    if (!mLightingError.empty())
      ImGui::TextWrapped("Could not write config/%s.", mLightingError.c_str());
    if (!mLightingIssue.empty())
      ImGui::TextWrapped("Not applied: %s", mLightingIssue.c_str());
    else if (!modernAvailable)
      ImGui::TextWrapped("Per-pixel needs the deko3d renderer and its shadercache/deko3d.pack.");
    ImGui::TextDisabled("deko3d renderer. Applies at once, at 30 or 60 FPS. Per-pixel is the Modern lighting "
                        "model of Lighting > Scene lights; it costs about 1-2.5 ms of GPU per frame: at 60 FPS "
                        "busy rooms may drop frames.");
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
  Fast::Oot3d::GrassSwitchDistance mGrassDistance = Fast::Oot3d::GrassSwitchDistance::Near;
  bool mGrassDistanceError = false;
  bool mGrassLod = true;
  bool mGrassLodError = false;
  bool mGrassFill = true;
  bool mGrassFillError = false;
  std::string mLightingError;
  std::string mLightingIssue;
  bool mGpuHigh = false;
  bool mGpuSaveError = false;
  GpuPerformanceStatus mGpuStatus;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config) {
  return std::make_shared<VideoSettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
