// Copyright Citra Emulator Project / Azahar Emulator Project
// Copyright TriAevum contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// C++ port of tools/triaevum_release/ctr_rom.py and oot3d_region_assets.py
// (GPL-2.0-or-later; Azahar provenance in oot3d_rom_import.h).

#include "oot3d_rom_import.h"
#include "ctr_title_profile.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

#if defined(__SWITCH__)
#include <sys/statvfs.h>
#endif
#include <unistd.h>

#if defined(__ARM_FEATURE_SHA2) || defined(__ARM_FEATURE_CRYPTO)
#include <arm_neon.h>
#define OOT3D_ROM_IMPORT_ARM_SHA256 1
#endif

namespace Oot3dNativeGame::RomImport {
namespace {

#include "oot3d_rom_import_usa_copies.inc"

namespace fs = std::filesystem;

constexpr std::uint64_t kMediaUnit = 0x200;
constexpr std::size_t kHeaderSize = 0x200;
constexpr std::size_t kExheaderSize = 0x800;
constexpr std::size_t kExeFsHeaderSize = 0x200;
constexpr std::uint64_t kMaxDecompressedCode = 512ULL * 1024 * 1024;
constexpr std::uint64_t kMaxCodeFile = 16ULL * 1024 * 1024;
constexpr std::uint64_t kIvfcSuperblock = 0x1000;
constexpr std::size_t kChunk = 4U * 1024U * 1024U;
constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;

// ---------------------------------------------------------------- SHA-256

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#if defined(OOT3D_ROM_IMPORT_ARM_SHA256)
void Sha256Blocks(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) {
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);
    for (; blocks != 0; --blocks, data += 64) {
        const uint32x4_t abcdSaved = abcd;
        const uint32x4_t efghSaved = efgh;
        uint32x4_t message[4];
        for (int i = 0; i < 4; ++i)
            message[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(data + 16 * i)));
        for (int group = 0; group < 16; ++group) {
            uint32x4_t& current = message[group & 3];
            const uint32x4_t wk = vaddq_u32(current, vld1q_u32(kSha256K + 4 * group));
            if (group < 12) {
                current = vsha256su1q_u32(vsha256su0q_u32(current, message[(group + 1) & 3]),
                                          message[(group + 2) & 3], message[(group + 3) & 3]);
            }
            const uint32x4_t previous = abcd;
            abcd = vsha256hq_u32(abcd, efgh, wk);
            efgh = vsha256h2q_u32(efgh, previous, wk);
        }
        abcd = vaddq_u32(abcd, abcdSaved);
        efgh = vaddq_u32(efgh, efghSaved);
    }
    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}
#else
constexpr std::uint32_t Rotr(std::uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

void Sha256Blocks(std::uint32_t* state, const std::uint8_t* data, std::size_t blocks) {
    for (; blocks != 0; --blocks, data += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{data[4 * i]} << 24) | (std::uint32_t{data[4 * i + 1]} << 16) |
                   (std::uint32_t{data[4 * i + 2]} << 8) | std::uint32_t{data[4 * i + 3]};
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) +
                                     ((e & f) ^ (~e & g)) + kSha256K[i] + w[i];
            const std::uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) +
                                     ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
}
#endif

// ---------------------------------------------------------------- helpers

std::uint32_t U32(const std::uint8_t* data) {
    return std::uint32_t{data[0]} | (std::uint32_t{data[1]} << 8) |
           (std::uint32_t{data[2]} << 16) | (std::uint32_t{data[3]} << 24);
}

std::uint64_t U64(const std::uint8_t* data) {
    return std::uint64_t{U32(data)} | (std::uint64_t{U32(data + 4)} << 32);
}

void PutU32(std::uint8_t* data, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        data[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::string Lower(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::string SystemError(const char* what, const fs::path& path) {
    return std::string(what) + " " + path.string() + ": " + std::strerror(errno);
}

struct FileCloser {
    void operator()(std::FILE* file) const { if (file) std::fclose(file); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

File OpenFile(const fs::path& path, const char* mode) {
    File file(std::fopen(path.string().c_str(), mode));
    if (!file)
        throw ImportError(SystemError("cannot open", path));
    return file;
}

void ReadAt(std::FILE* file, std::uint64_t offset, std::size_t size, std::uint8_t* out,
            const char* label) {
    if (std::fseek(file, static_cast<long>(offset), SEEK_SET) != 0 ||
        std::fread(out, 1, size, file) != size)
        throw ImportError(std::string("the dump is truncated while reading ") + label);
}

std::vector<std::uint8_t> ReadAt(std::FILE* file, std::uint64_t offset, std::size_t size,
                                 const char* label) {
    std::vector<std::uint8_t> data(size);
    ReadAt(file, offset, size, data.data(), label);
    return data;
}

void WriteAll(std::FILE* file, const void* data, std::size_t size, const fs::path& path) {
    if (size != 0 && std::fwrite(data, 1, size, file) != size)
        throw ImportError(SystemError("cannot write", path));
}

void CloseSynced(File& file, const fs::path& path) {
    if (std::fflush(file.get()) != 0 || fsync(fileno(file.get())) != 0)
        throw ImportError(SystemError("cannot flush", path));
    std::FILE* raw = file.release();
    if (std::fclose(raw) != 0)
        throw ImportError(SystemError("cannot close", path));
}

void WriteFileSynced(const fs::path& path, std::span<const std::uint8_t> data) {
    File file = OpenFile(path, "wb");
    WriteAll(file.get(), data.data(), data.size(), path);
    CloseSynced(file, path);
}

std::uint64_t FileSize(const fs::path& path) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error)
        throw ImportError("cannot read the size of " + path.string() + ": " + error.message());
    return size;
}

void CheckRegion(std::uint64_t offset, std::uint64_t size, std::uint64_t lower, std::uint64_t upper,
                 const std::string& label) {
    if (offset < lower || size == 0 || offset > upper || size > upper - offset)
        throw ImportError(label + " lies outside the title partition");
}

// ---------------------------------------------------------------- NCSD/NCCH

struct Layout {
    std::uint64_t ProgramId = 0;
    bool CompressedCode = false;
    std::uint64_t CodeOffset = 0, CodeSize = 0;
    std::uint64_t ExheaderOffset = 0;
    std::uint64_t RomFsOffset = 0, RomFsSize = 0;
};

constexpr const char* kEncryptedHint =
    "the dump may still be encrypted; decrypt it first (for example with GodMode9)";

Layout ParseNcch(std::FILE* file, std::uint64_t fileSize, std::uint64_t base, std::uint64_t size,
                 int index) {
    const std::string partition = "partition " + std::to_string(index);
    const std::uint64_t end = base + size;
    CheckRegion(base, kHeaderSize, 0, fileSize, partition + " NCCH header");
    if (end > fileSize)
        throw ImportError(partition + " exceeds the dump size (trimmed or incomplete file?)");
    const auto header = ReadAt(file, base, kHeaderSize, "the NCCH header");
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) != 0)
        throw ImportError(partition + " is not an NCCH title");
    if (!(header[0x18D] & 0x02))
        throw ImportError(partition + " is not executable");
    if (U32(header.data() + 0x180) == 0)
        throw ImportError(partition + " has no ExHeader");

    Layout layout;
    layout.ProgramId = U64(header.data() + 0x118);
    layout.ExheaderOffset = base + kHeaderSize;
    CheckRegion(layout.ExheaderOffset, kExheaderSize, base, end, "ExHeader");
    const auto exheader = ReadAt(file, layout.ExheaderOffset, kExheaderSize, "the ExHeader");
    layout.CompressedCode = (exheader[0x0D] & 1) != 0;

    const std::uint64_t exefsUnits = U32(header.data() + 0x1A0);
    const std::uint64_t exefsSizeUnits = U32(header.data() + 0x1A4);
    if (exefsUnits == 0 || exefsSizeUnits == 0)
        throw ImportError(partition + " has no ExeFS");
    const std::uint64_t exefsOffset = base + exefsUnits * kMediaUnit;
    const std::uint64_t exefsSize = exefsSizeUnits * kMediaUnit;
    CheckRegion(exefsOffset, exefsSize, base, end, "ExeFS");
    const auto exefs = ReadAt(file, exefsOffset, kExeFsHeaderSize, "the ExeFS header");
    bool found = false;
    for (int section = 0; section < 8; ++section) {
        const std::uint8_t* entry = exefs.data() + section * 0x10;
        const std::string_view name(reinterpret_cast<const char*>(entry),
                                    strnlen(reinterpret_cast<const char*>(entry), 8));
        if (name != ".code")
            continue;
        layout.CodeOffset = exefsOffset + kExeFsHeaderSize + U32(entry + 8);
        layout.CodeSize = U32(entry + 12);
        CheckRegion(layout.CodeOffset, layout.CodeSize, exefsOffset + kExeFsHeaderSize,
                    exefsOffset + exefsSize, "ExeFS .code");
        found = true;
        break;
    }
    if (!found)
        throw ImportError(partition + " has no readable ExeFS .code section; " + kEncryptedHint);

    const std::uint64_t romfsUnits = U32(header.data() + 0x1B0);
    const std::uint64_t romfsSizeUnits = U32(header.data() + 0x1B4);
    if (romfsUnits == 0 || romfsSizeUnits == 0)
        throw ImportError(partition + " has no RomFS");
    layout.RomFsOffset = base + romfsUnits * kMediaUnit;
    layout.RomFsSize = romfsSizeUnits * kMediaUnit;
    CheckRegion(layout.RomFsOffset, layout.RomFsSize, base, end, "RomFS");
    std::uint8_t magic[4];
    ReadAt(file, layout.RomFsOffset, 4, magic, "the RomFS header");
    if (std::memcmp(magic, "IVFC", 4) != 0)
        throw ImportError(std::string("the RomFS is not decrypted or has an invalid IVFC header; ") +
                          kEncryptedHint);
    return layout;
}

Layout FindTitleLayout(std::FILE* file, std::uint64_t fileSize) {
    if (fileSize < kHeaderSize)
        throw ImportError("the file is too small to be a 3DS dump");
    const auto header = ReadAt(file, 0, kHeaderSize, "the container header");
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) == 0)
        return ParseNcch(file, fileSize, 0, fileSize, 0);
    if (std::memcmp(header.data() + 0x100, "NCSD", 4) != 0)
        throw ImportError("the file is not a Nintendo 3DS NCSD/NCCH image (or it is encrypted "
                          "as a whole; .cia files are not supported)");
    std::string failures;
    for (int index = 0; index < 8; ++index) {
        const std::uint64_t base = U32(header.data() + 0x120 + index * 8) * kMediaUnit;
        const std::uint64_t size = U32(header.data() + 0x124 + index * 8) * kMediaUnit;
        if (base == 0 || size == 0)
            continue;
        try {
            return ParseNcch(file, fileSize, base, size, index);
        } catch (const ImportError& error) {
            if (!failures.empty()) failures += "; ";
            failures += error.what();
        }
    }
    throw ImportError("no usable decrypted application partition was found: " +
                      (failures.empty() ? std::string("the partition table is empty") : failures));
}

