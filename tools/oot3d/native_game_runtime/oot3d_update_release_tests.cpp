// Host tests for the updater's pure part (oot3d_update_release.h): versions,
// channels, release.json, release selection and the install steps on a
// temporary folder.
#include "oot3d_update_release.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
using namespace Oot3dNativeGame::Update;
namespace fs = std::filesystem;

int gFailures = 0;

void Require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

int Compare(const char *a, const char *b) {
  return CompareSemVer(*ParseSemVer(a), *ParseSemVer(b));
}

void TestVersions() {
  // Semver 2.0.0's precedence example, plus the release scheme.
  const std::vector<const char *> ordered{
      "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",  "1.0.0-beta.2",
      "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0-test.1",     "1.0.0-test.2", "1.0.0-test.10",
      "1.0.0",       "1.0.1-test.1", "1.0.1",           "1.1.0",       "2.0.0"};
  for (size_t i = 0; i < ordered.size(); ++i) {
    Require(ParseSemVer(ordered[i]).has_value(), "parse a valid version");
    for (size_t j = 0; j < ordered.size(); ++j) {
      const int expected = i < j ? -1 : (i > j ? 1 : 0);
      if (Compare(ordered[i], ordered[j]) != expected) {
        std::fprintf(stderr, "  %s vs %s\n", ordered[i], ordered[j]);
        Require(false, "semver precedence");
      }
    }
  }
  Require(Compare("1.0.0+abc", "1.0.0+def") == 0, "build metadata is ignored");
  for (const char *invalid : {"", "1", "1.0", "1.0.0.0", "01.0.0", "1.0.0-", "1.0.0-01",
                              "v1.0.0", "1.0.0-a..b", "1.0.0+", "1.0.x", "-1.0.0"})
    Require(!ParseSemVer(invalid).has_value(), "reject an invalid version");
  Require(IsReleaseVersion("1.0.0-test.10"), "13 characters fit the NACP");
  Require(!IsReleaseVersion("10.20.30-test.40"), "16 characters do not fit the NACP");
}

void TestChannels() {
  Require(ParseChannel("stable\n") == Channel::Stable, "parse stable");
  Require(ParseChannel(" test ") == Channel::Test, "parse test");
  Require(!ParseChannel("beta").has_value(), "reject other channels");
  Require(DefaultChannelFor("1.0.0-test.1") == Channel::Test, "a test build starts on Test");
  Require(DefaultChannelFor("1.0.0") == Channel::Stable, "a release starts on Stable");
  Require(DefaultChannelFor("garbage") == Channel::Stable, "unknown versions start on Stable");
}

void TestPaths() {
  for (const char *good : {"oot3d_native_game.nro", "resources.o2r", "shadercache/deko3d.pack",
                           "licenses/LICENSE.txt"})
    Require(IsManagedInstallPath(good), "accept a runtime path");
  for (const char *bad :
       {"", "/oot3d_native_game.nro", "../x", "a/../b", "./x", "game/romfs.bin", "Game/romfs.bin",
        "savedata/save.bin", "logs/perf.txt", "config/renderer", "CONFIG/fps60", "update/pending.json",
        "game_language.json", "oot3d_native_game.nro.old", "x.new", "x.tmp", "a//b", "a\\b",
        "a b", ".hidden", "a/b/c/d/e", "config", "sd:/x"})
    if (IsManagedInstallPath(bad)) {
      std::fprintf(stderr, "  accepted %s\n", bad);
      Require(false, "reject a path outside the runtime");
    }
}

std::string Sha(char digit) { return std::string(64, digit); }

std::string Manifest(const std::string &version, unsigned build, const char *channel,
                     const std::string &files) {
  return R"({"format":"oot3dre_switch_release_v1","version":")" + version +
         R"(","build":)" + std::to_string(build) + R"(,"channel":")" + channel +
         R"(","files":[)" + files + "]}";
}

