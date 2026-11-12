#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

inline constexpr std::string_view kTopScreenUiConfigSchema =
    "oot3d_topscreen_ui_v2";
inline constexpr std::string_view kTopScreenUiLegacyConfigSchema =
    "oot3d_topscreen_ui_v1";
inline constexpr float kTopScreenHudScaleMinimum = 0.60F;
inline constexpr float kTopScreenHudScaleMaximum = 1.20F;
// Switch: 0.75, near the Single Screen Experience mod's screenshots (~0.70);
// config/topscreen_ui.switch.json ships the same value. PC keeps 0.80.
#ifdef __SWITCH__
inline constexpr float kTopScreenHudScaleDefault = 0.75F;
#else
inline constexpr float kTopScreenHudScaleDefault = 0.80F;
#endif
inline constexpr float kTopScreenHudScaleStep = 0.05F;
inline constexpr std::int8_t kTopScreenHudMarginMinimum = -3;
inline constexpr std::int8_t kTopScreenHudMarginMaximum = 16;
inline constexpr std::uint8_t kTopScreenCameraZoomMinimum = 75U;
inline constexpr std::uint8_t kTopScreenCameraZoomMaximum = 170U;
inline constexpr std::uint8_t kTopScreenCameraFovMinimum = 70U;
inline constexpr std::uint8_t kTopScreenCameraFovMaximum = 140U;

enum class TopScreenSelectAction : std::uint8_t {
  SaveScreen,
  MinimapToggle,
};

const char *TopScreenSelectActionName(TopScreenSelectAction action) noexcept;
bool ParseTopScreenSelectAction(std::string_view value,
                                TopScreenSelectAction *action) noexcept;

enum class TopScreenDpadAction : std::uint8_t {
  None = 0,
  View = 1,
  Ocarina = 2,
  IronBoots = 3,
  HoverBoots = 4,
  ItemZr = 5,
  ItemZl = 6,
  SwordToggle = 7,
  AllBootsToggle = 8,
  MinimapToggle = 9,
  Boomerang = 10,
  Slingshot = 11,
  TunicToggle = 12,
  ShieldToggle = 13,
  // SingleScreen 3.2.2 action 14: the item in the direction's inventory slot,
  // used through D-pad item buttons 6..9 (oot3d_top_screen_dpad_items.h).
  Item = 14,
};

const char *TopScreenDpadActionName(TopScreenDpadAction action) noexcept;
bool ParseTopScreenDpadAction(std::string_view value,
                              TopScreenDpadAction *action) noexcept;

// Inventory slots (SaveContext items[0..23]) an Item direction can hold. The
// magic arrow slots are absent: 3.2.2 refuses them (0x005D15F0), the bow
// switches arrows while aiming instead.
inline constexpr std::uint8_t kTopScreenDpadItemSlotNone = 0xFFU;
inline constexpr std::array<std::uint8_t, 21> kTopScreenDpadItemSlots{
    0U,  1U,  2U,  3U,  5U,  6U,  7U,  8U,  9U,  11U, 12U,
    13U, 14U, 15U, 17U, 18U, 19U, 20U, 21U, 22U, 23U};

bool IsTopScreenDpadItemSlot(std::uint8_t slot) noexcept;
const char *TopScreenDpadItemSlotName(std::uint8_t slot) noexcept;
bool ParseTopScreenDpadItemSlot(std::string_view value,
                                std::uint8_t *slot) noexcept;

enum class TopScreenFreeCameraSmoothing : std::uint8_t {
  Off = 0,
  Light = 1,
  Medium = 2,
  Default = 3,
  Heavy = 4,
};

const char *TopScreenFreeCameraSmoothingName(
    TopScreenFreeCameraSmoothing smoothing) noexcept;
bool ParseTopScreenFreeCameraSmoothing(
    std::string_view value, TopScreenFreeCameraSmoothing *smoothing) noexcept;

inline constexpr std::array<TopScreenDpadAction, 4>
    kTopScreenChildDpadDefaults{TopScreenDpadAction::View,
                                TopScreenDpadAction::Ocarina,
                                TopScreenDpadAction::ItemZr,
                                TopScreenDpadAction::ItemZl};
inline constexpr std::array<TopScreenDpadAction, 4>
    kTopScreenAdultDpadDefaults{TopScreenDpadAction::View,
                                TopScreenDpadAction::Ocarina,
                                TopScreenDpadAction::IronBoots,
                                TopScreenDpadAction::HoverBoots};

