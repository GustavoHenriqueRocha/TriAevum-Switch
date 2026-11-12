#include "oot3d_update_release.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame::Update {
namespace {
using Json = nlohmann::json;

void SetError(std::string *error, std::string text) {
  if (error != nullptr) *error = std::move(text);
}

std::vector<std::string_view> Split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  size_t start = 0;
  while (true) {
    const size_t end = text.find(separator, start);
    parts.push_back(text.substr(start, end == std::string_view::npos ? text.npos : end - start));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

bool IsDigits(std::string_view text) {
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool IsIdentifier(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-';
  });
}

// A numeric identifier: digits without a leading zero (except "0").
std::optional<std::uint64_t> ParseNumber(std::string_view text) {
  if (!IsDigits(text) || (text.size() > 1U && text.front() == '0') || text.size() > 19U)
    return std::nullopt;
  std::uint64_t value = 0;
  for (char c : text) value = value * 10U + static_cast<std::uint64_t>(c - '0');
  return value;
}

std::string Lower(std::string_view text) {
  std::string lower(text);
  for (char &c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower;
}

bool IsSha256Hex(std::string_view text) {
  return text.size() == 64U && std::all_of(text.begin(), text.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

const Json *Member(const Json &object, const char *name) {
  if (!object.is_object()) return nullptr;
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &*found;
}

std::optional<std::string> StringMember(const Json &object, const char *name) {
  const Json *value = Member(object, name);
  if (value == nullptr || !value->is_string()) return std::nullopt;
  return value->get<std::string>();
}

std::optional<std::uint64_t> CountMember(const Json &object, const char *name) {
  const Json *value = Member(object, name);
  if (value == nullptr || !value->is_number_unsigned()) return std::nullopt;
  return value->get<std::uint64_t>();
}

bool BoolMember(const Json &object, const char *name) {
  const Json *value = Member(object, name);
  return value != nullptr && value->is_boolean() && value->get<bool>();
}

std::string PathText(const std::filesystem::path &path) { return path.generic_string(); }

std::filesystem::path WithSuffix(const std::filesystem::path &path, const char *suffix) {
  std::filesystem::path result = path;
  result += suffix;
  return result;
}

std::optional<std::uint64_t> FileSize(const std::filesystem::path &path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) return std::nullopt;
  const auto size = std::filesystem::file_size(path, error);
  if (error) return std::nullopt;
  return static_cast<std::uint64_t>(size);
}

void RemoveQuietly(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
}

// Horizon cannot rename over an existing file: remove the destination first.
bool WriteFileReplacing(const std::filesystem::path &path, const std::string &text,
                        std::string *error) {
  std::error_code code;
  std::filesystem::create_directories(path.parent_path(), code);
  const auto temporary = WithSuffix(path, ".tmp");
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    file << text;
    file.flush();
    if (!file) {
      SetError(error, "cannot write " + PathText(temporary));
      RemoveQuietly(temporary);
      return false;
    }
  }
  std::filesystem::remove(path, code);
  std::filesystem::rename(temporary, path, code);
  if (code) {
    SetError(error, "cannot rename " + PathText(temporary) + ": " + code.message());
    RemoveQuietly(temporary);
    return false;
  }
  return true;
}

std::uint32_t U32At(const unsigned char *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | static_cast<std::uint32_t>(bytes[1]) << 8U |
         static_cast<std::uint32_t>(bytes[2]) << 16U | static_cast<std::uint32_t>(bytes[3]) << 24U;
}

std::uint64_t U64At(const unsigned char *bytes) {
  return static_cast<std::uint64_t>(U32At(bytes)) |
         static_cast<std::uint64_t>(U32At(bytes + 4)) << 32U;
}

constexpr const char *kPendingFormat = "oot3dre_switch_pending_v1";
} // namespace

// --- Versions -------------------------------------------------------------

std::optional<SemVer> ParseSemVer(std::string_view text) {
  if (const size_t plus = text.find('+'); plus != std::string_view::npos) {
    for (const auto identifier : Split(text.substr(plus + 1), '.'))
      if (!IsIdentifier(identifier)) return std::nullopt;
    text = text.substr(0, plus);
  }
  std::string_view prerelease;
  if (const size_t dash = text.find('-'); dash != std::string_view::npos) {
    prerelease = text.substr(dash + 1);
    if (prerelease.empty()) return std::nullopt;
    text = text.substr(0, dash);
  }
  const auto core = Split(text, '.');
  if (core.size() != 3U) return std::nullopt;
  SemVer version;
  const auto major = ParseNumber(core[0]);
  const auto minor = ParseNumber(core[1]);
  const auto patch = ParseNumber(core[2]);
  if (!major || !minor || !patch) return std::nullopt;
  version.Major = *major;
  version.Minor = *minor;
  version.Patch = *patch;
  if (!prerelease.empty()) {
    for (const auto identifier : Split(prerelease, '.')) {
      if (!IsIdentifier(identifier)) return std::nullopt;
      if (IsDigits(identifier) && !ParseNumber(identifier)) return std::nullopt;
      version.Prerelease.emplace_back(identifier);
    }
  }
  return version;
}

int CompareSemVer(const SemVer &left, const SemVer &right) {
  const auto compare = [](std::uint64_t a, std::uint64_t b) { return a < b ? -1 : (a > b ? 1 : 0); };
  if (const int core = compare(left.Major, right.Major)) return core;
  if (const int core = compare(left.Minor, right.Minor)) return core;
  if (const int core = compare(left.Patch, right.Patch)) return core;
  // A pre-release precedes its release.
  if (left.Prerelease.empty() || right.Prerelease.empty())
    return compare(left.Prerelease.empty() ? 1U : 0U, right.Prerelease.empty() ? 1U : 0U);
  const size_t shared = std::min(left.Prerelease.size(), right.Prerelease.size());
  for (size_t index = 0; index < shared; ++index) {
    const auto &a = left.Prerelease[index];
    const auto &b = right.Prerelease[index];
    const bool numericA = IsDigits(a);
    const bool numericB = IsDigits(b);
    if (numericA && numericB) {
      if (const int order = compare(*ParseNumber(a), *ParseNumber(b))) return order;
    } else if (numericA != numericB) {
      return numericA ? -1 : 1; // numeric identifiers sort first
    } else if (const int order = a.compare(b)) {
      return order < 0 ? -1 : 1;
    }
  }
  return compare(left.Prerelease.size(), right.Prerelease.size());
}

bool IsReleaseVersion(std::string_view text) {
  return !text.empty() && text.size() <= 15U && ParseSemVer(text).has_value();
}

// --- Channels -------------------------------------------------------------

const char *ChannelName(Channel channel) { return channel == Channel::Test ? "test" : "stable"; }

std::optional<Channel> ParseChannel(std::string_view text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
  if (text == "stable") return Channel::Stable;
  if (text == "test") return Channel::Test;
  return std::nullopt;
}

Channel DefaultChannelFor(std::string_view installedVersion) {
  const auto version = ParseSemVer(installedVersion);
  return version && !version->Prerelease.empty() ? Channel::Test : Channel::Stable;
}

// --- release.json ---------------------------------------------------------

std::string ReleaseFile::AssetName() const {
  const size_t slash = Path.rfind('/');
  return slash == std::string::npos ? Path : Path.substr(slash + 1);
}

bool IsManagedInstallPath(std::string_view relative) {
  if (relative.empty() || relative.size() > 200U) return false;
  const auto components = Split(relative, '/');
  if (components.size() > 4U) return false;
  for (const auto component : components) {
    if (component.empty() || component.front() == '.') return false;
    if (!std::all_of(component.begin(), component.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 c == '.' || c == '_' || c == '-';
        }))
      return false;
  }
  // FAT is case-insensitive: compare lower case.
  const std::string lower = Lower(relative);
  const std::string first = Lower(components.front());
  static constexpr std::array<std::string_view, 8> kPlayerDirectories{
      "game", "savedata", "savestates", "logs", "config", "update", "lab", "bench"};
  // The player's data, also as a plain file name.
  if (std::find(kPlayerDirectories.begin(), kPlayerDirectories.end(), first) != kPlayerDirectories.end())
    return false;
  // The save's siblings: savedata-backups/ (rotating backups, incomplete
  // writes, formatted saves) and savedata.format / savedata.icon
  // (oot3d_native_a32_ctr_save_safety.h). Anything named savedata* is the
  // player's.
  if (first.starts_with("savedata")) return false;
  if (lower == "game_language.json" || lower == "boot-status.txt") return false;
  const auto endsWith = [&](std::string_view suffix) {
    return lower.size() >= suffix.size() && lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  return !endsWith(".new") && !endsWith(".old") && !endsWith(".tmp");
}

std::optional<ReleaseManifest> ParseReleaseManifest(std::string_view json, std::string *error) {
  const Json root = Json::parse(json.begin(), json.end(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    SetError(error, "release.json is not a JSON object");
    return std::nullopt;
  }
  if (StringMember(root, "format") != std::string(kManifestFormat)) {
    SetError(error, "release.json is not format " + std::string(kManifestFormat));
    return std::nullopt;
  }
  ReleaseManifest manifest;
  const auto version = StringMember(root, "version");
  if (!version || !IsReleaseVersion(*version)) {
    SetError(error, "release.json has no valid version");
    return std::nullopt;
  }
  manifest.Version = *version;
  const auto build = CountMember(root, "build");
  if (!build || *build == 0U) {
    SetError(error, "release.json has no build number");
    return std::nullopt;
  }
  manifest.Build = *build;
  const auto channel = StringMember(root, "channel");
  const auto parsedChannel = channel ? ParseChannel(*channel) : std::nullopt;
  if (!parsedChannel) {
    SetError(error, "release.json has no channel (stable or test)");
    return std::nullopt;
  }
  manifest.ReleaseChannel = *parsedChannel;
  const Json *files = Member(root, "files");
  if (files == nullptr || !files->is_array() || files->empty()) {
    SetError(error, "release.json lists no files");
    return std::nullopt;
  }
  std::vector<std::string> seenPaths;
  std::vector<std::string> seenAssets;
  for (const Json &entry : *files) {
    ReleaseFile file;
    const auto path = StringMember(entry, "path");
    const auto size = CountMember(entry, "size");
    const auto sha256 = StringMember(entry, "sha256");
    if (!path || !IsManagedInstallPath(*path)) {
      SetError(error, "release.json lists a file outside the updater's paths: " + path.value_or("(none)"));
      return std::nullopt;
    }
    if (!size || *size == 0U || !sha256 || !IsSha256Hex(*sha256)) {
      SetError(error, "release.json has no size or sha256 for " + *path);
      return std::nullopt;
    }
    file.Path = *path;
    file.Size = *size;
    file.Sha256 = *sha256;
    const std::string lowerPath = Lower(file.Path);
    const std::string lowerAsset = Lower(file.AssetName());
    if (std::find(seenPaths.begin(), seenPaths.end(), lowerPath) != seenPaths.end() ||
        std::find(seenAssets.begin(), seenAssets.end(), lowerAsset) != seenAssets.end() ||
        lowerAsset == kManifestAsset) {
      SetError(error, "release.json lists " + file.Path + " twice or under a reserved name");
      return std::nullopt;
    }
    seenPaths.push_back(lowerPath);
    seenAssets.push_back(lowerAsset);
    manifest.Files.push_back(std::move(file));
  }
  if (std::find(seenPaths.begin(), seenPaths.end(), kNroPath) == seenPaths.end()) {
    SetError(error, "release.json does not list " + std::string(kNroPath));
    return std::nullopt;
  }
  return manifest;
}

// --- GitHub's release listing ---------------------------------------------

const ReleaseAsset *ReleaseCandidate::FindAsset(std::string_view name) const {
  for (const auto &asset : Assets)
    if (asset.Name == name) return &asset;
  return nullptr;
}

bool IsRepositoryName(std::string_view text) {
  const auto parts = Split(text, '/');
  if (parts.size() != 2U) return false;
  for (const auto part : parts) {
    if (part.empty() || part.size() > 100U || part == "." || part == "..") return false;
    if (!std::all_of(part.begin(), part.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                 c == '.' || c == '_' || c == '-';
        }))
      return false;
  }
  return true;
}

std::vector<ReleaseCandidate> SelectCandidates(std::string_view listingJson,
                                               std::string_view repository, Channel channel,
                                               std::string *error) {
  std::vector<ReleaseCandidate> candidates;
  const Json listing = Json::parse(listingJson.begin(), listingJson.end(), nullptr, false);
  if (listing.is_discarded() || !listing.is_array()) {
    SetError(error, "GitHub's release list is unreadable");
    return candidates;
  }
  const std::string downloads =
      "https://github.com/" + std::string(repository) + "/releases/download/";
  for (const Json &release : listing) {
    if (!release.is_object() || BoolMember(release, "draft")) continue;
    ReleaseCandidate candidate;
    candidate.Prerelease = BoolMember(release, "prerelease");
    if (candidate.Prerelease && channel == Channel::Stable) continue;
    const auto tag = StringMember(release, "tag_name");
    if (!tag || tag->size() < 2U || tag->front() != 'v') continue;
    candidate.Tag = *tag;
    candidate.Version = tag->substr(1);
    if (!IsReleaseVersion(candidate.Version)) continue;
    candidate.ParsedVersion = *ParseSemVer(candidate.Version);
    candidate.Notes = StringMember(release, "body").value_or("");
    const Json *assets = Member(release, "assets");
    if (assets == nullptr || !assets->is_array()) continue;
    // GitHub names are case-insensitive: config/update_repo may differ in
    // case from the URLs GitHub lists; the tag must match exactly.
    const std::string repositoryPrefix = Lower(downloads);
    const std::string tagPrefix = candidate.Tag + "/";
    for (const Json &entry : *assets) {
      ReleaseAsset asset;
      const auto name = StringMember(entry, "name");
      const auto size = CountMember(entry, "size");
      const auto url = StringMember(entry, "browser_download_url");
      const auto state = StringMember(entry, "state");
      if (!name || !size || !url || (state && *state != "uploaded")) continue;
      if (url->size() <= repositoryPrefix.size() + tagPrefix.size() ||
          Lower(std::string_view(*url).substr(0, repositoryPrefix.size())) != repositoryPrefix ||
          url->compare(repositoryPrefix.size(), tagPrefix.size(), tagPrefix) != 0)
        continue;
      asset.Name = *name;
      asset.Size = *size;
      asset.Url = *url;
      if (const auto digest = StringMember(entry, "digest");
          digest && digest->compare(0, 7, "sha256:") == 0 && IsSha256Hex(digest->substr(7)))
        asset.Sha256 = digest->substr(7);
      candidate.Assets.push_back(std::move(asset));
    }
    const ReleaseAsset *manifest = candidate.FindAsset(kManifestAsset);
    if (manifest == nullptr || manifest->Size == 0U || manifest->Size > kManifestMaxBytes) continue;
    candidates.push_back(std::move(candidate));
  }
  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const ReleaseCandidate &a, const ReleaseCandidate &b) {
                     return CompareSemVer(a.ParsedVersion, b.ParsedVersion) > 0;
                   });
  return candidates;
}

