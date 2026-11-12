#include "oot3d_update_panel.h"

#include "fast/oot3d/debug_menu.h"
#include "fast/oot3d/graphics_settings_window.h"
#include "oot3d_build_version.h"
#include "oot3d_switch_updater.h"
#include "oot3d_topscreen_import.h"
#include "oot3d_roll_dash.h"
#include "oot3d_update_release.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace Oot3dNativeGame {
namespace {
using namespace Update;

constexpr const char *kChannelFile = "update_channel";
constexpr const char *kRepositoryFile = "update_repo";
// Present = no check at launch (on by default).
constexpr const char *kNoLaunchCheckFile = "no_update_check";

std::string ReadTrimmed(const std::filesystem::path &path) {
  std::ifstream file(path);
  std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// config/update_repo, when it names a repository; else the default.
std::string LoadRepository(const std::filesystem::path &config) {
  const std::string text = ReadTrimmed(config / kRepositoryFile);
  return IsRepositoryName(text) ? text : std::string(kDefaultRepository);
}

std::string Megabytes(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
  return text;
}

class UpdatePanel final : public Fast::Oot3d::GraphicsSettingsPanelTab {
public:
  explicit UpdatePanel(std::filesystem::path root)
      : mRoot(std::move(root)), mConfig(mRoot / "config"),
        mUpdater(mRoot, LoadRepository(mConfig), BuildNumber()) {
    const auto saved = ParseChannel(ReadTrimmed(mConfig / kChannelFile));
    mChannel = saved.value_or(DefaultChannelFor(BuildVersion()));
    std::error_code ignored;
    mHudInstalled = std::filesystem::is_regular_file(mConfig / TopScreenImport::kPackName, ignored);
    mHudArtInstalled = TopScreenImport::HdArtInstalled(mConfig);
    mDashAnimationsInstalled = RollDashAnimationsInstalled(mConfig);
    mHudOffer = TopScreenImport::ParseOfferSetting(ReadTrimmed(mConfig / TopScreenImport::kOfferSettingName));
    // At launch (this tab is made once the game starts): a notice when a
    // newer version is out. Nothing downloads until the player chooses to.
    mLaunchCheck = !std::filesystem::exists(mConfig / kNoLaunchCheckFile, ignored);
    if (mLaunchCheck)
      mUpdater.Check(mChannel, [](const SwitchUpdater::Status &status) {
        if (status.Offer && status.Kind == OfferKind::Update)
          Fast::Oot3d::PostHostNotice("Version " + status.Offer->Manifest.Version +
                                          " is available: L3+R3 > Updates",
                                      false, std::chrono::seconds(10));
      });
  }

  const char *Label() const noexcept override { return "Updates"; }

  void Draw() override {
    ImGui::Text("Installed: %s", BuildVersion());
    DrawChannel();
    DrawLaunchCheck();
    ImGui::TextDisabled("Releases from github.com/%s", mUpdater.Repository().c_str());
    ImGui::Separator();
    const auto status = mUpdater.Snapshot();
    switch (status.State) {
    case SwitchUpdater::Phase::Idle:
      if (ImGui::Button("Check for updates")) mUpdater.Check(mChannel);
      break;
    case SwitchUpdater::Phase::Checking:
      ImGui::TextUnformatted(status.Cancelling ? "Cancelling..." : "Checking GitHub...");
      if (!status.Cancelling && ImGui::Button("Cancel")) mUpdater.Cancel();
      break;
    case SwitchUpdater::Phase::Checked:
      DrawChecked(status);
      break;
    case SwitchUpdater::Phase::Downloading:
      DrawDownloading(status);
      break;
    case SwitchUpdater::Phase::Installed:
      DrawInstalled(status);
      break;
    case SwitchUpdater::Phase::Failed:
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.45F, 0.4F, 1.0F));
      ImGui::TextWrapped("%s", status.Message.c_str());
      ImGui::PopStyleColor();
      if (ImGui::Button("Check again")) mUpdater.Check(mChannel);
      break;
    }
    DrawHudTextures();
  }

private:
  void DrawChannel() {
    const char *const channels[] = {"Stable (releases only)", "Test (also pre-releases)"};
    int channel = mChannel == Channel::Test ? 1 : 0;
    ImGui::BeginDisabled(mUpdater.Busy());
    if (ImGui::Combo("Channel", &channel, channels, 2)) {
      mChannel = channel == 1 ? Channel::Test : Channel::Stable;
      mUpdater.Reset(); // an offer from the other channel no longer applies
      std::error_code ignored;
      std::filesystem::create_directories(mConfig, ignored);
      std::ofstream file(mConfig / kChannelFile, std::ios::trunc);
      file << ChannelName(mChannel) << '\n';
      mChannelError = !file;
    }
    ImGui::EndDisabled();
    if (mChannelError) ImGui::TextWrapped("Could not write config/%s.", kChannelFile);
  }