struct TopScreenUiConfig {
  float HudScale = kTopScreenHudScaleDefault;
  std::int8_t HudMarginX = 4;
  std::int8_t HudMarginY = 1;
  std::uint8_t MagicBarY = 0U;
  bool MinimapVisible = true;
  bool RenderHud = true;
  bool RenderDpadIcons = true;
  bool RenderItemsHint = true;
  TopScreenSelectAction SelectAction = TopScreenSelectAction::SaveScreen;
  bool ExitItemsToSaveScreen = true;
  std::uint8_t CameraZoomPercent = 100U;
  std::uint8_t CameraFovPercent = 100U;
  std::array<TopScreenDpadAction, 4> ChildDpad =
      kTopScreenChildDpadDefaults;
  std::array<TopScreenDpadAction, 4> AdultDpad =
      kTopScreenAdultDpadDefaults;
  // Slot of each direction set to TopScreenDpadAction::Item, none elsewhere.
  std::array<std::uint8_t, 4> ChildDpadItemSlots{
      kTopScreenDpadItemSlotNone, kTopScreenDpadItemSlotNone,
      kTopScreenDpadItemSlotNone, kTopScreenDpadItemSlotNone};
  std::array<std::uint8_t, 4> AdultDpadItemSlots{
      kTopScreenDpadItemSlotNone, kTopScreenDpadItemSlotNone,
      kTopScreenDpadItemSlotNone, kTopScreenDpadItemSlotNone};
  bool FreeCameraEnabled = false;
  std::uint8_t FreeCameraSpeedLevel = 3U;
  TopScreenFreeCameraSmoothing FreeCameraSmoothing =
      TopScreenFreeCameraSmoothing::Default;
  bool FreeCameraInvertX = false;
  bool FreeCameraInvertY = false;
  std::uint8_t CStickAimSpeedLevel = 3U;
  bool CStickAimInvertX = false;
  bool CStickAimInvertY = false;

  bool operator==(const TopScreenUiConfig &) const = default;
};

// The UI config text holds what builds without D-pad items accept: they
// reject unknown fields and actions and stop at launch, so an Item direction
// is written there as "none" and its slot goes to the D-pad items file.
bool ParseTopScreenUiConfigText(std::string_view text,
                                TopScreenUiConfig *config,
                                std::string *error = nullptr);
bool SerializeTopScreenUiConfigText(const TopScreenUiConfig &config,
                                    std::string *text,
                                    std::string *error = nullptr);

inline constexpr std::string_view kTopScreenDpadItemsSchema =
    "oot3d_topscreen_dpad_items_v1";
// <UI config stem>_dpad_items.json, next to the UI config.
std::filesystem::path
TopScreenDpadItemsPath(const std::filesystem::path &uiConfigPath);
// Sets the Item directions it names; the others keep their action.
bool ParseTopScreenDpadItemsText(std::string_view text,
                                 TopScreenUiConfig *config,
                                 std::string *error = nullptr);
bool SerializeTopScreenDpadItemsText(const TopScreenUiConfig &config,
                                     std::string *text,
                                     std::string *error = nullptr);

// Both files. A D-pad items file that is missing or unreadable leaves the
// directions as the UI config has them, so it never stops a launch.
bool LoadTopScreenUiConfig(const std::filesystem::path &path,
                           TopScreenUiConfig *config,
                           std::string *error = nullptr);
bool SaveTopScreenUiConfig(const std::filesystem::path &path,
                           const TopScreenUiConfig &config,
                           std::string *error = nullptr);

struct TopScreenUiConfigSnapshot {
  TopScreenUiConfig Config;
  std::uint64_t Revision = 0U;
};

class TopScreenUiConfigRuntime final {
public:
  TopScreenUiConfigRuntime(std::filesystem::path path,
                           TopScreenUiConfig initial);

  [[nodiscard]] TopScreenUiConfigSnapshot Snapshot() const;
  [[nodiscard]] const std::filesystem::path &Path() const noexcept;
  [[nodiscard]] bool Persistent() const noexcept;
  void Preview(const TopScreenUiConfig &config);
  bool Apply(const TopScreenUiConfig &config, std::string *error = nullptr);
  bool Reload(std::string *error = nullptr);

private:
  std::filesystem::path mPath;
  mutable std::mutex mMutex;
  TopScreenUiConfig mConfig;
  std::uint64_t mRevision = 1U;
};

} // namespace Oot3dNativeGame
