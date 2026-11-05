#pragma once
// The Switch updater (L3+R3 > Updates): checks GitHub Releases and installs
// a release on a worker thread. The rules (which release, which files, how
// files are swapped) are in oot3d_update_release.h; this is the network,
// download and thread part. Sockets and libcurl are only up while a check
// or download runs.
#include "oot3d_update_release.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace Oot3dNativeGame {

class SwitchUpdater {
public:
  enum class Phase { Idle, Checking, Checked, Downloading, Installed, Failed };
  struct Status {
    Phase State = Phase::Idle;
    // Checked without an offer: why; Failed: the reason; Installed: what.
    std::string Message;
    std::optional<Update::QualifiedRelease> Offer;
    Update::OfferKind Kind = Update::OfferKind::Update;
    // Downloading.
    std::uint64_t BytesDone = 0;
    std::uint64_t BytesTotal = 0;
    std::string CurrentFile;
    bool Cancelling = false;
  };

  // `root` is sdmc:/switch/oot3dre; `installedBuild` this build's number.
  SwitchUpdater(std::filesystem::path root, std::string repository, std::uint64_t installedBuild);
  // Cancels what is running and waits for the thread.
  ~SwitchUpdater();
  SwitchUpdater(const SwitchUpdater &) = delete;
  SwitchUpdater &operator=(const SwitchUpdater &) = delete;

  // Start a check / install of the checked offer; false while busy.
  bool Check(Update::Channel channel);
  bool Install();
  void Cancel();
  // Back to Idle (e.g. the channel changed); false while busy.
  bool Reset();
  Status Snapshot() const;
  bool Busy() const;
  const std::string &Repository() const { return mRepository; }

private:
  bool Start(Phase phase, void (SwitchUpdater::*job)());
  void RunCheck();
  void RunInstall();
  void Finish(Phase phase, std::string message);
  void Log(const std::string &line) const;

  std::filesystem::path mRoot;
  std::string mRepository;
  std::uint64_t mInstalledBuild;
  Update::Channel mChannel = Update::Channel::Stable;
  mutable std::mutex mMutex;
  Status mStatus;
  std::atomic<bool> mCancel{false};
  std::atomic<bool> mRunning{false};
  std::atomic<std::uint64_t> mBytesDone{0};
  std::thread mWorker;
};

// Whether the homebrew loader can start another NRO when this one exits
// (hbmenu/hbloader can; some forwarders cannot).
bool CanChainloadNro();
// Asks the loader to start the installed NRO (`root`/oot3d_native_game.nro)
// next, then asks the game to close the way Exit game does. False, with
// nothing requested, when the loader cannot chainload.
bool RestartIntoInstalledNro(const std::filesystem::path &root);

} // namespace Oot3dNativeGame
