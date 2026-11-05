#include "oot3d_switch_updater.h"

#include "oot3d_build_version.h"
#include "oot3d_switch_network.h"

#include <mbedtls/sha256.h>
#include <switch.h>

#include <array>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>

namespace Oot3dNativeGame {
namespace {
using namespace Update;
using SwitchNetwork::GetResult;
using SwitchNetwork::NetworkSession;
using SwitchNetwork::Transfer;

constexpr std::uint64_t kListingMaxBytes = 4U << 20U;
// The newest release.json files a check reads looking for a complete release.
constexpr size_t kManifestsRead = 5;
// Room left on the SD card beyond the downloads.
constexpr std::uint64_t kSpareBytes = 32U << 20U;
constexpr size_t kWriteBuffer = 1U << 20U;

// HTTPS GET from GitHub's API or a release asset (which redirects to
// GitHub's CDN), explaining GitHub's rate limit.
GetResult HttpGet(const std::string &url, const char *accept, Transfer &transfer, std::string *error) {
  long status = 0;
  const GetResult result = SwitchNetwork::HttpGet(
      url, {std::string("Accept: ") + accept, "X-GitHub-Api-Version: 2022-11-28"}, transfer, &status, error);
  if (result == GetResult::Failed && status >= 400) {
    *error = status == 403 || status == 429
                 ? "GitHub refused the request (HTTP " + std::to_string(status) +
                       "): it allows 60 checks an hour per internet address; try again later"
                 : "GitHub answered HTTP " + std::to_string(status);
  }
  return result;
}

GetResult GetText(const std::string &url, const char *accept, std::uint64_t limit,
                  const std::atomic<bool> &cancel, std::string *text, std::string *error) {
  Transfer transfer;
  transfer.Limit = limit;
  transfer.Cancel = &cancel;
  transfer.Write = [text](const char *data, size_t length) {
    text->append(data, length);
    return true;
  };
  return HttpGet(url, accept, transfer, error);
}

std::string Hex(const unsigned char *bytes, size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(size * 2U);
  for (size_t index = 0; index < size; ++index) {
    text.push_back(kDigits[bytes[index] >> 4U]);
    text.push_back(kDigits[bytes[index] & 0x0FU]);
  }
  return text;
}

std::string Megabytes(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / 1e6);
  return text;
}
} // namespace

SwitchUpdater::SwitchUpdater(std::filesystem::path root, std::string repository,
                             std::uint64_t installedBuild)
    : mRoot(std::move(root)), mRepository(std::move(repository)), mInstalledBuild(installedBuild) {}

SwitchUpdater::~SwitchUpdater() {
  mCancel.store(true);
  if (mWorker.joinable()) mWorker.join();
}

bool SwitchUpdater::Busy() const { return mRunning.load(); }

bool SwitchUpdater::Start(Phase phase, void (SwitchUpdater::*job)()) {
  if (mRunning.load()) return false;
  // The previous job set its final state as its last step: its thread is
  // ending or has ended.
  if (mWorker.joinable()) mWorker.join();
  mCancel.store(false);
  mBytesDone.store(0);
  {
    std::scoped_lock lock(mMutex);
    mStatus.State = phase;
    mStatus.Message.clear();
    mStatus.Cancelling = false;
    mStatus.BytesDone = 0;
    if (phase == Phase::Checking) mStatus.Offer.reset();
  }
  mRunning.store(true);
  try {
    mWorker = std::thread([this, job] { (this->*job)(); });
  } catch (const std::exception &exception) {
    mRunning.store(false);
    Finish(Phase::Failed, std::string("cannot start the update thread: ") + exception.what());
    return false;
  }
  return true;
}

bool SwitchUpdater::Check(Channel channel) {
  // The worker reads mChannel: only change it while none runs (the menu
  // calls Check from one thread).
  if (mRunning.load()) return false;
  mChannel = channel;
  return Start(Phase::Checking, &SwitchUpdater::RunCheck);
}

bool SwitchUpdater::Install() {
  {
    std::scoped_lock lock(mMutex);
    if (!mStatus.Offer || mStatus.State != Phase::Checked) return false;
  }
  return Start(Phase::Downloading, &SwitchUpdater::RunInstall);
}

