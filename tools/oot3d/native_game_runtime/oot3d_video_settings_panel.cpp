#include "oot3d_video_settings_panel.h"
#include "oot3d_video_settings.h"
#include "oot3d_fps_counter.h"
#include "oot3d_native_launch_frame_rate.h"
#include "oot3d_draw_distance_config.h"
#include "oot3d_switch_performance.h"
#include "oot3d_simple_shadow.h"
#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/oot3d/config_writer.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"
#include "fast/oot3d/grass_switch_profile.h"
#include <imgui.h>
#include <array>
#include <atomic>
#include <fstream>
#include <memory>
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
      : mConfig(std::move(config)), mBoot(VideoSettings::Load(mConfig)), mSettings(mBoot),
        mPack(::Oot3d::Renderer::TexturePackSettings::Load(mConfig)) {
    std::error_code error;
    PerfOverlayEnabled().store(std::filesystem::exists(mConfig / kPerfOverlayConfigName, error),
                               std::memory_order_relaxed);
    mPackFound->store(PackFolderExists(mPack.Folder), std::memory_order_relaxed);
    mGrassBoot = mGrass = Fast::Oot3d::ReadGrassSwitchMode(mConfig);
    mGrassDistance = Fast::Oot3d::ReadGrassSwitchDistance(mConfig);
    mGrassLod = Fast::Oot3d::ReadGrassSwitchLod(mConfig);
    mGrassFill = Fast::Oot3d::ReadGrassSwitchFill(mConfig);
#if defined(__SWITCH__)
    SimpleShadowEnabled().store(ReadSimpleShadowEnabled(mConfig), std::memory_order_relaxed);
#endif
    // config/cpu_performance, gpu_performance and memory_performance apply at
    // start (the panel is created once, at window setup); all Standard opens
    // no apm session and no clkrst.
    mPerf = ReadPerformanceChoice(mConfig);
    if (DerivePerformanceMode(mPerf) != PerformanceMode::Off) mPerfStatus = ApplyPerformance(mPerf);
    mDrawDistance = ReadActorDrawDistance(mConfig);
    LiveActorDrawDistance().store(mDrawDistance, std::memory_order_relaxed);
    LiveDrawDistanceNativeGroups().store(ReadDrawDistanceNativeGroups(mConfig), std::memory_order_relaxed);
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
    const bool upscalerApplies = VideoSettings::UpscalerApplies(Fast::Deko3dRendererSelected());
    ImGui::BeginDisabled(!upscalerApplies);
    if (ImGui::Combo("Upscaling", &upscaler, upscalers, 3)) {
      mSettings.Upscaler = static_cast<VideoSettings::Upscale>(upscaler);
      changed = true;
    }
    ImGui::EndDisabled();
    if (!upscalerApplies) ImGui::TextDisabled("%s", VideoSettings::kUpscalerDeko3dNote);
    const char *const filters[] = {"Off", "4x (default)"};
    int anisotropy = mSettings.Anisotropy ? 1 : 0;
    if (ImGui::Combo("Anisotropic filter", &anisotropy, filters, 2)) {
      mSettings.Anisotropy = anisotropy == 1;
      changed = true;
    }
    // Every file of this tab is written by the settings writer, off the game
    // thread (fast/oot3d/config_writer.h); the values apply at once.
    if (changed) Write("video", [settings = mSettings, config = mConfig] { return settings.Save(config).empty(); });
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
    if (Writer().Failed("video"))
      ImGui::TextWrapped("Could not write the video settings to config/.");
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
      Write(kPerfOverlayConfigName, [config = mConfig, overlay] { return SaveOverlay(config, overlay); });
    }
    if (Writer().Failed(kPerfOverlayConfigName))
      ImGui::TextWrapped("Could not write config/%s.", kPerfOverlayConfigName);
    DrawPerformance();
    DrawDrawDistance();
    DrawTexturePack();
    DrawGrass();
    DrawSimpleShadow();
    DrawLighting();
  }