// ---------------------------------------------------------------- revisions

constexpr FileIdentity kCanonicalCode{
    4567040, "16a6b0aa4c4784680220a6f780f7f8a73cfb205557aa9f9f0e705179e0613220"};
constexpr FileIdentity kEurRomFs{
    479260672, "a3ad05dcf12893de5f1617a60f4e80f54f5e1c80069922a96a4f9c8c8cbac8ca"};
constexpr FileIdentity kUsaExheader{
    2048, "dbe5fa0174d73bffb75d7cf0fbaa05d3e0ee080df0e257afc58b6c45ae5de3d0"};
constexpr FileIdentity kEurBaselineExheader{
    2048, "9f065aec342b7df2421998deacedde44d475d36264f861cffd3381a1adf8900a"};
constexpr FileIdentity kEurCatalogueExheader{
    2048, "a584b067427f81edcca739497ec7b4ce1217eb9612090e6ae5b846e5674d12c1"};

constexpr Revision kRevisions[] = {
    {"oot3d-usa-input-canonical-eur-ef210566",
     "USA Rev 1 (En,Fr,Es) - converted to the EUR layout",
     {4567040, "ef210566e1d9d16879a746dfb063fcbad232f0171d860de906531ecc526cc020"},
     kUsaExheader,
     {473526272, "dd6def65af151d40fcbba7202c36bbdd8ed5b5b431373dc6a8c89bfd708af690"},
     kCanonicalCode,
     kUsaExheader,
     {473522176, "dae620379c805e96a6187c212dc2b177cf5fdb0c184d526a06ebaeeaa481aa56"},
     Adaptation::UsaToEur},
    {"oot3d-eur-catalogue-a584b067", "EUR (original and Rev 1)", kCanonicalCode,
     kEurCatalogueExheader, kEurRomFs, kCanonicalCode, kEurCatalogueExheader, kEurRomFs,
     Adaptation::None},
    {"oot3d-eur-project-baseline-16a6b0aa", "EUR (TriAevum project baseline dump)", kCanonicalCode,
     kEurBaselineExheader, kEurRomFs, kCanonicalCode, kEurBaselineExheader, kEurRomFs,
     Adaptation::None},
};

// ---------------------------------------------------------------- JSON

// Mirrors Python's json.dumps(value, indent=2, sort_keys=True) for ints,
// bools, ASCII strings, arrays and objects.
struct Json {
    enum class Kind { Int, Bool, String, Array, Object } Type = Kind::Int;
    std::int64_t Int = 0;
    bool Bool = false;
    std::string String;
    std::vector<Json> Items;
    std::vector<std::pair<std::string, Json>> Fields;

    Json() = default;
    Json(std::int64_t value) : Type(Kind::Int), Int(value) {}
    Json(std::uint64_t value) : Json(static_cast<std::int64_t>(value)) {}
    Json(std::uint32_t value) : Json(static_cast<std::int64_t>(value)) {}
    Json(int value) : Json(static_cast<std::int64_t>(value)) {}
    Json(bool value) : Type(Kind::Bool), Bool(value) {}
    Json(const char* value) : Type(Kind::String), String(value) {}
    Json(std::string value) : Type(Kind::String), String(std::move(value)) {}

    static Json Array(std::vector<Json> items = {}) {
        Json json;
        json.Type = Kind::Array;
        json.Items = std::move(items);
        return json;
    }
    static Json Object(std::vector<std::pair<std::string, Json>> fields = {}) {
        Json json;
        json.Type = Kind::Object;
        json.Fields = std::move(fields);
        return json;
    }
};

void Quote(std::string& out, const std::string& text) {
    out += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7F) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned char>(c));
            out += escaped;
        } else {
            out += c;
        }
    }
    out += '"';
}