std::uint64_t QualifiedRelease::TotalBytes() const {
  std::uint64_t total = 0;
  for (const auto &file : Manifest.Files) total += file.Size;
  return total;
}

std::optional<QualifiedRelease> QualifyRelease(const ReleaseCandidate &candidate,
                                               std::string_view manifestJson,
                                               std::string *error) {
  std::string reason;
  auto manifest = ParseReleaseManifest(manifestJson, &reason);
  if (!manifest) {
    SetError(error, candidate.Tag + ": " + reason);
    return std::nullopt;
  }
  if (manifest->Version != candidate.Version) {
    SetError(error, candidate.Tag + ": release.json is for version " + manifest->Version);
    return std::nullopt;
  }
  const Channel expected = candidate.Prerelease ? Channel::Test : Channel::Stable;
  if (manifest->ReleaseChannel != expected) {
    SetError(error, candidate.Tag + ": release.json says channel " +
                        ChannelName(manifest->ReleaseChannel) + ", GitHub says " +
                        ChannelName(expected));
    return std::nullopt;
  }
  QualifiedRelease qualified;
  for (const auto &file : manifest->Files) {
    const ReleaseAsset *asset = candidate.FindAsset(file.AssetName());
    if (asset == nullptr) {
      SetError(error, candidate.Tag + ": asset " + file.AssetName() + " is missing");
      return std::nullopt;
    }
    if (asset->Size != file.Size || (!asset->Sha256.empty() && asset->Sha256 != file.Sha256)) {
      SetError(error, candidate.Tag + ": asset " + file.AssetName() + " does not match release.json");
      return std::nullopt;
    }
    qualified.Downloads.push_back(*asset);
  }
  qualified.Release = candidate;
  qualified.Manifest = std::move(*manifest);
  return qualified;
}

