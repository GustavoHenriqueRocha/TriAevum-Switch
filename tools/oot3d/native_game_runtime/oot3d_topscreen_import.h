#pragma once

// Builds config/atlas_overrides.o3tu (and the HD font coverage and the HD UI
// atlas beside it) on the console from the official "OoT3D: Single Screen
// Experience" archive by
// M-1 (https://gamebanana.com/mods/695893, CC BY-NC-ND 4.0) and the player's
// own RomFS. The mod may be downloaded and installed but not redistributed,
// so the game never ships it: the player's console downloads it and builds
// the pack, as upstream's Forge does on the PC.
//
// C++ port of the 3.x path of
// tools/oot3d/decomp_support/scripts/build_topscreen_texture_override_pack.py
// (build_topscreen_30_pack: ETC1A4 decode of custom_ui01.ctxb, the 2.1.1
// menu atlas, hash-matched menu_top_parts00/menu_cursor00 replacements, O3TU
// v2) and of prepare_qbf_font_pack.py / qbf_font.py (font_coverage.zip). For
// the same archive and RomFS both outputs are byte-identical to the Python
// tools' (oot3d_topscreen_import_tests --build). Only the texture and font
// entries listed below and the HD UI atlas PNG (custom_ui01 for Azahar,
// reduced here to 3x) are read; the mod's executable patches, ExHeader,
// models and other PNGs are never extracted.
//
// Standard C++ plus zlib (inflate, deflate, crc32) and the pure
// azahar_texture_scale filter, so it builds on the host.

#include "oot3d_rom_import.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame::TopScreenImport {

class ImportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Bytes = std::vector<std::uint8_t>;

// A verified release of the mod. Root is the archive's top directory.
struct Archive {
    const char* Version;
    const char* Root;
    const char* FileName; // the name GameBanana gives the download
    std::uint64_t Bytes;
    const char* Sha256;   // lowercase hex
    const char* Url;      // nullptr: no longer downloadable
    // The HD UI atlas inside it (4096x2048 PNG of custom_ui01 for Azahar,
    // the same in every region folder): size and SHA-256, or 0/nullptr when
    // the release is not known to carry one.
    std::uint64_t UiAtlasHdBytes;
    const char* UiAtlasHdSha256;
};

// The release the console downloads (3.2.2, GameBanana file 1879915).
[[nodiscard]] const Archive& DownloadArchive();
// Every release the builder accepts (3.0, 3.2.1 and 3.2.2: the same custom_ui01).
[[nodiscard]] std::span<const Archive> KnownArchives();

inline constexpr const char* kModName = "OoT3D: Single Screen Experience";
inline constexpr const char* kModAuthor = "M-1";
inline constexpr const char* kModPage = "https://gamebanana.com/mods/695893";
inline constexpr const char* kModLicense = "CC BY-NC-ND 4.0";

// Files in config/.
inline constexpr const char* kPackName = "atlas_overrides.o3tu";
inline constexpr const char* kFontPackName = "font_coverage.zip";
inline constexpr const char* kReceiptName = "topscreen_import.json";
// The mod's HD UI atlas reduced to kUiAtlasHdScale x the native atlas: the
// HUD's art at one texel per pixel on a 720p screen (--topscreen-ui-atlas-hd).
inline constexpr const char* kUiAtlasHdName = "topscreen_ui_atlas_hd.png";
inline constexpr std::uint32_t kUiAtlasHdScale = 3;
// The player's answer to the start-up offer: "never" stops it; anything else
// (or no file) offers the download at the next start when the pack or the HD
// atlas is missing.
inline constexpr const char* kOfferSettingName = "topscreen_offer";

enum class OfferSetting { Ask, Never };
[[nodiscard]] OfferSetting ParseOfferSetting(std::string_view text);
[[nodiscard]] const char* OfferSettingText(OfferSetting setting);
// Whether the start-up screen offers the download: no pack yet, or a pack
// without the HD atlas (an install from before it was imported).
[[nodiscard]] bool ShouldOffer(bool packExists, bool hdAtlasExists, bool romfsExists, OfferSetting setting);
// Whether a verified archive the player copied to switch/oot3dre/ is built
// at start-up without asking: always without a pack; with a pack, only when
// the HD atlas is missing and that archive carries one.
[[nodiscard]] bool ShouldBuildFromLocalArchive(bool packExists, bool hdAtlasExists, const Archive& archive);

// ---- ZIP (stored or deflated entries, no ZIP64, no encryption) ----

class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    struct Entry {
        std::string Name;
        std::uint16_t Method = 0;
        std::uint16_t Flags = 0;
        std::uint32_t Crc = 0;
        std::uint64_t CompressedSize = 0;
        std::uint64_t Size = 0;
        std::uint64_t LocalHeader = 0;
    };
    [[nodiscard]] const std::vector<Entry>& Entries() const { return mEntries; }
    // The last entry with this name (as Python's zipfile), or nullptr.
    [[nodiscard]] const Entry* Find(std::string_view name) const;
    // Inflates and CRC-checks one entry; throws when it exceeds maxBytes.
    [[nodiscard]] Bytes Read(const Entry& entry, std::uint64_t maxBytes) const;

private:
    std::FILE* mFile = nullptr;
    std::vector<Entry> mEntries;
    std::map<std::string, std::size_t, std::less<>> mByName;
};