void Render(std::string& out, const Json& value, int depth) {
    const auto newline = [&](int level) {
        out += '\n';
        out.append(static_cast<std::size_t>(level) * 2U, ' ');
    };
    switch (value.Type) {
    case Json::Kind::Int: out += std::to_string(value.Int); break;
    case Json::Kind::Bool: out += value.Bool ? "true" : "false"; break;
    case Json::Kind::String: Quote(out, value.String); break;
    case Json::Kind::Array:
        if (value.Items.empty()) { out += "[]"; break; }
        out += '[';
        for (std::size_t i = 0; i < value.Items.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            Render(out, value.Items[i], depth + 1);
        }
        newline(depth);
        out += ']';
        break;
    case Json::Kind::Object: {
        if (value.Fields.empty()) { out += "{}"; break; }
        std::vector<const std::pair<std::string, Json>*> sorted;
        for (const auto& field : value.Fields) sorted.push_back(&field);
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto* a, const auto* b) { return a->first < b->first; });
        out += '{';
        for (std::size_t i = 0; i < sorted.size(); ++i) {
            if (i) out += ',';
            newline(depth + 1);
            Quote(out, sorted[i]->first);
            out += ": ";
            Render(out, sorted[i]->second, depth + 1);
        }
        newline(depth);
        out += '}';
        break;
    }
    }
}

std::string Dump(const Json& value) {
    std::string out;
    Render(out, value, 0);
    out += '\n';
    return out;
}

Json IdentityJson(std::uint64_t bytes, const std::string& sha256) {
    return Json::Object({{"bytes", bytes}, {"sha256", sha256}});
}

// ---------------------------------------------------------------- RomFS

struct DirRecord {
    std::uint32_t Parent;
    std::vector<std::uint8_t> Name;
    std::u16string Path;
};

struct FileRecord {
    std::uint32_t Parent;
    std::vector<std::uint8_t> Name;
    std::u16string Path;
    std::uint64_t Payload, Length;
};

std::u16string Utf16(std::string_view ascii) {
    return {ascii.begin(), ascii.end()};
}

std::u16string Units(const std::vector<std::uint8_t>& encoded) {
    std::u16string text;
    for (std::size_t i = 0; i + 1 < encoded.size(); i += 2)
        text.push_back(static_cast<char16_t>(encoded[i] | (encoded[i + 1] << 8)));
    return text;
}

bool ValidName(const std::u16string& name) {
    if (name == u"." || name == u"..") return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char16_t c = name[i];
        if (c == u'/' || c == u'\\' || c == 0) return false;
        // Python decodes the name as UTF-16: reject unpaired surrogates too.
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= name.size() || name[i + 1] < 0xDC00 || name[i + 1] > 0xDFFF) return false;
            ++i;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
    }
    return true;
}

std::vector<std::uint8_t> EncodeName(std::string_view ascii) {
    std::vector<std::uint8_t> out;
    for (const char c : ascii) {
        out.push_back(static_cast<std::uint8_t>(c));
        out.push_back(0);
    }
    return out;
}

// ---------------------------------------------------------------- import pieces

std::string Stamp(const Source& source) {
    const auto entry = [](const fs::path& path) {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error) return std::string("missing");
        const auto time = fs::last_write_time(path, error);
        const long long ticks = error ? 0LL : static_cast<long long>(time.time_since_epoch().count());
        return std::to_string(size) + "@" + std::to_string(ticks);
    };
    if (source.Kind == SourceKind::Cartridge)
        return "cartridge:" + source.Path.filename().string() + ":" + entry(source.Path);
    return "extracted:code.bin:" + entry(source.Path / "code.bin") + ";exheader.bin:" +
           entry(source.Path / "exheader.bin") + ";romfs.bin:" + entry(source.Path / "romfs.bin");
}

std::string Receipt(const std::string& stamp, const Revision& revision) {
    return Dump(Json::Object({
        {"format", "oot3d_switch_rom_import_v1"},
        {"recipe", revision.Id},
        {"source", stamp},
        {"execution_inputs",
         Json::Object({{"code", IdentityJson(revision.Code.Bytes, revision.Code.Sha256)},
                       {"exheader", IdentityJson(revision.Exheader.Bytes, revision.Exheader.Sha256)},
                       {"romfs", IdentityJson(revision.RomFs.Bytes, revision.RomFs.Sha256)}})},
    }));
}

std::uint64_t FreeBytes(const fs::path& directory) {
#if defined(__SWITCH__)
    struct statvfs info{};
    if (statvfs(directory.string().c_str(), &info) != 0)
        return UINT64_MAX; // unknown: let the writes report it
    return static_cast<std::uint64_t>(info.f_bavail) * info.f_frsize;
#else
    std::error_code error;
    const auto info = fs::space(directory, error);
    return error ? UINT64_MAX : info.available;
#endif
}

// Horizon's FS does not rename over an existing file (EEXIST).
void Replace(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::remove(to, error);
    fs::rename(from, to, error);
    if (error)
        throw ImportError("cannot move " + from.string() + " to " + to.string() + ": " +
                          error.message());
}

Digest HashFile(const fs::path& path, std::vector<std::uint8_t>& buffer, std::uint64_t& done,
                std::uint64_t total, const ProgressCallback& progress) {
    File file = OpenFile(path, "rb");
    Sha256 hash;
    for (;;) {
        const std::size_t count = std::fread(buffer.data(), 1, buffer.size(), file.get());
        if (count == 0) {
            if (std::ferror(file.get()))
                throw ImportError(SystemError("cannot read", path));
            break;
        }
        hash.Update(buffer.data(), count);
        done += count;
        if (progress) progress({"Verifying", done, total});
    }
    return hash.Finish();
}

void ExpectIdentity(const char* what, const FileIdentity& expected, std::uint64_t bytes,
                    const Digest& digest) {
    if (!Matches(expected, bytes, digest)) {
        throw ImportError(std::string(what) + " does not match the recipe (got " +
                          std::to_string(bytes) + " bytes, sha256 " + Hex(digest) + ", expected " +
                          std::to_string(expected.Bytes) + " bytes, " + expected.Sha256 + ")");
    }
}

// O game/ de sdmc:/switch/oot3dre é só do OoT3D: o importador conhece só as
// receitas dele, e um outro título ali dividiria a pasta do save do Zelda.
// O program id passa pelo perfil de título (ctr_title_profile.h) antes de
// qualquer hash; outros jogos de 3DS vão pelo conversor do PC.
void RequireOot3dTitle(std::uint64_t programId) {
    if (Oot3dNativeGame::IsOot3dTitleId(programId))
        return;
    char text[17];
    std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(programId));
    throw ImportError(std::string("this dump is not Ocarina of Time 3D (program ID ") + text +
                      "); other 3DS games are not imported on the console: use "
                      "tools/ctr/ctr_convert.py on a PC");
}

} // namespace

// ---------------------------------------------------------------- SHA-256 API

