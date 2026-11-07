// Host tests for oot3d_topscreen_import (ZIP reader, RomFS reader, texture
// codecs, QBF pairing, font pack, PNG codec, HD UI atlas extraction and 3x
// reduction, offer rules, verified writes). Plain main(); exits non-zero on
// failure.
//
//   oot3d_topscreen_import_tests                      run the tests
//   oot3d_topscreen_import_tests --build <archive> <romfs.bin> <pack.o3tu> <font_coverage.zip> [<ui_atlas_hd.png>]
//       build the outputs from the real mod archive and RomFS (compare the
//       first two with build_topscreen_texture_override_pack.py /
//       prepare_qbf_font_pack.py)

#include "oot3d_topscreen_import.h"

#include <zlib.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ts = Oot3dNativeGame::TopScreenImport;
namespace ri = Oot3dNativeGame::RomImport;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

int gFailures = 0;

void Check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++gFailures;
    }
}
#define CHECK(x) Check((x), #x, __LINE__)

template <typename Function>
void CheckThrows(Function function, std::string_view fragment, int line) {
    try {
        function();
    } catch (const ts::ImportError& error) {
        if (std::string_view(error.what()).find(fragment) == std::string_view::npos) {
            std::fprintf(stderr, "FAIL line %d: unexpected message: %s\n", line, error.what());
            ++gFailures;
        }
        return;
    }
    std::fprintf(stderr, "FAIL line %d: no ImportError (%.*s)\n", line, static_cast<int>(fragment.size()),
                 fragment.data());
    ++gFailures;
}
#define CHECK_THROWS(expression, fragment) CheckThrows([&] { (void)(expression); }, fragment, __LINE__)

void Put16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}
void Put32(Bytes& out, std::uint32_t value) {
    Put16(out, static_cast<std::uint16_t>(value));
    Put16(out, static_cast<std::uint16_t>(value >> 16));
}
void Put64(Bytes& out, std::uint64_t value) {
    Put32(out, static_cast<std::uint32_t>(value));
    Put32(out, static_cast<std::uint32_t>(value >> 32));
}
void Set32(Bytes& out, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
Bytes Text(std::string_view text) { return Bytes(text.begin(), text.end()); }
std::string AsString(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

void WriteFile(const fs::path& path, const Bytes& data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()),
                                                static_cast<std::streamsize>(data.size()));
}
Bytes ReadFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// ---- Fixtures ----

