#include "oot3d_switch_updater.h"

#include "oot3d_build_version.h"

#include <curl/curl.h>
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

constexpr long kConnectTimeoutSeconds = 15;
// A transfer that stays below 1 KiB/s for this long fails.
constexpr long kStallSeconds = 30;
constexpr std::uint64_t kListingMaxBytes = 4U << 20U;
// The newest release.json files a check reads looking for a complete release.
constexpr size_t kManifestsRead = 5;
// Room left on the SD card beyond the downloads.
constexpr std::uint64_t kSpareBytes = 32U << 20U;
constexpr size_t kWriteBuffer = 1U << 20U;

// Sockets and libcurl for one check or download. curl_global_init opens
// the console's SSL service (devkitPro's libcurl uses libnx's TLS backend:
// certificates are checked by the system against its trusted CAs); both
// are closed again when the job ends.
class NetworkSession {
public:
  NetworkSession() {
    mSockets = R_SUCCEEDED(socketInitializeDefault());
    mCurl = mSockets && curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
  }
  ~NetworkSession() {
    if (mCurl) curl_global_cleanup();
    if (mSockets) socketExit();
  }
  NetworkSession(const NetworkSession &) = delete;
  NetworkSession &operator=(const NetworkSession &) = delete;
  bool Ready() const { return mCurl; }

private:
  bool mSockets = false;
  bool mCurl = false;
};

struct Transfer {
  std::function<bool(const char *, size_t)> Write;
  std::uint64_t Limit = 0;
  std::uint64_t Received = 0;
  const std::atomic<bool> *Cancel = nullptr;
  std::string Failure;
};

size_t WriteCallback(char *data, size_t size, size_t count, void *user) {
  auto &transfer = *static_cast<Transfer *>(user);
  const size_t length = size * count;
  if (transfer.Cancel->load(std::memory_order_relaxed)) return 0;
  if (transfer.Received + length > transfer.Limit) {
    transfer.Failure = "the download is longer than expected";
    return 0;
  }
  if (!transfer.Write(data, length)) {
    if (transfer.Failure.empty()) transfer.Failure = "cannot write to the SD card";
    return 0;
  }
  transfer.Received += length;
  return length;
}

int ProgressCallback(void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<Transfer *>(user)->Cancel->load(std::memory_order_relaxed) ? 1 : 0;
}

enum class GetResult { Ok, Cancelled, Failed };

// HTTPS GET of `url`, following redirects (release assets redirect to
// GitHub's CDN), HTTPS only.
GetResult HttpGet(const std::string &url, const char *accept, Transfer &transfer, std::string *error) {
  CURL *curl = curl_easy_init();
  if (curl == nullptr) {
    *error = "libcurl could not start a request";
    return GetResult::Failed;
  }
  const std::string acceptHeader = std::string("Accept: ") + accept;
  curl_slist *headers = curl_slist_append(nullptr, acceptHeader.c_str());
  headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
  const std::string userAgent = std::string("oot3dre-switch/") + BuildVersion();
  std::array<char, CURL_ERROR_SIZE> message{};
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStallSeconds);
  curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 128L * 1024L);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(transfer.Limit));
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, message.data());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (code == CURLE_OK) return GetResult::Ok;
  if (transfer.Cancel->load(std::memory_order_relaxed) || code == CURLE_ABORTED_BY_CALLBACK)
    return GetResult::Cancelled;
  if (code == CURLE_WRITE_ERROR && !transfer.Failure.empty()) {
    *error = transfer.Failure;
  } else if (code == CURLE_FILESIZE_EXCEEDED) {
    *error = "the download is longer than expected";
  } else if (code == CURLE_HTTP_RETURNED_ERROR) {
    *error = status == 403 || status == 429
                 ? "GitHub refused the request (HTTP " + std::to_string(status) +
                       "): it allows 60 checks an hour per internet address; try again later"
                 : "GitHub answered HTTP " + std::to_string(status);
  } else {
    *error = message[0] != '\0' ? std::string(message.data()) : curl_easy_strerror(code);
  }
  return GetResult::Failed;
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