std::string FileEntry(const char *path, unsigned size, char sha) {
  return std::string(R"({"path":")") + path + R"(","size":)" + std::to_string(size) +
         R"(,"sha256":")" + Sha(sha) + R"("})";
}

const std::string kFiles = FileEntry("oot3d_native_game.nro", 1000, 'a') + "," +
                           FileEntry("shadercache/deko3d.pack", 200, 'b') + "," +
                           FileEntry("resources.o2r", 30, 'c');

void TestManifest() {
  std::string error;
  const auto manifest = ParseReleaseManifest(Manifest("1.0.0", 6, "stable", kFiles), &error);
  Require(manifest.has_value(), "parse a valid release.json");
  if (manifest) {
    Require(manifest->Version == "1.0.0" && manifest->Build == 6U &&
                manifest->ReleaseChannel == Channel::Stable && manifest->Files.size() == 3U,
            "release.json fields");
    Require(manifest->Files[1].AssetName() == "deko3d.pack", "asset name is the file name");
  }
  const std::vector<std::string> invalid{
      "not json",
      R"({"format":"other","version":"1.0.0","build":6,"channel":"stable","files":[]})",
      Manifest("1.0", 6, "stable", kFiles),
      Manifest("1.0.0", 0, "stable", kFiles),
      Manifest("1.0.0", 6, "beta", kFiles),
      Manifest("1.0.0", 6, "stable", ""),
      Manifest("1.0.0", 6, "stable", FileEntry("resources.o2r", 30, 'c')), // no NRO
      Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("game/code.bin", 5, 'd')),
      Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("other/resources.o2r", 5, 'd')),
      Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("x/release.json", 5, 'd')),
      Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("x.bin", 0, 'd')),
      Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("x.bin", 5, 'G')),
  };
  for (const auto &text : invalid) {
    error.clear();
    if (ParseReleaseManifest(text, &error).has_value()) {
      std::fprintf(stderr, "  accepted %s\n", text.c_str());
      Require(false, "reject an invalid release.json");
    } else {
      Require(!error.empty(), "an invalid release.json says why");
    }
  }
}

constexpr const char *kRepo = "JohnUzoka/TriAevum-Switch";

std::string Asset(const std::string &tag, const char *name, unsigned size, const char *digest = nullptr,
                  const char *repo = kRepo) {
  std::string text = std::string(R"({"name":")") + name + R"(","size":)" + std::to_string(size) +
                     R"(,"state":"uploaded","browser_download_url":"https://github.com/)" + repo +
                     "/releases/download/" + tag + "/" + name + "\"";
  if (digest != nullptr) text += std::string(R"(,"digest":")") + digest + "\"";
  return text + "}";
}

std::string FullAssets(const std::string &tag) {
  return Asset(tag, "release.json", 400) + "," + Asset(tag, "oot3d_native_game.nro", 1000) + "," +
         Asset(tag, "deko3d.pack", 200) + "," + Asset(tag, "resources.o2r", 30);
}

std::string Release(const std::string &tag, bool prerelease, const std::string &assets, bool draft = false) {
  return std::string(R"({"tag_name":")") + tag + R"(","body":"notes for )" + tag +
         R"(","draft":)" + (draft ? "true" : "false") + R"(,"prerelease":)" +
         (prerelease ? "true" : "false") + R"(,"assets":[)" + assets + "]}";
}

