// Host tests for oot3d_rom_import (synthetic NCSD/NCCH, LZSS, COPY program,
// RomFS adaptation, manifest). Plain main(); exits non-zero on failure.
//
//   oot3d_rom_import_tests                         run the tests
//   oot3d_rom_import_tests --import <src> <game>   import a real dump (a .3ds/.cci
//                                                  file or an extracted folder) into <game>

#include "oot3d_rom_import.h"
#include "ctr_title_profile.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace ri = Oot3dNativeGame::RomImport;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

// Program ids: OoT3D USA (ctr_title_profile.h) e um título qualquer (o SM3DL).
constexpr std::uint64_t kOot3dUsa = 0x0004000000033500ULL;
constexpr std::uint64_t kOtherTitle = 0x0004000000053F00ULL;

namespace {

int gFailures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++gFailures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

void Put32(Bytes& data, std::size_t offset, std::uint32_t value) {
    if (data.size() < offset + 4) data.resize(offset + 4);
    for (int i = 0; i < 4; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void Put64(Bytes& data, std::size_t offset, std::uint64_t value) {
    Put32(data, offset, static_cast<std::uint32_t>(value));
    Put32(data, offset + 4, static_cast<std::uint32_t>(value >> 32));
}
std::uint32_t Get32(const Bytes& data, std::size_t offset) {
    return data[offset] | (data[offset + 1] << 8) | (data[offset + 2] << 16) |
           (std::uint32_t{data[offset + 3]} << 24);
}
Bytes Ascii(std::string_view text) { return Bytes(text.begin(), text.end()); }
Bytes Utf16(std::string_view text) {
    Bytes out;
    for (char c : text) { out.push_back(static_cast<std::uint8_t>(c)); out.push_back(0); }
    return out;
}
void Varint(Bytes& out, std::uint64_t value) {
    do {
        std::uint8_t byte = value & 0x7F;
        value >>= 7;
        out.push_back(byte | (value ? 0x80 : 0));
    } while (value);
}

// ------------------------------------------------------------ SHA-256

void TestSha256() {
    CHECK(ri::Hex(ri::Sha256Of({})) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const Bytes abc = Ascii("abc");
    CHECK(ri::Hex(ri::Sha256Of(abc)) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const Bytes two = Ascii("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq");
    CHECK(ri::Hex(ri::Sha256Of(two)) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a', fed in uneven pieces across block boundaries.
    const Bytes million(1000000, 'a');
    ri::Sha256 hash;
    std::size_t offset = 0;
    for (std::size_t step = 1; offset < million.size(); step = step * 3 % 997 + 1) {
        const std::size_t count = std::min(step, million.size() - offset);
        hash.Update(million.data() + offset, count);
        offset += count;
    }
    CHECK(ri::Hex(hash.Finish()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ------------------------------------------------------------ LZSS

// "HEAD" + "ABC" * 34 as backwards LZSS: three literals, then five 18-byte
// and one 9-byte back-references at the shortest distance (3).
Bytes CompressedCode() {
    Bytes reading = {0x1F, 'C', 'B', 'A'};
    for (int i = 0; i < 5; ++i) { reading.push_back(0xF0); reading.push_back(0x00); }
    reading.push_back(0x80);
    reading.push_back(0x60); reading.push_back(0x00);
    Bytes compressed = Ascii("HEAD");
    compressed.insert(compressed.end(), reading.rbegin(), reading.rend());
    const std::uint32_t encoded = static_cast<std::uint32_t>(reading.size() + 8);
    const std::size_t total = compressed.size() + 8;
    Put32(compressed, compressed.size(), (8U << 24) | encoded);
    Put32(compressed, compressed.size(), static_cast<std::uint32_t>(106 - total));
    return compressed;
}

Bytes ExpectedCode() {
    Bytes out = Ascii("HEAD");
    for (int i = 0; i < 34; ++i) { out.push_back('A'); out.push_back('B'); out.push_back('C'); }
    return out;
}

void TestDecompress() {
    CHECK(ri::DecompressExeFsCode(CompressedCode()) == ExpectedCode());
    Bytes broken = CompressedCode();
    Put32(broken, broken.size() - 8, (4U << 24) | 3U); // footer below 8 bytes
    bool threw = false;
    try { (void)ri::DecompressExeFsCode(broken); } catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);
}

// ------------------------------------------------------------ COPY program

void TestCopyProgram() {
    const Bytes source = Ascii("hello world");
    Bytes program;
    for (auto [offset, length] : {std::pair{6, 5}, {5, 1}, {0, 5}}) {
        Varint(program, offset);
        Varint(program, length);
    }
    CHECK(ri::ApplyCopyProgram(source, program, 3, 11) == Ascii("world hello"));

    // Multi-byte varints.
    Bytes big(1000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 7);
    Bytes program2;
    Varint(program2, 300); Varint(program2, 200);
    Varint(program2, 0); Varint(program2, 130);
    const Bytes out = ri::ApplyCopyProgram(big, program2, 2, 330);
    CHECK(Bytes(out.begin(), out.begin() + 200) == Bytes(big.begin() + 300, big.begin() + 500));
    CHECK(Bytes(out.begin() + 200, out.end()) == Bytes(big.begin(), big.begin() + 130));

    bool threw = false;
    try { (void)ri::ApplyCopyProgram(source, program, 3, 12); } catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);
    threw = false;
    Bytes outside;
    Varint(outside, 8); Varint(outside, 5);
    try { (void)ri::ApplyCopyProgram(source, outside, 1, 5); } catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);
}

// ------------------------------------------------------------ RomFS

struct Dir { std::string Name; int Parent; };
struct File { std::string Name; int Parent; Bytes Data; };

struct Built {
    Bytes Image;
    std::vector<std::uint32_t> DirOffsets, FileOffsets;
};

Bytes QmFile() {
    constexpr std::uint32_t count = 2;
    const std::uint32_t tableEnd = 16 + count * 96;
    Bytes qm(tableEnd + 64, 0);
    std::memcpy(qm.data(), "QM\0\0", 4);
    Put32(qm, 4, 4);
    Put32(qm, 8, count);
    for (std::uint32_t record = 0; record < count; ++record) {
        const std::size_t entry = 16 + record * 96;
        for (int slot : {1, 5, 7}) {
            Put32(qm, entry + 16 + slot * 8, tableEnd + record * 32 + slot);
            Put32(qm, entry + 20 + slot * 8, 4);
        }
        Put32(qm, entry + 16 + 0 * 8, tableEnd); // Japanese slot, untouched
        Put32(qm, entry + 20 + 0 * 8, 2);
    }
    return qm;
}

Built BuildRomFs(bool ivfc) {
    const std::vector<Dir> dirs = {
        {"", 0}, {"message", 0}, {"misc", 0}, {"us", 1}, {"us", 2}, {"hint", 2}, {"movie", 5},
    };
    const std::vector<File> files = {
        {"us.qm", 3, QmFile()},
        {"title.bin", 4, Ascii("misc-us-payload")},
        {"hint181.moflex", 6, Ascii("jp")},
        {"hint182.moflex", 6, Ascii("en")},
        {"hint186.moflex", 6, Ascii("fr")},
        {"hint188.moflex", 6, Ascii("es")},
    };
    Built built;
    const auto align4 = [](std::size_t n) { return (n + 3) & ~std::size_t{3}; };
    Bytes dirTable, fileTable;
    for (const Dir& dir : dirs) {
        built.DirOffsets.push_back(static_cast<std::uint32_t>(dirTable.size()));
        dirTable.resize(dirTable.size() + 24 + align4(dir.Name.size() * 2));
    }
    for (const File& file : files) {
        built.FileOffsets.push_back(static_cast<std::uint32_t>(fileTable.size()));
        fileTable.resize(fileTable.size() + 32 + align4(file.Name.size() * 2));
    }
    Bytes data;
    std::vector<std::uint64_t> dataOffsets;
    for (const File& file : files) {
        dataOffsets.push_back(data.size());
        data.insert(data.end(), file.Data.begin(), file.Data.end());
        data.resize((data.size() + 15) & ~std::size_t{15});
    }
    constexpr std::uint32_t kNone = 0xFFFFFFFF;
    for (std::size_t i = 0; i < dirs.size(); ++i) {
        const std::size_t at = built.DirOffsets[i];
        std::uint32_t sibling = kNone, childDir = kNone, childFile = kNone;
        for (std::size_t j = i + 1; j < dirs.size() && i != 0; ++j)
            if (dirs[j].Parent == dirs[i].Parent) { sibling = built.DirOffsets[j]; break; }
        for (std::size_t j = 1; j < dirs.size(); ++j)
            if (dirs[j].Parent == static_cast<int>(i)) { childDir = built.DirOffsets[j]; break; }
        for (std::size_t j = 0; j < files.size(); ++j)
            if (files[j].Parent == static_cast<int>(i)) { childFile = built.FileOffsets[j]; break; }
        Put32(dirTable, at, built.DirOffsets[dirs[i].Parent]);
        Put32(dirTable, at + 4, sibling);
        Put32(dirTable, at + 8, childDir);
        Put32(dirTable, at + 12, childFile);
        Put32(dirTable, at + 16, 0x12345678); // stale hash link, rebuilt by the adapter
        Put32(dirTable, at + 20, static_cast<std::uint32_t>(dirs[i].Name.size() * 2));
        const Bytes name = Utf16(dirs[i].Name);
        std::copy(name.begin(), name.end(), dirTable.begin() + at + 24);
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t at = built.FileOffsets[i];
        std::uint32_t sibling = kNone;
        for (std::size_t j = i + 1; j < files.size(); ++j)
            if (files[j].Parent == files[i].Parent) { sibling = built.FileOffsets[j]; break; }
        Put32(fileTable, at, built.DirOffsets[files[i].Parent]);
        Put32(fileTable, at + 4, sibling);
        Put64(fileTable, at + 8, dataOffsets[i]);
        Put64(fileTable, at + 16, files[i].Data.size());
        Put32(fileTable, at + 24, 0x12345678);
        Put32(fileTable, at + 28, static_cast<std::uint32_t>(files[i].Name.size() * 2));
        const Bytes name = Utf16(files[i].Name);
        std::copy(name.begin(), name.end(), fileTable.begin() + at + 32);
    }
    const std::uint32_t dirHashSize = 3 * 4, fileHashSize = 5 * 4;
    std::uint32_t cursor = 40;
    const std::uint32_t dirHashOff = cursor; cursor += dirHashSize;
    const std::uint32_t dirMetaOff = cursor; cursor += static_cast<std::uint32_t>(dirTable.size());
    const std::uint32_t fileHashOff = cursor; cursor += fileHashSize;
    const std::uint32_t fileMetaOff = cursor; cursor += static_cast<std::uint32_t>(fileTable.size());
    const std::uint32_t dataOff = (cursor + 15) & ~15U;
    Bytes level3(dataOff + data.size(), 0xEE);
    const std::uint32_t header[10] = {40, dirHashOff, dirHashSize, dirMetaOff, static_cast<std::uint32_t>(dirTable.size()),
                                      fileHashOff, fileHashSize, fileMetaOff, static_cast<std::uint32_t>(fileTable.size()), dataOff};
    for (int i = 0; i < 10; ++i) Put32(level3, 4 * i, header[i]);
    std::copy(dirTable.begin(), dirTable.end(), level3.begin() + dirMetaOff);
    std::copy(fileTable.begin(), fileTable.end(), level3.begin() + fileMetaOff);
    std::copy(data.begin(), data.end(), level3.begin() + dataOff);
    if (ivfc) {
        built.Image.assign(0x1000, 0xAA);
        std::memcpy(built.Image.data(), "IVFC", 4);
    }
    built.Image.insert(built.Image.end(), level3.begin(), level3.end());
    return built;
}

Bytes Apply(const Bytes& image, const ri::RomFsAdaptation& adaptation) {
    Bytes out(image.begin() + static_cast<std::ptrdiff_t>(adaptation.Base), image.end());
    for (const ri::RomFsPatch& patch : adaptation.Patches)
        std::copy(patch.Bytes.begin(), patch.Bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(patch.Offset));
    return out;
}

// Looks a name up through the rebuilt hash chain, as the game's RomFS reader does.
std::uint32_t Lookup(const Bytes& level3, bool file, std::uint32_t parent, std::string_view name) {
    const std::uint32_t hashOff = Get32(level3, file ? 20 : 4), hashSize = Get32(level3, file ? 24 : 8);
    const std::uint32_t metaOff = Get32(level3, file ? 28 : 12);
    const Bytes encoded = Utf16(name);
    const std::uint32_t bucket = ri::RomFsNameHash(parent, encoded) % (hashSize / 4);
    const std::size_t fixed = file ? 32 : 24;
    for (std::uint32_t at = Get32(level3, hashOff + bucket * 4); at != 0xFFFFFFFF;
         at = Get32(level3, metaOff + at + fixed - 8)) {
        const std::size_t entry = metaOff + at;
        const std::uint32_t length = Get32(level3, entry + fixed - 4);
        if (Get32(level3, entry) == parent && length == encoded.size() &&
            std::equal(encoded.begin(), encoded.end(), level3.begin() + entry + fixed))
            return at;
    }
    return 0xFFFFFFFF;
}

void TestRomFs(bool ivfc) {
    const Built built = BuildRomFs(ivfc);
    const auto reader = [&](std::uint64_t offset, std::size_t count, std::uint8_t* out) {
        if (offset + count > built.Image.size()) throw ri::ImportError("read past the image");
        std::memcpy(out, built.Image.data() + offset, count);
    };
    const ri::RomFsAdaptation adaptation = ri::PlanUsaToEurRomFs(reader, built.Image.size());
    CHECK(adaptation.Base == (ivfc ? 0x1000U : 0U));
    CHECK(adaptation.Size == built.Image.size() - adaptation.Base);
    CHECK(adaptation.Stats.Files == 6);
    CHECK(adaptation.Stats.QmRecords == 2);
    const Bytes out = Apply(built.Image, adaptation);

    const std::uint32_t message = built.DirOffsets[1], misc = built.DirOffsets[2];
    const std::uint32_t movie = built.DirOffsets[6];
    CHECK(Lookup(out, false, 0, "message") == message);
    CHECK(Lookup(out, false, 0, "misc") == misc);
    CHECK(Lookup(out, false, message, "eu") == built.DirOffsets[3]);
    CHECK(Lookup(out, false, misc, "eu") == built.DirOffsets[4]);
    CHECK(Lookup(out, false, message, "us") == 0xFFFFFFFF);
    CHECK(Lookup(out, true, built.DirOffsets[3], "eu.qm") == built.FileOffsets[0]);
    CHECK(Lookup(out, true, built.DirOffsets[4], "title.bin") == built.FileOffsets[1]);
    CHECK(Lookup(out, true, movie, "hint181.moflex") == built.FileOffsets[2]);
    CHECK(Lookup(out, true, movie, "hint183.moflex") == built.FileOffsets[3]);
    CHECK(Lookup(out, true, movie, "hint185.moflex") == built.FileOffsets[4]);
    CHECK(Lookup(out, true, movie, "hint187.moflex") == built.FileOffsets[5]);
    CHECK(Lookup(out, true, movie, "hint182.moflex") == 0xFFFFFFFF);

    // QM slots 2/4/6 now reference the USA 1/5/7 payloads; the rest is unchanged.
    const std::uint32_t dataOff = Get32(out, 36);
    const std::size_t qm = dataOff; // first file payload
    for (int record = 0; record < 2; ++record) {
        const std::size_t entry = qm + 16 + record * 96 + 16;
        for (auto [destination, source] : {std::pair{2, 1}, {4, 5}, {6, 7}}) {
            CHECK(Get32(out, entry + destination * 8) == Get32(out, entry + source * 8));
            CHECK(Get32(out, entry + destination * 8 + 4) == 4);
        }
        CHECK(Get32(out, entry) == Get32(built.Image, adaptation.Base + entry));
    }
    // Payloads untouched.
    CHECK(std::equal(out.begin() + dataOff + 16 + 2 * 96, out.end(),
                     built.Image.begin() + static_cast<std::ptrdiff_t>(adaptation.Base + dataOff + 16 + 2 * 96)));

    // Running it on its own output fails: the EU names already exist.
    bool threw = false;
    try {
        (void)ri::PlanUsaToEurRomFs(
            [&](std::uint64_t offset, std::size_t count, std::uint8_t* dst) { std::memcpy(dst, out.data() + offset, count); },
            out.size());
    } catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);
}

void TestQmRejectsPopulatedSlot() {
    Bytes qm = QmFile();
    const std::size_t tableEnd = 16 + 2 * 96;
    Put32(qm, 16 + 16 + 2 * 8, static_cast<std::uint32_t>(tableEnd));
    Put32(qm, 16 + 20 + 2 * 8, 1);
    bool threw = false;
    try { (void)ri::AdaptQmTable(Bytes(qm.begin(), qm.begin() + tableEnd), qm.size()); }
    catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);
}

// ------------------------------------------------------------ NCSD/NCCH + manifest

Bytes Exheader(bool compressed, std::uint64_t titleId = kOot3dUsa) {
    Bytes x(0x800, 0);
    std::memcpy(x.data(), "CtrApp", 6);
    x[0x0D] = compressed ? 1 : 0;
    // text: 1 page, ro: 1 page, data: 1 page (code is 3 pages).
    Put32(x, 0x10, 0x00100000); Put32(x, 0x14, 1); Put32(x, 0x18, 0x0F00);
    Put32(x, 0x1C, 0x4000);     // stack
    Put32(x, 0x20, 0x00101000); Put32(x, 0x24, 1); Put32(x, 0x28, 0x0800);
    Put32(x, 0x30, 0x00102000); Put32(x, 0x34, 1); Put32(x, 0x38, 0x0100);
    Put32(x, 0x3C, 0x1800);     // bss
    x[0x20D] = 0;               // memory mode 0 (64 MB application)
    x[0x20F] = 48;
    Put64(x, 0x200, titleId);
    return x;
}

// NCSD with one NCCH partition at 0x4000: ExHeader, ExeFS with .code, RomFS.
Bytes BuildNcsd(const Bytes& code, bool compressed, bool encrypted,
                std::uint64_t titleId = kOot3dUsa) {
    constexpr std::size_t base = 0x4000;
    Bytes image(base, 0);
    std::memcpy(image.data() + 0x100, "NCSD", 4);
    Bytes ncch(0x200, 0);
    std::memcpy(ncch.data() + 0x100, "NCCH", 4);
    Put64(ncch, 0x118, titleId);
    ncch[0x18D] = 0x02;
    Put32(ncch, 0x180, 0x400);
    const Bytes exheader = Exheader(compressed, titleId);
    ncch.insert(ncch.end(), exheader.begin(), exheader.end()); // 0x200..0xA00
    ncch.resize(0xA00, 0);
    Bytes exefs(0x200, 0);
    if (!encrypted) std::memcpy(exefs.data(), ".code", 5);
    Put32(exefs, 8, 0);
    Put32(exefs, 12, static_cast<std::uint32_t>(code.size()));
    exefs.insert(exefs.end(), code.begin(), code.end());
    exefs.resize((exefs.size() + 0x1FF) & ~std::size_t{0x1FF}, 0);
    Put32(ncch, 0x1A0, 0xA00 / 0x200);
    Put32(ncch, 0x1A4, static_cast<std::uint32_t>(exefs.size() / 0x200));
    ncch.insert(ncch.end(), exefs.begin(), exefs.end());
    Bytes romfs(0x2000, 0);
    std::memcpy(romfs.data(), encrypted ? "\x13\x37\x00\x00" : "IVFC", 4);
    Put32(ncch, 0x1B0, static_cast<std::uint32_t>(ncch.size() / 0x200));
    Put32(ncch, 0x1B4, static_cast<std::uint32_t>(romfs.size() / 0x200));
    ncch.insert(ncch.end(), romfs.begin(), romfs.end());
    Put32(image, 0x120, base / 0x200);
    Put32(image, 0x124, static_cast<std::uint32_t>(ncch.size() / 0x200));
    image.insert(image.end(), ncch.begin(), ncch.end());
    return image;
}

std::string IdentifyError(const fs::path& path) {
    try {
        (void)ri::Identify({ri::SourceKind::Cartridge, path});
    } catch (const ri::ImportError& error) {
        return error.what();
    }
    return {};
}

void WriteFile(const fs::path& path, const Bytes& data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                static_cast<std::streamsize>(data.size()));
}

void TestContainer(const fs::path& directory) {
    // Compressed .code: Identify parses NCSD -> NCCH, decompresses and then
    // rejects the unknown revision, quoting the decompressed code hash.
    const fs::path rom = directory / "synthetic.cci";
    WriteFile(rom, BuildNcsd(CompressedCode(), true, false));
    std::string message = IdentifyError(rom);
    CHECK(message.find("unsupported game or revision") != std::string::npos);
    CHECK(message.find("0004000000033500") != std::string::npos);
    CHECK(message.find(ri::Hex(ri::Sha256Of(ExpectedCode())).substr(0, 16)) != std::string::npos);

    WriteFile(rom, BuildNcsd(ExpectedCode(), false, false));
    message = IdentifyError(rom);
    CHECK(message.find(ri::Hex(ri::Sha256Of(ExpectedCode())).substr(0, 16)) != std::string::npos);

    WriteFile(rom, BuildNcsd(ExpectedCode(), false, true));
    message = IdentifyError(rom);
    CHECK(message.find("encrypted") != std::string::npos);

    WriteFile(rom, Bytes(0x400, 0x5A));
    CHECK(IdentifyError(rom).find("not a Nintendo 3DS") != std::string::npos);

    // Another 3DS game (generic profile in ctr_title_profile.h): refused by its
    // program id before any hash, pointing at the PC converter. OoT3D's game/
    // never receives another title.
    WriteFile(rom, BuildNcsd(CompressedCode(), true, false, kOtherTitle));
    message = IdentifyError(rom);
    CHECK(message.find("is not Ocarina of Time 3D") != std::string::npos);
    CHECK(message.find("0004000000053F00") != std::string::npos);
    CHECK(message.find("tools/ctr/ctr_convert.py") != std::string::npos);
    fs::create_directories(directory / "other");
    WriteFile(directory / "other" / "code.bin", ExpectedCode());
    WriteFile(directory / "other" / "exheader.bin", Exheader(false, kOtherTitle));
    Bytes ivfc(0x2000, 0);
    std::memcpy(ivfc.data(), "IVFC", 4);
    WriteFile(directory / "other" / "romfs.bin", ivfc);
    message.clear();
    try { (void)ri::Identify({ri::SourceKind::Extracted, directory / "other"}); }
    catch (const ri::ImportError& error) { message = error.what(); }
    CHECK(message.find("is not Ocarina of Time 3D") != std::string::npos);
    fs::remove_all(directory / "other");
    WriteFile(rom, BuildNcsd(CompressedCode(), true, false));

    // Source discovery: dumps sorted by name, then import/.
    WriteFile(directory / "b.3DS", Bytes(16));
    WriteFile(directory / "notes.txt", Bytes(16));
    fs::create_directories(directory / "import");
    WriteFile(directory / "import" / "romfs.bin", Bytes(16));
    const auto sources = ri::FindSources(directory);
    CHECK(sources.size() == 3);
    if (sources.size() == 3) {
        CHECK(sources[0].Path.filename() == "b.3DS");
        CHECK(sources[1].Path.filename() == "synthetic.cci");
        CHECK(sources[2].Kind == ri::SourceKind::Extracted);
    }
    const auto extracted = ri::FindSources(directory).back();
    bool threw = false;
    try { (void)ri::Identify(extracted); } catch (const ri::ImportError& error) {
        threw = std::string(error.what()).find("code.bin is missing") != std::string::npos;
    }
    CHECK(threw);
    CHECK(!ri::ImportIsCurrent(sources[1], directory / "game"));
}

void TestManifest() {
    Bytes code(3 * 0x1000, 0x11);
    const std::string manifest = ri::BuildProcessManifest(Exheader(false), code, 0x3000, 0x1000);
    CHECK(manifest.rfind("{\n  \"format\": \"oot3d_native_process_manifest_v1\",\n  \"primary_thread\": {\n"
                         "    \"argument0\": 0,\n", 0) == 0);
    CHECK(manifest.find("\"bss_mapped_size\": 8192,") != std::string::npos);
    CHECK(manifest.find("\"initial_values\": [],") != std::string::npos);
    CHECK(manifest.find("\"romfs_service_offset\": 4096,") != std::string::npos);
    CHECK(manifest.find("\"romfs_service_size\": 8192\n") != std::string::npos);
    CHECK(manifest.find("\"stack_base_address\": 268419072,") != std::string::npos);
    CHECK(manifest.find("\"name\": \"CtrApp\",") != std::string::npos);
    CHECK(manifest.find("\"value\": 1127205576933378") != std::string::npos);
    CHECK(manifest.size() > 2 && manifest.substr(manifest.size() - 2) == "}\n");
    bool threw = false;
    try { (void)ri::BuildProcessManifest(Exheader(false), Bytes(10), 0x3000, 0); } catch (const ri::ImportError&) { threw = true; }
    CHECK(threw);

    // process.title_id = program id do ExHeader, como no build_process_manifest.py:
    // o runtime escolhe o perfil de título por ele (oot3d_native_a32_process_image.cpp
    // lê o campo como inteiro sem sinal). Último campo de "process" (chaves ordenadas).
    CHECK(manifest.find("\"title_id\": 1125899907052800\n") != std::string::npos);
    const auto document = nlohmann::json::parse(manifest);
    const auto& titleId = document.at("process").at("title_id");
    CHECK(titleId.is_number_unsigned());
    CHECK(titleId.get<std::uint64_t>() == kOot3dUsa);
    CHECK(Oot3dNativeGame::SelectCtrTitleProfile(titleId.get<std::uint64_t>()).Kind ==
          Oot3dNativeGame::CtrTitleKind::Oot3d);
}

int RealImport(const fs::path& source, const fs::path& game) {
    const auto started = std::chrono::steady_clock::now();
    const ri::Source input{fs::is_directory(source) ? ri::SourceKind::Extracted : ri::SourceKind::Cartridge, source};
    const ri::Plan plan = ri::Identify(input);
    std::printf("recipe %s (%s)\n", plan.Recipe->Id, plan.Recipe->Name);
    int lastPercent = -1;
    const ri::Result result = ri::Import(plan, game, [&](const ri::Progress& progress) {
        const int percent = static_cast<int>(progress.Done * 100 / progress.Total);
        if (percent / 10 != lastPercent / 10) {
            std::printf("  %s %d%%\n", progress.Stage, percent);
            lastPercent = percent;
        }
    });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("code.bin     %s\nexheader.bin %s\nromfs.bin    %s (%llu bytes, %u files, %u QM records)\n",
                ri::Hex(result.Code).c_str(), ri::Hex(result.Exheader).c_str(), ri::Hex(result.RomFs).c_str(),
                static_cast<unsigned long long>(result.RomFsBytes), result.Resources.Files,
                result.Resources.QmRecords);
    std::printf("seconds %.2f; current after import: %s\n", seconds,
                ri::ImportIsCurrent(input, game) ? "yes" : "no");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--import") {
        try {
            return RealImport(argv[2], argv[3]);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "import failed: %s\n", error.what());
            return 1;
        }
    }
    const fs::path directory = fs::temp_directory_path() / "oot3d_rom_import_tests";
    fs::remove_all(directory);
    fs::create_directories(directory);
    try {
        TestSha256();
        TestDecompress();
        TestCopyProgram();
        TestRomFs(false);
        TestRomFs(true);
        TestQmRejectsPopulatedSlot();
        TestContainer(directory);
        TestManifest();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", error.what());
        ++gFailures;
    }
    fs::remove_all(directory);
    if (gFailures) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("oot3d_rom_import_tests: ok\n");
    return 0;
}
