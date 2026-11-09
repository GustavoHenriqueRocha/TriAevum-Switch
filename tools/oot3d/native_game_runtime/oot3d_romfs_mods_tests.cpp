// RomFS mods (oot3d_romfs_mods.h): the composer's scan, path mapping,
// classification, O3RO overlay and receipt, read back through the runtime's
// overlay (oot3d_native_a32_romfs_overlay.h) over Level-3 images made like
// game/romfs.bin: a EUR dump's IVFC image (view at 0x1000) and a USA dump's
// converted view (at 0). Plain main(); exits non-zero on failure.
#include "oot3d_native_a32_romfs_overlay.h"
#include "oot3d_romfs_mods.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(x) Check((x), #x, __LINE__)

void Put16(Bytes &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Put32(Bytes &out, std::uint32_t value) {
  Put16(out, static_cast<std::uint16_t>(value));
  Put16(out, static_cast<std::uint16_t>(value >> 16));
}
void Put64(Bytes &out, std::uint64_t value) {
  Put32(out, static_cast<std::uint32_t>(value));
  Put32(out, static_cast<std::uint32_t>(value >> 32));
}
std::uint32_t Get32(const Bytes &bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (int i = 3; i >= 0; --i) value = (value << 8) | bytes[offset + static_cast<std::size_t>(i)];
  return value;
}
std::uint64_t Get64(const Bytes &bytes, std::size_t offset) {
  return Get32(bytes, offset) | (static_cast<std::uint64_t>(Get32(bytes, offset + 4)) << 32);
}

Bytes Text(std::string_view text) { return Bytes(text.begin(), text.end()); }
Bytes Filled(std::size_t size, std::uint8_t seed) {
  Bytes out(size);
  for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(seed + i * 7U);
  return out;
}

void WriteFile(const fs::path &path, const Bytes &data) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}
Bytes ReadFile(const fs::path &path) {
  std::ifstream file(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// A Level-3 RomFS image holding `files` (path -> data); child lists only
// (as oot3d_topscreen_import_tests' MakeRomFs).
Bytes MakeRomFs(const std::map<std::string, Bytes> &files) {
  struct Directory {
    std::string Name;
    int Parent = -1;
    std::vector<int> Children;
    std::vector<std::pair<std::string, const Bytes *>> Files;
    std::uint32_t Offset = 0;
  };
  std::vector<Directory> directories(1);
  for (const auto &[path, data] : files) {
    int current = 0;
    std::size_t start = 0;
    for (std::size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1) {
      const std::string name = path.substr(start, slash - start);
      int found = -1;
      for (const int child : directories[current].Children)
        if (directories[child].Name == name) found = child;
      if (found < 0) {
        found = static_cast<int>(directories.size());
        directories.push_back({name, current, {}, {}, 0});
        directories[current].Children.push_back(found);
      }
      current = found;
    }
    directories[current].Files.emplace_back(path.substr(start), &data);
  }
  const auto utf16 = [](const std::string &name) {
    Bytes out;
    for (const char c : name) {
      out.push_back(static_cast<std::uint8_t>(c));
      out.push_back(0);
    }
    return out;
  };
  const auto padded = [](std::size_t size) { return (size + 3) & ~std::size_t{3}; };
  std::uint32_t offset = 0;
  for (auto &directory : directories) {
    directory.Offset = offset;
    offset += static_cast<std::uint32_t>(24 + padded(directory.Name.size() * 2));
  }
  std::vector<std::vector<std::uint32_t>> fileOffsets(directories.size());
  std::uint32_t fileOffset = 0;
  for (std::size_t index = 0; index < directories.size(); ++index)
    for (const auto &[name, data] : directories[index].Files) {
      fileOffsets[index].push_back(fileOffset);
      fileOffset += static_cast<std::uint32_t>(32 + padded(name.size() * 2));
    }
  constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;
  Bytes directoryTable, fileTable, dataTable;
  for (std::size_t index = 0; index < directories.size(); ++index) {
    const auto &directory = directories[index];
    std::uint32_t sibling = kInvalid;
    if (directory.Parent >= 0) {
      const auto &siblings = directories[directory.Parent].Children;
      for (std::size_t i = 0; i + 1 < siblings.size(); ++i)
        if (siblings[i] == static_cast<int>(index)) sibling = directories[siblings[i + 1]].Offset;
    }
    Put32(directoryTable, directory.Parent >= 0 ? directories[directory.Parent].Offset : 0);
    Put32(directoryTable, sibling);
    Put32(directoryTable, directory.Children.empty() ? kInvalid : directories[directory.Children[0]].Offset);
    Put32(directoryTable, fileOffsets[index].empty() ? kInvalid : fileOffsets[index][0]);
    Put32(directoryTable, kInvalid);
    const Bytes name = utf16(directory.Name);
    Put32(directoryTable, static_cast<std::uint32_t>(name.size()));
    directoryTable.insert(directoryTable.end(), name.begin(), name.end());
    directoryTable.resize(padded(directoryTable.size()));
    for (std::size_t file = 0; file < directory.Files.size(); ++file) {
      const auto &[fileName, data] = directory.Files[file];
      Put32(fileTable, directory.Offset);
      Put32(fileTable, file + 1 < directory.Files.size() ? fileOffsets[index][file + 1] : kInvalid);
      Put64(fileTable, dataTable.size());
      Put64(fileTable, data->size());
      Put32(fileTable, kInvalid);
      const Bytes encoded = utf16(fileName);
      Put32(fileTable, static_cast<std::uint32_t>(encoded.size()));
      fileTable.insert(fileTable.end(), encoded.begin(), encoded.end());
      fileTable.resize(padded(fileTable.size()));
      dataTable.insert(dataTable.end(), data->begin(), data->end());
      dataTable.resize((dataTable.size() + 15) & ~std::size_t{15});
    }
  }
  Bytes image;
  const std::uint32_t directoryOffset = 0x28;
  const std::uint32_t fileTableOffset = directoryOffset + static_cast<std::uint32_t>(directoryTable.size());
  const std::uint32_t dataOffset = fileTableOffset + static_cast<std::uint32_t>(fileTable.size());
  for (const std::uint32_t value : {0x28U, directoryOffset, 0U, directoryOffset,
                                    static_cast<std::uint32_t>(directoryTable.size()), fileTableOffset, 0U,
                                    fileTableOffset, static_cast<std::uint32_t>(fileTable.size()), dataOffset})
    Put32(image, value);
  image.insert(image.end(), directoryTable.begin(), directoryTable.end());
  image.insert(image.end(), fileTable.begin(), fileTable.end());
  image.insert(image.end(), dataTable.begin(), dataTable.end());
  return image;
}

// A file by path in a composed Level-3 view (lookup by child lists).
std::optional<Bytes> ReadPath(const Bytes &view, const std::string &path) {
  const std::uint32_t dirTable = Get32(view, 12), fileTable = Get32(view, 28), data = Get32(view, 36);
  const auto nameAt = [&](std::size_t offset) {
    std::string name;
    const std::uint32_t size = Get32(view, offset - 4);
    for (std::uint32_t i = 0; i < size; i += 2) name.push_back(static_cast<char>(view[offset + i]));
    return name;
  };
  std::uint32_t directory = 0;
  std::size_t start = 0;
  for (std::size_t slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1) {
    const std::string part = path.substr(start, slash - start);
    std::uint32_t child = Get32(view, dirTable + directory + 8);
    while (child != 0xFFFFFFFFU && nameAt(dirTable + child + 24) != part) child = Get32(view, dirTable + child + 4);
    if (child == 0xFFFFFFFFU) return std::nullopt;
    directory = child;
  }
  const std::string leaf = path.substr(start);
  for (std::uint32_t file = Get32(view, dirTable + directory + 12); file != 0xFFFFFFFFU;
       file = Get32(view, fileTable + file + 4)) {
    if (nameAt(fileTable + file + 32) != leaf) continue;
    const std::uint64_t offset = data + Get64(view, fileTable + file + 8), size = Get64(view, fileTable + file + 16);
    if (offset + size > view.size()) return std::nullopt;
    return Bytes(view.begin() + static_cast<std::ptrdiff_t>(offset),
                 view.begin() + static_cast<std::ptrdiff_t>(offset + size));
  }
  return std::nullopt;
}

struct Fixture {
  fs::path Root;
  fs::path Mods;
  fs::path Image;
  RomFsServiceView View;
  Bytes ViewBytes;

  RomFsModsRequest Request() const { return {Mods, View, Root / "game/romfs_mods.o3ro", Root / "game/romfs_mods.json", {}}; }
};

// `ivfc`: a EUR dump's image (Level 3 at 0x1000); else a converted view at 0.
Fixture MakeFixture(const fs::path &root, const std::map<std::string, Bytes> &files, bool ivfc) {
  std::error_code ignored;
  fs::remove_all(root, ignored);
  Fixture fixture{root, root / "mods", root / "game/romfs.bin", {}, MakeRomFs(files)};
  Bytes image;
  if (ivfc) {
    image = Text("IVFC");
    image.resize(0x1000, 0xA5);
  }
  image.insert(image.end(), fixture.ViewBytes.begin(), fixture.ViewBytes.end());
  WriteFile(fixture.Image, image);
  fixture.View = {fixture.Image, ivfc ? 0x1000U : 0U, fixture.ViewBytes.size()};
  return fixture;
}

// The composed view through the runtime's overlay loader (expected bytes
// checked against the image), or nullopt when it rejects the overlay.
std::optional<Bytes> ComposedView(const Fixture &fixture) {
  const Bytes image = ReadFile(fixture.Image);
  const std::uint64_t base = fixture.View.Offset;
  const NativeA32RomFsOverlay::BaseReader reader = [&](std::uint64_t offset, std::span<std::uint8_t> bytes) {
    if (base + offset + bytes.size() > image.size()) return false;
    std::copy_n(image.begin() + static_cast<std::ptrdiff_t>(base + offset), bytes.size(), bytes.begin());
    return true;
  };
  std::string error;
  const auto overlay = NativeA32RomFsOverlay::Load(fixture.Root / "game/romfs_mods.o3ro", fixture.View.Size, reader, &error);
  if (!overlay) {
    std::fprintf(stderr, "overlay rejected: %s\n", error.c_str());
    return std::nullopt;
  }
  Bytes view(overlay->ViewSize());
  if (!overlay->Read(0, view, reader)) return std::nullopt;
  return view;
}

bool HasNote(const std::vector<RomFsModNote> &notes, std::string_view path, std::string_view reason) {
  return std::any_of(notes.begin(), notes.end(), [&](const RomFsModNote &note) {
    return note.Path == path && note.Reason.find(reason) != std::string::npos;
  });
}

const std::map<std::string, Bytes> &GameFiles() {
  static const std::map<std::string, Bytes> files{
      {"actor/zelda_link_boy_new.zar", Filled(300, 1)},
      {"actor/zelda_keep.zar", Filled(77, 2)},
      {"misc/menu_link.zar", Filled(50, 3)},
      {"message/eu/eu.qm", Text("native messages")},
      {"misc/eu/title.bch", Filled(40, 4)},
      {"misc/hint/movie/hint183.moflex", Filled(33, 5)},
  };
  return files;
}

void TestUsaPathMapping() {
  CHECK(UsaConvertedRomFsPath("message/us/us.qm") == std::optional<std::string>("message/eu/eu.qm"));
  CHECK(UsaConvertedRomFsPath("message/us/other.bin") == std::optional<std::string>("message/eu/other.bin"));
  CHECK(UsaConvertedRomFsPath("misc/us/title.bch") == std::optional<std::string>("misc/eu/title.bch"));
  CHECK(UsaConvertedRomFsPath("misc/hint/movie/hint182.moflex") ==
        std::optional<std::string>("misc/hint/movie/hint183.moflex"));
  CHECK(!UsaConvertedRomFsPath("actor/zelda_keep.zar"));
  CHECK(!UsaConvertedRomFsPath("misc/usx/a.bin"));
}

// Replace, read back byte-exact, everything else unchanged; both layouts.
void TestReplace(const fs::path &root, bool ivfc) {
  const Fixture fixture = MakeFixture(root, GameFiles(), ivfc);
  const Bytes link = Filled(5000, 9), menu = Filled(123, 11);
  WriteFile(fixture.Mods / "vanilla_plus_link/romfs/actor/zelda_link_boy_new.zar", link);
  WriteFile(fixture.Mods / "vanilla_plus_link/romfs/misc/menu_link.zar", menu);
  const RomFsModsReport report = ComposeRomFsMods(fixture.Request());
  CHECK(report.Outcome == RomFsModsOutcome::Built);
  CHECK(report.Applied.size() == 2U && report.Ignored.empty() && report.Overridden.empty());
  CHECK(report.Summary() == "romfs mods: 2 files from 1 mod (vanilla_plus_link), 0 ignored");
  const auto view = ComposedView(fixture);
  CHECK(view.has_value());
  if (!view) return;
  CHECK(ReadPath(*view, "actor/zelda_link_boy_new.zar") == link);
  CHECK(ReadPath(*view, "misc/menu_link.zar") == menu);
  for (const auto &[path, data] : GameFiles())
    if (path != "actor/zelda_link_boy_new.zar" && path != "misc/menu_link.zar")
      CHECK(ReadPath(*view, path) == data);
  // Base bytes never move: only the two 16-byte fields differ.
  std::size_t changed = 0;
  for (std::size_t i = 0; i < fixture.ViewBytes.size(); ++i) changed += (*view)[i] != fixture.ViewBytes[i];
  CHECK(changed > 0U && changed <= 32U);
  CHECK(view->size() > fixture.ViewBytes.size());
  // game/romfs.bin is never written.
  Bytes image = ReadFile(fixture.Image);
  CHECK(Bytes(image.begin() + static_cast<std::ptrdiff_t>(fixture.View.Offset), image.end()) == fixture.ViewBytes);
}

void TestTwoModsSameFile(const fs::path &root) {
  const Fixture fixture = MakeFixture(root, GameFiles(), false);
  WriteFile(fixture.Mods / "a_first/romfs/actor/zelda_keep.zar", Filled(10, 20));
  const Bytes later = Filled(20, 30);
  WriteFile(fixture.Mods / "b_second/romfs/actor/zelda_keep.zar", later);
  WriteFile(fixture.Mods / "a_first/romfs/misc/menu_link.zar", Filled(8, 40));
  const RomFsModsReport report = ComposeRomFsMods(fixture.Request());
  CHECK(report.Outcome == RomFsModsOutcome::Built);
  CHECK(report.Applied.size() == 2U && report.Overridden.size() == 1U);
  CHECK(HasNote(report.Overridden, "romfs/actor/zelda_keep.zar", "replaced by b_second/"));
  CHECK(report.Overridden.front().Mod == "a_first");
  CHECK(report.Summary() == "romfs mods: 2 files from 2 mods (a_first, b_second), 0 ignored, 1 overridden");
  const auto view = ComposedView(fixture);
  CHECK(view && ReadPath(*view, "actor/zelda_keep.zar") == later);
}

// USA paths: mapped on the converted view, exact on a USA IVFC image.
void TestUsaPaths(const fs::path &root) {
  {
    const Fixture fixture = MakeFixture(root, GameFiles(), false);
    const Bytes qm = Text("modded messages"), title = Filled(64, 50), hint = Filled(17, 60);
    WriteFile(fixture.Mods / "usa_mod/romfs/message/us/us.qm", qm);
    WriteFile(fixture.Mods / "usa_mod/romfs/misc/us/title.bch", title);
    WriteFile(fixture.Mods / "usa_mod/romfs/misc/hint/movie/hint182.moflex", hint);
    const RomFsModsReport report = ComposeRomFsMods(fixture.Request());
    CHECK(report.Outcome == RomFsModsOutcome::Built && report.Applied.size() == 3U && report.Ignored.empty());
    const auto view = ComposedView(fixture);
    CHECK(view && ReadPath(*view, "message/eu/eu.qm") == qm);
    CHECK(view && ReadPath(*view, "misc/eu/title.bch") == title);
    CHECK(view && ReadPath(*view, "misc/hint/movie/hint183.moflex") == hint);
    const auto lines = report.LogLines();
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string &line) {
      return line.find("usa_mod/romfs/message/us/us.qm replaces message/eu/eu.qm") != std::string::npos;
    }));
  }
  {
    std::map<std::string, Bytes> usa{{"message/us/us.qm", Text("usa messages")}, {"actor/a.zar", Filled(9, 1)}};
    const Fixture fixture = MakeFixture(root, usa, true);
    const Bytes qm = Text("modded usa messages");
    WriteFile(fixture.Mods / "usa_mod/romfs/message/us/us.qm", qm);
    const RomFsModsReport report = ComposeRomFsMods(fixture.Request());
    CHECK(report.Outcome == RomFsModsOutcome::Built && report.Applied.size() == 1U);
    CHECK(!report.Applied.empty() && report.Applied.front().Target == "message/us/us.qm");
    const auto view = ComposedView(fixture);
    CHECK(view && ReadPath(*view, "message/us/us.qm") == qm);
  }
}