void TestSelection() {
  const std::string listing =
      "[" + Release("v1.1.0-test.1", true, FullAssets("v1.1.0-test.1")) + "," +
      Release("v1.0.0", false, FullAssets("v1.0.0")) + "," +
      Release("v1.0.0-test.2", true, FullAssets("v1.0.0-test.2")) + "," +
      Release("v2.0.0", false, FullAssets("v2.0.0"), true) + "," +             // draft
      Release("1.5.0", false, FullAssets("1.5.0")) + "," +                     // no "v"
      Release("v1.4.0", false, Asset("v1.4.0", "oot3d_native_game.nro", 9)) + "," + // no release.json
      Release("v1.3.0", false, Asset("v1.3.0", "release.json", 100000)) + "," +     // too large
      Release("v1.2.0", false, Asset("v1.2.0", "release.json", 400, nullptr, "someone/else")) + "]";
  std::string error;
  const auto stable = SelectCandidates(listing, kRepo, Channel::Stable, &error);
  Require(stable.size() == 1U && stable[0].Tag == "v1.0.0", "Stable offers only v1.0.0");
  Require(SelectCandidates(listing, "johnuzoka/triaevum-switch", Channel::Stable, &error).size() == 1U,
          "the repository name is case-insensitive");
  const auto test = SelectCandidates(listing, kRepo, Channel::Test, &error);
  Require(test.size() == 3U, "Test also offers pre-releases");
  if (test.size() == 3U) {
    Require(test[0].Tag == "v1.1.0-test.1" && test[1].Tag == "v1.0.0" && test[2].Tag == "v1.0.0-test.2",
            "highest version first");
    Require(test[0].Notes == "notes for v1.1.0-test.1", "release notes are kept");
    Require(test[0].Prerelease && !test[1].Prerelease, "pre-release flag is kept");
  }
  error.clear();
  Require(SelectCandidates("{}", kRepo, Channel::Test, &error).empty() && !error.empty(),
          "an unreadable listing is an error");
  Require(IsRepositoryName(kRepo) && !IsRepositoryName("a") && !IsRepositoryName("a/b/c") &&
              !IsRepositoryName("a/..") && !IsRepositoryName("a b/c"),
          "repository names");
}

void TestQualify() {
  const std::string listing = "[" + Release("v1.0.0", false, FullAssets("v1.0.0")) + "," +
                              Release("v1.0.0-test.2", true, FullAssets("v1.0.0-test.2")) + "]";
  std::string error;
  const auto candidates = SelectCandidates(listing, kRepo, Channel::Test, &error);
  Require(candidates.size() == 2U, "two candidates");
  if (candidates.size() != 2U) return;
  const auto &release = candidates[0];
  const auto &prerelease = candidates[1];
  auto qualified = QualifyRelease(release, Manifest("1.0.0", 6, "stable", kFiles), &error);
  Require(qualified.has_value(), "a complete release qualifies");
  if (qualified) {
    Require(qualified->Downloads.size() == 3U && qualified->Downloads[1].Name == "deko3d.pack",
            "downloads follow release.json");
    Require(qualified->TotalBytes() == 1230U, "total bytes");
  }
  Require(QualifyRelease(prerelease, Manifest("1.0.0-test.2", 5, "test", kFiles), &error).has_value(),
          "a test release qualifies");
  Require(!QualifyRelease(release, Manifest("1.0.1", 6, "stable", kFiles), &error), "version must match the tag");
  Require(!QualifyRelease(release, Manifest("1.0.0", 6, "test", kFiles), &error),
          "a full release must say stable");
  Require(!QualifyRelease(prerelease, Manifest("1.0.0-test.2", 5, "stable", kFiles), &error),
          "a pre-release must say test");
  Require(!QualifyRelease(release,
                          Manifest("1.0.0", 6, "stable", kFiles + "," + FileEntry("extra.bin", 5, 'd')),
                          &error),
          "every listed file must be an asset");
  Require(!QualifyRelease(release,
                          Manifest("1.0.0", 6, "stable",
                                   FileEntry("oot3d_native_game.nro", 999, 'a') + "," +
                                       FileEntry("resources.o2r", 30, 'c')),
                          &error),
          "asset sizes must match");
  const std::string digested =
      "[" + Release("v1.0.0", false,
                    Asset("v1.0.0", "release.json", 400) + "," +
                        Asset("v1.0.0", "oot3d_native_game.nro", 1000,
                              ("sha256:" + Sha('e')).c_str())) + "]";
  const auto withDigest = SelectCandidates(digested, kRepo, Channel::Stable, &error);
  Require(withDigest.size() == 1U && withDigest[0].Assets[1].Sha256 == Sha('e'), "GitHub digest is read");
  if (withDigest.size() == 1U) {
    const std::string nroOnly = FileEntry("oot3d_native_game.nro", 1000, 'a');
    Require(!QualifyRelease(withDigest[0], Manifest("1.0.0", 6, "stable", nroOnly), &error),
            "a GitHub digest must match release.json");
  }
  Require(ClassifyOffer(5, 6) == OfferKind::Update && ClassifyOffer(6, 6) == OfferKind::Reinstall &&
              ClassifyOffer(7, 6) == OfferKind::Older,
          "offer kinds follow the build number");
}