OfferKind ClassifyOffer(std::uint64_t installedBuild, std::uint64_t releaseBuild) {
  if (releaseBuild > installedBuild) return OfferKind::Update;
  return releaseBuild == installedBuild ? OfferKind::Reinstall : OfferKind::Older;
}

// --- Installing -----------------------------------------------------------

std::filesystem::path StagedPath(const std::filesystem::path &target) {
  return WithSuffix(target, ".new");
}

std::filesystem::path BackupPath(const std::filesystem::path &target) {
  return WithSuffix(target, ".old");
}

bool ReplaceFile(const std::filesystem::path &target, const std::filesystem::path &staged,
                 bool keepBackup, std::string *error) {
  std::error_code code;
  const auto backup = BackupPath(target);
  if (std::filesystem::exists(backup, code)) {
    std::filesystem::remove(backup, code);
    if (code) {
      SetError(error, "cannot remove " + PathText(backup) + ": " + code.message());
      return false;
    }
  }
  const bool hadTarget = std::filesystem::exists(target, code);
  if (hadTarget) {
    std::filesystem::rename(target, backup, code);
    if (code) {
      SetError(error, "cannot move " + PathText(target) + " aside: " + code.message());
      return false;
    }
  }
  std::filesystem::rename(staged, target, code);
  if (code) {
    SetError(error, "cannot move " + PathText(staged) + " into place: " + code.message());
    if (hadTarget) {
      std::error_code ignored;
      std::filesystem::rename(backup, target, ignored);
    }
    return false;
  }
  if (hadTarget && !keepBackup) RemoveQuietly(backup);
  return true;
}

