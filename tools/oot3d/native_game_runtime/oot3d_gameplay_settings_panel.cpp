#include "oot3d_gameplay_settings_panel.h"
#include "oot3d_landing_roll.h"
#include "oot3d_romfs_mods.h"
#include <imgui.h>
namespace Oot3dNativeGame {
namespace {
class GameplaySettingsPanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit GameplaySettingsPanel(std::filesystem::path config) : mConfig(std::move(config)) {
    LandingRollEnabled().store(ReadLandingRollEnabled(mConfig), std::memory_order_relaxed);
  }
  const char *Label() const noexcept override { return "Gameplay"; }
  const char *Group() const noexcept override { return Fast::Oot3d::kGameSettingsGroup; }
  // Applies at once (the hook reads the flag at every landing); the file
  // keeps it for the next start.
  void Draw() override {
    bool roll = LandingRollEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Landing roll (N64)", &roll)) {
      LandingRollEnabled().store(roll, std::memory_order_relaxed);
      mError = !SaveLandingRollEnabled(mConfig, roll);
    }
    ImGui::TextWrapped("After a run jump, holding the stick forward when landing from a fall of "
                       "80 to 800 units rolls with no fall damage, as on N64.");
    if (mError)
      ImGui::TextWrapped("Could not write config/%s.", kLandingRollOffConfigName);
#if defined(__SWITCH__)
    // RomFS mods (oot3d_romfs_mods.h): read once at start-up, so the change
    // applies at the next start.
    ImGui::Separator();
    if (ImGui::Checkbox("Mods (RomFS)", &mRomFsMods))
      mRomFsModsError = !SaveRomFsModsEnabled(mConfig, mRomFsMods);
    ImGui::TextWrapped("Replaces game files with the ones in switch/oot3dre/mods/<name>/romfs/ "
                       "(e.g. a Link model). Applies at the next start.");
    const std::string &state = RomFsModsStartupState().PanelLine;
    if (!state.empty())
      ImGui::TextWrapped("%s", state.c_str());
    if (mRomFsModsError)
      ImGui::TextWrapped("Could not write config/%s.", kRomFsModsOffConfigName);
#endif
  }

private:
  std::filesystem::path mConfig;
  bool mError = false;
#if defined(__SWITCH__)
  bool mRomFsMods = ReadRomFsModsEnabled(mConfig);
  bool mRomFsModsError = false;
#endif
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateGameplaySettingsPanel(std::filesystem::path config) {
  return std::make_shared<GameplaySettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
