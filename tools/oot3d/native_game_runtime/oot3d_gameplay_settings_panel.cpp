#include "oot3d_gameplay_settings_panel.h"
#include "oot3d_damage_multiplier.h"
#include "oot3d_landing_roll.h"
#include "oot3d_master_quest_mirror.h"
#include "oot3d_quick_put_away.h"
#include "oot3d_roll_dash.h"
#include "oot3d_romfs_mods.h"
#include "oot3d_simple_shadow.h"
#include <imgui.h>
namespace Oot3dNativeGame {
namespace {
class GameplaySettingsPanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit GameplaySettingsPanel(std::filesystem::path config)
      : mConfig(std::move(config)), mDashAnimations(RollDashAnimationsInstalled(mConfig)) {
    LandingRollEnabled().store(ReadLandingRollEnabled(mConfig), std::memory_order_relaxed);
    RollDashEnabled().store(ReadRollDashEnabled(mConfig), std::memory_order_relaxed);
    QuickPutAwayEnabled().store(ReadQuickPutAwayEnabled(mConfig), std::memory_order_relaxed);
    DamageMultiplier().store(ReadDamageMultiplier(mConfig), std::memory_order_relaxed);
    MasterQuestUnmirrored().store(ReadMasterQuestUnmirrored(mConfig), std::memory_order_relaxed);
    ApplyMasterQuestUnmirrored();
#if defined(__SWITCH__)
    SimpleShadowEnabled().store(ReadSimpleShadowEnabled(mConfig), std::memory_order_relaxed);
#endif
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
    ImGui::Separator();
    bool putAway = QuickPutAwayEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Quick put away", &putAway)) {
      QuickPutAwayEnabled().store(putAway, std::memory_order_relaxed);
      mPutAwayError = !SaveQuickPutAwayEnabled(mConfig, putAway);
    }
    ImGui::TextWrapped("A offers Put Away right after Link uses his sword or an item, instead of "
                       "a second later (Single Screen Experience's Quick put away).");
    if (mPutAwayError)
      ImGui::TextWrapped("Could not write config/%s.", kQuickPutAwayConfigName);
    ImGui::Separator();
    const std::uint8_t damage = DamageMultiplier().load(std::memory_order_relaxed);
    if (ImGui::BeginCombo("Damage taken", DamageMultiplierName(damage).c_str())) {
      for (const std::uint8_t choice : kDamageMultiplierChoices) {
        const bool selected = choice == damage;
        if (ImGui::Selectable(DamageMultiplierName(choice).c_str(), selected) && !selected) {
          DamageMultiplier().store(choice, std::memory_order_relaxed);
          mDamageError = !SaveDamageMultiplier(mConfig, choice);
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::TextWrapped("Multiplies the damage Link takes, or makes any hit fatal (Single Screen "
                       "Experience's Damage multiplier). Master Quest still doubles it and the "
                       "double defence still halves it. Healing is unchanged.");
    if (mDamageError)
      ImGui::TextWrapped("Could not write config/%s.", kDamageMultiplierConfigName);
    ImGui::Separator();
    bool unmirrored = MasterQuestUnmirrored().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Unmirrored Master Quest", &unmirrored)) {
      MasterQuestUnmirrored().store(unmirrored, std::memory_order_relaxed);
      mUnmirroredError = !SaveMasterQuestUnmirrored(mConfig, unmirrored);
    }
    ImGui::TextWrapped("Master Quest without the mirrored world; its dungeons stay Master Quest "
                       "(Single Screen Experience's option). Takes effect when a file is loaded "
                       "or the pause menu closes.");
    if (mUnmirroredError)
      ImGui::TextWrapped("Could not write config/%s.", kMasterQuestUnmirroredConfigName);
#if defined(__SWITCH__)
    // Simple shadow (oot3d_simple_shadow.h): the deko3d backend draws the body.
    ImGui::Separator();
    bool simpleShadow = SimpleShadowEnabled().load(std::memory_order_relaxed);
    if (ImGui::Checkbox("Simple shadow", &simpleShadow)) {
      SimpleShadowEnabled().store(simpleShadow, std::memory_order_relaxed);
      mSimpleShadowError = !SaveSimpleShadowEnabled(mConfig, simpleShadow);
    }
    ImGui::TextWrapped("Link's whole body casts its shadow on the floor, along the scene's light, "
                       "instead of the two spots under his feet (Single Screen Experience's Simple "
                       "shadows).");
    if (mSimpleShadowError)
      ImGui::TextWrapped("Could not write config/%s.", kSimpleShadowConfigName);
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
  bool mDashAnimations = false;
  bool mError = false;
  bool mDashError = false;
  bool mPutAwayError = false;
  bool mDamageError = false;
  bool mUnmirroredError = false;
#if defined(__SWITCH__)
  bool mSimpleShadowError = false;
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
