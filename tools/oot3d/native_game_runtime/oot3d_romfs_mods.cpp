// RomFS mods: scan, Level-3 table, O3RO overlay and receipt (oot3d_romfs_mods.h).
// The O3RO layout and the overlay construction follow upstream
// coccofresco/TriAevum 9c90abb9 (tools/triaevum_release/romfs_mods.py,
// build_romfs_overlay), GPL-3.0-or-later like this file.
#include "oot3d_romfs_mods.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace Oot3dNativeGame {
namespace {

namespace fs = std::filesystem;

constexpr int kReceiptFormat = 1;
constexpr std::uint32_t kOverlayVersion = 1U;
constexpr std::uint64_t kPayloadAlignment = 16U;
constexpr std::uint32_t kInvalidOffset = 0xFFFFFFFFU;
constexpr std::uint64_t kMaximumTableBytes = 16U * 1024U * 1024U;
constexpr std::size_t kFieldBytes = 16U; // a file record's payload offset + size
constexpr std::size_t kCopyBufferBytes = 1U << 20U;

struct Failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint32_t Le32(const std::uint8_t *bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8U) |
         (static_cast<std::uint32_t>(bytes[2]) << 16U) | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::uint64_t Le64(const std::uint8_t *bytes) {
  return static_cast<std::uint64_t>(Le32(bytes)) | (static_cast<std::uint64_t>(Le32(bytes + 4)) << 32U);
}

void Put32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (unsigned shift = 0U; shift < 32U; shift += 8U) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void Put64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (unsigned shift = 0U; shift < 64U; shift += 8U) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint64_t Align(std::uint64_t value) {
  return (value + kPayloadAlignment - 1U) / kPayloadAlignment * kPayloadAlignment;
}

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char &c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

void ReadExactly(std::ifstream &stream, std::uint64_t offset, std::uint8_t *out, std::size_t count) {
  stream.clear();
  stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  stream.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(count));
  if (!stream) throw Failure("cannot read the game's RomFS (romfs.bin)");
}

// A Level-3 name (UTF-16LE) as UTF-8.
std::string NameOf(const std::vector<std::uint8_t> &table, std::uint64_t offset, std::size_t fixed) {
  if (offset % 4U != 0U || offset + fixed > table.size()) throw Failure("invalid RomFS record");
  const std::uint32_t size = Le32(table.data() + offset + fixed - 4U);
  if (size % 2U != 0U || offset + fixed + size > table.size()) throw Failure("invalid RomFS name");
  std::string name;
  const std::uint8_t *bytes = table.data() + offset + fixed;
  for (std::uint32_t index = 0U; index < size; index += 2U) {
    std::uint32_t unit = static_cast<std::uint32_t>(bytes[index]) | (static_cast<std::uint32_t>(bytes[index + 1U]) << 8U);
    if (unit >= 0xD800U && unit < 0xDC00U && index + 3U < size) {
      const std::uint32_t low =
          static_cast<std::uint32_t>(bytes[index + 2U]) | (static_cast<std::uint32_t>(bytes[index + 3U]) << 8U);
      if (low >= 0xDC00U && low < 0xE000U) {
        unit = 0x10000U + ((unit - 0xD800U) << 10U) + (low - 0xDC00U);
        index += 2U;
      }
    }
    if (unit == 0U || unit == '/' || unit == '\\') throw Failure("invalid RomFS name");
    if (unit < 0x80U) {
      name.push_back(static_cast<char>(unit));
    } else if (unit < 0x800U) {
      name.push_back(static_cast<char>(0xC0U | (unit >> 6U)));
      name.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
    } else if (unit < 0x10000U) {
      name.push_back(static_cast<char>(0xE0U | (unit >> 12U)));
      name.push_back(static_cast<char>(0x80U | ((unit >> 6U) & 0x3FU)));
      name.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
    } else {
      name.push_back(static_cast<char>(0xF0U | (unit >> 18U)));
      name.push_back(static_cast<char>(0x80U | ((unit >> 12U) & 0x3FU)));
      name.push_back(static_cast<char>(0x80U | ((unit >> 6U) & 0x3FU)));
      name.push_back(static_cast<char>(0x80U | (unit & 0x3FU)));
    }
  }
  return name;
}