// A Level-3 RomFS image holding `files` (path -> data); child lists only.
Bytes MakeRomFs(const std::map<std::string, Bytes>& files) {
    struct Directory {
        std::string Name;
        int Parent = -1;
        std::vector<int> Children;
        std::vector<std::pair<std::string, const Bytes*>> Files;
        std::uint32_t Offset = 0;
    };
    std::vector<Directory> directories(1);
    for (const auto& [path, data] : files) {
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
    const auto utf16 = [](const std::string& name) {
        Bytes out;
        for (const char c : name) { out.push_back(static_cast<std::uint8_t>(c)); out.push_back(0); }
        return out;
    };
    const auto padded = [](std::size_t size) { return (size + 3) & ~std::size_t{3}; };
    std::uint32_t offset = 0;
    for (auto& directory : directories) {
        directory.Offset = offset;
        offset += static_cast<std::uint32_t>(24 + padded(directory.Name.size() * 2));
    }
    // File entries in directory order; each directory's files are consecutive.
    std::vector<std::vector<std::uint32_t>> fileOffsets(directories.size());
    std::uint32_t fileOffset = 0;
    for (std::size_t index = 0; index < directories.size(); ++index)
        for (const auto& [name, data] : directories[index].Files) {
            fileOffsets[index].push_back(fileOffset);
            fileOffset += static_cast<std::uint32_t>(32 + padded(name.size() * 2));
        }
    constexpr std::uint32_t kInvalid = 0xFFFFFFFFU;
    Bytes directoryTable, fileTable, dataTable;
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto& directory = directories[index];
        std::uint32_t sibling = kInvalid;
        if (directory.Parent >= 0) {
            const auto& siblings = directories[directory.Parent].Children;
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
            const auto& [fileName, data] = directory.Files[file];
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

struct ZipFixtureEntry {
    std::string Name;
    Bytes Data;
    bool Deflate = false;
};
// A ZIP written by hand (stored or raw-deflated entries).
Bytes MakeZip(const std::vector<ZipFixtureEntry>& entries) {
    Bytes out, central;
    for (const auto& entry : entries) {
        Bytes stored = entry.Data;
        if (entry.Deflate) {
            z_stream stream{};
            deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
            stored.resize(deflateBound(&stream, static_cast<uLong>(entry.Data.size())));
            stream.next_in = const_cast<Bytef*>(entry.Data.data());
            stream.avail_in = static_cast<uInt>(entry.Data.size());
            stream.next_out = stored.data();
            stream.avail_out = static_cast<uInt>(stored.size());
            deflate(&stream, Z_FINISH);
            stored.resize(stream.total_out);
            deflateEnd(&stream);
        }
        const auto crc = static_cast<std::uint32_t>(crc32(0L, entry.Data.data(), static_cast<uInt>(entry.Data.size())));
        const auto offset = static_cast<std::uint32_t>(out.size());
        for (const bool isCentral : {false, true}) {
            Bytes& target = isCentral ? central : out;
            Put32(target, isCentral ? 0x02014b50U : 0x04034b50U);
            if (isCentral) Put16(target, 20);
            Put16(target, 20);
            Put16(target, 0);
            Put16(target, entry.Deflate ? 8 : 0);
            Put32(target, 0);
            Put32(target, crc);
            Put32(target, static_cast<std::uint32_t>(stored.size()));
            Put32(target, static_cast<std::uint32_t>(entry.Data.size()));
            Put16(target, static_cast<std::uint16_t>(entry.Name.size()));
            Put16(target, 0);
            if (isCentral) {
                for (int i = 0; i < 3; ++i) Put16(target, 0);
                Put32(target, 0);
                Put32(target, offset);
            }
            target.insert(target.end(), entry.Name.begin(), entry.Name.end());
            if (!isCentral) target.insert(target.end(), stored.begin(), stored.end());
        }
    }
    const auto centralOffset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    Put32(out, 0x06054b50U);
    Put32(out, 0);
    Put16(out, static_cast<std::uint16_t>(entries.size()));
    Put16(out, static_cast<std::uint16_t>(entries.size()));
    Put32(out, static_cast<std::uint32_t>(central.size()));
    Put32(out, centralOffset);
    Put16(out, 0);
    return out;
}

// test_qbf_font.font_bytes: two characters (32, 42), fallback 42.
Bytes FontBytes(std::uint8_t size, std::vector<std::pair<std::uint16_t, std::uint16_t>> records = {{32, 0}, {42, 1}},
                std::uint8_t pixelBase = 0x1f) {
    std::uint16_t glyphs = 0;
    for (const auto& record : records) glyphs = std::max<std::uint16_t>(glyphs, record.second + 1);
    Bytes out = Text("QBF1");
    Put16(out, static_cast<std::uint16_t>(records.size()));
    Put16(out, glyphs);
    Put16(out, 42);
    Put16(out, 0);
    for (const std::uint8_t value : {std::uint8_t{4}, size, size, std::uint8_t{2}}) out.push_back(value);
    for (const auto& [code, glyph] : records) {
        Put16(out, code);
        Put16(out, glyph);
        for (const std::uint8_t metric : {std::uint8_t(code == 32 ? 6 : 5), std::uint8_t(4), std::uint8_t(0), std::uint8_t(0)})
            out.push_back(metric);
    }
    for (std::uint16_t index = 0; index < glyphs; ++index)
        out.insert(out.end(), static_cast<std::size_t>(size) * size / 2, static_cast<std::uint8_t>(pixelBase + index));
    return out;
}

// ---- Tests ----

void TestFnv() {
    CHECK(ts::Fnv1a64({}) == 0xCBF29CE484222325ULL);
    const Bytes a = Text("a");
    CHECK(ts::Fnv1a64(a) == 0xAF63DC4C8601EC8CULL);
}

// test_topscreen_30_pack.test_etc1a4_differential_flip_block.
void TestEtc1() {
    std::uint64_t color = (16ULL << 59) | (1ULL << 56) | (8ULL << 51) | (7ULL << 48) | (4ULL << 43) | (1ULL << 37) |
                          (2ULL << 34) | (1ULL << 33) | (1ULL << 32);
    color |= 1ULL << 0;
    color |= 1ULL << (16 + 3);
    Bytes payload;
    for (int block = 0; block < 4; ++block) {
        Put64(payload, 0xFEDCBA9876543210ULL);
        Put64(payload, color);
    }
    const Bytes rgba = ts::DecodeEtc1a4(payload, 8, 8);
    const auto texel = [&](int x, int y) { return Bytes(rgba.begin() + (y * 8 + x) * 4, rgba.begin() + (y * 8 + x) * 4 + 4); };
    const auto expand = [](int value) { return (value << 3) | (value >> 2); };
    const int first[3] = {expand(16), expand(8), expand(4)};
    const int second[3] = {expand(17), expand(7), expand(4)};
    const auto make = [](const int base[3], int delta, int alpha) {
        return Bytes{static_cast<std::uint8_t>(base[0] + delta), static_cast<std::uint8_t>(base[1] + delta),
                     static_cast<std::uint8_t>(base[2] + delta), static_cast<std::uint8_t>(alpha)};
    };
    CHECK(texel(0, 0) == make(first, 17, 0));
    CHECK(texel(1, 0) == make(first, 5, 0x44));
    CHECK(texel(0, 3) == make(second, -9, 0x33));
    CHECK_THROWS(ts::DecodeEtc1a4(payload, 8, 4), "multiples of 8");
    CHECK_THROWS(ts::DecodeEtc1a4(Bytes(10), 8, 8), "size mismatch");
}

void TestTiling() {
    Bytes image(16 * 16 * 4);
    for (std::size_t i = 0; i < image.size(); ++i) image[i] = static_cast<std::uint8_t>(i);
    const Bytes encoded = ts::EncodeRgba8Tiled(image, 16, 16);
    // Pixel (1, 0) is texel 1 and (0, 1) texel 2 of the first tile, ABGR.
    CHECK((Bytes(encoded.begin() + 4, encoded.begin() + 8) == Bytes{7, 6, 5, 4}));
    CHECK((Bytes(encoded.begin() + 8, encoded.begin() + 12) == Bytes{67, 66, 65, 64}));
    // Second tile starts at pixel (8, 0).
    CHECK((Bytes(encoded.begin() + 256, encoded.begin() + 260) == Bytes{35, 34, 33, 32}));

    Bytes rgba4(8 * 8 * 2);
    for (std::size_t i = 0; i < rgba4.size(); ++i) rgba4[i] = static_cast<std::uint8_t>(i * 37);
    CHECK(ts::EncodeRgba4Tiled(ts::DecodeRgba4Tiled(rgba4, 8, 8), 8, 8) == rgba4);
    const Bytes decoded = ts::DecodeRgba4Tiled(rgba4, 8, 8);
    // Texel 0 is 0x2500: R=2, G=5, B=0, A=0.
    CHECK((Bytes(decoded.begin(), decoded.begin() + 4) == Bytes{34, 85, 0, 0}));
    // (c * 15 + 127) / 255 rounding.
    const Bytes gray(8 * 8 * 4, 8);
    CHECK(ts::EncodeRgba4Tiled(gray, 8, 8)[0] == 0x00);
    const Bytes light(8 * 8 * 4, 9);
    CHECK(ts::EncodeRgba4Tiled(light, 8, 8)[0] == 0x11);
    CHECK_THROWS(ts::EncodeRgba8Tiled(image, 16, 8), "invalid");
}

void TestZip(const fs::path& directory) {
    Bytes big(100000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>((i * 7) ^ (i >> 5));
    const fs::path path = directory / "fixture.zip";
    WriteFile(path, MakeZip({{"root/stored.bin", Text("stored"), false},
                             {"root/deflated.bin", big, true},
                             {"root/twice.txt", Text("first"), false},
                             {"root/twice.txt", Text("second"), true}}));
    ts::ZipReader zip(path);
    CHECK(zip.Entries().size() == 4);
    CHECK(AsString(zip.Read(*zip.Find("root/stored.bin"), 1024)) == "stored");
    CHECK(zip.Read(*zip.Find("root/deflated.bin"), 1U << 20U) == big);
    CHECK(AsString(zip.Read(*zip.Find("root/twice.txt"), 1024)) == "second"); // last wins
    CHECK(zip.Find("root/missing") == nullptr);
    CHECK_THROWS(zip.Read(*zip.Find("root/deflated.bin"), 1000), "oversized");

    // A flipped payload byte fails the CRC.
    Bytes damaged = MakeZip({{"root/stored.bin", Text("stored"), false}});
    damaged[30 + 15] ^= 1;
    WriteFile(path, damaged);
    ts::ZipReader damagedZip(path);
    CHECK_THROWS(damagedZip.Read(damagedZip.Entries()[0], 1024), "CRC");

    WriteFile(path, Text("not a zip at all, just text"));
    CHECK_THROWS(ts::ZipReader{path}, "not a ZIP");
    CHECK_THROWS(ts::ZipReader{directory / "absent.zip"}, "cannot open");
}

void TestArchiveRoot(const fs::path& directory) {
    const fs::path path = directory / "roots.zip";
    WriteFile(path, MakeZip({{"SingleScreen_3.2.2/USA/a", Text("x"), false}}));
    {
        ts::ZipReader zip(path);
        const ts::Archive* root = ts::FindArchiveRoot(zip);
        CHECK(root != nullptr && std::string_view(root->Version) == "3.2.2");
        CHECK(root == &ts::DownloadArchive());
    }
    // 3.2.1 (no longer downloadable) is still recognized.
    WriteFile(path, MakeZip({{"SingleScreen_3.2.1/USA/a", Text("x"), false}}));
    {
        ts::ZipReader zip(path);
        const ts::Archive* root = ts::FindArchiveRoot(zip);
        CHECK(root != nullptr && std::string_view(root->Version) == "3.2.1" && root->Url == nullptr);
    }
    WriteFile(path, MakeZip({{"TopScreen2.1.1/EUR/a", Text("x"), false}}));
    {
        ts::ZipReader zip(path);
        CHECK(ts::FindArchiveRoot(zip) == nullptr);
    }
    WriteFile(path, MakeZip({{"SingleScreen_3.0/a", Text("x"), false}, {"SingleScreen_3.2.1/b", Text("y"), false}}));
    {
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::FindArchiveRoot(zip), "mixes");
    }
    CHECK(ts::KnownArchives().size() == 3);
    CHECK(ts::DownloadArchive().Bytes == 56'493'832);
    CHECK(std::string_view(ts::DownloadArchive().Sha256) ==
          "d07f5ae7a77b83951836d80a000a9625a5e2da52ae0fa262d9b44409fda31aa0");
    CHECK(std::string_view(ts::DownloadArchive().Url) == "https://gamebanana.com/dl/1879915");
    CHECK(std::string_view(ts::DownloadArchive().FileName) == "singlescreen_322.zip");
    // Every release that carries the HD UI atlas carries the same one.
    CHECK(std::string_view(ts::KnownArchives()[1].UiAtlasHdSha256) == ts::DownloadArchive().UiAtlasHdSha256);
}

void TestRomFs(const fs::path& directory) {
    const Bytes ctxb = Text("payload of the cursor");
    const Bytes image = MakeRomFs({{"menu/01_US_ENGLISH/menu_cursor00.ctxb", ctxb},
                                   {"menu/01_US_ENGLISH/menu_top_parts00.ctxb", Text("top")},
                                   {"menu/02_EU_ENGLISH/menu_cursor00.ctxb", Text("eu")},
                                   {"message/us/ltn16.qbf", Text("QBF")},
                                   {"top.bin", Text("at the root")}});
    for (const bool ivfc : {false, true}) {
        Bytes file = image;
        if (ivfc) {
            Bytes prefix(0x1000);
            std::memcpy(prefix.data(), "IVFC", 4);
            file.insert(file.begin(), prefix.begin(), prefix.end());
        }
        const fs::path path = directory / "romfs.bin";
        WriteFile(path, file);
        ts::RomFsImage romfs(path);
        CHECK(romfs.Read("menu/01_US_ENGLISH/menu_cursor00.ctxb") == ctxb);
        CHECK(AsString(*romfs.Read("menu/01_US_ENGLISH/menu_top_parts00.ctxb")) == "top");
        CHECK(AsString(*romfs.Read("menu/02_EU_ENGLISH/menu_cursor00.ctxb")) == "eu");
        CHECK(AsString(*romfs.Read("top.bin")) == "at the root");
        CHECK(!romfs.Read("menu/03_EU_GERMAN/menu_cursor00.ctxb"));
        CHECK(!romfs.Read("menu/01_US_ENGLISH/absent.ctxb"));
        CHECK(ts::DetectRegion(romfs) == "USA");
    }
    // Both regions: ambiguous.
    WriteFile(directory / "romfs.bin",
              MakeRomFs({{"message/us/ltn16.qbf", Text("a")}, {"message/eu/ltn16.qbf", Text("b")}}));
    {
        ts::RomFsImage romfs(directory / "romfs.bin");
        CHECK_THROWS(ts::DetectRegion(romfs), "region");
    }
    WriteFile(directory / "romfs.bin", Bytes(0x28));
    CHECK_THROWS(ts::RomFsImage{directory / "romfs.bin"}, "Level-3");
    // A metadata offset outside its table.
    Bytes broken = image;
    Set32(broken, 16, 8); // directory table only 8 bytes long
    WriteFile(directory / "romfs.bin", broken);
    {
        ts::RomFsImage romfs(directory / "romfs.bin");
        CHECK_THROWS(romfs.Read("menu/01_US_ENGLISH/menu_cursor00.ctxb"), "outside its table");
    }
}

void TestPackRejections(const fs::path& directory) {
    const fs::path romfsPath = directory / "romfs.bin";
    WriteFile(romfsPath, MakeRomFs({{"message/us/ltn16.qbf", Text("QBF")}}));
    ts::RomFsImage romfs(romfsPath);
    const fs::path path = directory / "pack.zip";
    // Only an EUR atlas: a USA RomFS finds none.
    WriteFile(path, MakeZip({{"SingleScreen_3.2.2/EUR/Emulator/load/mods/0004000000033600/romfs/menu/custom_ui01.ctxb",
                              Text("ctxb"), true}}));
    {
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildTexturePack(zip, ts::DownloadArchive(), romfs), "has no TopScreen 3.0 USA UI atlas");
        // The atlas is pinned by SHA-256 (GameBanana changing the art).
        CHECK_THROWS(ts::BuildTexturePack(zip, ts::DownloadArchive(), romfs, "EUR"), "unexpected TopScreen 3.0");
        CHECK_THROWS(ts::BuildTexturePack(zip, ts::DownloadArchive(), romfs, "KOR"), "unsupported");
    }
    // The 3DS Hardware target is also accepted.
    WriteFile(path, MakeZip({{"SingleScreen_3.2.2/USA/3DS Hardware/luma/titles/0004000000033500/romfs/menu/custom_ui01.ctxb",
                              Text("ctxb"), false}}));
    {
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildTexturePack(zip, ts::DownloadArchive(), romfs), "unexpected TopScreen 3.0");
    }
}

void TestFontPack(const fs::path& directory) {
    const fs::path romfsPath = directory / "romfs.bin";
    const Bytes native = FontBytes(16);
    WriteFile(romfsPath, MakeRomFs({{"message/us/ltn16.qbf", native}}));
    ts::RomFsImage romfs(romfsPath);
    const std::string hdPath = "SingleScreen_3.2.2/USA/Emulator/load/mods/0004000000033500/romfs/message/us/ltn16.qbf";
    const fs::path path = directory / "fonts.zip";
    const Bytes hd = FontBytes(64);
    WriteFile(path, MakeZip({{hdPath, hd, true},
                             {"SingleScreen_3.2.2/USA/Emulator/load/mods/0004000000033500/exefs/code.ips",
                              Text("DO NOT IMPORT"), false}}));
    {
        ts::ZipReader zip(path);
        const Bytes pack = ts::BuildFontPack(zip, ts::DownloadArchive(), romfs, "ab");
        const std::string text = AsString(pack);
        CHECK(text.find("DO NOT IMPORT") == std::string::npos);
        const fs::path output = directory / "font_coverage.zip";
        WriteFile(output, pack);
        ts::ZipReader written(output);
        CHECK(written.Entries().size() == 3);
        const std::string nativeName = "native/" + ri::Hex(ri::Sha256Of(native)) + ".qbf";
        const std::string hdName = "coverage/" + ri::Hex(ri::Sha256Of(hd)) + ".qbf";
        CHECK(written.Entries()[0].Name == hdName); // sorted: coverage/ < native/
        CHECK(written.Entries()[1].Name == nativeName);
        CHECK(written.Entries()[2].Name == "manifest.json");
        CHECK(written.Read(written.Entries()[0], 1U << 20U) == hd);
        CHECK(written.Read(written.Entries()[1], 1U << 20U) == native);
        const std::string manifest = AsString(written.Read(written.Entries()[2], 1U << 20U));
        CHECK(manifest.rfind("{\n  \"attribution\": \"TopScreen / Single Screen Experience by M-1 / rlgcarrot\",\n"
                             "  \"fonts\": [\n    {\n      \"characters\": [\n        {\n          \"code\": 32,\n"
                             "          \"coverage_glyph\": 0,\n          \"native_glyph\": 0,\n"
                             "          \"native_metrics_hex\": \"06040000\"\n        },\n", 0) == 0);
        CHECK(manifest.find("\"coverage_cell\": [\n        64,\n        64\n      ],") != std::string::npos);
        CHECK(manifest.find("\"density\": 4,") != std::string::npos);
        CHECK(manifest.find("\"romfs_path\": \"message/us/ltn16.qbf\"") != std::string::npos);
        CHECK(manifest.find("\"runtime_enabled\": true,") != std::string::npos);
        CHECK(manifest.find("\"source_archive_sha256\": \"ab\",") != std::string::npos);
        CHECK(manifest.back() == '}');
    }
    // A native glyph shared by two codes whose HD glyphs differ.
    WriteFile(romfsPath, MakeRomFs({{"message/us/ltn16.qbf", FontBytes(16, {{32, 0}, {42, 0}})}}));
    {
        ts::RomFsImage aliased(romfsPath);
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildFontPack(zip, ts::DownloadArchive(), aliased, "ab"), "conflicting HD coverage");
    }
    // Missing character, non-integral density, no font in the ROM.
    WriteFile(romfsPath, MakeRomFs({{"message/us/ltn16.qbf", FontBytes(16, {{32, 0}, {42, 1}, {65, 1}})}}));
    {
        ts::RomFsImage romfsMissing(romfsPath);
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildFontPack(zip, ts::DownloadArchive(), romfsMissing, "ab"), "missing native character U+0041");
    }
    WriteFile(path, MakeZip({{hdPath, FontBytes(24), false}}));
    {
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildFontPack(zip, ts::DownloadArchive(), romfs, "ab"), "uniform integral");
    }
    WriteFile(romfsPath, MakeRomFs({{"message/fr/other.qbf", native}}));
    {
        ts::RomFsImage noFont(romfsPath);
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildFontPack(zip, ts::DownloadArchive(), noFont, "ab"), "no supported Latin QBF");
    }
    // Truncated QBF.
    WriteFile(romfsPath, MakeRomFs({{"message/us/ltn16.qbf", Bytes(native.begin(), native.end() - 1)}}));
    {
        ts::RomFsImage truncated(romfsPath);
        ts::ZipReader zip(path);
        CHECK_THROWS(ts::BuildFontPack(zip, ts::DownloadArchive(), truncated, "ab"), "size mismatch");
    }
}