std::optional<std::string> ReadNroDisplayVersion(const std::filesystem::path &nro,
                                                 std::string *error) {
  // libnx nro.h / nacp.h: NroStart (0x10) + NroHeader (0x70); after the
  // image, NroAssetHeader with the NACP's offset and size; the NACP's
  // display_version is 0x10 bytes at 0x3060.
  constexpr std::uint64_t kHeader = 0x80U, kAssetHeader = 0x38U, kNacpSize = 0x4000U;
  constexpr std::uint64_t kDisplayVersion = 0x3060U;
  const auto length = FileSize(nro);
  std::ifstream file(nro, std::ios::binary);
  if (!length || !file) {
    SetError(error, "cannot open " + PathText(nro));
    return std::nullopt;
  }
  std::array<unsigned char, kHeader> header{};
  if (!file.read(reinterpret_cast<char *>(header.data()), header.size()) ||
      std::memcmp(header.data() + 0x10, "NRO0", 4) != 0) {
    SetError(error, "no NRO header");
    return std::nullopt;
  }
  const std::uint64_t image = U32At(header.data() + 0x18);
  if (image < kHeader || image > *length) {
    SetError(error, "NRO image size past the file");
    return std::nullopt;
  }
  for (size_t segment = 0; segment < 3U; ++segment) {
    const std::uint64_t offset = U32At(header.data() + 0x20 + segment * 8U);
    const std::uint64_t size = U32At(header.data() + 0x24 + segment * 8U);
    if (offset + size > image) {
      SetError(error, "NRO segment past the image");
      return std::nullopt;
    }
  }
  std::array<unsigned char, kAssetHeader> assets{};
  file.seekg(static_cast<std::streamoff>(image));
  if (!file.read(reinterpret_cast<char *>(assets.data()), assets.size()) ||
      std::memcmp(assets.data(), "ASET", 4) != 0) {
    SetError(error, "no NRO asset section");
    return std::nullopt;
  }
  const std::uint64_t nacp = U64At(assets.data() + 0x18);
  const std::uint64_t nacpSize = U64At(assets.data() + 0x20);
  if (nacpSize != kNacpSize || nacp > *length || image + nacp + kNacpSize > *length) {
    SetError(error, "no whole NACP");
    return std::nullopt;
  }
  std::array<char, 0x10> version{};
  file.seekg(static_cast<std::streamoff>(image + nacp + kDisplayVersion));
  if (!file.read(version.data(), version.size())) {
    SetError(error, "cannot read the NACP");
    return std::nullopt;
  }
  return std::string(version.data(), strnlen(version.data(), version.size()));
}