struct FileRecord {
  std::uint64_t RecordOffset = 0U; // in the file metadata table
};

// The view's Level-3 file table and every file path in it.
struct RomFsTable {
  std::uint32_t FileTableOffset = 0U;
  std::uint32_t FileDataOffset = 0U;
  std::vector<std::uint8_t> Files;
  std::unordered_map<std::string, FileRecord> Records;
};

RomFsTable ReadRomFsTable(const RomFsServiceView &view) {
  std::error_code sizeError;
  const std::uint64_t imageBytes = fs::file_size(view.ImagePath, sizeError);
  if (sizeError) throw Failure("cannot read " + view.ImagePath.string());
  if (view.Size < 0x28U || view.Offset > imageBytes || view.Size > imageBytes - view.Offset)
    throw Failure("romfs.bin does not hold its service view");
  std::ifstream image(view.ImagePath, std::ios::binary);
  if (!image) throw Failure("cannot open " + view.ImagePath.string());
  std::array<std::uint8_t, 0x28> header{};
  ReadExactly(image, view.Offset, header.data(), header.size());
  std::array<std::uint32_t, 10> fields{};
  for (std::size_t index = 0U; index < fields.size(); ++index) fields[index] = Le32(header.data() + index * 4U);
  if (fields[0] != 0x28U) throw Failure("romfs.bin has no Level-3 header at its service view");
  const auto table = [&](std::uint32_t offset, std::uint32_t size) {
    if (offset % 4U != 0U || size == 0U || size > kMaximumTableBytes ||
        static_cast<std::uint64_t>(offset) + size > view.Size)
      throw Failure("invalid RomFS metadata bounds");
    std::vector<std::uint8_t> bytes(size);
    ReadExactly(image, view.Offset + offset, bytes.data(), bytes.size());
    return bytes;
  };
  RomFsTable result;
  const std::vector<std::uint8_t> directories = table(fields[3], fields[4]);
  result.Files = table(fields[7], fields[8]);
  result.FileTableOffset = fields[7];
  result.FileDataOffset = fields[9];
  if (result.FileDataOffset > view.Size) throw Failure("invalid RomFS data offset");

  std::set<std::uint32_t> visitedDirectories, visitedFiles;
  std::vector<std::pair<std::uint32_t, std::string>> pending{{0U, std::string()}};
  while (!pending.empty()) {
    auto [offset, prefix] = std::move(pending.back());
    pending.pop_back();
    if (!visitedDirectories.insert(offset).second) throw Failure("cyclic RomFS directory");
    const std::string name = NameOf(directories, offset, 24U);
    const std::uint8_t *record = directories.data() + offset;
    const std::uint32_t sibling = Le32(record + 4), child = Le32(record + 8);
    std::uint32_t file = Le32(record + 12);
    const std::string path = prefix.empty() ? name : (name.empty() ? prefix : prefix + "/" + name);
    if (sibling != kInvalidOffset) pending.emplace_back(sibling, prefix);
    if (child != kInvalidOffset) pending.emplace_back(child, path);
    while (file != kInvalidOffset) {
      if (!visitedFiles.insert(file).second) throw Failure("cyclic RomFS file list");
      const std::string fileName = NameOf(result.Files, file, 32U);
      const std::uint8_t *fixed = result.Files.data() + file;
      const std::uint64_t payload = Le64(fixed + 8), length = Le64(fixed + 16);
      if (Le32(fixed) != offset || payload > view.Size || length > view.Size - payload ||
          result.FileDataOffset + payload + length > view.Size)
        throw Failure("invalid RomFS file record");
      const std::string full = path.empty() ? fileName : path + "/" + fileName;
      if (!result.Records.emplace(full, FileRecord{file}).second) throw Failure("duplicate RomFS path " + full);
      file = Le32(fixed + 4);
    }
  }
  return result;
}

// ---- The mods folder ----