void TestIgnored(const fs::path &root) {
  const Fixture fixture = MakeFixture(root, GameFiles(), false);
  WriteFile(fixture.Mods / "m/romfs/actor/zelda_keep.zar", Filled(12, 70));
  WriteFile(fixture.Mods / "m/romfs/actor/brand_new.zar", Filled(12, 71));
  WriteFile(fixture.Mods / "m/exefs/code.ips", Text("PATCHEOF"));
  WriteFile(fixture.Mods / "m/code.ips", Text("PATCHEOF"));
  WriteFile(fixture.Mods / "m/readme.txt", Text("hi"));
  WriteFile(fixture.Mods / "m/romfs/misc/menu_link.zar", Bytes());
  WriteFile(fixture.Mods / "loose.zar", Text("x"));
  const RomFsModsReport report = ComposeRomFsMods(fixture.Request());
  CHECK(report.Outcome == RomFsModsOutcome::Built && report.Applied.size() == 1U);
  CHECK(report.Ignored.size() == 6U);
  CHECK(HasNote(report.Ignored, "romfs/actor/brand_new.zar", "not in the game's RomFS"));
  CHECK(HasNote(report.Ignored, "exefs/code.ips", "compiled ahead of time"));
  CHECK(HasNote(report.Ignored, "code.ips", "compiled ahead of time"));
  CHECK(HasNote(report.Ignored, "readme.txt", "outside romfs/"));
  CHECK(HasNote(report.Ignored, "romfs/misc/menu_link.zar", "empty"));
  CHECK(HasNote(report.Ignored, "loose.zar", "not in a mod folder"));
  CHECK(report.Summary() == "romfs mods: 1 file from 1 mod (m), 6 ignored");
  const auto view = ComposedView(fixture);
  CHECK(view && !ReadPath(*view, "actor/brand_new.zar"));
  CHECK(view && ReadPath(*view, "misc/menu_link.zar") == GameFiles().at("misc/menu_link.zar"));
  // Only ignored files: no overlay.
  fs::remove_all(fixture.Mods / "m/romfs/actor");
  const RomFsModsReport none = ComposeRomFsMods(fixture.Request());
  CHECK(none.Outcome == RomFsModsOutcome::NoMods && none.Ignored.size() == 5U);
  CHECK(none.Summary() == "romfs mods: no file applies, 5 ignored");
  CHECK(!fs::exists(fixture.Request().OverlayPath) && !fs::exists(fixture.Request().ReceiptPath));
}