Sha256::Sha256()
    : mState{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Update(const void* data, std::size_t size) {
    auto bytes = static_cast<const std::uint8_t*>(data);
    mLength += size;
    if (mBlockSize != 0) {
        const std::size_t take = std::min(size, mBlock.size() - mBlockSize);
        std::memcpy(mBlock.data() + mBlockSize, bytes, take);
        mBlockSize += take;
        bytes += take;
        size -= take;
        if (mBlockSize < mBlock.size())
            return;
        Sha256Blocks(mState.data(), mBlock.data(), 1);
        mBlockSize = 0;
    }
    const std::size_t blocks = size / 64;
    if (blocks != 0)
        Sha256Blocks(mState.data(), bytes, blocks);
    bytes += blocks * 64;
    size -= blocks * 64;
    std::memcpy(mBlock.data(), bytes, size);
    mBlockSize = size;
}

Digest Sha256::Finish() {
    const std::uint64_t bits = mLength * 8;
    mBlock[mBlockSize++] = 0x80;
    if (mBlockSize > 56) {
        std::fill(mBlock.begin() + static_cast<std::ptrdiff_t>(mBlockSize), mBlock.end(), 0);
        Sha256Blocks(mState.data(), mBlock.data(), 1);
        mBlockSize = 0;
    }
    std::fill(mBlock.begin() + static_cast<std::ptrdiff_t>(mBlockSize), mBlock.begin() + 56, 0);
    for (int i = 0; i < 8; ++i)
        mBlock[56 + i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    Sha256Blocks(mState.data(), mBlock.data(), 1);
    Digest digest{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j)
            digest[4 * i + j] = static_cast<std::uint8_t>(mState[i] >> (24 - 8 * j));
    return digest;
}

Digest Sha256Of(std::span<const std::uint8_t> data) {
    Sha256 hash;
    hash.Update(data.data(), data.size());
    return hash.Finish();
}

std::string Hex(const Digest& digest) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    for (const std::uint8_t byte : digest) {
        text += kDigits[byte >> 4];
        text += kDigits[byte & 15];
    }
    return text;
}

bool Matches(const FileIdentity& identity, std::uint64_t bytes, const Digest& digest) {
    return identity.Bytes == bytes && Hex(digest) == identity.Sha256;
}

std::span<const Revision> SupportedRevisions() {
    return kRevisions;
}

std::string SupportedRevisionsText() {
    std::string text;
    for (const Revision& revision : kRevisions) {
        text += "- The Legend of Zelda: Ocarina of Time 3D, ";
        text += revision.Name;
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------- building blocks

std::vector<std::uint8_t> DecompressExeFsCode(std::span<const std::uint8_t> compressed) {
    if (compressed.size() < 8)
        throw ImportError("compressed ExeFS .code is too small");
    const std::uint32_t topAndBottom = U32(compressed.data() + compressed.size() - 8);
    const std::uint64_t additional = U32(compressed.data() + compressed.size() - 4);
    const std::uint64_t outputSize = compressed.size() + additional;
    if (outputSize > kMaxDecompressedCode)
        throw ImportError("compressed ExeFS .code declares an invalid output size");
    const std::size_t footer = (topAndBottom >> 24) & 0xFF;
    const std::size_t encoded = topAndBottom & 0xFFFFFF;
    if (footer < 8 || footer > compressed.size() || encoded < footer || encoded > compressed.size())
        throw ImportError("compressed ExeFS .code has an invalid footer");

    std::size_t index = compressed.size() - footer;
    const std::size_t stop = compressed.size() - encoded;
    std::size_t outputIndex = outputSize;
    std::vector<std::uint8_t> output(outputSize);
    std::copy(compressed.begin(), compressed.end(), output.begin());
    while (index > stop) {
        --index;
        std::uint8_t control = compressed[index];
        for (int bit = 0; bit < 8; ++bit) {
            if (index <= stop || outputIndex == 0)
                break;
            if (control & 0x80) {
                if (index < 2)
                    throw ImportError("compressed ExeFS .code back-reference is truncated");
                index -= 2;
                const std::uint32_t segment = compressed[index] | (compressed[index + 1] << 8);
                const std::size_t segmentSize = ((segment >> 12) & 0xF) + 3;
                const std::size_t segmentOffset = (segment & 0xFFF) + 2;
                if (outputIndex < segmentSize)
                    throw ImportError("compressed ExeFS .code output underflow");
                for (std::size_t i = 0; i < segmentSize; ++i) {
                    const std::size_t from = outputIndex + segmentOffset;
                    if (from >= output.size())
                        throw ImportError("compressed ExeFS .code back-reference is out of range");
                    --outputIndex;
                    output[outputIndex] = output[from];
                }
            } else {
                --index;
                --outputIndex;
                output[outputIndex] = compressed[index];
            }
            control = static_cast<std::uint8_t>(control << 1);
        }
    }
    return output;
}

std::vector<std::uint8_t> ApplyCopyProgram(std::span<const std::uint8_t> source,
                                           std::span<const std::uint8_t> program,
                                           std::size_t count, std::size_t outputSize) {
    std::size_t cursor = 0;
    const auto next = [&]() {
        std::uint64_t value = 0;
        for (int shift = 0;; shift += 7) {
            if (cursor >= program.size() || shift > 56)
                throw ImportError("truncated COPY program");
            const std::uint8_t byte = program[cursor++];
            value |= std::uint64_t{byte & 0x7FU} << shift;
            if (!(byte & 0x80))
                return value;
        }
    };
    std::vector<std::uint8_t> output;
    output.reserve(outputSize);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t offset = next();
        const std::uint64_t length = next();
        if (length == 0 || offset > source.size() || length > source.size() - offset ||
            length > outputSize - output.size())
            throw ImportError("COPY operation exceeds input or output bounds");
        output.insert(output.end(), source.begin() + static_cast<std::ptrdiff_t>(offset),
                      source.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    if (cursor != program.size() || output.size() != outputSize)
        throw ImportError("incomplete COPY program output");
    return output;
}

std::uint32_t RomFsNameHash(std::uint32_t parent, std::span<const std::uint8_t> name) {
    std::uint32_t value = parent ^ 123456789U;
    for (std::size_t i = 0; i + 1 < name.size(); i += 2) {
        const std::uint32_t unit = name[i] | (name[i + 1] << 8);
        value = ((value >> 5) | (value << 27)) ^ unit;
    }
    return value;
}

std::vector<std::uint8_t> AdaptQmTable(std::span<const std::uint8_t> data, std::uint64_t fileSize) {
    if (data.size() < 16)
        throw ImportError("truncated QM header");
    const std::uint32_t version = U32(data.data() + 4);
    const std::uint32_t count = U32(data.data() + 8);
    const std::uint32_t reserved = U32(data.data() + 12);
    if (std::memcmp(data.data(), "QM\0\0", 4) != 0 || version != 4 || reserved != 0 || count == 0 ||
        count > 100000)
        throw ImportError("unsupported QM container");
    const std::uint64_t tableEnd = 16ULL + std::uint64_t{count} * 96ULL;
    if (data.size() != tableEnd || tableEnd > fileSize)
        throw ImportError("invalid QM record table");
    std::vector<std::uint8_t> output(data.begin(), data.end());
    static constexpr std::pair<int, int> kSlots[] = {{2, 1}, {4, 5}, {6, 7}};
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry = 16U + index * 96U;
        for (int slot = 0; slot < 9; ++slot) {
            const std::uint64_t offset = U32(data.data() + entry + 16 + slot * 8);
            const std::uint64_t size = U32(data.data() + entry + 20 + slot * 8);
            if ((offset != 0) != (size != 0) ||
                (offset != 0 && (offset < tableEnd || offset + size > fileSize)))
                throw ImportError("QM payload is outside the file");
        }
        // Native language slots: US English 1 -> EU English 2,
        // US French 5 -> EU French 4, US Spanish 7 -> EU Spanish 6.
        for (const auto& [destination, source] : kSlots) {
            const std::uint8_t* original = data.data() + entry + 16 + destination * 8;
            const std::uint8_t* replacement = data.data() + entry + 16 + source * 8;
            static constexpr std::uint8_t kZero[8] = {};
            if (std::memcmp(original, kZero, 8) != 0 && std::memcmp(original, replacement, 8) != 0)
                throw ImportError("QM destination language is already populated");
            std::memcpy(output.data() + entry + 16 + destination * 8, replacement, 8);
        }
    }
    return output;
}

RomFsAdaptation PlanUsaToEurRomFs(const RomFsReader& readImage, std::uint64_t imageSize) {
    RomFsAdaptation result;
    std::uint8_t magic[4] = {};
    if (imageSize >= 4) readImage(0, 4, magic);
    result.Base = std::memcmp(magic, "IVFC", 4) == 0 ? kIvfcSuperblock : 0;
    if (imageSize < result.Base)
        throw ImportError("RomFS extent outside image");
    const std::uint64_t size = imageSize - result.Base;
    result.Size = size;
    const auto read = [&](std::uint64_t offset, std::uint64_t count) {
        if (offset > size || count > size - offset)
            throw ImportError("RomFS extent outside image");
        std::vector<std::uint8_t> data(count);
        if (count) readImage(result.Base + offset, count, data.data());
        return data;
    };
    const auto rawHeader = read(0, 40);
    std::uint32_t h[10];
    for (int i = 0; i < 10; ++i) h[i] = U32(rawHeader.data() + 4 * i);
    if (h[0] != 40 || h[2] % 4 || h[6] % 4 || !h[2] || !h[6])
        throw ImportError("invalid Level-3 header");
    if (std::max({h[2], h[4], h[6], h[8]}) > 16U * 1024U * 1024U)
        throw ImportError("RomFS metadata exceeds limits");
    std::uint64_t end = 40;
    for (int i = 0; i < 4; ++i) {
        const std::uint64_t start = h[1 + 2 * i], length = h[2 + 2 * i];
        if (start < end || start % 4 || !length || start + length > size)
            throw ImportError("invalid or overlapping RomFS metadata extents");
        end = start + length;
    }
    if (h[9] < end || h[9] > size || h[9] % 16)
        throw ImportError("invalid RomFS data extent");
    auto dirs = read(h[3], h[4]);
    auto files = read(h[7], h[8]);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dirExtents, fileExtents;
    const auto record = [](std::vector<std::uint8_t>& table, std::uint64_t offset, std::size_t fixed,
                           std::vector<std::pair<std::uint64_t, std::uint64_t>>& extents) {
        if (offset + fixed > table.size() || offset % 4)
            throw ImportError("invalid RomFS record offset");
        const std::uint64_t nameSize = U32(table.data() + offset + fixed - 4);
        if (nameSize % 2 || offset + fixed + nameSize > table.size())
            throw ImportError("invalid RomFS record name");
        extents.emplace_back(offset, (offset + fixed + nameSize + 3) & ~std::uint64_t{3});
        return std::vector<std::uint8_t>(table.begin() + static_cast<std::ptrdiff_t>(offset + fixed),
                                         table.begin() + static_cast<std::ptrdiff_t>(offset + fixed + nameSize));
    };

    std::map<std::uint32_t, DirRecord> dirRecords;
    std::map<std::uint32_t, FileRecord> fileRecords;
    struct Pending { std::uint32_t Offset; std::u16string Prefix; std::uint32_t ExpectedParent; };
    std::vector<Pending> pending{{0, u"", 0}};
    while (!pending.empty()) {
        const Pending item = std::move(pending.back());
        pending.pop_back();
        if (dirRecords.count(item.Offset))
            throw ImportError("cyclic or duplicate RomFS directory");
        auto encoded = record(dirs, item.Offset, 24, dirExtents);
        const std::u16string name = Units(encoded);
        const std::uint8_t* entry = dirs.data() + item.Offset;
        const std::uint32_t parent = U32(entry), sibling = U32(entry + 4);
        const std::uint32_t childDir = U32(entry + 8);
        std::uint32_t childFile = U32(entry + 12);
        const bool root = item.Offset == 0;
        if (parent != item.ExpectedParent || (!name.empty() && !ValidName(name)) ||
            (root && (!name.empty() || sibling != kInvalid)) || (!root && name.empty()))
            throw ImportError("invalid RomFS directory ancestry");
        const std::u16string path = item.Prefix.empty() ? name : item.Prefix + u"/" + name;
        dirRecords[item.Offset] = {parent, std::move(encoded), path};
        if (sibling != kInvalid) pending.push_back({sibling, item.Prefix, item.ExpectedParent});
        if (childDir != kInvalid) pending.push_back({childDir, path, item.Offset});
        while (childFile != kInvalid) {
            if (fileRecords.count(childFile))
                throw ImportError("cyclic or duplicate RomFS file");
            auto fileName = record(files, childFile, 32, fileExtents);
            const std::u16string text = Units(fileName);
            const std::uint8_t* fileEntry = files.data() + childFile;
            const std::uint32_t owner = U32(fileEntry), following = U32(fileEntry + 4);
            const std::uint64_t payload = U64(fileEntry + 8), length = U64(fileEntry + 16);
            if (owner != item.Offset || text.empty() || !ValidName(text))
                throw ImportError("invalid RomFS file ancestry");
            if (h[9] + payload + length > size || payload > size || length > size)
                throw ImportError("RomFS file outside image");
            fileRecords[childFile] = {owner, std::move(fileName),
                                      path.empty() ? text : path + u"/" + text, payload, length};
            childFile = following;
        }
    }
    for (auto* extents : {&dirExtents, &fileExtents}) {
        std::sort(extents->begin(), extents->end());
        std::uint64_t limit = 0;
        for (const auto& [start, stop] : *extents) {
            if (start < limit) throw ImportError("overlapping RomFS records");
            limit = stop;
        }
    }
    std::set<std::u16string> dirPaths, filePaths;
    for (const auto& [offset, dir] : dirRecords) dirPaths.insert(dir.Path);
    for (const auto& [offset, file] : fileRecords) filePaths.insert(file.Path);
    if (dirPaths.size() != dirRecords.size() || filePaths.size() != fileRecords.size())
        throw ImportError("duplicate RomFS path");
    const std::u16string usDirs[] = {u"message/us", u"misc/us"};
    for (const auto& dir : usDirs)
        if (!dirPaths.count(dir)) throw ImportError("missing USA resource directories");
    if (dirPaths.count(u"message/eu") || dirPaths.count(u"misc/eu"))
        throw ImportError("EU resource directories already exist");
    // The EUR executable opens hint movie 181 + its language index (English 2,
    // French 4, Spanish 6); the USA ROM stores the same movies under 1, 5 and 7.
    static constexpr std::pair<std::string_view, std::string_view> kFileRenames[] = {
        {"message/us/us.qm", "eu.qm"},
        {"misc/hint/movie/hint182.moflex", "hint183.moflex"},
        {"misc/hint/movie/hint186.moflex", "hint185.moflex"},
        {"misc/hint/movie/hint188.moflex", "hint187.moflex"},
    };
    for (std::size_t i = 1; i < std::size(kFileRenames); ++i) {
        const std::u16string from = Utf16(kFileRenames[i].first);
        if (!filePaths.count(from)) throw ImportError("missing USA hint movies");
        const std::u16string to = from.substr(0, from.rfind(u'/') + 1) + Utf16(kFileRenames[i].second);
        if (filePaths.count(to)) throw ImportError("EU hint movies already exist");
    }
    const FileRecord* qm = nullptr;
    std::size_t qmMatches = 0;
    for (const auto& [offset, file] : fileRecords)
        if (file.Path == u"message/us/us.qm") { qm = &file; ++qmMatches; }
    if (qmMatches != 1)
        throw ImportError("missing USA message container");
    const std::uint64_t qmOffset = h[9] + qm->Payload, qmSize = qm->Length;
    const auto qmHead = read(qmOffset, 16);
    const std::uint32_t count = U32(qmHead.data() + 8);
    if (count == 0 || count > 100000 || 16ULL + count * 96ULL > qmSize)
        throw ImportError("invalid QM count");
    auto qmTable = AdaptQmTable(read(qmOffset, 16ULL + count * 96ULL), qmSize);

    std::vector<std::uint32_t> dirHashes(h[2] / 4, kInvalid), fileHashes(h[6] / 4, kInvalid);
    for (auto& [offset, dir] : dirRecords) {
        if (dir.Path == usDirs[0] || dir.Path == usDirs[1]) {
            dir.Name = EncodeName("eu");
            std::copy(dir.Name.begin(), dir.Name.end(), dirs.begin() + offset + 24);
        }
        const std::size_t bucket = RomFsNameHash(dir.Parent, dir.Name) % dirHashes.size();
        PutU32(dirs.data() + offset + 16, dirHashes[bucket]);
        dirHashes[bucket] = offset;
    }
    for (auto& [offset, file] : fileRecords) {
        for (const auto& [from, to] : kFileRenames) {
            if (file.Path == Utf16(from)) {
                file.Name = EncodeName(to);
                std::copy(file.Name.begin(), file.Name.end(), files.begin() + offset + 32);
            }
        }
        const std::size_t bucket = RomFsNameHash(file.Parent, file.Name) % fileHashes.size();
        PutU32(files.data() + offset + 24, fileHashes[bucket]);
        fileHashes[bucket] = offset;
    }
    const auto table = [](const std::vector<std::uint32_t>& values) {
        std::vector<std::uint8_t> bytes(values.size() * 4);
        for (std::size_t i = 0; i < values.size(); ++i) PutU32(bytes.data() + 4 * i, values[i]);
        return bytes;
    };
    result.Patches.push_back({h[1], table(dirHashes)});
    result.Patches.push_back({h[3], std::move(dirs)});
    result.Patches.push_back({h[5], table(fileHashes)});
    result.Patches.push_back({h[7], std::move(files)});
    result.Patches.push_back({qmOffset, std::move(qmTable)});
    result.Stats.Files = static_cast<std::uint32_t>(fileRecords.size());
    result.Stats.QmRecords = count;
    return result;
}

std::string BuildProcessManifest(std::span<const std::uint8_t> exheader,
                                 std::span<const std::uint8_t> code, std::uint64_t romfsFileSize,
                                 std::uint64_t romfsServiceOffset) {
    constexpr std::uint32_t kPage = 0x1000;
    constexpr std::uint32_t kHeapEnd = 0x10000000;
    constexpr std::uint32_t kTls = 0x1FF82000;
    // APPLICATION, SYSTEM and BASE region sizes by ExHeader memory mode
    // (the CTR kernel layout also used by Azahar's KernelSystem).
    static constexpr std::uint32_t kRegions[8][3] = {
        {0x04000000, 0x02C00000, 0x01400000}, {0, 0, 0},
        {0x06000000, 0x00C00000, 0x01400000}, {0x05000000, 0x01C00000, 0x01400000},
        {0x04800000, 0x02400000, 0x01400000}, {0x02000000, 0x04C00000, 0x01400000},
        {0x07C00000, 0x06400000, 0x02000000}, {0x0B200000, 0x02E00000, 0x02000000},
    };
    if (exheader.size() < 0x400)
        throw ImportError("ExHeader is truncated");
    const std::uint8_t* x = exheader.data();
    struct Segment { std::uint32_t Address, Pages, CodeSize; };
    const auto segment = [&](std::size_t offset) {
        return Segment{U32(x + offset), U32(x + offset + 4), U32(x + offset + 8)};
    };
    const Segment text = segment(0x10), ro = segment(0x20), data = segment(0x30);
    for (const Segment& item : {text, ro, data}) {
        if (item.Address % kPage || item.Pages == 0 ||
            item.CodeSize > std::uint64_t{item.Pages} * kPage)
            throw ImportError("ExHeader code segment layout is invalid");
    }
    const std::uint64_t textPages = std::uint64_t{text.Pages} * kPage;
    const std::uint64_t roPages = std::uint64_t{ro.Pages} * kPage;
    const std::uint64_t dataPages = std::uint64_t{data.Pages} * kPage;
    if (code.size() != textPages + roPages + dataPages)
        throw ImportError("decompressed code size does not match the ExHeader page total");
    if (text.Address + textPages > ro.Address || ro.Address + roPages > data.Address)
        throw ImportError("ExHeader code segments overlap");
    const std::uint32_t stackSize = U32(x + 0x1C);
    const std::uint32_t bssSize = U32(x + 0x3C);
    if (stackSize == 0 || stackSize % kPage)
        throw ImportError("main stack size is zero or not page aligned");
    const std::uint64_t bssPages = (std::uint64_t{bssSize} + kPage - 1) & ~std::uint64_t{kPage - 1};
    const std::uint64_t dataMapped = dataPages + bssPages;
    if (data.Address + dataMapped >= kHeapEnd - stackSize)
        throw ImportError("process image collides with the main stack");
    const std::uint32_t memoryMode = x[0x20D] & 0x0F;
    if (memoryMode >= 8 || kRegions[memoryMode][0] == 0)
        throw ImportError("unsupported CTR memory mode");
    const std::uint32_t appMemory = kRegions[memoryMode][0];
    const std::string name(reinterpret_cast<const char*>(x), strnlen(reinterpret_cast<const char*>(x), 8));
    for (const char c : name)
        if (static_cast<unsigned char>(c) >= 0x80)
            throw ImportError("ExHeader process name is not ASCII");

    if (romfsServiceOffset != 0 && romfsServiceOffset != kIvfcSuperblock)
        throw ImportError("unsupported RomFS service offset");
    const auto value = [](int offset, int size, std::uint64_t v) {
        return Json::Object({{"offset", offset}, {"size", size}, {"value", v}});
    };
    const auto region = [](const char* regionName, std::uint32_t address, std::uint32_t mapped,
                           bool writable, std::vector<Json> values) {
        return Json::Object({{"name", regionName}, {"address", address}, {"mapped_size", mapped},
                             {"writable", writable}, {"executable", false},
                             {"initial_values", Json::Array(std::move(values))}});
    };
    const auto seg = [](const char* segName, std::uint32_t address, std::uint64_t mapped,
                        std::uint32_t declared, std::uint64_t offset, std::uint64_t size,
                        bool writable, bool executable) {
        return Json::Object({{"name", segName}, {"address", address}, {"mapped_size", mapped},
                             {"declared_code_size", declared}, {"file_offset", offset},
                             {"file_size", size}, {"writable", writable},
                             {"executable", executable}});
    };
    const std::uint64_t initialCommit = textPages + roPages + dataMapped + stackSize;
    std::vector<Json> limits, initial;
    for (const std::uint32_t v : {0x18U, appMemory, 0x20U, 0x20U, 0x20U, 0x08U, 0x08U, 0x10U, 0x02U, 0U})
        limits.emplace_back(v);
    for (const std::uint64_t v : {std::uint64_t{0}, initialCommit, std::uint64_t{1}, std::uint64_t{0},
                                  std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{0},
                                  std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{0}})
        initial.emplace_back(v);

    const Json manifest = Json::Object({
        {"format", "oot3d_native_process_manifest_v1"},
        {"source",
         Json::Object({
             {"exheader_path", "exheader.bin"},
             {"exheader_sha256", Hex(Sha256Of(exheader))},
             {"code_bin_path", "code.bin"},
             {"code_bin_sha256", Hex(Sha256Of(code))},
             {"code_bin_size", std::uint64_t{code.size()}},
             {"romfs_image_path", "romfs.bin"},
             {"romfs_image_file_size", romfsFileSize},
             {"romfs_service_offset", romfsServiceOffset},
             {"romfs_service_size", romfsFileSize - romfsServiceOffset},
             {"layout_authority",
              "CTR ExHeader; page/BSS/stack/TLS semantics verified against Azahar NCCH loader"},
         })},
        {"process",
         Json::Object({
             {"name", name},
             // Program id do ExHeader, como no build_process_manifest.py: o
             // runtime escolhe o perfil de título por ele (ctr_title_profile.h).
             {"title_id", U64(x + 0x200)},
             {"entrypoint", text.Address},
             {"page_size", kPage},
             {"bss_declared_size", bssSize},
             {"bss_mapped_size", bssPages},
             {"segments",
              Json::Array({seg("text", text.Address, textPages, text.CodeSize, 0, textPages, false, true),
                           seg("rodata", ro.Address, roPages, ro.CodeSize, textPages, roPages, false, false),
                           seg("data_bss", data.Address, dataMapped, data.CodeSize, textPages + roPages,
                               dataPages, true, false)})},
             {"system_regions",
              Json::Array({
                  region("ctr_vram", 0x1F000000, 0x00600000, true, {}),
                  region("ctr_dsp_ram", 0x1FF00000, 0x00080000, true, {}),
                  region("ctr_config_memory", 0x1FF80000, kPage, false,
                         {value(0x01, 1, 0x3A), value(0x03, 1, 0x02),
                          value(0x08, 8, 0x0004013000008002ULL), value(0x10, 4, 0x02),
                          value(0x14, 1, 0x01), value(0x16, 1, 0x01), value(0x18, 4, 0xF450),
                          value(0x30, 4, memoryMode), value(0x40, 4, appMemory),
                          value(0x44, 4, kRegions[memoryMode][1]),
                          value(0x48, 4, kRegions[memoryMode][2]), value(0x62, 1, 0x3A),
                          value(0x63, 1, 0x02), value(0x64, 4, 0x02), value(0x68, 4, 0xF450)}),
                  region("ctr_shared_page", 0x1FF81000, kPage, false,
                         {value(0x04, 1, 0x01), value(0x66, 1, 0x03), value(0x67, 1, 0x02),
                          value(0x85, 1, 0x17), value(0x86, 1, 0x01)}),
              })},
             {"resource_limit", Json::Object({{"limit_values", Json::Array(std::move(limits))},
                                              {"initial_values", Json::Array(std::move(initial))}})},
             {"linear_heap", Json::Object({{"base_address", 0x14000000}, {"size", appMemory}})},
             {"heap", Json::Object({{"base_address", 0x08000000}, {"size", 0x08000000}})},
         })},
        {"primary_thread",
         Json::Object({
             {"stack_base_address", kHeapEnd - stackSize},
             {"stack_size", stackSize},
             {"tls_base_address", kTls},
             {"tls_size", kPage},
             {"thread_pointer", kTls},
             {"argument0", 0},
             {"initial_cpsr", 0x10},
             {"initial_fpscr", 0x03C00010},
             {"priority", std::uint32_t{x[0x20F]}},
         })},
    });
    return Dump(manifest);
}

// ---------------------------------------------------------------- sources

std::vector<Source> FindSources(const fs::path& root) {
    std::vector<Source> sources;
    std::error_code error;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        std::error_code typeError;
        if (!it->is_regular_file(typeError))
            continue;
        const std::string extension = Lower(it->path().extension().string());
        if (extension == ".3ds" || extension == ".cci")
            sources.push_back({SourceKind::Cartridge, it->path()});
    }
    std::sort(sources.begin(), sources.end(),
              [](const Source& a, const Source& b) { return a.Path < b.Path; });
    const fs::path import = root / "import";
    for (const char* name : {"code.bin", "exheader.bin", "romfs.bin"}) {
        if (fs::exists(import / name, error)) {
            sources.push_back({SourceKind::Extracted, import});
            break;
        }
    }
    return sources;
}

Plan Identify(const Source& source) {
    Plan plan;
    plan.Input = source;
    if (source.Kind == SourceKind::Cartridge) {
        File file = OpenFile(source.Path, "rb");
        const std::uint64_t fileSize = FileSize(source.Path);
        const Layout layout = FindTitleLayout(file.get(), fileSize);
        plan.ProgramId = layout.ProgramId;
        RequireOot3dTitle(plan.ProgramId);
        plan.Exheader = ReadAt(file.get(), layout.ExheaderOffset, kExheaderSize, "the ExHeader");
        if (layout.CodeSize > kMaxCodeFile)
            throw ImportError("ExeFS .code is too large");
        plan.Code = ReadAt(file.get(), layout.CodeOffset, layout.CodeSize, "ExeFS .code");
        if (layout.CompressedCode)
            plan.Code = DecompressExeFsCode(plan.Code);
        plan.RomFsPath = source.Path;
        plan.RomFsOffset = layout.RomFsOffset;
        plan.RomFsSize = layout.RomFsSize;
    } else {
        const fs::path code = source.Path / "code.bin";
        const fs::path exheader = source.Path / "exheader.bin";
        const fs::path romfs = source.Path / "romfs.bin";
        for (const fs::path& path : {code, exheader, romfs}) {
            std::error_code error;
            if (!fs::is_regular_file(path, error))
                throw ImportError("import/ needs code.bin, exheader.bin and romfs.bin; " +
                                  path.filename().string() + " is missing");
        }
        if (FileSize(exheader) != kExheaderSize)
            throw ImportError("import/exheader.bin must contain exactly 2048 bytes");
        const std::uint64_t codeSize = FileSize(code);
        if (codeSize == 0 || codeSize > kMaxCodeFile)
            throw ImportError("import/code.bin has an invalid size");
        File codeFile = OpenFile(code, "rb");
        plan.Code = ReadAt(codeFile.get(), 0, codeSize, "code.bin");
        File exheaderFile = OpenFile(exheader, "rb");
        plan.Exheader = ReadAt(exheaderFile.get(), 0, kExheaderSize, "exheader.bin");
        plan.ProgramId = U64(plan.Exheader.data() + 0x200);
        RequireOot3dTitle(plan.ProgramId);
        plan.RomFsPath = romfs;
        plan.RomFsSize = FileSize(romfs);
        File romfsFile = OpenFile(romfs, "rb");
        std::uint8_t magic[4] = {};
        if (plan.RomFsSize < 4)
            throw ImportError("import/romfs.bin is too small");
        ReadAt(romfsFile.get(), 0, 4, magic, "romfs.bin");
        if (std::memcmp(magic, "IVFC", 4) != 0)
            throw ImportError("import/romfs.bin must be a decrypted IVFC image");
    }
    const Digest code = Sha256Of(plan.Code);
    const Digest exheader = Sha256Of(plan.Exheader);
    for (const Revision& revision : kRevisions) {
        if (Matches(revision.SourceCode, plan.Code.size(), code) &&
            Matches(revision.SourceExheader, plan.Exheader.size(), exheader)) {
            if (plan.RomFsSize != revision.SourceRomFs.Bytes)
                throw ImportError(std::string("the RomFS size of this ") + revision.Name +
                                  " dump does not match (" + std::to_string(plan.RomFsSize) +
                                  " bytes, expected " + std::to_string(revision.SourceRomFs.Bytes) + ")");
            plan.Recipe = &revision;
            return plan;
        }
    }
    char programId[17];
    std::snprintf(programId, sizeof programId, "%016llX",
                  static_cast<unsigned long long>(plan.ProgramId));
    throw ImportError(std::string("unsupported game or revision (program ID ") + programId +
                      ", code sha256 " + Hex(code).substr(0, 16) + "..., ExHeader sha256 " +
                      Hex(exheader).substr(0, 16) + "...)");
}

bool ImportIsCurrent(const Source& source, const fs::path& gameDirectory) {
    std::error_code error;
    std::FILE* raw = std::fopen((gameDirectory / kReceiptName).string().c_str(), "rb");
    if (!raw)
        return false;
    File file(raw);
    std::string text;
    char buffer[1024];
    for (std::size_t count; (count = std::fread(buffer, 1, sizeof buffer, raw)) != 0;)
        text.append(buffer, count);
    const std::string stamp = Stamp(source);
    for (const Revision& revision : kRevisions) {
        if (text != Receipt(stamp, revision))
            continue;
        return fs::is_regular_file(gameDirectory / kManifestName, error) &&
               fs::file_size(gameDirectory / "code.bin", error) == revision.Code.Bytes && !error &&
               fs::file_size(gameDirectory / "exheader.bin", error) == revision.Exheader.Bytes && !error &&
               fs::file_size(gameDirectory / "romfs.bin", error) == revision.RomFs.Bytes && !error;
    }
    return false;
}

void RemoveSource(const Source& source) {
    std::error_code error;
    if (source.Kind == SourceKind::Cartridge) {
        if (!fs::remove(source.Path, error) || error)
            throw ImportError("cannot delete " + source.Path.string() + ": " + error.message());
        return;
    }
    for (const char* name : {"code.bin", "exheader.bin", "romfs.bin"}) {
        fs::remove(source.Path / name, error);
        if (error)
            throw ImportError("cannot delete " + (source.Path / name).string() + ": " + error.message());
    }
    fs::remove(source.Path, error); // only when empty
}

Result Import(const Plan& plan, const fs::path& gameDirectory, const ProgressCallback& progress) {
    if (!plan.Recipe)
        throw ImportError("no recipe selected");
    const Revision& recipe = *plan.Recipe;
    std::error_code error;
    fs::create_directories(gameDirectory, error);
    if (error)
        throw ImportError("cannot create " + gameDirectory.string() + ": " + error.message());
    // Leftovers of an interrupted import.
    for (fs::directory_iterator it(gameDirectory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".partial") {
            std::error_code removeError;
            fs::remove(it->path(), removeError);
        }
    }
    const std::uint64_t needed = recipe.Code.Bytes + recipe.Exheader.Bytes + recipe.RomFs.Bytes + (1U << 20);
    const std::uint64_t available = FreeBytes(gameDirectory);
    if (available < needed)
        throw ImportError("not enough free space on the SD card: " + std::to_string(needed >> 20) +
                          " MB needed, " + std::to_string(available >> 20) + " MB free");

    const fs::path codePartial = gameDirectory / "code.bin.partial";
    const fs::path exheaderPartial = gameDirectory / "exheader.bin.partial";
    const fs::path romfsPartial = gameDirectory / "romfs.bin.partial";
    const fs::path manifestPartial = gameDirectory / (std::string(kManifestName) + ".partial");
    const fs::path receiptPartial = gameDirectory / (std::string(kReceiptName) + ".partial");
    const auto cleanup = [&] {
        for (const fs::path& path : {codePartial, exheaderPartial, romfsPartial, manifestPartial, receiptPartial}) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
    };
    try {
        Result result;
        result.Recipe = &recipe;
        const std::uint64_t total = plan.RomFsSize + recipe.RomFs.Bytes + recipe.Code.Bytes + recipe.Exheader.Bytes;
        std::uint64_t done = 0;

        // Code and ExHeader.
        std::vector<std::uint8_t> code =
            recipe.Adapt == Adaptation::UsaToEur
                ? ApplyCopyProgram(plan.Code, kUsaCopyProgram, kUsaCopyCount, kUsaCopyOutputBytes)
                : plan.Code;
        result.Code = Sha256Of(code);
        ExpectIdentity("code.bin", recipe.Code, code.size(), result.Code);
        result.Exheader = Sha256Of(plan.Exheader);
        ExpectIdentity("exheader.bin", recipe.Exheader, plan.Exheader.size(), result.Exheader);
        WriteFileSynced(codePartial, code);
        WriteFileSynced(exheaderPartial, plan.Exheader);

        // RomFS: one streaming pass hashes the source, applies the patches and
        // hashes and writes the output.
        File input = OpenFile(plan.RomFsPath, "rb");
        RomFsAdaptation adaptation;
        if (recipe.Adapt == Adaptation::UsaToEur) {
            adaptation = PlanUsaToEurRomFs(
                [&](std::uint64_t offset, std::size_t count, std::uint8_t* out) {
                    if (offset > plan.RomFsSize || count > plan.RomFsSize - offset)
                        throw ImportError("RomFS extent outside image");
                    ReadAt(input.get(), plan.RomFsOffset + offset, count, out, "the RomFS");
                },
                plan.RomFsSize);
        } else {
            adaptation.Base = 0;
            adaptation.Size = plan.RomFsSize;
        }
        result.Resources = adaptation.Stats;
        std::vector<std::uint8_t> buffer(kChunk);
        {
            File output = OpenFile(romfsPartial, "wb");
            Sha256 sourceHash, outputHash;
            if (std::fseek(input.get(), static_cast<long>(plan.RomFsOffset), SEEK_SET) != 0)
                throw ImportError(SystemError("cannot seek", plan.RomFsPath));
            for (std::uint64_t position = 0; position < plan.RomFsSize;) {
                const std::size_t count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, plan.RomFsSize - position));
                if (std::fread(buffer.data(), 1, count, input.get()) != count)
                    throw ImportError("the dump ended while reading the RomFS");
                sourceHash.Update(buffer.data(), count);
                const std::uint64_t chunkEnd = position + count;
                const std::uint64_t start = std::max(position, adaptation.Base);
                if (start < chunkEnd) {
                    std::uint8_t* bytes = buffer.data() + (start - position);
                    const std::uint64_t outStart = start - adaptation.Base;
                    const std::uint64_t outEnd = chunkEnd - adaptation.Base;
                    for (const RomFsPatch& patch : adaptation.Patches) {
                        const std::uint64_t from = std::max(outStart, patch.Offset);
                        const std::uint64_t to = std::min(outEnd, patch.Offset + patch.Bytes.size());
                        if (from < to)
                            std::memcpy(bytes + (from - outStart), patch.Bytes.data() + (from - patch.Offset),
                                        static_cast<std::size_t>(to - from));
                    }
                    outputHash.Update(bytes, static_cast<std::size_t>(outEnd - outStart));
                    WriteAll(output.get(), bytes, static_cast<std::size_t>(outEnd - outStart), romfsPartial);
                }
                position = chunkEnd;
                done += count;
                if (progress)
                    progress({recipe.Adapt == Adaptation::UsaToEur ? "Converting RomFS" : "Copying RomFS",
                              done, total});
            }
            CloseSynced(output, romfsPartial);
            ExpectIdentity("the dump's RomFS", recipe.SourceRomFs, plan.RomFsSize, sourceHash.Finish());
            result.RomFs = outputHash.Finish();
            result.RomFsBytes = adaptation.Size;
            ExpectIdentity("romfs.bin", recipe.RomFs, adaptation.Size, result.RomFs);
        }
        input.reset();

        // Re-read what reached the SD card before anything is replaced.
        const std::pair<const fs::path*, const FileIdentity*> written[] = {
            {&codePartial, &recipe.Code}, {&exheaderPartial, &recipe.Exheader}, {&romfsPartial, &recipe.RomFs}};
        for (const auto& [path, identity] : written) {
            const Digest digest = HashFile(*path, buffer, done, total, progress);
            ExpectIdentity("the written file", *identity, FileSize(*path), digest);
        }

        const std::string manifest = BuildProcessManifest(
            plan.Exheader, code, adaptation.Size, recipe.Adapt == Adaptation::UsaToEur ? 0 : kIvfcSuperblock);
        WriteFileSynced(manifestPartial, {reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()});
        const std::string receipt = Receipt(Stamp(plan.Input), recipe);
        WriteFileSynced(receiptPartial, {reinterpret_cast<const std::uint8_t*>(receipt.data()), receipt.size()});

        // Commit: no manifest while the inputs change, the receipt last.
        for (const char* name : {kManifestName, kReceiptName}) {
            fs::remove(gameDirectory / name, error);
            if (error)
                throw ImportError("cannot replace " + (gameDirectory / name).string() + ": " + error.message());
        }
        Replace(codePartial, gameDirectory / "code.bin");
        Replace(exheaderPartial, gameDirectory / "exheader.bin");
        Replace(romfsPartial, gameDirectory / "romfs.bin");
        Replace(manifestPartial, gameDirectory / kManifestName);
        Replace(receiptPartial, gameDirectory / kReceiptName);
        return result;
    } catch (...) {
        cleanup();
        throw;
    }
}

} // namespace Oot3dNativeGame::RomImport