struct ScannedFile {
  std::string Mod;  // folder under mods/; empty for a file directly in mods/
  std::string Path; // generic, relative to the mod folder
  fs::path Full;
  std::uint64_t Size = 0U;
  std::int64_t Mtime = 0;
};

std::int64_t MtimeOf(const fs::path &path) {
  std::error_code error;
  const auto time = fs::last_write_time(path, error);
  return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

std::vector<ScannedFile> ScanMods(const fs::path &mods) {
  std::vector<fs::directory_entry> entries;
  std::error_code error;
  for (fs::directory_iterator it(mods, error), end; !error && it != end; it.increment(error))
    entries.push_back(*it);
  std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
    return a.path().filename().generic_string() < b.path().filename().generic_string();
  });
  std::vector<ScannedFile> files;
  for (const auto &entry : entries) {
    std::error_code kindError;
    const std::string name = entry.path().filename().generic_string();
    if (entry.is_regular_file(kindError)) {
      files.push_back({std::string(), name, entry.path(), entry.file_size(kindError), MtimeOf(entry.path())});
      continue;
    }
    if (!entry.is_directory(kindError)) continue;
    std::vector<ScannedFile> modFiles;
    std::error_code walkError;
    for (fs::recursive_directory_iterator it(entry.path(), fs::directory_options::skip_permission_denied, walkError),
         end;
         !walkError && it != end; it.increment(walkError)) {
      std::error_code fileError;
      if (!it->is_regular_file(fileError)) continue;
      // Lexical: canonical paths do not work with sdmc:/ (devoptab).
      modFiles.push_back({name, it->path().lexically_relative(entry.path()).generic_string(), it->path(),
                          it->file_size(fileError), MtimeOf(it->path())});
    }
    std::sort(modFiles.begin(), modFiles.end(),
              [](const ScannedFile &a, const ScannedFile &b) { return a.Path < b.Path; });
    files.insert(files.end(), std::make_move_iterator(modFiles.begin()), std::make_move_iterator(modFiles.end()));
  }
  return files;
}

nlohmann::json Fingerprint(const RomFsServiceView &view, const std::vector<ScannedFile> &files) {
  std::error_code error;
  nlohmann::json list = nlohmann::json::array();
  for (const auto &file : files) list.push_back({file.Mod, file.Path, file.Size, file.Mtime});
  return {{"format", kReceiptFormat},
          {"romfs",
           {{"file_bytes", fs::file_size(view.ImagePath, error)},
            {"mtime", MtimeOf(view.ImagePath)},
            {"service_offset", view.Offset},
            {"service_size", view.Size}}},
          {"files", std::move(list)}};
}

// ---- Classification ----

constexpr const char *kCodeReason =
    "code patch: the game's code is compiled ahead of time, exefs/ and code.ips cannot apply";

struct Plan {
  std::map<std::string, std::pair<RomFsModFile, fs::path>> Targets; // by RomFS path
  std::vector<RomFsModNote> Ignored;
  std::vector<RomFsModNote> Overridden;
};

Plan Classify(const std::vector<ScannedFile> &files, const RomFsTable &table) {
  Plan plan;
  for (const auto &file : files) {
    const auto ignore = [&](std::string reason) { plan.Ignored.push_back({file.Mod, file.Path, std::move(reason)}); };
    if (file.Mod.empty()) {
      ignore("not in a mod folder (mods/<name>/romfs/...)");
      continue;
    }
    const std::string lower = Lower(file.Path);
    const std::size_t slash = lower.find('/');
    const std::string first = lower.substr(0, slash);
    const std::string fileName = lower.substr(lower.rfind('/') + 1U);
    if (first == "exefs" || fileName == "code.ips" || fileName == "code.bin" ||
        (fileName.size() > 4U && fileName.compare(fileName.size() - 4U, 4U, ".ips") == 0)) {
      ignore(kCodeReason);
      continue;
    }
    if (first != "romfs" || slash == std::string::npos || slash + 1U == lower.size()) {
      ignore("outside romfs/ (game files go in mods/<name>/romfs/)");
      continue;
    }
    if (file.Size == 0U) {
      ignore("empty file");
      continue;
    }
    if (file.Size > kRomFsModMaximumFileBytes) {
      ignore("larger than 64 MiB");
      continue;
    }
    std::string target = file.Path.substr(slash + 1U);
    if (!table.Records.count(target)) {
      const auto converted = UsaConvertedRomFsPath(target);
      if (!converted || !table.Records.count(*converted)) {
        ignore("not in the game's RomFS (mods can replace files, not add them)");
        continue;
      }
      target = *converted;
    }
    auto [entry, inserted] = plan.Targets.try_emplace(target);
    if (!inserted)
      plan.Overridden.push_back({entry->second.first.Mod, entry->second.first.Path,
                                 "replaced by " + file.Mod + "/" + file.Path});
    entry->second = {RomFsModFile{file.Mod, file.Path, target, file.Size}, file.Full};
  }
  return plan;
}