void TestReceipt(const fs::path &root) {
  const Fixture fixture = MakeFixture(root, GameFiles(), true);
  const fs::path link = fixture.Mods / "vanilla_plus_link/romfs/actor/zelda_link_boy_new.zar";
  WriteFile(link, Filled(400, 80));
  WriteFile(fixture.Mods / "vanilla_plus_link/readme.txt", Text("credits"));
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  const auto overlayTime = fs::last_write_time(fixture.Request().OverlayPath);
  const RomFsModsReport reused = ComposeRomFsMods(fixture.Request());
  CHECK(reused.Outcome == RomFsModsOutcome::Reused);
  CHECK(reused.Applied.size() == 1U && reused.Ignored.size() == 1U);
  CHECK(reused.Summary() == "romfs mods: 1 file from 1 mod (vanilla_plus_link), 1 ignored");
  CHECK(fs::last_write_time(fixture.Request().OverlayPath) == overlayTime);
  // A changed mod file (size), a touched one (mtime), a changed romfs.bin
  // (mtime) and a truncated overlay rebuild.
  const Bytes changed = Filled(401, 81);
  WriteFile(link, changed);
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  auto view = ComposedView(fixture);
  CHECK(view && ReadPath(*view, "actor/zelda_link_boy_new.zar") == changed);
  fs::last_write_time(link, fs::last_write_time(link) + std::chrono::seconds(5));
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  fs::last_write_time(fixture.Image, fs::last_write_time(fixture.Image) + std::chrono::seconds(5));
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  fs::resize_file(fixture.Request().OverlayPath, fs::file_size(fixture.Request().OverlayPath) - 1U);
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Reused);
  // A new mod file joins the receipt.
  WriteFile(fixture.Mods / "vanilla_plus_link/romfs/misc/menu_link.zar", Filled(5, 82));
  const RomFsModsReport added = ComposeRomFsMods(fixture.Request());
  CHECK(added.Outcome == RomFsModsOutcome::Built && added.Applied.size() == 2U);
  // Every mod removed: the overlay and receipt go.
  fs::remove_all(fixture.Mods / "vanilla_plus_link");
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::NoMods);
  CHECK(!fs::exists(fixture.Request().OverlayPath) && !fs::exists(fixture.Request().ReceiptPath));
}

