#pragma once
#include "fast/oot3d/graphics_settings_window.h"
#include <filesystem>
#include <memory>
namespace Oot3dNativeGame {
// Debug menu tab "Updates" (Switch): the installed version and build, the
// update channel (config/update_channel: stable or test), the repository
// (config/update_repo, default JohnUzoka/TriAevum-Switch), checking GitHub
// Releases, the release notes, download and install with progress and
// cancel, and "Restart now" into the installed NRO. `root` is
// sdmc:/switch/oot3dre.
std::shared_ptr<Fast::Oot3d::GraphicsSettingsPanelTab>
CreateUpdatePanel(std::filesystem::path root);
// At start-up, before anything opens the managed files: finishes an update
// installed by the previous run (oot3d_update_release.h,
// ApplyPendingUpdate) and logs what it did to logs/update.txt.
void FinishPendingUpdate(const std::filesystem::path &root);
} // namespace Oot3dNativeGame