// ---- Writing ----

// Horizon's FS does not rename over an existing file (EEXIST).
void ReplaceFile(const fs::path &from, const fs::path &to) {
  std::error_code error;
  fs::remove(to, error);
  error.clear();
  fs::rename(from, to, error);
  if (error) throw Failure("cannot move " + from.string() + " to " + to.string() + ": " + error.message());
}

void RemoveOutputs(const RomFsModsRequest &request) {
  std::error_code ignored;
  for (const fs::path &path : {request.ReceiptPath, request.OverlayPath})
    for (const fs::path &file : {path, fs::path(path.string() + ".partial")}) fs::remove(file, ignored);
}

struct Built {
  std::uint64_t OverlayBytes = 0U;
  std::uint64_t AppendedBytes = 0U;
};

Built WriteOverlay(const RomFsModsRequest &request, const RomFsTable &table, const Plan &plan) {
  struct Patch {
    std::uint64_t Field;
    std::vector<std::uint8_t> Expected, Replacement;
  };
  struct Payload {
    std::uint64_t ViewOffset;
    const fs::path *Source;
    std::uint64_t Size;
  };
  std::vector<Patch> patches;
  std::vector<Payload> payloads;
  std::uint64_t cursor = request.View.Size;
  for (const auto &[target, entry] : plan.Targets) { // sorted by path, like upstream
    const std::uint64_t record = table.Records.at(target).RecordOffset;
    cursor = Align(cursor);
    Patch patch{table.FileTableOffset + record + 8U,
                std::vector<std::uint8_t>(table.Files.begin() + static_cast<std::ptrdiff_t>(record + 8U),
                                          table.Files.begin() + static_cast<std::ptrdiff_t>(record + 8U + kFieldBytes)),
                {}};
    Put64(patch.Replacement, cursor - table.FileDataOffset);
    Put64(patch.Replacement, entry.first.Size);
    patches.push_back(std::move(patch));
    payloads.push_back({cursor, &entry.second, entry.first.Size});
    cursor += entry.first.Size;
  }
  std::sort(patches.begin(), patches.end(), [](const Patch &a, const Patch &b) { return a.Field < b.Field; });
  const std::uint64_t viewSize = cursor;
  const std::uint64_t tableEnd = 64U + 32U * patches.size();
  const std::uint64_t tailOffset = tableEnd + 2U * kFieldBytes * patches.size();

  std::vector<std::uint8_t> head{'O', '3', 'R', 'O'};
  Put32(head, kOverlayVersion);
  Put64(head, request.View.Size);
  Put64(head, viewSize);
  Put32(head, static_cast<std::uint32_t>(patches.size()));
  Put32(head, 0U);
  Put64(head, tailOffset);
  head.resize(64U, 0U);
  std::uint64_t data = tableEnd;
  for (const auto &patch : patches) {
    Put64(head, patch.Field);
    Put32(head, static_cast<std::uint32_t>(kFieldBytes));
    Put32(head, 0U);
    Put64(head, data);
    Put64(head, data + kFieldBytes);
    data += 2U * kFieldBytes;
  }
  for (const auto &patch : patches) {
    head.insert(head.end(), patch.Expected.begin(), patch.Expected.end());
    head.insert(head.end(), patch.Replacement.begin(), patch.Replacement.end());
  }

  const fs::path partial = request.OverlayPath.string() + ".partial";
  std::error_code directoryError;
  fs::create_directories(request.OverlayPath.parent_path(), directoryError);
  {
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw Failure("cannot write " + partial.string());
    output.write(reinterpret_cast<const char *>(head.data()), static_cast<std::streamsize>(head.size()));
    std::vector<char> buffer(kCopyBufferBytes);
    std::uint64_t position = request.View.Size;
    for (const auto &payload : payloads) {
      static constexpr std::array<char, kPayloadAlignment> kZeros{};
      output.write(kZeros.data(), static_cast<std::streamsize>(payload.ViewOffset - position));
      std::ifstream source(*payload.Source, std::ios::binary);
      std::uint64_t left = payload.Size;
      while (source && left != 0U) {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(left, buffer.size()));
        source.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (static_cast<std::size_t>(source.gcount()) != chunk) break;
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        left -= chunk;
      }
      if (left != 0U || source.peek() != std::char_traits<char>::eof())
        throw Failure("mod file changed while it was read: " + payload.Source->string());
      position = payload.ViewOffset + payload.Size;
    }
    output.flush();
    if (!output) throw Failure("cannot write " + partial.string() + " (SD card full?)");
  }
  std::error_code sizeError;
  const std::uint64_t written = fs::file_size(partial, sizeError);
  if (sizeError || written != tailOffset + (viewSize - request.View.Size))
    throw Failure("the overlay was not written completely (SD card full?)");
  // The receipt goes first: it never vouches for an overlay being replaced.
  std::error_code ignored;
  fs::remove(request.ReceiptPath, ignored);
  ReplaceFile(partial, request.OverlayPath);
  return {written, viewSize - request.View.Size};
}

