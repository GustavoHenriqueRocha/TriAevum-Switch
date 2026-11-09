#include "oot3d_gameplay_settings_panel.h"
#include "oot3d_landing_roll.h"
#include "oot3d_roll_dash.h"
#include <imgui.h>
namespace Oot3dNativeGame {
namespace {
class GameplaySettingsPanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit GameplaySettingsPanel(std::filesystem::path config)
      : mConfig(std::move(config)), mDashAnimations(RollDashAnimationsInstalled(mConfig)) {
    LandingRollEnabled().store(ReadLandingRollEnabled(mConfig), std::memory_order_relaxed);
    RollDashEnabled().store(ReadRollDashEnabled(mConfig), std::memory_order_relaxed);
  }
  const char *Label() const noexcept override { return "Gameplay"; }
  const char *Group() const noexcept override { return Fast::Oot3d::kGameSettingsGroup; }
  // Applies at once (the hooks read the flags at every landing / host
  // frame); the files keep them for the next start.
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
    ImGui::Separator();
    bool dash = RollDashEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Roll dash", &dash)) {
      RollDashEnabled().store(dash, std::memory_order_relaxed);
      mDashError = !SaveRollDashEnabled(mConfig, dash);
    }
    ImGui::TextWrapped("Keep A held after a roll: Link dashes out of it, faster than he runs, "
                       "until A is released (Single Screen Experience's Roll Dash).");
    if (!mDashAnimations)
      ImGui::TextWrapped("Link's dash animations (config/%s, from the Single Screen Experience "
                         "download) are not installed, so he dashes with his normal run. With "
                         "Roll dash on, the next start offers the download (if you chose "
                         "\"never\", L3+R3 > Updates turns the offer back on), or copy "
                         "singlescreen_322.zip into switch/oot3dre/.",
                         kRollDashAnimationsName);
    if (mDashError)
      ImGui::TextWrapped("Could not write config/%s.", kRollDashConfigName);
  }

private:
  std::filesystem::path mConfig;
  bool mDashAnimations = false;
  bool mError = false;
  bool mDashError = false;
};
} // namespace
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateGameplaySettingsPanel(std::filesystem::path config) {
  return std::make_shared<GameplaySettingsPanel>(std::move(config));
}
} // namespace Oot3dNativeGame