void SwitchUpdater::Cancel() {
  mCancel.store(true);
  std::scoped_lock lock(mMutex);
  if (mRunning.load()) mStatus.Cancelling = true;
}

bool SwitchUpdater::Reset() {
  if (mRunning.load()) return false;
  std::scoped_lock lock(mMutex);
  mStatus = Status{};
  return true;
}

SwitchUpdater::Status SwitchUpdater::Snapshot() const {
  std::scoped_lock lock(mMutex);
  Status status = mStatus;
  status.BytesDone = mBytesDone.load(std::memory_order_relaxed);
  return status;
}

void SwitchUpdater::Finish(Phase phase, std::string message) {
  Log(message);
  std::scoped_lock lock(mMutex);
  mStatus.State = phase;
  mStatus.Message = std::move(message);
  mStatus.Cancelling = false;
  mRunning.store(false);
}

void SwitchUpdater::Log(const std::string &line) const {
  if (line.empty()) return;
  std::error_code ignored;
  std::filesystem::create_directories(mRoot / "logs", ignored);
  std::ofstream file(mRoot / "logs" / "update.txt", std::ios::app);
  const std::time_t now = std::time(nullptr);
  char stamp[32] = "";
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  file << stamp << " build " << BuildNumber() << ": " << line << '\n';
}

void SwitchUpdater::RunCheck() {
  NetworkSession network;
  if (!network.Ready()) {
    Finish(Phase::Failed, "No network: connect the console to the internet and try again.");
    return;
  }
  std::string listing;
  std::string error;
  const std::string url = "https://api.github.com/repos/" + mRepository + "/releases?per_page=30";
  switch (GetText(url, "application/vnd.github+json", kListingMaxBytes, mCancel, &listing, &error)) {
  case GetResult::Cancelled:
    Finish(Phase::Idle, "");
    return;
  case GetResult::Failed:
    Finish(Phase::Failed, "Could not read the releases of " + mRepository + ": " + error);
    return;
  case GetResult::Ok:
    break;
  }
  const auto candidates = SelectCandidates(listing, mRepository, mChannel, &error);
  if (candidates.empty()) {
    Finish(Phase::Checked, error.empty() ? std::string("No release on the ") + ChannelName(mChannel) +
                                               " channel yet."
                                         : error);
    return;
  }
  std::string rejected;
  for (size_t index = 0; index < candidates.size() && index < kManifestsRead; ++index) {
    const auto &candidate = candidates[index];
    const ReleaseAsset *manifestAsset = candidate.FindAsset(kManifestAsset);
    std::string manifest;
    switch (GetText(manifestAsset->Url, "application/octet-stream", kManifestMaxBytes, mCancel, &manifest,
                    &error)) {
    case GetResult::Cancelled:
      Finish(Phase::Idle, "");
      return;
    case GetResult::Failed:
      rejected += candidate.Tag + ": " + error + "\n";
      continue;
    case GetResult::Ok:
      break;
    }
    auto qualified = QualifyRelease(candidate, manifest, &error);
    if (!qualified) {
      rejected += error + "\n";
      continue;
    }
    const OfferKind kind = ClassifyOffer(mInstalledBuild, qualified->Manifest.Build);
    {
      std::scoped_lock lock(mMutex);
      mStatus.Offer = std::move(qualified);
      mStatus.Kind = kind;
      mStatus.BytesTotal = mStatus.Offer->TotalBytes();
    }
    Log(rejected);
    Finish(Phase::Checked, "Found " + candidate.Tag);
    return;
  }
  Finish(Phase::Checked, "No complete release on the " + std::string(ChannelName(mChannel)) +
                             " channel:\n" + rejected);
}