private:
  // config/fps_counter: present = overlay on.
  static bool SaveOverlay(const std::filesystem::path &config, bool enabled) {
    std::error_code error;
    if (!enabled) {
      std::filesystem::remove(config / kPerfOverlayConfigName, error);
      return !error;
    }
    std::filesystem::create_directories(config, error);
    std::ofstream file(config / kPerfOverlayConfigName, std::ios::trunc);
    return static_cast<bool>(file);
  }

  static Fast::Oot3d::ConfigWriter &Writer() { return Fast::Oot3d::SettingsConfigWriter(); }
  // A write of config file(s) `key`, on the settings writer's thread.
  static void Write(std::string key, Fast::Oot3d::ConfigWriter::Write write) {
    Writer().Submit(std::move(key), std::move(write));
  }

  // L3+R3 > Video > Performance mode (oot3d_switch_performance.h): the mode
  // row sets all three clocks, each clock row its own file; applied at once
  // and at start.
  void DrawPerformance() {
    ImGui::Separator();
    PerformanceChoice choice = mPerf;
    const PerformanceMode mode = DerivePerformanceMode(mPerf);
    if (ImGui::BeginCombo("Performance mode", PerformanceModeName(mode))) {
      if (ImGui::Selectable("Off", mode == PerformanceMode::Off)) choice = {false, false, false};
      if (ImGui::Selectable("On", mode == PerformanceMode::On)) choice = {true, true, true};
      ImGui::EndCombo();
    }
    const char *const cpu[] = {"Standard (1020 MHz)", "High (1224 MHz)"};
    const char *const gpu[] = {"Standard (307 MHz)", "High (460.8 MHz)"};
    const char *const memory[] = {"Standard (1331 MHz)", "High (1600 MHz)"};
    int value = choice.Cpu ? 1 : 0;
    if (ImGui::Combo("CPU clock", &value, cpu, 2)) choice.Cpu = value == 1;
    value = choice.Gpu ? 1 : 0;
    if (ImGui::Combo("GPU clock", &value, gpu, 2)) choice.Gpu = value == 1;
    value = choice.Memory ? 1 : 0;
    if (ImGui::Combo("Memory clock", &value, memory, 2)) choice.Memory = value == 1;
    if (choice != mPerf) {
      mPerf = choice;
      Write("performance", [config = mConfig, choice = mPerf] { return SavePerformanceChoice(config, choice) == nullptr; });
      mPerfStatus = ApplyPerformance(mPerf);
    }
    if (Writer().Failed("performance"))
      ImGui::TextWrapped("Could not write the performance clocks to config/.");
    if (mPerfStatus.ApmAttempted && !mPerfStatus.ApmAvailable)
      ImGui::TextWrapped("GPU and memory unavailable: %s refused (0x%X); those clocks are unchanged.",
                         mPerfStatus.ApmFailedStep, mPerfStatus.ApmResult);
    // The CPU request changes after the choice (an override found later).
    const PerformanceStatus now = CurrentPerformanceStatus();
    if (mPerf.Cpu && now.CpuUnavailable)
      ImGui::TextWrapped("CPU High unavailable: %s refused (0x%X); the CPU clock is unchanged.",
                         now.CpuFailedStep, now.CpuResult);
    else if (mPerf.Cpu && now.CpuOverridden)
      ImGui::TextWrapped("CPU: another tool (sys-clk?) keeps setting %u MHz; the game leaves the CPU clock to it.",
                         static_cast<unsigned>((now.CpuForeignHz + 500000U) / 1000000U));
    ImGui::TextDisabled("More battery use. CPU High is above the clock Nintendo gives games; GPU High and Memory "
                        "High are official handheld settings (docked already runs them faster).");
  }

  // config/draw_distance (oot3d_draw_distance_config.h, SWITCH_PORT.md "Draw
  // distance"): applied at once (the game thread reads the published choice
  // every frame).
  void DrawDrawDistance() {
    ImGui::Separator();
    const char *const choices[] = {"Original (default)", "Far (2x)", "Farther (3x)", "Maximum (5x)"};
    int choice = static_cast<int>(mDrawDistance);
    if (ImGui::Combo("Draw distance", &choice, choices, 4)) {
      mDrawDistance = static_cast<ActorDrawDistance>(choice);
      LiveActorDrawDistance().store(mDrawDistance, std::memory_order_relaxed);
      Write(kDrawDistanceConfigName,
            [config = mConfig, choice = mDrawDistance] { return SaveActorDrawDistance(config, choice); });
    }
    if (Writer().Failed(kDrawDistanceConfigName))
      ImGui::TextWrapped("Could not write config/%s.", kDrawDistanceConfigName);
    ImGui::TextDisabled("Applies at once. How far away trees, signs, people and enemies are drawn. "
                        "Original: the game's own distance, so they appear as Link comes near. The "
                        "others draw them from farther away (up to 32 more at a time), and the trees "
                        "and bushes that grow around a tree appear with it. Far away they are only "
                        "drawn: they may stand still until Link comes near, as they would in "
                        "Original. Costs some CPU time in busy places.");
  }

  // HD textures (config/texture_pack) and their size cap
  // (config/texture_pack_scale): applied by the deko3d renderer at the start
  // of its next frame; the files are written, and the pack folder looked for,
  // on the settings writer's thread (test.15: 117-157 ms game-thread frames).
  void DrawTexturePack() {
    ImGui::Separator();
    bool changed = ImGui::Checkbox("HD textures (texture pack)", &mPack.Enabled);
    // The sizes this pack has (azahar_pack_scale.h): this start's measure once
    // the pack worker has one, else the last measure remembered in
    // config/texture_pack_sizes (with HD textures off too), else all of them.
    const uint32_t largest = ::Oot3d::Renderer::TexturePackMenuLargest(
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().LargestImageScale(), mRememberedSizes);
    if (largest != mScaleMenuLargest || mPack.MaxScale != mScaleMenuCap || mScaleMenu.Choices.empty()) {
      mScaleMenu = ::Oot3d::Renderer::MakeTexturePackScaleMenu(mPack.MaxScale, largest);
      mScaleMenuLargest = largest;
      mScaleMenuCap = mPack.MaxScale;
    }
    const char *current =
        mScaleMenu.Selected >= 0 ? mScaleMenu.Choices[static_cast<size_t>(mScaleMenu.Selected)].Label.c_str()
                                 : "Custom";
    if (ImGui::BeginCombo("HD texture size", current)) {
      for (size_t index = 0; index < mScaleMenu.Choices.size(); ++index) {
        const auto &choice = mScaleMenu.Choices[index];
        // A cap above the pack's largest shows the largest selected: picking
        // it again leaves config/texture_pack_scale as it is.
        const bool selected = static_cast<int>(index) == mScaleMenu.Selected;
        if (ImGui::Selectable(choice.Label.c_str(), selected) && !selected) {
          changed = true;
          mPack.MaxScale = choice.Scale;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    if (changed) {
      ::Oot3d::Renderer::LiveTexturePackSettings().Publish(mPack);
      Write(::Oot3d::Renderer::kTexturePackConfigName, [settings = mPack, config = mConfig, found = mPackFound] {
        const bool written = settings.Save(config).empty();
        found->store(PackFolderExists(settings.Folder), std::memory_order_relaxed);
        return written;
      });
    }
    if (!mPackFound->load(std::memory_order_relaxed))
      ImGui::TextWrapped("No texture pack found in %s.",
                         ::Oot3d::Renderer::TexturePackDisplayFolder(mPack.Folder).c_str());
    if (Writer().Failed(::Oot3d::Renderer::kTexturePackConfigName))
      ImGui::TextWrapped("Could not write config/%s or %s.", ::Oot3d::Renderer::kTexturePackConfigName,
                         ::Oot3d::Renderer::kTexturePackScaleConfigName);
    if (mScaleMenuLargest != 0U)
      ImGui::TextDisabled("Only the sizes this pack has: its world and character textures go up to %ux "
                          "(the HUD stays at most 3x).",
                          mScaleMenuLargest);
    ImGui::TextDisabled("Applies at once. Larger sizes use more memory and GPU time.");
  }

  // config/simple_shadow (oot3d_simple_shadow.h): present = on, applied at
  // once. Only the deko3d backend draws the body, so only the Switch has it.
  void DrawSimpleShadow() {
#if defined(__SWITCH__)
    ImGui::Separator();
    bool simpleShadow = SimpleShadowEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Simple shadow", &simpleShadow)) {
      SimpleShadowEnabled().store(simpleShadow, std::memory_order_relaxed);
      Write(kSimpleShadowConfigName,
            [config = mConfig, simpleShadow] { return SaveSimpleShadowEnabled(config, simpleShadow); });
    }
    if (Writer().Failed(kSimpleShadowConfigName))
      ImGui::TextWrapped("Could not write config/%s.", kSimpleShadowConfigName);
    ImGui::TextDisabled("Applies at once. Link's whole body casts his shadow along the scene's light, "
                        "instead of the two spots under his feet (Single Screen Experience's Simple "
                        "shadows). deko3d renderer only; drawing Link twice costs some GPU time.");
#endif
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
      Write("grass", [config = mConfig, mode = mGrass] { return Fast::Oot3d::SaveGrassSwitchMode(config, mode); });
    }
    if (Writer().Failed("grass"))
      ImGui::TextWrapped("Could not write config/grass.");
    else if (mGrass != mGrassBoot)
      ImGui::TextUnformatted("Saved. Close and reopen the game to apply.");
    ImGui::TextDisabled("deko3d renderer only. Drawn grass costs GPU time; "
                        "with 30 FPS it reaches farther.");
    // Far was removed after test.15 (SWITCH_PORT.md "Grass distance: Near
    // and Whole area"); a saved "far" reads as Whole area.
    const char *const distances[] = {"Near (default)", "Whole area"};
    int distance = static_cast<int>(mGrassDistance);
    if (ImGui::Combo("Grass distance", &distance, distances, 2)) {
      mGrassDistance = static_cast<Fast::Oot3d::GrassSwitchDistance>(distance);
      Write("grass_distance", [config = mConfig, distance = mGrassDistance] {
        return Fast::Oot3d::SaveGrassSwitchDistance(config, distance);
      });
      Fast::Oot3d::PublishGrassSwitchDistance(mGrassDistance);
    }
    if (Writer().Failed("grass_distance"))
      ImGui::TextWrapped("Could not write config/grass_distance.");
    ImGui::TextDisabled("Applies at once. Near: full density to 10 m (16 m at 30 FPS), thinner beyond. "
                        "Whole area adds distant grass in open places like Hyrule Field; in small areas "
                        "like Kokiri Forest it looks the same as Near. In Hyrule Field it costs up to "
                        "about 1.5 ms more GPU time per frame.");
    // config/grass_lod (SWITCH_PORT.md "Grass on deko3d: cost per tuft"):
    // applied at once (the renderer's next cull takes the published choice).
    const char *const details[] = {"Levels (fewer, wider blades far away; default)",
                                   "Every blade (test.9)"};
    int detail = mGrassLod ? 0 : 1;
    if (ImGui::Combo("Grass detail", &detail, details, 2)) {
      mGrassLod = detail == 0;
      Write("grass_lod", [config = mConfig, lod = mGrassLod] { return Fast::Oot3d::SaveGrassSwitchLod(config, lod); });
      Fast::Oot3d::PublishGrassSwitchLod(mGrassLod);
    }
    if (Writer().Failed("grass_lod"))
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
      Write("grass_fill", [config = mConfig, fill = mGrassFill] { return Fast::Oot3d::SaveGrassSwitchFill(config, fill); });
      Fast::Oot3d::PublishGrassSwitchFill(mGrassFill);
    }
    if (Writer().Failed("grass_fill"))
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
      Write("lighting", [config = mConfig, applied] { return Fast::Deko3d::SaveLightingMode(config, applied).empty(); });
      if (applied != chosen && !validation.Issues.empty()) mLightingIssue = validation.Issues.front().Message;
      else mLightingIssue.clear();
    }
    if (Writer().Failed("lighting"))
      ImGui::TextWrapped("Could not write config/lighting.");
    if (!mLightingIssue.empty())
      ImGui::TextWrapped("Not applied: %s", mLightingIssue.c_str());
    else if (!modernAvailable)
      ImGui::TextWrapped("Per-pixel needs the deko3d renderer and its shadercache/deko3d.pack.");
    ImGui::TextDisabled("deko3d renderer. Applies at once, at 30 or 60 FPS. Per-pixel is the Modern lighting "
                        "model of Lighting > Scene lights; it costs about 1-2.5 ms of GPU per frame: at 60 FPS "
                        "busy rooms may drop frames.");
  }

  static bool PackFolderExists(const std::string &folder) {
    std::error_code error;
    return std::filesystem::is_directory(folder, error);
  }

  std::filesystem::path mConfig;
  VideoSettings mBoot;
  VideoSettings mSettings;
  ::Oot3d::Renderer::TexturePackSettings mPack;
  // Set by the writer's thread after each HD textures change.
  std::shared_ptr<std::atomic<bool>> mPackFound = std::make_shared<std::atomic<bool>>(false);
  ::Oot3d::Renderer::TexturePackScaleMenu mScaleMenu;
  uint32_t mScaleMenuLargest = 0;
  uint32_t mScaleMenuCap = 0;
  // config/texture_pack_sizes as this start found it (read once: the pack
  // worker's measure takes over when it has one).
  std::optional<::Oot3d::Renderer::PackScaleRecord> mRememberedSizes =
      ::Oot3d::Renderer::ReadPackScaleRecord(mConfig / ::Oot3d::Renderer::kTexturePackSizesName);
  Fast::Oot3d::GrassSwitchMode mGrassBoot = Fast::Oot3d::GrassSwitchMode::Off;
  Fast::Oot3d::GrassSwitchMode mGrass = Fast::Oot3d::GrassSwitchMode::Off;
  Fast::Oot3d::GrassSwitchDistance mGrassDistance = Fast::Oot3d::GrassSwitchDistance::Near;
  bool mGrassLod = true;
  bool mGrassFill = true;
  std::string mLightingIssue;
  PerformanceChoice mPerf;
  PerformanceStatus mPerfStatus;
  ActorDrawDistance mDrawDistance = kDefaultActorDrawDistance;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateVideoSettingsPanel(std::filesystem::path config) {
  return std::make_shared<VideoSettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
