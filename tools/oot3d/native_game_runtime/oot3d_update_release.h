#pragma once
// Over-the-air updates for the Switch build: the pure part (no network, no
// libnx), shared by the updater (oot3d_switch_updater.h), the start-up step
// that finishes an install and the host tests (oot3d_update_release_tests.cpp).
//
// A release is a GitHub release tagged `v<version>` with a `release.json`
// asset (ReleaseManifest) and one asset per file it lists, named after the
// file's last path component. The build number orders releases; the version
// is what players see.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame::Update {

// --- Versions -------------------------------------------------------------

// Semantic version 2.0.0: MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD].
struct SemVer {
  std::uint64_t Major = 0;
  std::uint64_t Minor = 0;
  std::uint64_t Patch = 0;
  std::vector<std::string> Prerelease; // dot-separated identifiers
};
std::optional<SemVer> ParseSemVer(std::string_view text);
// Semver precedence: <0, 0 or >0. Build metadata is ignored.
int CompareSemVer(const SemVer &left, const SemVer &right);
// A version a release may carry: valid semver that fits the NACP's 16-byte
// display version (15 characters and the terminator).
bool IsReleaseVersion(std::string_view text);

// --- Channels -------------------------------------------------------------

// Stable offers only full releases; Test also offers pre-releases.
enum class Channel { Stable, Test };
const char *ChannelName(Channel channel); // "stable" / "test"
std::optional<Channel> ParseChannel(std::string_view text);
// The channel a build starts on before the player picks one: Test for a
// pre-release version, Stable otherwise.
Channel DefaultChannelFor(std::string_view installedVersion);

// --- release.json ---------------------------------------------------------

inline constexpr std::string_view kManifestFormat = "oot3dre_switch_release_v1";
inline constexpr std::string_view kManifestAsset = "release.json";
inline constexpr std::uint64_t kManifestMaxBytes = 64U << 10U;
// The runtime itself; every release carries it.
inline constexpr std::string_view kNroPath = "oot3d_native_game.nro";
inline constexpr std::string_view kDefaultRepository = "JohnUzoka/TriAevum-Switch";

struct ReleaseFile {
  std::string Path;   // relative to sdmc:/switch/oot3dre/, '/'-separated
  std::uint64_t Size = 0;
  std::string Sha256; // 64 lowercase hex digits
  // The release asset holding it: the last component of Path.
  std::string AssetName() const;
};

struct ReleaseManifest {
  std::string Version;
  std::uint64_t Build = 0;
  Channel ReleaseChannel = Channel::Stable;
  std::vector<ReleaseFile> Files;
};

// Whether a release may write `relative` (under sdmc:/switch/oot3dre/): a
// plain relative path of [A-Za-z0-9._-] components, outside the player's
// data (game/, savedata/ and every savedata* sibling such as
// savedata-backups/ and savedata.format, savestates/, logs/, config/,
// game_language.json, update/) and never one of the updater's own .new/.old
// names.
bool IsManagedInstallPath(std::string_view relative);
// Parses and checks a release.json: format, version, build > 0, channel,
// managed and unique paths with unique asset names, sizes > 0, sha256 hex,
// and the NRO among the files.
std::optional<ReleaseManifest> ParseReleaseManifest(std::string_view json, std::string *error);

// --- GitHub's release listing ---------------------------------------------

struct ReleaseAsset {
  std::string Name;
  std::uint64_t Size = 0;
  std::string Url;    // browser_download_url (redirects to GitHub's CDN)
  std::string Sha256; // from GitHub's "digest" when it lists one, else empty
};

struct ReleaseCandidate {
  std::string Tag;
  std::string Version; // the tag without its "v"
  SemVer ParsedVersion;
  bool Prerelease = false;
  std::string Notes; // the release body, as written on GitHub
  std::vector<ReleaseAsset> Assets;
  const ReleaseAsset *FindAsset(std::string_view name) const;
};

// The releases a check may offer on `channel`, from GET
// /repos/{repository}/releases: published (not drafts), pre-releases only on
// Test, tagged `v<release version>`, with an uploaded release.json no larger
// than kManifestMaxBytes, and only assets downloaded from this repository's
// releases. Highest version first.
std::vector<ReleaseCandidate> SelectCandidates(std::string_view listingJson,
                                               std::string_view repository, Channel channel,
                                               std::string *error);
// A valid repository name, "owner/name".
bool IsRepositoryName(std::string_view text);

struct QualifiedRelease {
  ReleaseCandidate Release;
  ReleaseManifest Manifest;
  // Parallel to Manifest.Files.
  std::vector<ReleaseAsset> Downloads;
  std::uint64_t TotalBytes() const;
};

// A candidate whose release.json is valid, says the tag's version and the
// release's channel (pre-release = test), and whose every file is an asset
// of the listed size (and SHA-256, when GitHub lists a digest).
std::optional<QualifiedRelease> QualifyRelease(const ReleaseCandidate &candidate,
                                               std::string_view manifestJson,
                                               std::string *error);

enum class OfferKind {
  Update,    // a newer build
  Reinstall, // the installed build again (repair)
  Older,     // an older build (e.g. leaving the Test channel)
};
OfferKind ClassifyOffer(std::uint64_t installedBuild, std::uint64_t releaseBuild);

// --- Installing -----------------------------------------------------------
// Downloads go to `<file>.new` beside each file. The NRO is swapped at once
// (the homebrew loader read the whole NRO into memory, so the running file is
// never read again), keeping `<nro>.old` for rollback. The other files may be
// open while the game runs (resources.o2r is), and Horizon cannot rename or
// delete an open file, so they are listed in update/pending.json and swapped
// at the next start, before anything opens them.

inline constexpr std::string_view kPendingJournal = "update/pending.json";

std::filesystem::path StagedPath(const std::filesystem::path &target); // <target>.new
std::filesystem::path BackupPath(const std::filesystem::path &target); // <target>.old

// Puts `staged` in `target`'s place. Horizon cannot rename over an existing
// file, so the target moves to <target>.old first; if the second rename
// fails, it moves back. `keepBackup` keeps <target>.old afterwards.
bool ReplaceFile(const std::filesystem::path &target, const std::filesystem::path &staged,
                 bool keepBackup, std::string *error);

// The display version in an NRO's NACP, once the file is a whole NRO
// (header, asset section and NACP inside the file).
std::optional<std::string> ReadNroDisplayVersion(const std::filesystem::path &nro,
                                                 std::string *error);

// After every file of `manifest` was downloaded and checked to its
// StagedPath under `root`: journals the files other than the NRO for
// `manifest.Build`, then swaps the NRO. On failure nothing changes: the
// journal and every staged file are removed.
bool CommitStagedRelease(const std::filesystem::path &root, const ReleaseManifest &manifest,
                         std::string *error);
// Removes the staged downloads of `manifest` (cancel or failure).
void DiscardStagedFiles(const std::filesystem::path &root, const ReleaseManifest &manifest);

// At start-up, before any managed file is opened: finishes a journal written
// for `runningBuild` (each staged file of the right size takes its place),
// or drops one written for another build (the NRO swap never happened), and
// removes a staged NRO left by an interrupted download. Returns what it did,
// for the log; empty when there was nothing to do.
std::string ApplyPendingUpdate(const std::filesystem::path &root, std::uint64_t runningBuild);

} // namespace Oot3dNativeGame::Update