void SwitchUpdater::RunInstall() {
  QualifiedRelease release;
  {
    std::scoped_lock lock(mMutex);
    release = *mStatus.Offer;
  }
  const ReleaseManifest &manifest = release.Manifest;
  const auto fail = [&](Phase phase, std::string message) {
    DiscardStagedFiles(mRoot, manifest);
    Finish(phase, std::move(message));
  };
  std::error_code spaceError;
  const auto space = std::filesystem::space(mRoot, spaceError);
  if (!spaceError && space.available < release.TotalBytes() + kSpareBytes) {
    fail(Phase::Failed, "Not enough space on the SD card: " + Megabytes(release.TotalBytes() + kSpareBytes) +
                            " needed, " + Megabytes(space.available) + " free.");
    return;
  }
  NetworkSession network;
  if (!network.Ready()) {
    fail(Phase::Failed, "No network: connect the console to the internet and try again.");
    return;
  }
  Log("installing " + release.Release.Tag + " (build " + std::to_string(manifest.Build) + ")");
  std::vector<char> buffer(kWriteBuffer);
  std::uint64_t doneBefore = 0;
  for (size_t index = 0; index < manifest.Files.size(); ++index) {
    const ReleaseFile &file = manifest.Files[index];
    {
      std::scoped_lock lock(mMutex);
      mStatus.CurrentFile = file.Path + " (" + std::to_string(index + 1U) + "/" +
                            std::to_string(manifest.Files.size()) + ")";
    }
    const auto staged = StagedPath(mRoot / file.Path);
    std::error_code ignored;
    std::filesystem::create_directories(staged.parent_path(), ignored);
    std::unique_ptr<std::FILE, int (*)(std::FILE *)> out(std::fopen(staged.c_str(), "wb"), &std::fclose);
    if (!out) {
      fail(Phase::Failed, "Cannot write " + staged.string());
      return;
    }
    std::setvbuf(out.get(), buffer.data(), _IOFBF, buffer.size());
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts_ret(&sha, 0);
    Transfer transfer;
    transfer.Limit = file.Size;
    transfer.Cancel = &mCancel;
    transfer.Write = [&](const char *data, size_t length) {
      if (std::fwrite(data, 1, length, out.get()) != length) return false;
      mbedtls_sha256_update_ret(&sha, reinterpret_cast<const unsigned char *>(data), length);
      mBytesDone.fetch_add(length, std::memory_order_relaxed);
      return true;
    };
    std::string error;
    const GetResult result = HttpGet(release.Downloads[index].Url, "application/octet-stream", transfer, &error);
    std::array<unsigned char, 32> digest{};
    mbedtls_sha256_finish_ret(&sha, digest.data());
    mbedtls_sha256_free(&sha);
    const bool closed = std::fclose(out.release()) == 0;
    if (result == GetResult::Cancelled) {
      fail(Phase::Checked, "Download cancelled.");
      return;
    }
    if (result == GetResult::Failed) {
      fail(Phase::Failed, "Download of " + file.Path + " failed: " + error);
      return;
    }
    if (!closed || transfer.Received != file.Size) {
      fail(Phase::Failed, "Download of " + file.Path + " is incomplete (" + std::to_string(transfer.Received) +
                              " of " + std::to_string(file.Size) + " bytes).");
      return;
    }
    if (Hex(digest.data(), digest.size()) != file.Sha256) {
      fail(Phase::Failed, "Download of " + file.Path + " is damaged (SHA-256 mismatch).");
      return;
    }
    doneBefore += file.Size;
    mBytesDone.store(doneBefore, std::memory_order_relaxed);
  }
  std::string error;
  const auto nroVersion = ReadNroDisplayVersion(StagedPath(mRoot / kNroPath), &error);
  if (!nroVersion || *nroVersion != manifest.Version) {
    fail(Phase::Failed, "The downloaded NRO is not version " + manifest.Version + ": " +
                            (nroVersion ? "it says " + *nroVersion : error));
    return;
  }
  if (mCancel.load()) {
    fail(Phase::Checked, "Download cancelled.");
    return;
  }
  if (!CommitStagedRelease(mRoot, manifest, &error)) {
    // CommitStagedRelease already removed the staged files.
    Finish(Phase::Failed, "Could not install: " + error + ". The installed version is unchanged.");
    return;
  }
  Finish(Phase::Installed, "Installed " + release.Release.Tag + ". It starts with the next launch.");
}

} // namespace Oot3dNativeGame