void DiscardStagedFiles(const std::filesystem::path &root, const ReleaseManifest &manifest) {
  for (const auto &file : manifest.Files) RemoveQuietly(StagedPath(root / file.Path));
}

bool CommitStagedRelease(const std::filesystem::path &root, const ReleaseManifest &manifest,
                         std::string *error) {
  const auto fail = [&](std::string text) {
    RemoveQuietly(root / kPendingJournal);
    DiscardStagedFiles(root, manifest);
    SetError(error, std::move(text));
    return false;
  };
  for (const auto &file : manifest.Files) {
    if (FileSize(StagedPath(root / file.Path)) != file.Size)
      return fail("the download of " + file.Path + " is missing or incomplete");
  }
  Json pending = {{"format", kPendingFormat},
                  {"version", manifest.Version},
                  {"build", manifest.Build},
                  {"files", Json::array()}};
  for (const auto &file : manifest.Files) {
    if (file.Path == kNroPath) continue;
    pending["files"].push_back({{"path", file.Path}, {"size", file.Size}});
  }
  std::string reason;
  if (!pending["files"].empty() &&
      !WriteFileReplacing(root / kPendingJournal, pending.dump(2) + "\n", &reason))
    return fail(reason);
  if (!ReplaceFile(root / kNroPath, StagedPath(root / kNroPath), true, &reason))
    return fail(reason);
  return true;
}