  // config/no_update_check: read at launch, so a change applies at the next one.
  void DrawLaunchCheck() {
    if (ImGui::Checkbox("Check at launch", &mLaunchCheck)) {
      std::error_code error;
      if (mLaunchCheck) {
        std::filesystem::remove(mConfig / kNoLaunchCheckFile, error);
      } else {
        std::filesystem::create_directories(mConfig, error);
        if (!std::ofstream(mConfig / kNoLaunchCheckFile, std::ios::trunc))
          error = std::make_error_code(std::errc::io_error);
      }
      mLaunchCheckError = static_cast<bool>(error);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(a notice when a newer version is out)");
    if (mLaunchCheckError) ImGui::TextWrapped("Could not write config/%s.", kNoLaunchCheckFile);
  }

  void DrawChecked(const SwitchUpdater::Status &status) {
    if (!status.Offer) {
      ImGui::TextWrapped("%s", status.Message.c_str());
      if (ImGui::Button("Check again")) mUpdater.Check(mChannel);
      return;
    }
    const auto &offer = *status.Offer;
    ImGui::Text("Latest on %s: %s, %s", ChannelName(mChannel), offer.Manifest.Version.c_str(),
                Megabytes(offer.TotalBytes()).c_str());
    const char *action = "Download and install";
    switch (status.Kind) {
    case OfferKind::Update:
      ImGui::TextUnformatted("A newer version is available.");
      break;
    case OfferKind::Reinstall:
      ImGui::TextUnformatted("You have this version. Reinstall it to repair damaged or missing files.");
      action = "Reinstall (repair)";
      break;
    case OfferKind::Older:
      ImGui::TextUnformatted("This version is older than the installed one (e.g. a newer test version).");
      action = "Install this older version";
      break;
    }
    if (status.Message.rfind("Download cancelled", 0) == 0) ImGui::TextDisabled("%s", status.Message.c_str());
    if (ImGui::Button(action)) mUpdater.Install();
    ImGui::SameLine();
    if (ImGui::Button("Check again")) mUpdater.Check(mChannel);
    ImGui::SeparatorText("Release notes");
    const float height = ImGui::GetTextLineHeightWithSpacing() * 10.0F;
    if (ImGui::BeginChild("##ReleaseNotes", ImVec2(0.0F, height), true)) {
      ImGui::TextWrapped("%s", offer.Release.Notes.empty() ? "(none)" : offer.Release.Notes.c_str());
    }
    ImGui::EndChild();
  }

  void DrawDownloading(const SwitchUpdater::Status &status) {
    const float fraction =
        status.BytesTotal == 0 ? 0.0F
                               : static_cast<float>(static_cast<double>(status.BytesDone) /
                                                    static_cast<double>(status.BytesTotal));
    const std::string overlay = Megabytes(status.BytesDone) + " / " + Megabytes(status.BytesTotal);
    ImGui::TextUnformatted(status.CurrentFile.c_str());
    ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F), overlay.c_str());
    ImGui::TextDisabled("You can keep playing; the game is not changed until the download is verified.");
    if (status.Cancelling) {
      ImGui::TextUnformatted("Cancelling...");
    } else if (ImGui::Button("Cancel")) {
      mUpdater.Cancel();
    }
  }

  // Exit game ends the application (oot3d_native_game.cpp): the homebrew loader
  // cannot start the new NRO from here, so the player starts it again.
  void DrawInstalled(const SwitchUpdater::Status &status) {
    constexpr const char *kConfirm = "Close the game?";
    ImGui::TextWrapped("%s", status.Message.c_str());
    ImGui::TextWrapped("Close the game and start it again to play the new version.");
    if (ImGui::Button("Close game")) ImGui::OpenPopup(kConfirm);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
    if (ImGui::BeginPopupModal(kConfirm, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoMove)) {
      ImGui::SetWindowFontScale(std::max(1.0F, ImGui::GetMainViewport()->WorkSize.y / 540.0F));
      ImGui::TextUnformatted("Progress since your last save will be lost.");
      if (ImGui::Button("Close game")) {
        Fast::Oot3d::RequestExitGame();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::SetItemDefaultFocus();
      ImGui::EndPopup();
    }
  }

  // The TopScreen HUD textures (oot3d_switch_topscreen_import.h): built at
  // start-up from the mod downloaded from GameBanana. A player who skipped
  // the offer turns it back on here; it shows at the next start.
  void DrawHudTextures() {
    ImGui::SeparatorText("HUD textures");
    ImGui::TextWrapped("Art from the mod \"%s\" by %s (%s, %s).", TopScreenImport::kModName,
                       TopScreenImport::kModAuthor, TopScreenImport::kModPage, TopScreenImport::kModLicense);
    // Roll dash (oot3d_roll_dash.h) with the HUD built before the import
    // extracted Link's dash animations: the start-up offer adds them.
    const bool dashAnimationsMissing =
        RollDashEnabled().load(std::memory_order_relaxed) && !mDashAnimationsInstalled;
    if (mHudInstalled && mHudArtInstalled && !dashAnimationsMissing) {
      ImGui::TextUnformatted("Installed, with the HD HUD art.");
      return;
    }
    if (mHudInstalled && mHudArtInstalled)
      ImGui::TextWrapped("Installed, with the HD HUD art, but without Link's dash animations for Roll dash "
                         "(L3+R3 > Game > Gameplay). The console downloads the mod again from GameBanana (%s) "
                         "at start-up to add them.",
                         Megabytes(TopScreenImport::DownloadArchive().Bytes).c_str());
    else if (mHudInstalled)
      ImGui::TextWrapped("Installed without all of the HD HUD art (built by an earlier version). The console downloads "
                         "the mod again from GameBanana (%s) at start-up to add it.",
                         Megabytes(TopScreenImport::DownloadArchive().Bytes).c_str());
    else
      ImGui::TextWrapped("Not installed. The console downloads the mod from GameBanana (%s) at start-up and builds "
                         "the textures from your game data.",
                         Megabytes(TopScreenImport::DownloadArchive().Bytes).c_str());
    if (mHudOfferRequested) {
      ImGui::TextWrapped("The next start offers the download. Close the game and start it again.");
      return;
    }
    if (mHudOffer == TopScreenImport::OfferSetting::Never)
      ImGui::TextDisabled("You chose not to be asked at start-up.");
    if (ImGui::Button("Offer the download at the next start")) {
      std::error_code ignored;
      std::filesystem::create_directories(mConfig, ignored);
      std::ofstream file(mConfig / TopScreenImport::kOfferSettingName, std::ios::trunc);
      file << TopScreenImport::OfferSettingText(TopScreenImport::OfferSetting::Ask) << '\n';
      mHudOfferRequested = static_cast<bool>(file);
      if (mHudOfferRequested) mHudOffer = TopScreenImport::OfferSetting::Ask;
      mHudOfferError = !file;
    }
    if (mHudOfferError) ImGui::TextWrapped("Could not write config/%s.", TopScreenImport::kOfferSettingName);
  }

  std::filesystem::path mRoot;
  std::filesystem::path mConfig;
  SwitchUpdater mUpdater;
  Channel mChannel = Channel::Stable;
  bool mChannelError = false;
  bool mLaunchCheck = true;
  bool mLaunchCheckError = false;
  bool mHudInstalled = false;
  bool mHudArtInstalled = false;
  bool mDashAnimationsInstalled = false;
  TopScreenImport::OfferSetting mHudOffer = TopScreenImport::OfferSetting::Ask;
  bool mHudOfferRequested = false;
  bool mHudOfferError = false;
};
} // namespace

std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab> CreateUpdatePanel(std::filesystem::path root) {
  return std::make_shared<UpdatePanel>(std::move(root));
}

void FinishPendingUpdate(const std::filesystem::path &root) {
  const std::string log = ApplyPendingUpdate(root, BuildNumber());
  if (log.empty()) return;
  std::error_code ignored;
  std::filesystem::create_directories(root / "logs", ignored);
  std::ofstream(root / "logs" / "update.txt", std::ios::app)
      << "start of build " << BuildNumber() << ":\n" << log;
}

} // namespace Oot3dNativeGame