void TestOffer() {
    using ts::OfferSetting;
    CHECK(ts::ParseOfferSetting("") == OfferSetting::Ask);
    CHECK(ts::ParseOfferSetting("never\n") == OfferSetting::Never);
    CHECK(ts::ParseOfferSetting("  never ") == OfferSetting::Never);
    CHECK(ts::ParseOfferSetting("ask\n") == OfferSetting::Ask);
    CHECK(ts::ParseOfferSetting("garbage") == OfferSetting::Ask);
    CHECK(ts::ParseOfferSetting(ts::OfferSettingText(OfferSetting::Never)) == OfferSetting::Never);
    // No pack: offer the download.
    CHECK(ts::ShouldOffer(false, false, true, OfferSetting::Ask));
    CHECK(ts::ShouldOffer(false, true, true, OfferSetting::Ask));
    // A pack from before the HD atlas: offer it again.
    CHECK(ts::ShouldOffer(true, false, true, OfferSetting::Ask));
    CHECK(!ts::ShouldOffer(true, true, true, OfferSetting::Ask));
    CHECK(!ts::ShouldOffer(false, false, false, OfferSetting::Ask));
    CHECK(!ts::ShouldOffer(true, false, false, OfferSetting::Ask));
    CHECK(!ts::ShouldOffer(false, false, true, OfferSetting::Never));
    CHECK(!ts::ShouldOffer(true, false, true, OfferSetting::Never));
    // A local archive: always without a pack; with one, only to add the HD
    // atlas, and only from a release that carries it (3.2.x, not 3.0).
    const ts::Archive& v30 = ts::KnownArchives()[0];
    const ts::Archive& v321 = ts::KnownArchives()[1];
    CHECK(ts::ShouldBuildFromLocalArchive(true, false, ts::DownloadArchive()));
    CHECK(std::string_view(v30.Version) == "3.0" && v30.UiAtlasHdSha256 == nullptr);
    CHECK(ts::ShouldBuildFromLocalArchive(false, false, v30));
    CHECK(ts::ShouldBuildFromLocalArchive(false, false, v321));
    CHECK(ts::ShouldBuildFromLocalArchive(true, false, v321));
    CHECK(!ts::ShouldBuildFromLocalArchive(true, false, v30));
    CHECK(!ts::ShouldBuildFromLocalArchive(true, true, v321));
}