nlohmann::json NotesJson(const std::vector<RomFsModNote> &notes) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto &note : notes) out.push_back({{"mod", note.Mod}, {"path", note.Path}, {"reason", note.Reason}});
  return out;
}

std::vector<RomFsModNote> NotesFrom(const nlohmann::json &json) {
  std::vector<RomFsModNote> out;
  for (const auto &note : json)
    out.push_back({note.at("mod").get<std::string>(), note.at("path").get<std::string>(),
                   note.at("reason").get<std::string>()});
  return out;
}

nlohmann::json ReportJson(const RomFsModsReport &report) {
  nlohmann::json applied = nlohmann::json::array();
  for (const auto &file : report.Applied)
    applied.push_back({{"mod", file.Mod}, {"path", file.Path}, {"target", file.Target}, {"size", file.Size}});
  return {{"mods", report.Mods},
          {"applied", std::move(applied)},
          {"ignored", NotesJson(report.Ignored)},
          {"overridden", NotesJson(report.Overridden)},
          {"overlay_bytes", report.OverlayBytes},
          {"appended_bytes", report.AppendedBytes}};
}

void ReportFrom(const nlohmann::json &json, RomFsModsReport &report) {
  report.Mods = json.at("mods").get<std::vector<std::string>>();
  for (const auto &file : json.at("applied"))
    report.Applied.push_back({file.at("mod").get<std::string>(), file.at("path").get<std::string>(),
                              file.at("target").get<std::string>(), file.at("size").get<std::uint64_t>()});
  report.Ignored = NotesFrom(json.at("ignored"));
  report.Overridden = NotesFrom(json.at("overridden"));
  report.OverlayBytes = json.at("overlay_bytes").get<std::uint64_t>();
  report.AppendedBytes = json.at("appended_bytes").get<std::uint64_t>();
}

void WriteReceipt(const fs::path &path, const nlohmann::json &receipt) {
  const fs::path partial = path.string() + ".partial";
  {
    std::ofstream output(partial, std::ios::trunc);
    output << receipt.dump(2) << '\n';
    output.flush();
    if (!output) throw Failure("cannot write " + partial.string());
  }
  ReplaceFile(partial, path);
}