// The known 3.x root the archive is laid out under; nullptr when none.
// Throws when it mixes two.
[[nodiscard]] const Archive* FindArchiveRoot(const ZipReader& archive);

// ---- The player's RomFS (raw IVFC image or its Level-3 service view) ----

class RomFsImage {
public:
    explicit RomFsImage(const std::filesystem::path& path);
    ~RomFsImage();
    RomFsImage(const RomFsImage&) = delete;
    RomFsImage& operator=(const RomFsImage&) = delete;
    // nullopt when the file does not exist; throws on a damaged image.
    [[nodiscard]] std::optional<Bytes> Read(std::string_view relativePath) const;

private:
    void ReadAt(std::uint64_t offset, std::size_t count, std::uint8_t* out) const;
    struct DirectoryEntry {
        std::uint32_t Sibling, ChildDirectory, ChildFile;
        Bytes Name; // UTF-16LE
    };
    [[nodiscard]] DirectoryEntry ReadDirectory(std::uint32_t offset) const;

    std::FILE* mFile = nullptr;
    std::uint64_t mSize = 0;
    std::uint64_t mBase = 0;
    std::uint32_t mDirectoryMetadataOffset = 0, mDirectoryMetadataSize = 0;
    std::uint32_t mFileMetadataOffset = 0, mFileMetadataSize = 0;
    std::uint32_t mFileDataOffset = 0;
};

// "EUR" or "USA" from message/<eu|us>/ltn16.qbf.
[[nodiscard]] std::string DetectRegion(const RomFsImage& romfs);

// ---- Texture building blocks (exercised by the host tests) ----

[[nodiscard]] std::uint64_t Fnv1a64(std::span<const std::uint8_t> data);
// Tiled PICA ETC1A4 to linear top-down RGBA8.
[[nodiscard]] Bytes DecodeEtc1a4(std::span<const std::uint8_t> payload, std::uint32_t width,
                                 std::uint32_t height);
[[nodiscard]] Bytes EncodeRgba8Tiled(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                     std::uint32_t height);
[[nodiscard]] Bytes DecodeRgba4Tiled(std::span<const std::uint8_t> payload, std::uint32_t width,
                                     std::uint32_t height);
[[nodiscard]] Bytes EncodeRgba4Tiled(std::span<const std::uint8_t> rgba, std::uint32_t width,
                                     std::uint32_t height);

// ---- PNG (8-bit RGBA, non-interlaced; what the HD atlas is) ----

struct Rgba8Image {
    std::uint32_t Width = 0, Height = 0;
    Bytes Pixels; // top-down rows, Width x Height x 4
};
// Throws ImportError on any other PNG, a damaged one, or one larger than
// maxPixels.
[[nodiscard]] Rgba8Image DecodePngRgba8(std::span<const std::uint8_t> png, std::uint64_t maxPixels);
// A PNG of one IDAT, Paeth-filtered rows, zlib's default level.
[[nodiscard]] Bytes EncodePngRgba8(const Rgba8Image& image);

// ---- Outputs ----

// O3TU v2 from a 3.x archive (`root`) and the player's RomFS. `region` empty:
// detected from the RomFS.
[[nodiscard]] Bytes BuildTexturePack(const ZipReader& archive, const Archive& root,
                                     const RomFsImage& romfs, std::string region = {});

// font_coverage.zip (manifest oot3d_qbf_coverage_v1): the archive's HD Latin
// font paired with the RomFS's native one, by character.
[[nodiscard]] Bytes BuildFontPack(const ZipReader& archive, const Archive& root,
                                  const RomFsImage& romfs, std::string_view archiveSha256);

// The archive's HD UI atlas PNG for `region` ("EUR", "USA" or "JPN"; the
// release's pin is checked). Empty when the release carries none.
[[nodiscard]] Bytes ExtractUiAtlasHd(const ZipReader& archive, const Archive& root, std::string_view region);
// The HD atlas reduced to kUiAtlasHdScale x the native 512x256 atlas
// (1536x768) by azahar_texture_scale's area-weighted, alpha-aware filter,
// as a PNG. Throws when the source is not a whole multiple of 512x256 at
// kUiAtlasHdScale or more.
[[nodiscard]] Bytes BuildUiAtlasHd(std::span<const std::uint8_t> sourcePng);

// Size and SHA-256 of a file against a known release; nullptr and a reason
// when it is none of them. `progress(done, total)` may be empty.
[[nodiscard]] const Archive* IdentifyArchiveFile(
    const std::filesystem::path& path, std::string* error,
    const std::function<void(std::uint64_t, std::uint64_t)>& progress = {});

struct Outputs {
    const Archive* Source = nullptr;
    Bytes Pack;
    Bytes Fonts;
    Bytes UiAtlasHd; // empty when the release has no HD atlas
};
// The outputs from a verified archive (`source`, see IdentifyArchiveFile).
[[nodiscard]] Outputs Build(const std::filesystem::path& archive, const Archive& source,
                            const std::filesystem::path& romfs);

// Writes `bytes` as `path`.partial, reads it back, then replaces `path`
// (removed first: Horizon cannot rename over a file). Throws ImportError.
void WriteFileVerified(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

// Writes the font pack, the receipt, the HD atlas (when built) and the pack
// (last: it is what the game looks for) into `configDirectory`.
void Install(const Outputs& outputs, const std::filesystem::path& configDirectory);

} // namespace Oot3dNativeGame::TopScreenImport