// 8-bit RGBA PNG rows with the given filter types (raw zlib stream), for the
// decoder's filter paths the encoder does not write.
Bytes MakePng(std::uint32_t width, std::uint32_t height, const Bytes& rgba, const std::vector<std::uint8_t>& filters,
              std::uint8_t colorType = 6) {
    const std::size_t stride = static_cast<std::size_t>(width) * 4;
    Bytes raw;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t filter = filters[y % filters.size()];
        raw.push_back(filter);
        for (std::size_t x = 0; x < stride; ++x) {
            const int value = rgba[y * stride + x];
            const int left = x >= 4 ? rgba[y * stride + x - 4] : 0;
            const int up = y ? rgba[(y - 1) * stride + x] : 0;
            const int predicted = filter == 1 ? left : filter == 2 ? up : filter == 3 ? (left + up) / 2 : 0;
            raw.push_back(static_cast<std::uint8_t>(value - predicted));
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    Bytes compressed(size);
    compress2(compressed.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6);
    compressed.resize(size);
    const auto chunk = [](Bytes& out, const char* type, const Bytes& data) {
        const auto be32 = [&](std::uint32_t value) {
            for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
        };
        be32(static_cast<std::uint32_t>(data.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(static_cast<std::uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
    };
    Bytes png{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    for (const std::uint32_t value : {width, height})
        for (int shift = 24; shift >= 0; shift -= 8) header.push_back(static_cast<std::uint8_t>(value >> shift));
    for (const std::uint8_t value : {std::uint8_t{8}, colorType, std::uint8_t{0}, std::uint8_t{0}, std::uint8_t{0}})
        header.push_back(value);
    chunk(png, "IHDR", header);
    chunk(png, "tEXt", Text("Comment"));
    chunk(png, "IDAT", compressed);
    chunk(png, "IEND", {});
    return png;
}

void TestPng() {
    ts::Rgba8Image image;
    image.Width = 7;
    image.Height = 5;
    for (std::uint32_t i = 0; i < image.Width * image.Height * 4; ++i)
        image.Pixels.push_back(static_cast<std::uint8_t>(i * 37 + (i >> 3)));
    const Bytes encoded = ts::EncodePngRgba8(image);
    const ts::Rgba8Image decoded = ts::DecodePngRgba8(encoded, 1000);
    CHECK(decoded.Width == 7 && decoded.Height == 5 && decoded.Pixels == image.Pixels);
    CHECK(ts::EncodePngRgba8(image) == encoded); // deterministic
    // None, Sub, Up and Average rows, and an ancillary chunk to skip.
    const ts::Rgba8Image filtered = ts::DecodePngRgba8(MakePng(7, 5, image.Pixels, {0, 1, 2, 3}), 1000);
    CHECK(filtered.Pixels == image.Pixels);
    CHECK_THROWS(ts::DecodePngRgba8(MakePng(7, 5, image.Pixels, {0}, 2), 1000), "not 8-bit RGBA");
    CHECK_THROWS(ts::DecodePngRgba8(encoded, 34), "unexpected PNG size");
    Bytes damaged = encoded;
    damaged[40] ^= 0xFF;
    CHECK_THROWS(ts::DecodePngRgba8(damaged, 1000), "CRC");
    CHECK_THROWS(ts::DecodePngRgba8(Bytes(encoded.begin(), encoded.end() - 20), 1000), "truncated");
    CHECK_THROWS(ts::DecodePngRgba8(Text("GIF89a not a png"), 1000), "not a PNG");
}

void TestUiAtlasHd(const fs::path& directory) {
    // A 4x atlas (2048x1024): an opaque red block on transparent black,
    // with an edge that is not on the 3x grid.
    ts::Rgba8Image source;
    source.Width = 2048;
    source.Height = 1024;
    source.Pixels.assign(static_cast<std::size_t>(source.Width) * source.Height * 4, 0);
    for (std::uint32_t y = 100; y < 300; ++y)
        for (std::uint32_t x = 202; x < 602; ++x) {
            std::uint8_t* pixel = &source.Pixels[(static_cast<std::size_t>(y) * source.Width + x) * 4];
            pixel[0] = 255;
            pixel[3] = 255;
        }
    const Bytes built = ts::BuildUiAtlasHd(ts::EncodePngRgba8(source));
    const ts::Rgba8Image reduced = ts::DecodePngRgba8(built, 1U << 22U);
    CHECK(reduced.Width == 512 * ts::kUiAtlasHdScale && reduced.Height == 256 * ts::kUiAtlasHdScale);
    CHECK(reduced.Width == 1536 && reduced.Height == 768);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        return &reduced.Pixels[(static_cast<std::size_t>(y) * reduced.Width + x) * 4];
    };
    // Inside: opaque red; outside: fully transparent.
    CHECK(at(300, 150)[0] == 255 && at(300, 150)[3] == 255 && at(300, 150)[1] == 0);
    CHECK(at(10, 10)[3] == 0 && at(1000, 600)[3] == 0);
    // Left edge, x 202 in the source = 151.5 at 3/4: half covered. The
    // colour stays red (alpha-weighted), only the alpha drops.
    const std::uint8_t* edge = at(151, 150);
    CHECK(edge[3] > 0 && edge[3] < 255 && edge[0] == 255 && edge[1] == 0 && edge[2] == 0);
    // A 3x source is kept as it is; 2x, or another aspect, is refused.
    ts::Rgba8Image exact;
    exact.Width = 1536;
    exact.Height = 768;
    exact.Pixels.assign(static_cast<std::size_t>(exact.Width) * exact.Height * 4, 0x80);
    CHECK(ts::DecodePngRgba8(ts::BuildUiAtlasHd(ts::EncodePngRgba8(exact)), 1U << 22U).Pixels == exact.Pixels);
    ts::Rgba8Image small;
    small.Width = 1024;
    small.Height = 512;
    small.Pixels.assign(static_cast<std::size_t>(small.Width) * small.Height * 4, 0);
    CHECK_THROWS(ts::BuildUiAtlasHd(ts::EncodePngRgba8(small)), "not a multiple of 512x256");
    small.Width = 1536;
    small.Height = 1536;
    small.Pixels.assign(static_cast<std::size_t>(small.Width) * small.Height * 4, 0);
    CHECK_THROWS(ts::BuildUiAtlasHd(ts::EncodePngRgba8(small)), "not a multiple of 512x256");

    // Extraction: the release's pin (size and SHA-256) is checked; 3.0 has none.
    const ts::Archive& v321 = ts::DownloadArchive();
    const std::string eur = std::string(v321.Root) +
                            "/EUR/Emulator/load/textures/0004000000033600/UI/tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png";
    const auto zip = [&](const char* name, std::vector<ZipFixtureEntry> entries) {
        WriteFile(directory / name, MakeZip(entries));
        return directory / name;
    };
    {
        const ts::ZipReader archive(zip("hd_missing.zip", {{std::string(v321.Root) + "/EUR/readme.txt", Text("x")}}));
        CHECK_THROWS(ts::ExtractUiAtlasHd(archive, v321, "EUR"), "has no HD UI atlas");
        CHECK(ts::ExtractUiAtlasHd(archive, ts::KnownArchives()[0], "EUR").empty());
        CHECK_THROWS(ts::ExtractUiAtlasHd(archive, v321, "KOR"), "unsupported TopScreen region");
    }
    {
        const ts::ZipReader archive(zip("hd_small.zip", {{eur, built}}));
        CHECK_THROWS(ts::ExtractUiAtlasHd(archive, v321, "EUR"), "unexpected HD UI atlas size");
    }
    {
        // The pinned size with other content (a re-uploaded file).
        const ts::ZipReader archive(zip("hd_other.zip", {{eur, Bytes(v321.UiAtlasHdBytes, 0x5A), true}}));
        CHECK_THROWS(ts::ExtractUiAtlasHd(archive, v321, "EUR"), "unexpected HD UI atlas");
    }
}

void TestInstall(const fs::path& directory) {
    const fs::path config = directory / "install" / "config";
    ts::Outputs outputs;
    outputs.Source = &ts::DownloadArchive();
    outputs.Pack = Text("pack");
    outputs.Fonts = Text("fonts");
    outputs.UiAtlasHd = Text("atlas");
    ts::Install(outputs, config);
    CHECK(AsString(ReadFile(config / ts::kUiAtlasHdName)) == "atlas");
    CHECK(AsString(ReadFile(config / ts::kPackName)) == "pack");
    const std::string receipt = AsString(ReadFile(config / ts::kReceiptName));
    CHECK(receipt.find("\"ui_atlas_hd_sha256\"") != std::string::npos);
    CHECK(receipt.find(ts::DownloadArchive().UiAtlasHdSha256) != std::string::npos);
    CHECK(receipt.find("\"ui_atlas_hd_scale\": 3") != std::string::npos ||
          receipt.find("\"ui_atlas_hd_scale\":3") != std::string::npos);
    // A release without the HD atlas leaves an existing one alone.
    outputs.UiAtlasHd.clear();
    ts::Install(outputs, config);
    CHECK(AsString(ReadFile(config / ts::kUiAtlasHdName)) == "atlas");
    CHECK(AsString(ReadFile(config / ts::kReceiptName)).find("ui_atlas_hd") == std::string::npos);
}

void TestWriteAndIdentify(const fs::path& directory) {
    const fs::path path = directory / "config" / "atlas_overrides.o3tu";
    fs::create_directories(path.parent_path());
    WriteFile(path, Text("old pack, replaced"));
    ts::WriteFileVerified(path, Text("new"));
    CHECK(AsString(ReadFile(path)) == "new");
    CHECK(!fs::exists(directory / "config" / "atlas_overrides.o3tu.partial"));
    CHECK_THROWS(ts::WriteFileVerified(directory / "absent" / "x.o3tu", Text("x")), "cannot write");

    std::string error;
    WriteFile(directory / "singlescreen_322.zip", Text("too short"));
    CHECK(ts::IdentifyArchiveFile(directory / "singlescreen_322.zip", &error) == nullptr);
    CHECK(error.find("not a known Single Screen Experience release (3.2.2 is 56493832 bytes)") != std::string::npos);
    CHECK(ts::IdentifyArchiveFile(directory / "absent.zip", &error) == nullptr);
    // Right size, wrong content (GameBanana replacing the file).
    WriteFile(directory / "singlescreen_322.zip", Bytes(ts::DownloadArchive().Bytes));
    std::uint64_t lastDone = 0;
    CHECK(ts::IdentifyArchiveFile(directory / "singlescreen_322.zip", &error,
                                  [&](std::uint64_t done, std::uint64_t) { lastDone = done; }) == nullptr);
    CHECK(lastDone == ts::DownloadArchive().Bytes);
    CHECK(error.find("is not the official 3.2.2 archive") != std::string::npos);
    // The 3.2.1 size is still identified as 3.2.1.
    WriteFile(directory / "singlescreen_321.zip", Bytes(ts::KnownArchives()[1].Bytes));
    CHECK(ts::IdentifyArchiveFile(directory / "singlescreen_321.zip", &error) == nullptr);
    CHECK(error.find("is not the official 3.2.1 archive") != std::string::npos);
    fs::remove(directory / "singlescreen_321.zip");
    fs::remove(directory / "singlescreen_322.zip");
}

std::string Sha256Of(const Bytes& data) { return ri::Hex(ri::Sha256Of(data)); }

int RealBuild(const fs::path& archive, const fs::path& romfs, const fs::path& pack, const fs::path& fonts,
              const fs::path& atlas) {
    const auto started = std::chrono::steady_clock::now();
    std::string error;
    const ts::Archive* source = ts::IdentifyArchiveFile(archive, &error);
    if (!source) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    const auto verified = std::chrono::steady_clock::now();
    const ts::Outputs outputs = ts::Build(archive, *source, romfs);
    const auto built = std::chrono::steady_clock::now();
    WriteFile(pack, outputs.Pack);
    WriteFile(fonts, outputs.Fonts);
    const auto seconds = [](auto from, auto to) { return std::chrono::duration<double>(to - from).count(); };
    std::printf("archive %s (verified in %.2f s)\n", source->Version, seconds(started, verified));
    std::printf("built in %.2f s\n", seconds(verified, built));
    std::printf("%s %zu bytes sha256 %s\n", pack.string().c_str(), outputs.Pack.size(), Sha256Of(outputs.Pack).c_str());
    std::printf("%s %zu bytes sha256 %s\n", fonts.string().c_str(), outputs.Fonts.size(),
                Sha256Of(outputs.Fonts).c_str());
    if (outputs.UiAtlasHd.empty()) {
        std::printf("no HD UI atlas in %s\n", source->Version);
        return 0;
    }
    if (!atlas.empty()) WriteFile(atlas, outputs.UiAtlasHd);
    // The 3x atlas against the native custom_ui01 it replaces: the same
    // layout, so the texels each one covers (alpha >= 128, the 3x atlas
    // reduced to 1x) mostly agree; upside down they would not. The redraw
    // has sharper edges and lighter shadows, so they never agree fully.
    const ts::Rgba8Image hd = ts::DecodePngRgba8(outputs.UiAtlasHd, 1U << 22U);
    const ts::ZipReader zip(archive);
    const auto* ctxb = zip.Find(std::string(source->Root) +
                                "/EUR/Emulator/load/mods/0004000000033600/romfs/menu/custom_ui01.ctxb");
    if (!ctxb) throw ts::ImportError("no EUR custom_ui01.ctxb");
    const Bytes ctxbData = zip.Read(*ctxb, 1U << 20U);
    const Bytes native = ts::DecodeEtc1a4(std::span(ctxbData).subspan(0x48, 512 * 256), 512, 256);
    double mismatch = 0, flippedMismatch = 0;
    for (std::uint32_t y = 0; y < 256; ++y)
        for (std::uint32_t x = 0; x < 512; ++x) {
            const auto covered = [&](std::uint32_t row) {
                int sum = 0;
                for (std::uint32_t dy = 0; dy < 3; ++dy)
                    for (std::uint32_t dx = 0; dx < 3; ++dx)
                        sum += hd.Pixels[((static_cast<std::size_t>(row) * 3 + dy) * hd.Width + x * 3 + dx) * 4 + 3];
                return sum >= 9 * 128;
            };
            const bool nativeCovered = native[(static_cast<std::size_t>(y) * 512 + x) * 4 + 3] >= 128;
            mismatch += covered(y) != nativeCovered;
            flippedMismatch += covered(255 - y) != nativeCovered;
        }
    mismatch /= 512.0 * 256.0;
    flippedMismatch /= 512.0 * 256.0;
    std::printf("%s %ux%u %zu bytes sha256 %s, pixels sha256 %s; coverage differs from custom_ui01 on %.1f%% "
                "of texels (%.1f%% upside down)\n",
                atlas.empty() ? "HD UI atlas" : atlas.string().c_str(), hd.Width, hd.Height,
                outputs.UiAtlasHd.size(), Sha256Of(outputs.UiAtlasHd).c_str(), Sha256Of(hd.Pixels).c_str(),
                mismatch * 100.0, flippedMismatch * 100.0);
    return hd.Width == 1536 && hd.Height == 768 && mismatch < 0.25 && mismatch * 2.0 < flippedMismatch ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if ((argc == 6 || argc == 7) && std::string_view(argv[1]) == "--build") {
        try {
            return RealBuild(argv[2], argv[3], argv[4], argv[5], argc == 7 ? fs::path(argv[6]) : fs::path());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "build failed: %s\n", error.what());
            return 1;
        }
    }
    const fs::path directory = fs::temp_directory_path() / "oot3d_topscreen_import_tests";
    fs::remove_all(directory);
    fs::create_directories(directory);
    try {
        TestFnv();
        TestEtc1();
        TestTiling();
        TestZip(directory);
        TestArchiveRoot(directory);
        TestRomFs(directory);
        TestPackRejections(directory);
        TestFontPack(directory);
        TestOffer();
        TestPng();
        TestUiAtlasHd(directory);
        TestInstall(directory);
        TestWriteAndIdentify(directory);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: unexpected exception: %s\n", error.what());
        ++gFailures;
    }
    fs::remove_all(directory);
    if (gFailures) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("oot3d_topscreen_import_tests: ok\n");
    return 0;
}