// The receipt's report when it matches `inputs` and the overlay is there.
bool Reuse(const RomFsModsRequest &request, const nlohmann::json &inputs, RomFsModsReport &report) {
  std::ifstream input(request.ReceiptPath);
  if (!input) return false;
  try {
    const nlohmann::json receipt = nlohmann::json::parse(input);
    if (receipt.at("inputs") != inputs) return false;
    RomFsModsReport stored;
    ReportFrom(receipt.at("report"), stored);
    std::error_code error;
    if (stored.Applied.empty() || fs::file_size(request.OverlayPath, error) != stored.OverlayBytes || error)
      return false;
    stored.Outcome = RomFsModsOutcome::Reused;
    report = std::move(stored);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

std::string Plural(std::size_t count, const char *one, const char *many) {
  return std::to_string(count) + " " + (count == 1U ? one : many);
}

std::string Megabytes(std::uint64_t bytes) {
  char text[32];
  std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return text;
}

} // namespace

std::optional<std::string> UsaConvertedRomFsPath(std::string_view path) {
  // oot3d_rom_import.cpp (PlanUsaToEurRomFs): the directory renames, us.qm,
  // and the hint movies the EUR executable opens under other numbers.
  static constexpr std::pair<std::string_view, std::string_view> kFiles[] = {
      {"message/us/us.qm", "message/eu/eu.qm"},
      {"misc/hint/movie/hint182.moflex", "misc/hint/movie/hint183.moflex"},
      {"misc/hint/movie/hint186.moflex", "misc/hint/movie/hint185.moflex"},
      {"misc/hint/movie/hint188.moflex", "misc/hint/movie/hint187.moflex"},
  };
  for (const auto &[from, to] : kFiles)
    if (path == from) return std::string(to);
  for (const std::string_view directory : {std::string_view("message/"), std::string_view("misc/")}) {
    const std::string us = std::string(directory) + "us/";
    if (path.substr(0, us.size()) == us)
      return std::string(directory) + "eu/" + std::string(path.substr(us.size()));
  }
  return std::nullopt;
}

RomFsModsReport ComposeRomFsMods(const RomFsModsRequest &request) {
  const auto started = std::chrono::steady_clock::now();
  RomFsModsReport report;
  const auto finish = [&](RomFsModsOutcome outcome) {
    report.Outcome = outcome;
    report.Seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return report;
  };
  try {
    const std::vector<ScannedFile> files = ScanMods(request.ModsDirectory);
    if (files.empty()) {
      RemoveOutputs(request);
      return finish(RomFsModsOutcome::NoMods);
    }
    const nlohmann::json inputs = Fingerprint(request.View, files);
    if (Reuse(request, inputs, report)) return finish(RomFsModsOutcome::Reused);

    const RomFsTable table = ReadRomFsTable(request.View);
    const Plan plan = Classify(files, table);
    report.Ignored = plan.Ignored;
    report.Overridden = plan.Overridden;
    if (plan.Targets.empty()) {
      RemoveOutputs(request);
      return finish(RomFsModsOutcome::NoMods);
    }
    std::set<std::string> mods;
    for (const auto &[target, entry] : plan.Targets) {
      report.Applied.push_back(entry.first);
      mods.insert(entry.first.Mod);
    }
    report.Mods.assign(mods.begin(), mods.end());
    if (request.BeforeBuild) request.BeforeBuild();
    const Built built = WriteOverlay(request, table, plan);
    report.OverlayBytes = built.OverlayBytes;
    report.AppendedBytes = built.AppendedBytes;
    WriteReceipt(request.ReceiptPath, {{"inputs", inputs}, {"report", ReportJson(report)}});
    return finish(RomFsModsOutcome::Built);
  } catch (const std::exception &error) {
    RemoveOutputs(request);
    report = RomFsModsReport{};
    report.Error = error.what();
    return finish(RomFsModsOutcome::Failed);
  }
}

std::string RomFsModsReport::Summary() const {
  if (Outcome == RomFsModsOutcome::Failed) return "romfs mods: failed, starting without mods: " + Error;
  std::string line = "romfs mods: ";
  if (Applied.empty()) {
    line += "no file applies";
  } else {
    line += Plural(Applied.size(), "file", "files") + " from " + Plural(Mods.size(), "mod", "mods") + " (";
    for (std::size_t index = 0U; index < Mods.size(); ++index) line += (index ? ", " : "") + Mods[index];
    line += ")";
  }
  line += ", " + std::to_string(Ignored.size()) + " ignored";
  if (!Overridden.empty()) line += ", " + std::to_string(Overridden.size()) + " overridden";
  return line;
}

std::vector<std::string> RomFsModsReport::LogLines(std::size_t maxNotes) const {
  std::vector<std::string> lines{Summary()};
  char seconds[32];
  std::snprintf(seconds, sizeof seconds, "%.2f s", Seconds);
  if (Outcome == RomFsModsOutcome::Built)
    lines.push_back("romfs mods: overlay built in " + std::string(seconds) + " (" + Megabytes(AppendedBytes) +
                    " appended, " + Megabytes(OverlayBytes) + " overlay file)");
  else if (Outcome == RomFsModsOutcome::Reused)
    lines.push_back("romfs mods: overlay reused, no mod file changed (" + std::string(seconds) + ")");
  std::vector<std::string> notes;
  for (const auto &file : Applied)
    if (file.Path.substr(file.Path.find('/') + 1U) != file.Target)
      notes.push_back("romfs mods: " + file.Mod + "/" + file.Path + " replaces " + file.Target +
                      " (the import's USA-to-EUR name)");
  for (const auto &note : Overridden)
    notes.push_back("romfs mods: " + note.Mod + "/" + note.Path + " overridden: " + note.Reason);
  for (const auto &note : Ignored)
    notes.push_back("romfs mods: " + (note.Mod.empty() ? std::string() : note.Mod + "/") + note.Path +
                    " ignored: " + note.Reason);
  for (std::size_t index = 0U; index < notes.size() && index < maxNotes; ++index) lines.push_back(notes[index]);
  if (notes.size() > maxNotes)
    lines.push_back("romfs mods: " + std::to_string(notes.size() - maxNotes) + " more notes not shown");
  return lines;
}

RomFsModsStartupResult PrepareRomFsMods(const RomFsModsStartup &startup) {
  RomFsModsStartupResult result;
  if (!ReadRomFsModsEnabled(startup.ConfigDirectory)) {
    result.BootStatus = "off";
    result.LogLines.push_back(std::string("romfs mods: off (config/") + kRomFsModsOffConfigName + ")");
    result.PanelLine = "Off at this start.";
    return result;
  }
  std::error_code error;
  if (!fs::is_directory(startup.ModsDirectory, error)) {
    // No mods folder: as before RomFS mods existed. An overlay left by an
    // earlier start with mods only takes SD space.
    fs::remove(startup.ReceiptPath, error);
    fs::remove(startup.OverlayPath, error);
    result.PanelLine = "No mods folder at this start.";
    return result;
  }
  std::string viewError;
  const std::optional<RomFsServiceView> view =
      startup.ReadView ? startup.ReadView(&viewError) : std::nullopt;
  RomFsModsReport report;
  if (!view) {
    report.Outcome = RomFsModsOutcome::Failed;
    report.Error = viewError.empty() ? "the game's RomFS service view is unknown" : viewError;
  } else {
    report = ComposeRomFsMods({startup.ModsDirectory, *view, startup.OverlayPath, startup.ReceiptPath,
                               startup.BeforeBuild});
  }
  result.LogLines = report.LogLines();
  switch (report.Outcome) {
  case RomFsModsOutcome::Built:
  case RomFsModsOutcome::Reused:
    result.Overlay = startup.OverlayPath;
    result.BootStatus = "active:" + std::to_string(report.Applied.size());
    break;
  case RomFsModsOutcome::NoMods:
    result.BootStatus = "none";
    break;
  case RomFsModsOutcome::Failed:
    result.BootStatus = "failed";
    break;
  }
  const std::string summary = report.Summary();
  result.PanelLine = "At this start: " + summary.substr(std::strlen("romfs mods: ")) + ".";
  return result;
}

} // namespace Oot3dNativeGame