// An NRO as elf2nro lays one out: header, three segments, the asset section
// with an icon and a NACP showing `version`.
std::string FakeNro(const std::string &version) {
  const size_t image = 0x1000;
  std::string bytes(image, '\0');
  const auto put32 = [&](size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<char>(value >> (8 * i));
  };
  const auto put64 = [&](std::string &into, size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) into[at + i] = static_cast<char>(value >> (8 * i));
  };
  bytes.replace(0x10, 4, "NRO0");
  put32(0x18, image);
  const std::uint32_t segments[3][2] = {{0, 0x800}, {0x800, 0x400}, {0xC00, 0x400}};
  for (int s = 0; s < 3; ++s) {
    put32(0x20 + s * 8, segments[s][0]);
    put32(0x24 + s * 8, segments[s][1]);
  }
  std::string assets(0x38, '\0');
  assets.replace(0, 4, "ASET");
  put64(assets, 0x08, 0x38);
  put64(assets, 0x10, 0x100);
  put64(assets, 0x18, 0x138);
  put64(assets, 0x20, 0x4000);
  std::string nacp(0x4000, '\0');
  nacp.replace(0x3060, version.size(), version);
  return bytes + assets + std::string(0x100, '\xFF') + nacp;
}

void Write(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
}

std::string Read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

ReleaseManifest ManifestFor(const std::string &nro, unsigned build) {
  ReleaseManifest manifest;
  manifest.Version = "1.0.0";
  manifest.Build = build;
  manifest.Files = {{"oot3d_native_game.nro", nro.size(), Sha('a')},
                    {"shadercache/deko3d.pack", 4, Sha('b')},
                    {"resources.o2r", 3, Sha('c')}};
  return manifest;
}