std::string ApplyPendingUpdate(const std::filesystem::path &root, std::uint64_t runningBuild) {
  std::string log;
  const auto note = [&](const std::string &line) { log += line + "\n"; };
  std::error_code code;
  const auto stagedNro = StagedPath(root / kNroPath);
  if (std::filesystem::exists(stagedNro, code)) {
    RemoveQuietly(stagedNro);
    note("removed an unfinished download: " + PathText(stagedNro));
  }
  const auto journalPath = root / kPendingJournal;
  if (!std::filesystem::exists(journalPath, code)) return log;
  std::string text;
  {
    std::ifstream file(journalPath, std::ios::binary);
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  const Json journal = Json::parse(text, nullptr, false);
  // Member() and CountMember() answer nothing for a discarded (invalid) value.
  const Json *files = Member(journal, "files");
  const auto parsedBuild = CountMember(journal, "build");
  if (StringMember(journal, "format") != std::string(kPendingFormat) || files == nullptr ||
      !files->is_array() || !parsedBuild) {
    RemoveQuietly(journalPath);
    note("removed an unreadable " + std::string(kPendingJournal));
    return log;
  }
  const std::uint64_t build = *parsedBuild;
  const bool forThisBuild = build == runningBuild;
  bool complete = true;
  for (const Json &entry : *files) {
    const auto path = StringMember(entry, "path");
    const auto size = CountMember(entry, "size");
    if (!path || !size || !IsManagedInstallPath(*path) || *path == kNroPath) continue;
    const auto target = root / *path;
    const auto staged = StagedPath(target);
    if (!std::filesystem::exists(staged, code)) continue; // already in place
    if (!forThisBuild || FileSize(staged) != *size) {
      RemoveQuietly(staged);
      note("dropped " + *path + (forThisBuild ? " (wrong size)" : ""));
      continue;
    }
    std::string reason;
    if (ReplaceFile(target, staged, false, &reason)) {
      note("installed " + *path);
    } else {
      complete = false;
      note("could not install " + *path + ": " + reason);
    }
  }
  if (!forThisBuild)
    note("dropped update files for build " + std::to_string(build) + " (running build " +
         std::to_string(runningBuild) + ")");
  if (complete) RemoveQuietly(journalPath);
  return log;
}

} // namespace Oot3dNativeGame::Update