void TestFailure(const fs::path &root) {
  Fixture fixture = MakeFixture(root, GameFiles(), false);
  WriteFile(fixture.Mods / "m/romfs/actor/zelda_keep.zar", Filled(12, 90));
  CHECK(ComposeRomFsMods(fixture.Request()).Outcome == RomFsModsOutcome::Built);
  // A view that is not a Level-3 RomFS: no overlay is left behind.
  fixture.View.Offset = 8U;
  fixture.View.Size -= 8U;
  const RomFsModsReport failed = ComposeRomFsMods(fixture.Request());
  CHECK(failed.Outcome == RomFsModsOutcome::Failed && !failed.Error.empty());
  CHECK(failed.Summary().rfind("romfs mods: failed, starting without mods: ", 0) == 0U);
  CHECK(!fs::exists(fixture.Request().OverlayPath) && !fs::exists(fixture.Request().ReceiptPath));
}

void TestStartup(const fs::path &root) {
  const Fixture fixture = MakeFixture(root, GameFiles(), true);
  const fs::path config = root / "config";
  int viewReads = 0;
  const RomFsModsStartup startup{config, fixture.Mods,
                                 [&](std::string *) -> std::optional<RomFsServiceView> {
                                   ++viewReads;
                                   return fixture.View;
                                 },
                                 fixture.Request().OverlayPath, fixture.Request().ReceiptPath, {}};
  // No mods folder: nothing read, no argument, no log line.
  RomFsModsStartupResult result = PrepareRomFsMods(startup);
  CHECK(!result.Overlay && result.BootStatus == "none" && result.LogLines.empty() && viewReads == 0);
  WriteFile(fixture.Mods / "vanilla_plus_link/romfs/actor/zelda_keep.zar", Filled(30, 91));
  // Off: no scan even with mods.
  CHECK(SaveRomFsModsEnabled(config, false) && !ReadRomFsModsEnabled(config));
  result = PrepareRomFsMods(startup);
  CHECK(!result.Overlay && result.BootStatus == "off" && viewReads == 0);
  CHECK(result.LogLines == std::vector<std::string>{"romfs mods: off (config/no_romfs_mods)"});
  CHECK(!fs::exists(fixture.Request().OverlayPath));
  // On: the overlay argument and the log lines.
  CHECK(SaveRomFsModsEnabled(config, true) && ReadRomFsModsEnabled(config));
  bool announced = false;
  RomFsModsStartup building = startup;
  building.BeforeBuild = [&] { announced = true; };
  result = PrepareRomFsMods(building);
  CHECK(result.Overlay == fixture.Request().OverlayPath && result.BootStatus == "active:1" && announced);
  CHECK(!result.LogLines.empty() &&
        result.LogLines.front() == "romfs mods: 1 file from 1 mod (vanilla_plus_link), 0 ignored");
  CHECK(result.LogLines.size() >= 2U && result.LogLines[1].rfind("romfs mods: overlay built in ", 0) == 0U);
  CHECK(result.PanelLine == "At this start: 1 file from 1 mod (vanilla_plus_link), 0 ignored.");
  announced = false;
  result = PrepareRomFsMods(building);
  CHECK(result.Overlay && !announced && result.LogLines.size() >= 2U &&
        result.LogLines[1].rfind("romfs mods: overlay reused", 0) == 0U);
  // An unknown view: no overlay, failed.
  RomFsModsStartup unknown = startup;
  unknown.ReadView = [](std::string *error) -> std::optional<RomFsServiceView> {
    *error = "no manifest";
    return std::nullopt;
  };
  result = PrepareRomFsMods(unknown);
  CHECK(!result.Overlay && result.BootStatus == "failed");
  // The mods folder removed: a stale overlay goes too.
  CHECK(PrepareRomFsMods(startup).Overlay.has_value());
  fs::remove_all(fixture.Mods);
  result = PrepareRomFsMods(startup);
  CHECK(!result.Overlay && !fs::exists(fixture.Request().OverlayPath));
}

} // namespace

int main() {
  const fs::path root = fs::temp_directory_path() / "oot3d_romfs_mods_tests";
  TestUsaPathMapping();
  TestReplace(root, false);
  TestReplace(root, true);
  TestTwoModsSameFile(root);
  TestUsaPaths(root);
  TestIgnored(root);
  TestReceipt(root);
  TestFailure(root);
  TestStartup(root);
  std::error_code ignored;
  fs::remove_all(root, ignored);
  if (gFailures != 0) {
    std::fprintf(stderr, "oot3d_romfs_mods_tests: %d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_romfs_mods_tests: passed\n");
  return 0;
}