void TestInstall() {
  const fs::path root = fs::temp_directory_path() / ("oot3d_update_tests_" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);
  std::string error;

  // NRO checks.
  const std::string oldNro = FakeNro("1.0.0-test.1");
  const std::string newNro = FakeNro("1.0.0");
  Write(root / "probe.nro", newNro);
  Require(ReadNroDisplayVersion(root / "probe.nro", &error) == std::string("1.0.0"), "read the NACP version");
  Write(root / "probe.nro", newNro.substr(0, newNro.size() - 1));
  Require(!ReadNroDisplayVersion(root / "probe.nro", &error), "a cut NRO has no version");
  Write(root / "probe.nro", "not an nro");
  Require(!ReadNroDisplayVersion(root / "probe.nro", &error), "garbage has no version");

  // ReplaceFile keeps or drops the backup and restores the target on failure.
  Write(root / "a.bin", "old");
  Write(root / "a.bin.new", "new");
  Require(ReplaceFile(root / "a.bin", root / "a.bin.new", true, &error), "replace a file");
  Require(Read(root / "a.bin") == "new" && Read(root / "a.bin.old") == "old" && !fs::exists(root / "a.bin.new"),
          "replace keeps the backup");
  Require(!ReplaceFile(root / "a.bin", root / "missing.new", false, &error), "a missing download fails");
  Require(Read(root / "a.bin") == "new", "a failed replace restores the target");
  Write(root / "b.bin.new", "fresh");
  Require(ReplaceFile(root / "b.bin", root / "b.bin.new", false, &error) && Read(root / "b.bin") == "fresh" &&
              !fs::exists(root / "b.bin.old"),
          "replace creates a missing target");

  // Installed test build, staged release.
  Write(root / "oot3d_native_game.nro", oldNro);
  Write(root / "shadercache/deko3d.pack", "PACK");
  Write(root / "resources.o2r", "OLD");
  Write(root / "savedata/save.bin", "SAVE");
  const auto manifest = ManifestFor(newNro, 6);
  Write(root / "oot3d_native_game.nro.new", newNro);
  Write(root / "shadercache/deko3d.pack.new", "NEW!");
  Write(root / "resources.o2r.new", "NEW");
  Require(CommitStagedRelease(root, manifest, &error), "commit a staged release");
  Require(Read(root / "oot3d_native_game.nro") == newNro && Read(root / "oot3d_native_game.nro.old") == oldNro,
          "the NRO is swapped at once and the old one kept");
  Require(Read(root / "resources.o2r") == "OLD" && fs::exists(root / kPendingJournal),
          "other files wait for the next start");
  // A start of the old build (rolled back by hand) drops them.
  {
    const fs::path copy = root.string() + "_rollback";
    fs::remove_all(copy);
    fs::copy(root, copy, fs::copy_options::recursive);
    const std::string log = ApplyPendingUpdate(copy, 5);
    Require(!log.empty() && Read(copy / "resources.o2r") == "OLD" && !fs::exists(copy / "resources.o2r.new") &&
                !fs::exists(copy / kPendingJournal),
            "another build drops the journal");
    fs::remove_all(copy);
  }
  // The new build finishes the install.
  const std::string log = ApplyPendingUpdate(root, 6);
  Require(log.find("installed resources.o2r") != std::string::npos, "the start-up step logs its work");
  Require(Read(root / "resources.o2r") == "NEW" && Read(root / "shadercache/deko3d.pack") == "NEW!" &&
              !fs::exists(root / "resources.o2r.old") && !fs::exists(root / kPendingJournal),
          "the next start swaps the remaining files");
  Require(Read(root / "savedata/save.bin") == "SAVE", "saves are untouched");
  Require(ApplyPendingUpdate(root, 6).empty(), "nothing left to do");

  // A commit with an incomplete download changes nothing.
  Write(root / "oot3d_native_game.nro.new", FakeNro("1.0.1"));
  Write(root / "shadercache/deko3d.pack.new", "NEW");  // 3 bytes, 4 expected
  Write(root / "resources.o2r.new", "NEW");
  auto next = ManifestFor(FakeNro("1.0.1"), 7);
  Require(!CommitStagedRelease(root, next, &error), "an incomplete download is refused");
  Require(Read(root / "oot3d_native_game.nro") == newNro && !fs::exists(root / "oot3d_native_game.nro.new") &&
              !fs::exists(root / "resources.o2r.new") && !fs::exists(root / kPendingJournal),
          "a refused commit leaves the installed version and no staged files");

  // An interrupted NRO download is removed at start-up.
  Write(root / "oot3d_native_game.nro.new", "partial");
  Require(!ApplyPendingUpdate(root, 6).empty() && !fs::exists(root / "oot3d_native_game.nro.new"),
          "an unfinished NRO download is removed");
  // An unreadable journal is removed.
  Write(root / kPendingJournal, "{");
  Require(!ApplyPendingUpdate(root, 6).empty() && !fs::exists(root / kPendingJournal),
          "an unreadable journal is removed");
  fs::remove_all(root);
}
} // namespace

int main() {
  TestVersions();
  TestChannels();
  TestPaths();
  TestManifest();
  TestSelection();
  TestQualify();
  TestInstall();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::puts("oot3d_update_release_tests: ok");
  return EXIT_SUCCESS;
}
