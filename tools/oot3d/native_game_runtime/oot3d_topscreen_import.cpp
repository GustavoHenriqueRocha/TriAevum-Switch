#include "oot3d_topscreen_import.h"

#include "oot3d/renderer/azahar_texture_scale.h"

#include <zlib.h>

#include <cstdlib>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <unordered_map>

namespace Oot3dNativeGame::TopScreenImport {
namespace {

namespace fs = std::filesystem;
namespace ri = RomImport;

// The HD images 3.2 and 3.2.2 ship for Azahar beside the HD UI atlas, named
// by Azahar's legacy hash of the game's own English menu_top_parts00 (the
// hearts, magic meter, B label, touch buttons and pause pages) and of
// menu_cursor00 (the cursor and tab hints). TopScreen 3.x leaves both CTXBs
// as the game has them, so on Azahar these images replace them. 3.2.1 is
// not pinned (its archive can no longer be checked).
constexpr HdTexture kHdTextures32[] = {
    {"tex1_512x256_BB00B25B152582B6_4_mip0.png", 512, 256, 8'970'114,
     "1237d0617c4a0f5b59c12b26895a489e92b89c38898726b0b2f7dac18f1398d7"},
    {"tex1_128x128_8064FDFB5065B51D_4_mip0.png", 128, 128, 478'849,
     "baf550ec007e1cac3226992d4d5ca8baaa0a56af1d0e8befd5673e4af0b3704e"},
};

constexpr Archive kArchives[] = {
    {"3.0", "SingleScreen_3.0", "singlescreen_30.zip", 28'883'512,
     "5668640fec8dc2b47b4516fd0b871c4d7d90ca1c2203acc2a22adb59aaab5993", nullptr, 0, nullptr},
    // The HD UI atlas is byte-identical in the EUR, USA and JPN folders. 3.2.1
    // (GameBanana file 1875895) was deleted when 3.2.2 replaced it; both carry
    // the same custom_ui01.ctxb, Latin font and HD UI atlas.
    {"3.2.1", "SingleScreen_3.2.1", "singlescreen_321.zip", 56'466'735,
     "737807fdefe59d21870c0b3270375698dcc796aa89b98ee4d75524d224c77aa0", nullptr, 7'007'958,
     "082582da95930114a149db8fa3c14e8be546d7df1b003c71c81bbbc2d4fde71e"},
    {"3.2.2", "SingleScreen_3.2.2", "singlescreen_322.zip", 56'493'832,
     "d07f5ae7a77b83951836d80a000a9625a5e2da52ae0fa262d9b44409fda31aa0",
     "https://gamebanana.com/dl/1879915", 7'007'958,
     "082582da95930114a149db8fa3c14e8be546d7df1b003c71c81bbbc2d4fde71e", kHdTextures32},
};
// The download is the newest release.
constexpr const Archive& kDownloadArchive = kArchives[std::size(kArchives) - 1];

constexpr const char* kUiAtlasSha256 = "20290e417c56973cd19daa1f61a6c3963f075d61776db3d996954ee3296d07d8";
// Azahar's name for the custom_ui01 replacement (its CityHash, ETC1A4).
constexpr const char* kUiAtlasHdFile = "tex1_512x256_1FAF9B5B2ADF6038_13_mip0.png";
constexpr std::uint32_t kUiAtlasWidth = 512, kUiAtlasHeight = 256;
constexpr const char* kUiAtlas = "menu/custom_ui01.ctxb";
constexpr const char* kTargetRomFs[] = {"Emulator/load/mods/", "3DS Hardware/luma/titles/"};
constexpr std::size_t kCtxbHeaderSize = 0x48;
constexpr std::uint64_t kNcchRomFsSuperblockSize = 0x1000;
constexpr std::uint32_t kInvalidRomFsOffset = 0xFFFFFFFFU;
constexpr std::uint64_t kMaxEntryBytes = 32U << 20U; // qbf_font.MAX_FONT_BYTES
constexpr std::uint32_t kMenuAtlasWidth = 512, kMenuAtlasHeight = 512;
constexpr std::uint8_t kFormatRgba8 = 0, kFormatRgba4 = 4, kFormatEtc1a4 = 13;
constexpr const char* kMenuAtlasSemantic = "oot3d/topscreen/2.1.1/menu_atlas";
constexpr const char* kFontAtlasSemantic = "oot3d/topscreen/2.1.1/font_atlas";
constexpr const char* kUiAtlasSemantic = "oot3d/topscreen/3.0/ui_atlas";

constexpr const char* kLanguages[] = {
    "00_JP_JAPANESE", "01_US_ENGLISH", "02_EU_ENGLISH", "03_EU_GERMAN", "04_EU_FRENCH",
    "05_US_FRENCH",   "06_EU_SPANISH", "07_US_SPANISH", "08_EU_ITALIAN",
};

struct TitleId {
    const char* Region;
    const char* Id;
};
constexpr TitleId kTitleIds[] = {
    {"EUR", "0004000000033600"}, {"USA", "0004000000033500"}, {"JPN", "0004000000033400"}};

struct Rect {
    std::uint32_t X, Y, W, H;
};

// The tables and their provenance are documented in the Python builder.
struct AtlasRegion {
    const char* Label;
    Rect Source;
    std::uint32_t DestinationX, DestinationY;
};
constexpr AtlasRegion kMenuAtlasRegions[] = {
    {"items_hint_select", {277, 222, 64, 16}, 434, 410},
    {"items_hint_question", {344, 222, 16, 16}, 492, 104},
    {"dpad_cycle_mark", {320, 3, 40, 40}, 458, 2},
};
constexpr int kRegionMargin = 1;

struct NativeRegion {
    const char* Label;
    Rect Source;
    Rect Destination;
    std::uint32_t OffsetX, OffsetY;
    bool QuarterTurn;
};
constexpr NativeRegion kPauseTopPageRegions[] = {
    {"dpad_cross", {385, 108, 30, 30}, {432, 218, 30, 30}, 0, 0, false},
    {"action_circle", {98, 3, 46, 48}, {386, 201, 48, 48}, 0, 0, false},
    {"item_i_label_zr", {47, 234, 16, 11}, {440, 190, 17, 11}, 1, 0, false},
    {"item_ii_label_zl", {245, 230, 16, 11}, {440, 201, 17, 11}, 1, 0, false},
    {"ocarina_arrow", {47, 124, 26, 26}, {486, 230, 26, 26}, 0, 0, false},
};
constexpr NativeRegion kCursorRegions[] = {
    {"tab_hint_r", {240, 117, 36, 28}, {72, 89, 28, 36}, 0, 0, true},
    {"tab_hint_l", {462, 108, 36, 28}, {100, 89, 28, 36}, 0, 0, true},
};
struct NativeTexture {
    const char* Name;
    std::span<const NativeRegion> Regions;
};
const NativeTexture kNativeTextures[] = {
    {"menu_top_parts00.ctxb", kPauseTopPageRegions},
    {"menu_cursor00.ctxb", kCursorRegions},
};

constexpr int kEtc1Modifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},  {13, 42},
                                      {18, 60}, {24, 80}, {33, 106}, {47, 183}};

struct PicaFormat {
    std::uint16_t Format, Type;
    std::uint8_t Native;
};
constexpr PicaFormat kPicaFormats[] = {
    {0x6752, 0x1401, 0}, {0x6754, 0x1401, 1},  {0x6752, 0x8034, 2},  {0x6754, 0x8363, 3},
    {0x6752, 0x8033, 4}, {0x6758, 0x1401, 5},  {0x6757, 0x1401, 7},  {0x6756, 0x1401, 8},
    {0x6758, 0x6760, 9}, {0x6757, 0x6761, 10}, {0x6756, 0x6761, 11}, {0x675A, 0x0000, 12},
    {0x675B, 0x0000, 13},
};

std::uint16_t Get16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}
std::uint32_t Get32(std::span<const std::uint8_t> data, std::size_t offset) {
    return Get16(data, offset) | (static_cast<std::uint32_t>(Get16(data, offset + 2)) << 16);
}
std::uint64_t Get64(std::span<const std::uint8_t> data, std::size_t offset) {
    return Get32(data, offset) | (static_cast<std::uint64_t>(Get32(data, offset + 4)) << 32);
}
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
void Append(Bytes& out, std::span<const std::uint8_t> data) { out.insert(out.end(), data.begin(), data.end()); }
void Append(Bytes& out, std::string_view text) { out.insert(out.end(), text.begin(), text.end()); }

std::uint32_t Morton8(std::uint32_t x, std::uint32_t y) {
    return (x & 1U) | ((y & 1U) << 1U) | ((x & 2U) << 1U) | ((y & 2U) << 2U) | ((x & 4U) << 2U) |
           ((y & 4U) << 3U);
}
std::size_t TiledPixel(std::uint32_t x, std::uint32_t y, std::uint32_t width) {
    return (static_cast<std::size_t>(y / 8) * (width / 8) + x / 8) * 64 + Morton8(x % 8, y % 8);
}

std::optional<std::uint8_t> NativeFormat(std::uint16_t format, std::uint16_t type) {
    for (const auto& entry : kPicaFormats)
        if (entry.Format == format && entry.Type == type) return entry.Native;
    return std::nullopt;
}

struct Ctxb {
    std::uint16_t Width, Height;
    std::optional<std::uint8_t> Format;
    std::span<const std::uint8_t> Payload;
};
std::optional<Ctxb> ParseCtxb(std::span<const std::uint8_t> data) {
    if (data.size() < kCtxbHeaderSize || std::memcmp(data.data(), "ctxb", 4) != 0) return std::nullopt;
    return Ctxb{Get16(data, 0x2C), Get16(data, 0x2E), NativeFormat(Get16(data, 0x30), Get16(data, 0x32)),
                data.subspan(kCtxbHeaderSize)};
}

// 16 RGB texels of one ETC1 block, indexed x * 4 + y.
std::array<std::array<std::uint8_t, 3>, 16> Etc1BlockRgb(std::uint64_t word) {
    const bool differential = (word >> 33U) & 1U;
    const bool flip = (word >> 32U) & 1U;
    const unsigned tables[2] = {static_cast<unsigned>((word >> 37U) & 7U),
                                static_cast<unsigned>((word >> 34U) & 7U)};
    int colors[2][3];
    if (differential) {
        const auto signed3 = [](int value) { return value & 4 ? value - 8 : value; };
        const auto expand5 = [](int value) { return (value << 3) | (value >> 2); };
        const int shifts[3][2] = {{59, 56}, {51, 48}, {43, 40}};
        for (int channel = 0; channel < 3; ++channel) {
            const int base = static_cast<int>((word >> shifts[channel][0]) & 31U);
            const int delta = signed3(static_cast<int>((word >> shifts[channel][1]) & 7U));
            colors[0][channel] = expand5(base);
            colors[1][channel] = expand5((base + delta) & 31);
        }
    } else {
        const int shifts[2][3] = {{60, 52, 44}, {56, 48, 40}};
        for (int half = 0; half < 2; ++half)
            for (int channel = 0; channel < 3; ++channel)
                colors[half][channel] = static_cast<int>((word >> shifts[half][channel]) & 15U) * 17;
    }
    std::array<std::array<std::uint8_t, 3>, 16> texels{};
    for (int x = 0; x < 4; ++x) {
        for (int y = 0; y < 4; ++y) {
            const int index = x * 4 + y;
            const int second = flip ? (y >= 2) : (x >= 2);
            const auto& modifiers = kEtc1Modifiers[tables[second]];
            int modifier = (word >> index) & 1U ? modifiers[1] : modifiers[0];
            if ((word >> (16 + index)) & 1U) modifier = -modifier;
            for (int channel = 0; channel < 3; ++channel)
                texels[index][channel] =
                    static_cast<std::uint8_t>(std::clamp(colors[second][channel] + modifier, 0, 255));
        }
    }
    return texels;
}

// Paints the 3.x sprites into an original RGBA4 menu texture at the 2.1.1
// rectangles; every rectangle must be transparent in the original.
Bytes ComposeNativeTexture(std::span<const std::uint8_t> original, std::uint32_t width, std::uint32_t height,
                           std::span<const std::uint8_t> ui, std::uint32_t uiWidth, std::uint32_t uiHeight,
                           std::span<const NativeRegion> regions, const std::string& label) {
    Bytes rgba = DecodeRgba4Tiled(original, width, height);
    // Rectangles may overlap: every check runs on the original before any paint.
    for (const auto& region : regions) {
        const auto& [sx, sy, sw, sh] = region.Source;
        const auto& [dx, dy, dw, dh] = region.Destination;
        if (dx + dw > width || dy + dh > height || sx + sw > uiWidth || sy + sh > uiHeight)
            throw ImportError(std::string("TopScreen 3.0 region out of bounds: ") + region.Label);
        const std::uint32_t placedW = region.QuarterTurn ? sh : sw, placedH = region.QuarterTurn ? sw : sh;
        if (region.OffsetX + placedW > dw || region.OffsetY + placedH > dh)
            throw ImportError(std::string("TopScreen 3.0 sprite exceeds its rect: ") + region.Label);
        for (std::uint32_t row = 0; row < dh; ++row)
            for (std::uint32_t column = 0; column < dw; ++column)
                if (rgba[(static_cast<std::size_t>(dy + row) * width + dx + column) * 4 + 3])
                    throw ImportError("original " + label + " is not transparent at the 2.1.1 " + region.Label +
                                      " rectangle");
    }
    for (const auto& region : regions) {
        const auto& [sx, sy, sw, sh] = region.Source;
        const std::uint32_t dx = region.Destination.X, dy = region.Destination.Y;
        const std::uint32_t placedW = region.QuarterTurn ? sh : sw, placedH = region.QuarterTurn ? sw : sh;
        for (std::uint32_t row = 0; row < placedH; ++row) {
            for (std::uint32_t column = 0; column < placedW; ++column) {
                // uv_quarter_turn shows stored texel (c, r) at display
                // (placedH - 1 - r, c) of the sw x sh sprite.
                const std::uint32_t sourceX = region.QuarterTurn ? sx + placedH - 1 - row : sx + column;
                const std::uint32_t sourceY = region.QuarterTurn ? sy + column : sy + row;
                const std::size_t source = (static_cast<std::size_t>(sourceY) * uiWidth + sourceX) * 4;
                const std::size_t target =
                    (static_cast<std::size_t>(dy + region.OffsetY + row) * width + dx + region.OffsetX + column) * 4;
                std::copy_n(ui.begin() + static_cast<std::ptrdiff_t>(source), 4, rgba.begin() + static_cast<std::ptrdiff_t>(target));
            }
        }
    }
    return EncodeRgba4Tiled(rgba, width, height);
}

// The 3.x Items-hint and D-pad cycle sprites at their 2.1.1 coordinates.
Bytes ComposeMenuAtlas(std::span<const std::uint8_t> ui, std::uint32_t width, std::uint32_t height) {
    Bytes composed(static_cast<std::size_t>(kMenuAtlasWidth) * kMenuAtlasHeight * 4);
    for (const auto& region : kMenuAtlasRegions) {
        const auto& [sx, sy, sw, sh] = region.Source;
        const std::uint32_t dx = region.DestinationX, dy = region.DestinationY;
        if (sx + sw > width || sy + sh > height || dx + sw > kMenuAtlasWidth || dy + sh > kMenuAtlasHeight)
            throw ImportError(std::string("TopScreen 3.0 region out of bounds: ") + region.Label);
        for (int row = -kRegionMargin; row < static_cast<int>(sh) + kRegionMargin; ++row) {
            for (int column = -kRegionMargin; column < static_cast<int>(sw) + kRegionMargin; ++column) {
                const std::int64_t sourceX = sx + column, sourceY = sy + row;
                const std::int64_t targetX = dx + column, targetY = dy + row;
                if (sourceX < 0 || sourceX >= width || sourceY < 0 || sourceY >= height || targetX < 0 ||
                    targetX >= kMenuAtlasWidth || targetY < 0 || targetY >= kMenuAtlasHeight)
                    continue;
                const auto source = static_cast<std::size_t>((sourceY * width + sourceX) * 4);
                const auto target = static_cast<std::size_t>((targetY * kMenuAtlasWidth + targetX) * 4);
                std::copy_n(ui.begin() + static_cast<std::ptrdiff_t>(source), 4,
                            composed.begin() + static_cast<std::ptrdiff_t>(target));
            }
        }
    }
    return composed;
}

struct PackEntry {
    std::uint64_t Original, Replacement;
    Bytes Payload;
};
struct ProfileTexture {
    const char* Semantic;
    std::uint16_t Width, Height;
    std::uint8_t Format;
    std::span<const std::uint8_t> Payload;
};

// O3TU v2 exactly as TopScreenTextureOverridePack parses it.
Bytes SerializePack(const std::vector<PackEntry>& entries, std::span<const ProfileTexture> textures) {
    std::size_t size = 16;
    for (const auto& entry : entries) size += 20 + entry.Payload.size();
    for (const auto& texture : textures) size += 24 + std::strlen(texture.Semantic) + texture.Payload.size();
    Bytes out;
    out.reserve(size);
    Append(out, std::string_view("O3TU"));
    Put32(out, 2);
    Put32(out, static_cast<std::uint32_t>(entries.size()));
    Put32(out, static_cast<std::uint32_t>(textures.size()));
    for (const auto& entry : entries) {
        Put64(out, entry.Original);
        Put64(out, entry.Replacement);
        Put32(out, static_cast<std::uint32_t>(entry.Payload.size()));
        Append(out, entry.Payload);
    }
    for (const auto& texture : textures) {
        const std::string_view semantic = texture.Semantic;
        Put32(out, static_cast<std::uint32_t>(semantic.size()));
        Put16(out, texture.Width);
        Put16(out, texture.Height);
        out.push_back(texture.Format);
        out.insert(out.end(), 3, 0);
        Put32(out, static_cast<std::uint32_t>(texture.Payload.size()));
        Put64(out, Fnv1a64(texture.Payload));
        Append(out, semantic);
        Append(out, texture.Payload);
    }
    return out;
}

// Hash-matched menu_top_parts00/menu_cursor00 replacements, in the order the
// Python builder inserts them (first language with each distinct payload).
std::vector<PackEntry> BuildNativeEntries(std::span<const std::uint8_t> ui, std::uint32_t uiWidth,
                                          std::uint32_t uiHeight, const RomFsImage& romfs) {
    std::vector<PackEntry> entries;
    std::set<std::uint64_t> seen;
    for (const auto& texture : kNativeTextures) {
        int found = 0;
        for (const char* language : kLanguages) {
            const auto original = romfs.Read(std::string("menu/") + language + "/" + texture.Name);
            if (!original) continue;
            ++found;
            const std::string label = std::string(language) + "/" + texture.Name;
            const auto ctxb = ParseCtxb(*original);
            if (!ctxb) throw ImportError("invalid original CTXB: " + label);
            if (ctxb->Format != kFormatRgba4 ||
                ctxb->Payload.size() != static_cast<std::size_t>(ctxb->Width) * ctxb->Height * 2)
                throw ImportError("original CTXB is not RGBA4: " + label);
            const std::uint64_t hash = Fnv1a64(ctxb->Payload);
            if (!seen.insert(hash).second) continue;
            Bytes replacement = ComposeNativeTexture(ctxb->Payload, ctxb->Width, ctxb->Height, ui, uiWidth,
                                                     uiHeight, texture.Regions, label);
            const std::uint64_t replacementHash = Fnv1a64(replacement);
            entries.push_back({hash, replacementHash, std::move(replacement)});
        }
        if (!found) throw ImportError(std::string("original RomFS has no menu/*/") + texture.Name);
    }
    return entries;
}

// ---- QBF (qbf_font.py) ----

struct QbfCharacter {
    std::uint16_t Code, Glyph;
    std::array<std::uint8_t, 4> Metrics;
};
struct Qbf {
    std::span<const std::uint8_t> Data;
    std::vector<QbfCharacter> Characters;
    std::uint16_t Glyphs = 0, Fallback = 0;
    std::uint8_t Width = 0, Height = 0;
    std::size_t GlyphOffset = 0;
    std::size_t GlyphBytes() const { return static_cast<std::size_t>(Width) * Height / 2; }
    std::span<const std::uint8_t> Glyph(std::uint16_t index) const {
        return Data.subspan(GlyphOffset + index * GlyphBytes(), GlyphBytes());
    }
};

Qbf ParseQbf(std::span<const std::uint8_t> data) {
    if (data.size() < 16 || data.size() > kMaxEntryBytes || std::memcmp(data.data(), "QBF1", 4) != 0)
        throw ImportError("invalid QBF header or size");
    Qbf font;
    font.Data = data;
    const std::uint16_t count = Get16(data, 4);
    font.Glyphs = Get16(data, 6);
    font.Fallback = Get16(data, 8);
    const std::uint8_t bpp = data[12], encoding = data[15];
    font.Width = data[13];
    font.Height = data[14];
    if (bpp != 4 || encoding != 2) throw ImportError("unsupported QBF coverage encoding (expected format 2/A4)");
    if (!font.Width || !font.Height || font.Width % 8 || font.Height % 8)
        throw ImportError("QBF cell dimensions must be nonzero multiples of eight");
    font.GlyphOffset = 16 + static_cast<std::size_t>(count) * 8;
    if (!count || !font.Glyphs || font.GlyphOffset + font.Glyphs * font.GlyphBytes() != data.size())
        throw ImportError("QBF directory/bitmap size mismatch");
    int previous = -1;
    bool fallbackFound = false;
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t position = 16 + index * 8;
        QbfCharacter character{Get16(data, position), Get16(data, position + 2), {}};
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(position + 4), 4, character.Metrics.begin());
        if (character.Code <= previous) throw ImportError("QBF character directory is not strictly ordered");
        if (character.Glyph >= font.Glyphs) throw ImportError("QBF character references an absent glyph");
        fallbackFound |= character.Code == font.Fallback;
        previous = character.Code;
        font.Characters.push_back(character);
    }
    if (!fallbackFound) throw ImportError("QBF fallback character is absent");
    return font;
}

std::string Sha256Hex(std::span<const std::uint8_t> data) { return ri::Hex(ri::Sha256Of(data)); }

std::string HexBytes(std::span<const std::uint8_t> data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text;
    for (const std::uint8_t byte : data) {
        text.push_back(kDigits[byte >> 4U]);
        text.push_back(kDigits[byte & 15U]);
    }
    return text;
}

// json.dumps(..., ensure_ascii=True) of a string.
std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (byte < 0x20 || byte >= 0x80) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

// A JSON value written as json.dumps(sort_keys=True, indent=2) writes it.
struct Json {
    enum class Kind { Number, Bool, String, Array, Object } Type = Kind::Number;
    std::int64_t Number = 0;
    bool Boolean = false;
    std::string Text;
    std::vector<Json> Items;
    std::map<std::string, Json> Members; // sorted keys

    static Json Of(std::int64_t value) { Json json; json.Number = value; return json; }
    static Json Of(bool value) { Json json; json.Type = Kind::Bool; json.Boolean = value; return json; }
    static Json Of(std::string value) { Json json; json.Type = Kind::String; json.Text = std::move(value); return json; }
    static Json Of(const char*) = delete; // would pick the bool overload
    static Json Array(std::vector<Json> items) { Json json; json.Type = Kind::Array; json.Items = std::move(items); return json; }
    static Json Object() { Json json; json.Type = Kind::Object; return json; }

    void Write(std::string& out, int depth) const {
        const std::string inner(static_cast<std::size_t>(depth + 1) * 2, ' ');
        const std::string outer(static_cast<std::size_t>(depth) * 2, ' ');
        switch (Type) {
        case Kind::Number: out += std::to_string(Number); break;
        case Kind::Bool: out += Boolean ? "true" : "false"; break;
        case Kind::String: out += JsonString(Text); break;
        case Kind::Array:
            if (Items.empty()) { out += "[]"; break; }
            out += "[\n";
            for (std::size_t index = 0; index < Items.size(); ++index) {
                out += inner;
                Items[index].Write(out, depth + 1);
                out += index + 1 < Items.size() ? ",\n" : "\n";
            }
            out += outer + "]";
            break;
        case Kind::Object: {
            if (Members.empty()) { out += "{}"; break; }
            out += "{\n";
            std::size_t index = 0;
            for (const auto& [key, value] : Members) {
                out += inner + JsonString(key) + ": ";
                value.Write(out, depth + 1);
                out += ++index < Members.size() ? ",\n" : "\n";
            }
            out += outer + "}";
            break;
        }
        }
    }
};

// pair_font_coverage: coverage by character code; native metrics stay.
Json PairFontCoverage(const Qbf& native, const Qbf& replacement) {
    if (replacement.Width % native.Width || replacement.Height % native.Height ||
        replacement.Width / native.Width != replacement.Height / native.Height)
        throw ImportError("QBF replacement needs a uniform integral coverage density");
    const int density = replacement.Width / native.Width;
    if (density < 1) throw ImportError("QBF replacement has lower coverage density");
    std::unordered_map<std::uint16_t, const QbfCharacter*> byCode;
    for (const auto& character : replacement.Characters) byCode[character.Code] = &character;
    std::vector<Json> mappings;
    std::unordered_map<std::uint16_t, std::span<const std::uint8_t>> pointerMap;
    std::int64_t metricDifferences = 0;
    for (const auto& entry : native.Characters) {
        const auto found = byCode.find(entry.Code);
        if (found == byCode.end()) {
            char message[80];
            std::snprintf(message, sizeof(message), "QBF replacement is missing native character U+%04X", entry.Code);
            throw ImportError(message);
        }
        const QbfCharacter& source = *found->second;
        Json mapping = Json::Object();
        mapping.Members["code"] = Json::Of(std::int64_t{entry.Code});
        mapping.Members["native_glyph"] = Json::Of(std::int64_t{entry.Glyph});
        mapping.Members["coverage_glyph"] = Json::Of(std::int64_t{source.Glyph});
        mapping.Members["native_metrics_hex"] = Json::Of(HexBytes(entry.Metrics));
        mappings.push_back(std::move(mapping));
        // A native glyph shared by several codes must not map to different
        // HD bitmaps (the 4-bit to 8-bit expansion is one-to-one).
        const auto coverage = replacement.Glyph(source.Glyph);
        const auto [previous, inserted] = pointerMap.emplace(entry.Glyph, coverage);
        if (!inserted && !std::equal(previous->second.begin(), previous->second.end(), coverage.begin(), coverage.end()))
            throw ImportError("QBF native glyph alias maps to conflicting HD coverage");
        metricDifferences += entry.Metrics != source.Metrics;
    }
    Json contract = Json::Object();
    contract.Members["native_sha256"] = Json::Of(Sha256Hex(native.Data));
    contract.Members["coverage_sha256"] = Json::Of(Sha256Hex(replacement.Data));
    contract.Members["logical_cell"] = Json::Array({Json::Of(std::int64_t{native.Width}), Json::Of(std::int64_t{native.Height})});
    contract.Members["coverage_cell"] =
        Json::Array({Json::Of(std::int64_t{replacement.Width}), Json::Of(std::int64_t{replacement.Height})});
    contract.Members["density"] = Json::Of(std::int64_t{density});
    contract.Members["fallback_code"] = Json::Of(std::int64_t{native.Fallback});
    contract.Members["characters"] = Json::Array(std::move(mappings));
    contract.Members["replacement_metric_differences"] = Json::Of(metricDifferences);
    return contract;
}

// Stored ZIP as Python's zipfile writes ZipInfo(name) entries (1980-01-01,
// made by Unix, mode 0600).
Bytes WriteStoredZip(const std::vector<std::pair<std::string, Bytes>>& files) {
    Bytes out, central;
    for (const auto& file : files) {
        const std::string& name = file.first;
        const Bytes& data = file.second;
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto crc = static_cast<std::uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size())));
        const auto size = static_cast<std::uint32_t>(data.size());
        const auto header = [&](Bytes& target, bool centralEntry) {
            Put32(target, centralEntry ? 0x02014b50U : 0x04034b50U);
            if (centralEntry) Put16(target, 0x0314);
            Put16(target, 20);   // version needed
            Put16(target, 0);    // flags
            Put16(target, 0);    // stored
            Put16(target, 0);    // time
            Put16(target, 0x21); // 1980-01-01
            Put32(target, crc);
            Put32(target, size);
            Put32(target, size);
            Put16(target, static_cast<std::uint16_t>(name.size()));
            Put16(target, 0); // extra
            if (centralEntry) {
                Put16(target, 0); // comment
                Put16(target, 0); // disk
                Put16(target, 0); // internal attributes
                Put32(target, 0600U << 16U);
                Put32(target, offset);
            }
            Append(target, std::string_view(name));
        };
        header(out, false);
        Append(out, data);
        header(central, true);
    }
    const auto centralOffset = static_cast<std::uint32_t>(out.size());
    Append(out, central);
    Put32(out, 0x06054b50U);
    Put16(out, 0);
    Put16(out, 0);
    Put16(out, static_cast<std::uint16_t>(files.size()));
    Put16(out, static_cast<std::uint16_t>(files.size()));
    Put32(out, static_cast<std::uint32_t>(central.size()));
    Put32(out, centralOffset);
    Put16(out, 0);
    return out;
}

const char* TitleIdFor(std::string_view region) {
    for (const auto& title : kTitleIds)
        if (region == title.Region) return title.Id;
    return nullptr;
}

std::unique_ptr<std::FILE, int (*)(std::FILE*)> Open(const fs::path& path, const char* mode) {
    return {std::fopen(path.string().c_str(), mode), &std::fclose};
}

void Seek(std::FILE* file, std::uint64_t offset) {
    if (fseeko(file, static_cast<off_t>(offset), SEEK_SET) != 0) throw ImportError("cannot seek");
}

} // namespace

const Archive& DownloadArchive() { return kDownloadArchive; }
std::span<const Archive> KnownArchives() { return kArchives; }

OfferSetting ParseOfferSetting(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return OfferSetting::Ask;
    text = text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    return text == "never" ? OfferSetting::Never : OfferSetting::Ask;
}

const char* OfferSettingText(OfferSetting setting) { return setting == OfferSetting::Never ? "never" : "ask"; }

bool ShouldOffer(bool packExists, bool hdArtExists, bool romfsExists, OfferSetting setting) {
    return (!packExists || !hdArtExists) && romfsExists && setting == OfferSetting::Ask;
}

bool ShouldBuildFromLocalArchive(bool packExists, bool hdArtExists, const Archive& archive) {
    return !packExists || (!hdArtExists && archive.UiAtlasHdSha256 != nullptr && !archive.HdTextures.empty());
}

bool HdArtInstalled(const fs::path& configDirectory) {
    std::error_code error;
    if (!fs::is_regular_file(configDirectory / kUiAtlasHdName, error)) return false;
    for (const HdTexture& texture : DownloadArchive().HdTextures)
        if (!fs::is_regular_file(configDirectory / kHdTexturesDirectoryName / texture.FileName, error)) return false;
    return true;
}

// ---- ZipReader ----

ZipReader::ZipReader(const fs::path& path) {
    auto file = Open(path, "rb");
    if (!file) throw ImportError("cannot open " + path.string());
    if (fseeko(file.get(), 0, SEEK_END) != 0) throw ImportError("cannot read " + path.string());
    const auto size = static_cast<std::uint64_t>(ftello(file.get()));
    // End of central directory: 22 bytes plus a comment of at most 64 KiB.
    const std::uint64_t tail = std::min<std::uint64_t>(size, 22 + 0xFFFF);
    Bytes end(static_cast<std::size_t>(tail));
    Seek(file.get(), size - tail);
    if (std::fread(end.data(), 1, end.size(), file.get()) != end.size()) throw ImportError("truncated ZIP");
    std::optional<std::size_t> found;
    for (std::size_t index = end.size() >= 22 ? end.size() - 22 + 1 : 0; index-- > 0;) {
        if (Get32(end, index) == 0x06054b50U) {
            found = index;
            break;
        }
    }
    if (!found) throw ImportError("not a ZIP archive");
    const std::uint16_t count = Get16(end, *found + 10);
    const std::uint32_t centralSize = Get32(end, *found + 12), centralOffset = Get32(end, *found + 16);
    if (count == 0xFFFF || centralOffset == 0xFFFFFFFFU) throw ImportError("ZIP64 archives are not supported");
    if (static_cast<std::uint64_t>(centralOffset) + centralSize > size) throw ImportError("damaged ZIP directory");
    Bytes central(centralSize);
    Seek(file.get(), centralOffset);
    if (std::fread(central.data(), 1, central.size(), file.get()) != central.size())
        throw ImportError("truncated ZIP directory");
    std::size_t position = 0;
    for (std::uint16_t index = 0; index < count; ++index) {
        if (position + 46 > central.size() || Get32(central, position) != 0x02014b50U)
            throw ImportError("damaged ZIP directory");
        Entry entry;
        entry.Flags = Get16(central, position + 8);
        entry.Method = Get16(central, position + 10);
        entry.Crc = Get32(central, position + 16);
        entry.CompressedSize = Get32(central, position + 20);
        entry.Size = Get32(central, position + 24);
        const std::size_t nameSize = Get16(central, position + 28);
        const std::size_t extraSize = Get16(central, position + 30);
        const std::size_t commentSize = Get16(central, position + 32);
        entry.LocalHeader = Get32(central, position + 42);
        if (position + 46 + nameSize + extraSize + commentSize > central.size())
            throw ImportError("damaged ZIP directory");
        entry.Name.assign(reinterpret_cast<const char*>(central.data() + position + 46), nameSize);
        position += 46 + nameSize + extraSize + commentSize;
        mByName[entry.Name] = mEntries.size();
        mEntries.push_back(std::move(entry));
    }
    mFile = file.release();
}

ZipReader::~ZipReader() {
    if (mFile) std::fclose(mFile);
}

const ZipReader::Entry* ZipReader::Find(std::string_view name) const {
    const auto found = mByName.find(name);
    return found == mByName.end() ? nullptr : &mEntries[found->second];
}

Bytes ZipReader::Read(const Entry& entry, std::uint64_t maxBytes) const {
    if (entry.Size > maxBytes || entry.CompressedSize > maxBytes + (maxBytes >> 3) + 1024)
        throw ImportError("oversized ZIP entry: " + entry.Name);
    if (entry.Flags & 1U) throw ImportError("encrypted ZIP entry: " + entry.Name);
    std::uint8_t local[30];
    Seek(mFile, entry.LocalHeader);
    if (std::fread(local, 1, sizeof(local), mFile) != sizeof(local) || Get32(local, 0) != 0x04034b50U)
        throw ImportError("damaged ZIP entry: " + entry.Name);
    Seek(mFile, entry.LocalHeader + 30 + Get16(local, 26) + Get16(local, 28));
    Bytes compressed(static_cast<std::size_t>(entry.CompressedSize));
    if (std::fread(compressed.data(), 1, compressed.size(), mFile) != compressed.size())
        throw ImportError("truncated ZIP entry: " + entry.Name);
    Bytes data;
    if (entry.Method == 0) {
        if (entry.CompressedSize != entry.Size) throw ImportError("damaged ZIP entry: " + entry.Name);
        data = std::move(compressed);
    } else if (entry.Method == 8) {
        data.resize(static_cast<std::size_t>(entry.Size));
        z_stream stream{};
        if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) throw ImportError("zlib could not start");
        stream.next_in = compressed.data();
        stream.avail_in = static_cast<uInt>(compressed.size());
        stream.next_out = data.data();
        stream.avail_out = static_cast<uInt>(data.size());
        const int result = inflate(&stream, Z_FINISH);
        const auto produced = stream.total_out;
        inflateEnd(&stream);
        if (result != Z_STREAM_END || produced != entry.Size)
            throw ImportError("damaged ZIP entry: " + entry.Name);
    } else {
        throw ImportError("unsupported ZIP compression in " + entry.Name);
    }
    if (static_cast<std::uint32_t>(crc32(0L, data.data(), static_cast<uInt>(data.size()))) != entry.Crc)
        throw ImportError("damaged ZIP entry (CRC): " + entry.Name);
    return data;
}

const Archive* FindArchiveRoot(const ZipReader& archive) {
    std::set<std::string_view> roots;
    for (const auto& entry : archive.Entries()) {
        const auto slash = entry.Name.find('/');
        if (slash != std::string::npos) roots.insert(std::string_view(entry.Name).substr(0, slash));
    }
    const Archive* found = nullptr;
    for (const auto& known : kArchives) {
        if (!roots.contains(known.Root)) continue;
        if (found) throw ImportError("archive mixes TopScreen 3.x roots");
        found = &known;
    }
    return found;
}

// ---- RomFsImage ----

RomFsImage::RomFsImage(const fs::path& path) {
    auto file = Open(path, "rb");
    if (!file) throw ImportError("cannot open " + path.string());
    if (fseeko(file.get(), 0, SEEK_END) != 0) throw ImportError("cannot read " + path.string());
    mSize = static_cast<std::uint64_t>(ftello(file.get()));
    mFile = file.release();
    std::uint8_t prefix[4]{};
    if (mSize >= 4) ReadAt(0, 4, prefix);
    mBase = std::memcmp(prefix, "IVFC", 4) == 0 ? kNcchRomFsSuperblockSize : 0;
    if (mBase + 0x28 > mSize) throw ImportError("truncated original RomFS image");
    std::uint8_t header[0x28];
    ReadAt(mBase, sizeof(header), header);
    if (Get32(header, 0) != 0x28) throw ImportError("original RomFS has no Level-3 header");
    mDirectoryMetadataOffset = Get32(header, 12);
    mDirectoryMetadataSize = Get32(header, 16);
    mFileMetadataOffset = Get32(header, 28);
    mFileMetadataSize = Get32(header, 32);
    mFileDataOffset = Get32(header, 36);
}

RomFsImage::~RomFsImage() {
    if (mFile) std::fclose(mFile);
}

void RomFsImage::ReadAt(std::uint64_t offset, std::size_t count, std::uint8_t* out) const {
    if (offset > mSize || count > mSize - offset) throw ImportError("truncated RomFS metadata");
    Seek(mFile, offset);
    if (std::fread(out, 1, count, mFile) != count) throw ImportError("cannot read the RomFS image");
}

RomFsImage::DirectoryEntry RomFsImage::ReadDirectory(std::uint32_t offset) const {
    if (offset >= mDirectoryMetadataSize || 24 > mDirectoryMetadataSize - offset)
        throw ImportError("RomFS metadata offset is outside its table");
    std::uint8_t fixed[24];
    ReadAt(mBase + mDirectoryMetadataOffset + offset, sizeof(fixed), fixed);
    const std::uint32_t nameSize = Get32(fixed, 20);
    if (nameSize % 2 != 0 || 24 + static_cast<std::uint64_t>(nameSize) > mDirectoryMetadataSize - offset)
        throw ImportError("invalid RomFS directory name");
    DirectoryEntry entry{Get32(fixed, 4), Get32(fixed, 8), Get32(fixed, 12), Bytes(nameSize)};
    ReadAt(mBase + mDirectoryMetadataOffset + offset + 24, nameSize, entry.Name.data());
    return entry;
}

std::optional<Bytes> RomFsImage::Read(std::string_view relativePath) const {
    std::vector<Bytes> parts;
    for (std::size_t start = 0; start <= relativePath.size();) {
        const auto slash = std::min(relativePath.find('/', start), relativePath.size());
        if (slash > start) {
            Bytes name;
            for (const char c : relativePath.substr(start, slash - start)) {
                name.push_back(static_cast<std::uint8_t>(c));
                name.push_back(0);
            }
            parts.push_back(std::move(name));
        }
        start = slash + 1;
    }
    if (parts.empty()) throw ImportError("empty RomFS path");
    std::uint32_t directory = 0;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) {
        std::uint32_t child = ReadDirectory(directory).ChildDirectory;
        std::set<std::uint32_t> visited;
        bool matched = false;
        while (child != kInvalidRomFsOffset) {
            if (!visited.insert(child).second) throw ImportError("cyclic RomFS directory list");
            const auto entry = ReadDirectory(child);
            if (entry.Name == parts[index]) {
                directory = child;
                matched = true;
                break;
            }
            child = entry.Sibling;
        }
        if (!matched) return std::nullopt;
    }
    std::uint32_t file = ReadDirectory(directory).ChildFile;
    std::set<std::uint32_t> visited;
    while (file != kInvalidRomFsOffset) {
        if (!visited.insert(file).second) throw ImportError("cyclic RomFS file list");
        if (file >= mFileMetadataSize || 32 > mFileMetadataSize - file)
            throw ImportError("RomFS metadata offset is outside its table");
        std::uint8_t fixed[32];
        ReadAt(mBase + mFileMetadataOffset + file, sizeof(fixed), fixed);
        const std::uint32_t sibling = Get32(fixed, 4);
        const std::uint64_t dataOffset = Get64(fixed, 8), dataSize = Get64(fixed, 16);
        const std::uint32_t nameSize = Get32(fixed, 28);
        if (nameSize % 2 != 0 || 32 + static_cast<std::uint64_t>(nameSize) > mFileMetadataSize - file)
            throw ImportError("invalid RomFS file name");
        Bytes name(nameSize);
        ReadAt(mBase + mFileMetadataOffset + file + 32, nameSize, name.data());
        if (name == parts.back()) {
            const std::uint64_t absolute = mBase + mFileDataOffset + dataOffset;
            if (absolute > mSize || dataSize > mSize - absolute || dataSize > (std::uint64_t{1} << 31))
                throw ImportError("RomFS file payload is outside the image");
            Bytes data(static_cast<std::size_t>(dataSize));
            ReadAt(absolute, data.size(), data.data());
            return data;
        }
        file = sibling;
    }
    return std::nullopt;
}

std::string DetectRegion(const RomFsImage& romfs) {
    std::vector<std::string> found;
    for (const auto& [region, path] : {std::pair{"USA", "message/us/ltn16.qbf"}, std::pair{"EUR", "message/eu/ltn16.qbf"}})
        if (romfs.Read(path)) found.emplace_back(region);
    if (found.size() != 1) throw ImportError("cannot identify the RomFS region");
    return found.front();
}

// ---- Texture codecs ----

std::uint64_t Fnv1a64(std::span<const std::uint8_t> data) {
    std::uint64_t value = 0xCBF29CE484222325ULL;
    for (const std::uint8_t byte : data) {
        value ^= byte;
        value *= 0x100000001B3ULL;
    }
    return value;
}

Bytes DecodeEtc1a4(std::span<const std::uint8_t> payload, std::uint32_t width, std::uint32_t height) {
    if (width % 8 || height % 8) throw ImportError("ETC1A4 dimensions must be multiples of 8");
    if (payload.size() != static_cast<std::size_t>(width) * height) throw ImportError("ETC1A4 payload size mismatch");
    Bytes rgba(static_cast<std::size_t>(width) * height * 4);
    std::size_t offset = 0;
    for (std::uint32_t tileY = 0; tileY < height; tileY += 8) {
        for (std::uint32_t tileX = 0; tileX < width; tileX += 8) {
            for (const auto& [blockX, blockY] :
                 {std::pair{0U, 0U}, std::pair{4U, 0U}, std::pair{0U, 4U}, std::pair{4U, 4U}}) {
                const std::uint64_t alpha = Get64(payload, offset), color = Get64(payload, offset + 8);
                offset += 16;
                const auto texels = Etc1BlockRgb(color);
                for (std::uint32_t x = 0; x < 4; ++x) {
                    for (std::uint32_t y = 0; y < 4; ++y) {
                        const std::uint32_t index = x * 4 + y;
                        const std::size_t target =
                            (static_cast<std::size_t>(tileY + blockY + y) * width + tileX + blockX + x) * 4;
                        std::copy(texels[index].begin(), texels[index].end(), rgba.begin() + static_cast<std::ptrdiff_t>(target));
                        rgba[target + 3] = static_cast<std::uint8_t>(((alpha >> (4 * index)) & 15U) * 17);
                    }
                }
            }
        }
    }
    return rgba;
}

Bytes EncodeRgba8Tiled(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    if (width % 8 || height % 8 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        throw ImportError("invalid RGBA8 image");
    Bytes out(rgba.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y) * width + x) * 4;
            const std::size_t target = TiledPixel(x, y, width) * 4;
            for (int channel = 0; channel < 4; ++channel) out[target + channel] = rgba[source + 3 - channel];
        }
    }
    return out;
}

Bytes DecodeRgba4Tiled(std::span<const std::uint8_t> payload, std::uint32_t width, std::uint32_t height) {
    if (width % 8 || height % 8 || payload.size() != static_cast<std::size_t>(width) * height * 2)
        throw ImportError("invalid RGBA4 image");
    Bytes rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint16_t value = Get16(payload, TiledPixel(x, y, width) * 2);
            const std::size_t target = (static_cast<std::size_t>(y) * width + x) * 4;
            for (int channel = 0; channel < 4; ++channel)
                rgba[target + channel] = static_cast<std::uint8_t>(((value >> (12 - 4 * channel)) & 15U) * 17);
        }
    }
    return rgba;
}

Bytes EncodeRgba4Tiled(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    if (width % 8 || height % 8 || rgba.size() != static_cast<std::size_t>(width) * height * 4)
        throw ImportError("invalid RGBA8 image");
    Bytes out(static_cast<std::size_t>(width) * height * 2);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t source = (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint16_t value = 0;
            for (int channel = 0; channel < 4; ++channel)
                value = static_cast<std::uint16_t>((value << 4) | ((rgba[source + channel] * 15 + 127) / 255));
            const std::size_t target = TiledPixel(x, y, width) * 2;
            out[target] = static_cast<std::uint8_t>(value);
            out[target + 1] = static_cast<std::uint8_t>(value >> 8);
        }
    }
    return out;
}

// ---- Outputs ----

Bytes BuildTexturePack(const ZipReader& archive, const Archive& root, const RomFsImage& romfs, std::string region) {
    if (region.empty()) region = DetectRegion(romfs);
    const char* titleId = TitleIdFor(region);
    if (!titleId) throw ImportError("unsupported TopScreen 3.0 region: " + region);
    const ZipReader::Entry* entry = nullptr;
    for (const char* target : kTargetRomFs) {
        entry = archive.Find(std::string(root.Root) + "/" + region + "/" + target + titleId + "/romfs/" + kUiAtlas);
        if (entry) break;
    }
    if (!entry) throw ImportError("archive has no TopScreen 3.0 " + region + " UI atlas");
    const Bytes data = archive.Read(*entry, kMaxEntryBytes);
    if (Sha256Hex(data) != kUiAtlasSha256) throw ImportError("unexpected TopScreen 3.0 custom_ui01.ctxb");
    const auto ctxb = ParseCtxb(data);
    if (!ctxb) throw ImportError("invalid TopScreen 3.0 custom_ui01.ctxb");
    if (ctxb->Format != kFormatEtc1a4) throw ImportError("TopScreen 3.0 UI atlas is not ETC1A4");
    const Bytes ui = DecodeEtc1a4(ctxb->Payload, ctxb->Width, ctxb->Height);
    const Bytes menu = EncodeRgba8Tiled(ComposeMenuAtlas(ui, ctxb->Width, ctxb->Height), kMenuAtlasWidth,
                                        kMenuAtlasHeight);
    const auto entries = BuildNativeEntries(ui, ctxb->Width, ctxb->Height, romfs);
    const ProfileTexture textures[] = {
        {kMenuAtlasSemantic, kMenuAtlasWidth, kMenuAtlasHeight, kFormatRgba8, menu},
        {kFontAtlasSemantic, ctxb->Width, ctxb->Height, kFormatEtc1a4, ctxb->Payload},
        {kUiAtlasSemantic, ctxb->Width, ctxb->Height, kFormatEtc1a4, ctxb->Payload},
    };
    return SerializePack(entries, textures);
}

Bytes BuildFontPack(const ZipReader& archive, const Archive& root, const RomFsImage& romfs,
                    std::string_view archiveSha256) {
    static constexpr std::pair<const char*, const char*> kSources[] = {
        {"message/eu/ltn16.qbf", "/EUR/Emulator/load/mods/0004000000033600/romfs/message/eu/ltn16.qbf"},
        {"message/us/ltn16.qbf", "/USA/Emulator/load/mods/0004000000033500/romfs/message/us/ltn16.qbf"},
    };
    std::map<std::string, Bytes> assets;
    std::vector<Json> fonts;
    for (const auto& [nativePath, archiveSuffix] : kSources) {
        auto nativeData = romfs.Read(nativePath);
        if (!nativeData) continue; // a region's ROM need not contain the other region
        const Qbf native = ParseQbf(*nativeData);
        const std::string archivePath = std::string(root.Root) + archiveSuffix;
        std::size_t matches = 0;
        const ZipReader::Entry* entry = nullptr;
        for (const auto& candidate : archive.Entries()) {
            if (candidate.Name != archivePath) continue;
            ++matches;
            entry = &candidate;
        }
        if (matches != 1 || entry->Size > kMaxEntryBytes)
            throw ImportError("absent, ambiguous or oversized QBF: " + archivePath);
        Bytes replacementData = archive.Read(*entry, kMaxEntryBytes);
        const Qbf replacement = ParseQbf(replacementData);
        Json font = PairFontCoverage(native, replacement);
        const std::string asset = "coverage/" + font.Members["coverage_sha256"].Text + ".qbf";
        const std::string originalAsset = "native/" + font.Members["native_sha256"].Text + ".qbf";
        font.Members["romfs_path"] = Json::Of(std::string(nativePath));
        font.Members["coverage_file"] = Json::Of(asset);
        font.Members["native_file"] = Json::Of(originalAsset);
        fonts.push_back(std::move(font));
        assets[asset] = std::move(replacementData);
        assets[originalAsset] = std::move(*nativeData);
    }
    if (fonts.empty()) throw ImportError("ROM contains no supported Latin QBF font");
    Json manifest = Json::Object();
    manifest.Members["format"] = Json::Of(std::string("oot3d_qbf_coverage_v1"));
    manifest.Members["runtime_enabled"] = Json::Of(true);
    manifest.Members["minimum_output_height"] = Json::Of(std::int64_t{480});
    manifest.Members["status"] = Json::Of(std::string("native_atlas_coverage_ready"));
    manifest.Members["fonts"] = Json::Array(std::move(fonts));
    manifest.Members["source_archive_sha256"] = Json::Of(std::string(archiveSha256));
    manifest.Members["attribution"] = Json::Of(std::string("TopScreen / Single Screen Experience by M-1 / rlgcarrot"));
    std::string text;
    manifest.Write(text, 0);
    std::vector<std::pair<std::string, Bytes>> files;
    for (auto& [name, data] : assets) files.emplace_back(name, std::move(data));
    files.emplace_back("manifest.json", Bytes(text.begin(), text.end()));
    return WriteStoredZip(files);
}

namespace {

constexpr std::uint8_t kPngSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

std::uint32_t GetBe32(const std::uint8_t* data) {
    return (std::uint32_t(data[0]) << 24U) | (std::uint32_t(data[1]) << 16U) | (std::uint32_t(data[2]) << 8U) |
           data[3];
}

void PutBe32(Bytes& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void PutPngChunk(Bytes& out, const char (&type)[5], std::span<const std::uint8_t> data) {
    PutBe32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutBe32(out, static_cast<std::uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(out.size() - start))));
}

std::uint8_t Paeth(int left, int up, int upLeft) {
    const int estimate = left + up - upLeft;
    const int toLeft = std::abs(estimate - left), toUp = std::abs(estimate - up), toUpLeft = std::abs(estimate - upLeft);
    if (toLeft <= toUp && toLeft <= toUpLeft) return static_cast<std::uint8_t>(left);
    return static_cast<std::uint8_t>(toUp <= toUpLeft ? up : upLeft);
}

} // namespace

Rgba8Image DecodePngRgba8(std::span<const std::uint8_t> png, std::uint64_t maxPixels) {
    if (png.size() < 8 || !std::equal(std::begin(kPngSignature), std::end(kPngSignature), png.begin()))
        throw ImportError("not a PNG file");
    Rgba8Image image;
    Bytes compressed;
    bool header = false, end = false;
    for (std::size_t position = 8; !end;) {
        if (png.size() - position < 12) throw ImportError("truncated PNG");
        const std::uint32_t length = GetBe32(&png[position]);
        if (length > png.size() - position - 12) throw ImportError("truncated PNG chunk");
        const std::uint8_t* type = &png[position + 4];
        const std::uint8_t* data = type + 4;
        if (crc32(0, type, length + 4) != GetBe32(data + length)) throw ImportError("PNG chunk CRC mismatch");
        const std::string_view name(reinterpret_cast<const char*>(type), 4);
        if (name == "IHDR") {
            if (header || length != 13) throw ImportError("invalid PNG header");
            image.Width = GetBe32(data);
            image.Height = GetBe32(data + 4);
            // 8-bit RGBA, deflate, adaptive filters, not interlaced.
            if (data[8] != 8 || data[9] != 6 || data[10] != 0 || data[11] != 0 || data[12] != 0)
                throw ImportError("the PNG is not 8-bit RGBA without interlacing");
            if (image.Width == 0 || image.Height == 0 ||
                static_cast<std::uint64_t>(image.Width) * image.Height > maxPixels)
                throw ImportError("unexpected PNG size " + std::to_string(image.Width) + "x" +
                                  std::to_string(image.Height));
            header = true;
        } else if (name == "IDAT") {
            if (!header) throw ImportError("PNG data before its header");
            compressed.insert(compressed.end(), data, data + length);
        } else if (name == "IEND") {
            end = true;
        } else if (name[0] >= 'A' && name[0] <= 'Z') {
            throw ImportError("unsupported critical PNG chunk " + std::string(name));
        }
        position += 12U + length;
    }
    if (!header || compressed.empty()) throw ImportError("PNG without image data");
    const std::size_t stride = static_cast<std::size_t>(image.Width) * 4U;
    Bytes raw(static_cast<std::size_t>(image.Height) * (stride + 1U));
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) throw ImportError("zlib inflateInit failed");
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = raw.data();
    stream.avail_out = static_cast<uInt>(raw.size());
    const int status = inflate(&stream, Z_FINISH);
    const bool complete = status == Z_STREAM_END && stream.total_out == raw.size();
    inflateEnd(&stream);
    if (!complete) throw ImportError("damaged PNG image data");
    compressed = Bytes();
    // Undo the row filters in place (the row above is already undone), then
    // drop the filter bytes.
    for (std::uint32_t y = 0; y < image.Height; ++y) {
        std::uint8_t* row = raw.data() + y * (stride + 1U);
        const std::uint8_t filter = row[0];
        std::uint8_t* line = row + 1;
        const std::uint8_t* up = y > 0 ? line - (stride + 1U) : nullptr;
        for (std::size_t x = 0; x < stride; ++x) {
            const int left = x >= 4 ? line[x - 4] : 0;
            const int above = up ? up[x] : 0;
            const int aboveLeft = up && x >= 4 ? up[x - 4] : 0;
            switch (filter) {
            case 0: break;
            case 1: line[x] = static_cast<std::uint8_t>(line[x] + left); break;
            case 2: line[x] = static_cast<std::uint8_t>(line[x] + above); break;
            case 3: line[x] = static_cast<std::uint8_t>(line[x] + ((left + above) >> 1)); break;
            case 4: line[x] = static_cast<std::uint8_t>(line[x] + Paeth(left, above, aboveLeft)); break;
            default: throw ImportError("invalid PNG row filter");
            }
        }
    }
    for (std::uint32_t y = 0; y < image.Height; ++y)
        std::memmove(raw.data() + y * stride, raw.data() + y * (stride + 1U) + 1U, stride);
    raw.resize(static_cast<std::size_t>(image.Height) * stride);
    image.Pixels = std::move(raw);
    return image;
}

Bytes EncodePngRgba8(const Rgba8Image& image) {
    const std::size_t stride = static_cast<std::size_t>(image.Width) * 4U;
    if (image.Width == 0 || image.Height == 0 || image.Pixels.size() != stride * image.Height)
        throw ImportError("invalid RGBA8 image");
    Bytes filtered(static_cast<std::size_t>(image.Height) * (stride + 1U));
    for (std::uint32_t y = 0; y < image.Height; ++y) {
        const std::uint8_t* line = image.Pixels.data() + y * stride;
        const std::uint8_t* up = y > 0 ? line - stride : nullptr;
        std::uint8_t* out = filtered.data() + y * (stride + 1U);
        out[0] = 4; // Paeth
        for (std::size_t x = 0; x < stride; ++x) {
            const int left = x >= 4 ? line[x - 4] : 0;
            const int above = up ? up[x] : 0;
            const int aboveLeft = up && x >= 4 ? up[x - 4] : 0;
            out[1 + x] = static_cast<std::uint8_t>(line[x] - Paeth(left, above, aboveLeft));
        }
    }
    uLongf compressedSize = compressBound(static_cast<uLong>(filtered.size()));
    Bytes compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, filtered.data(), static_cast<uLong>(filtered.size()),
                  Z_DEFAULT_COMPRESSION) != Z_OK)
        throw ImportError("zlib could not compress the PNG");
    compressed.resize(compressedSize);
    Bytes header;
    PutBe32(header, image.Width);
    PutBe32(header, image.Height);
    for (const std::uint8_t value : {8, 6, 0, 0, 0}) header.push_back(value);
    Bytes png(std::begin(kPngSignature), std::end(kPngSignature));
    PutPngChunk(png, "IHDR", header);
    PutPngChunk(png, "IDAT", compressed);
    PutPngChunk(png, "IEND", {});
    return png;
}

Bytes ExtractUiAtlasHd(const ZipReader& archive, const Archive& root, std::string_view region) {
    if (root.UiAtlasHdSha256 == nullptr) return {};
    const char* titleId = TitleIdFor(region);
    if (!titleId) throw ImportError("unsupported TopScreen region: " + std::string(region));
    const std::string name = std::string(root.Root) + "/" + std::string(region) + "/Emulator/load/textures/" +
                             titleId + "/UI/" + kUiAtlasHdFile;
    const ZipReader::Entry* entry = archive.Find(name);
    if (!entry) throw ImportError("archive has no HD UI atlas " + name);
    if (entry->Size != root.UiAtlasHdBytes)
        throw ImportError("unexpected HD UI atlas size " + std::to_string(entry->Size));
    Bytes data = archive.Read(*entry, kMaxEntryBytes);
    if (Sha256Hex(data) != root.UiAtlasHdSha256) throw ImportError("unexpected HD UI atlas " + name);
    return data;
}

namespace {

Bytes ReduceHdImage(std::span<const std::uint8_t> sourcePng, std::uint32_t width, std::uint32_t height,
                    std::string_view what) {
    // At most 8x the native size (the mod's own images are 8x).
    Rgba8Image source = DecodePngRgba8(sourcePng, static_cast<std::uint64_t>(width) * height * 64U);
    if (width == 0 || height == 0 || source.Width % width != 0 || source.Width / width < kUiAtlasHdScale ||
        static_cast<std::uint64_t>(source.Width) * height != static_cast<std::uint64_t>(source.Height) * width)
        throw ImportError(std::string(what) + " is " + std::to_string(source.Width) + "x" +
                          std::to_string(source.Height) + ", not a multiple of " + std::to_string(width) + "x" +
                          std::to_string(height) + " of at least " + std::to_string(kUiAtlasHdScale) + "x");
    Rgba8Image reduced;
    reduced.Width = width * kUiAtlasHdScale;
    reduced.Height = height * kUiAtlasHdScale;
    if (source.Width == reduced.Width) {
        reduced.Pixels = std::move(source.Pixels);
    } else {
        reduced.Pixels = Oot3d::Renderer::DownscaleRgba8(source.Pixels, {source.Width, source.Height},
                                                         {reduced.Width, reduced.Height});
        source.Pixels = Bytes();
        if (reduced.Pixels.empty()) throw ImportError("could not reduce " + std::string(what));
    }
    return EncodePngRgba8(reduced);
}

} // namespace

Bytes BuildHdImage(std::span<const std::uint8_t> sourcePng, std::uint32_t width, std::uint32_t height) {
    return ReduceHdImage(sourcePng, width, height, "the HD image");
}

Bytes BuildUiAtlasHd(std::span<const std::uint8_t> sourcePng) {
    return ReduceHdImage(sourcePng, kUiAtlasWidth, kUiAtlasHeight, "the HD UI atlas");
}

std::vector<Bytes> ExtractHdTextures(const ZipReader& archive, const Archive& root, std::string_view region) {
    std::vector<Bytes> images;
    if (root.HdTextures.empty()) return images;
    const char* titleId = TitleIdFor(region);
    if (!titleId) throw ImportError("unsupported TopScreen region: " + std::string(region));
    for (const HdTexture& texture : root.HdTextures) {
        const std::string name = std::string(root.Root) + "/" + std::string(region) + "/Emulator/load/textures/" +
                                 titleId + "/UI/" + texture.FileName;
        const ZipReader::Entry* entry = archive.Find(name);
        if (!entry) throw ImportError("archive has no HD image " + name);
        if (entry->Size != texture.Bytes)
            throw ImportError("unexpected HD image size " + std::to_string(entry->Size) + ": " + name);
        Bytes data = archive.Read(*entry, kMaxEntryBytes);
        if (Sha256Hex(data) != texture.Sha256) throw ImportError("unexpected HD image " + name);
        images.push_back(std::move(data));
    }
    return images;
}

const Archive* IdentifyArchiveFile(const fs::path& path, std::string* error,
                                   const std::function<void(std::uint64_t, std::uint64_t)>& progress) {
    auto file = Open(path, "rb");
    if (!file) {
        if (error) *error = "cannot open " + path.string();
        return nullptr;
    }
    std::error_code sizeError;
    const std::uint64_t size = fs::file_size(path, sizeError);
    const Archive* candidate = nullptr;
    for (const auto& known : kArchives)
        if (!sizeError && known.Bytes == size) candidate = &known;
    if (!candidate) {
        if (error)
            *error = path.filename().string() + " is " + std::to_string(size) +
                     " bytes, not a known Single Screen Experience release (" + DownloadArchive().Version +
                     " is " + std::to_string(DownloadArchive().Bytes) + " bytes)";
        return nullptr;
    }
    ri::Sha256 hash;
    Bytes buffer(1U << 20U);
    std::uint64_t done = 0;
    for (std::size_t count; (count = std::fread(buffer.data(), 1, buffer.size(), file.get())) > 0;) {
        hash.Update(buffer.data(), count);
        done += count;
        if (progress) progress(done, size);
    }
    const std::string digest = ri::Hex(hash.Finish());
    if (done != size || digest != candidate->Sha256) {
        if (error)
            *error = path.filename().string() + " is not the official " + candidate->Version +
                     " archive (SHA-256 " + digest + ", expected " + candidate->Sha256 + ")";
        return nullptr;
    }
    return candidate;
}

Outputs Build(const fs::path& archivePath, const Archive& source, const fs::path& romfsPath) {
    const ZipReader archive(archivePath);
    const Archive* root = FindArchiveRoot(archive);
    if (root != &source) throw ImportError("the archive is not laid out as Single Screen Experience " +
                                           std::string(source.Version));
    const RomFsImage romfs(romfsPath);
    const std::string region = DetectRegion(romfs);
    Outputs outputs;
    outputs.Source = &source;
    outputs.Pack = BuildTexturePack(archive, source, romfs, region);
    outputs.Fonts = BuildFontPack(archive, source, romfs, source.Sha256);
    if (Bytes hd = ExtractUiAtlasHd(archive, source, region); !hd.empty()) outputs.UiAtlasHd = BuildUiAtlasHd(hd);
    std::vector<Bytes> images = ExtractHdTextures(archive, source, region);
    for (std::size_t index = 0; index < images.size(); ++index) {
        const HdTexture& texture = source.HdTextures[index];
        outputs.HdTextures.push_back(BuildHdImage(images[index], texture.NativeWidth, texture.NativeHeight));
        images[index] = Bytes();
    }
    return outputs;
}

void WriteFileVerified(const fs::path& path, std::span<const std::uint8_t> bytes) {
    fs::path partial = path;
    partial += ".partial";
    std::error_code error;
    const auto fail = [&](const std::string& message) {
        fs::remove(partial, error);
        throw ImportError(message);
    };
    {
        auto file = Open(partial, "wb");
        if (!file) fail("cannot write " + partial.string());
        if (std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size() ||
            std::fclose(file.release()) != 0)
            fail("cannot write " + partial.string() + " (SD card full?)");
    }
    {
        auto file = Open(partial, "rb");
        Bytes check(bytes.size() + 1);
        if (!file || std::fread(check.data(), 1, check.size(), file.get()) != bytes.size() ||
            !std::equal(bytes.begin(), bytes.end(), check.begin()))
            fail("the SD card did not keep " + partial.string() + " intact");
    }
    fs::remove(path, error); // Horizon cannot rename over an existing file
    fs::rename(partial, path, error);
    if (error) fail("cannot move " + partial.string() + " to " + path.string() + ": " + error.message());
}

void Install(const Outputs& outputs, const fs::path& configDirectory) {
    std::error_code error;
    fs::create_directories(configDirectory, error);
    WriteFileVerified(configDirectory / kFontPackName, outputs.Fonts);
    Json receipt = Json::Object();
    receipt.Members["mod"] = Json::Of(std::string(kModName) + " " + outputs.Source->Version);
    receipt.Members["author"] = Json::Of(std::string(kModAuthor));
    receipt.Members["source_url"] = Json::Of(std::string(kModPage));
    receipt.Members["license"] = Json::Of(std::string(kModLicense));
    receipt.Members["archive_sha256"] = Json::Of(std::string(outputs.Source->Sha256));
    receipt.Members["pack_sha256"] = Json::Of(Sha256Hex(outputs.Pack));
    receipt.Members["font_pack_sha256"] = Json::Of(Sha256Hex(outputs.Fonts));
    if (!outputs.UiAtlasHd.empty()) {
        // The mod's HD UI atlas reduced to 3x the native one.
        receipt.Members["ui_atlas_hd_source_sha256"] = Json::Of(std::string(outputs.Source->UiAtlasHdSha256));
        receipt.Members["ui_atlas_hd_sha256"] = Json::Of(Sha256Hex(outputs.UiAtlasHd));
        receipt.Members["ui_atlas_hd_scale"] = Json::Of(std::int64_t{kUiAtlasHdScale});
    }
    if (!outputs.HdTextures.empty()) {
        // The mod's HD images of the game's textures, reduced to 3x.
        std::vector<Json> images;
        for (std::size_t index = 0; index < outputs.HdTextures.size(); ++index) {
            const HdTexture& texture = outputs.Source->HdTextures[index];
            Json image = Json::Object();
            image.Members["file"] = Json::Of(std::string(kHdTexturesDirectoryName) + "/" + texture.FileName);
            image.Members["source_sha256"] = Json::Of(std::string(texture.Sha256));
            image.Members["sha256"] = Json::Of(Sha256Hex(outputs.HdTextures[index]));
            images.push_back(std::move(image));
        }
        receipt.Members["hd_textures"] = Json::Array(std::move(images));
    }
    std::string text;
    receipt.Write(text, 0);
    text += '\n';
    WriteFileVerified(configDirectory / kReceiptName, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    if (!outputs.UiAtlasHd.empty()) WriteFileVerified(configDirectory / kUiAtlasHdName, outputs.UiAtlasHd);
    if (!outputs.HdTextures.empty()) {
        fs::create_directories(configDirectory / kHdTexturesDirectoryName, error);
        for (std::size_t index = 0; index < outputs.HdTextures.size(); ++index)
            WriteFileVerified(configDirectory / kHdTexturesDirectoryName / outputs.Source->HdTextures[index].FileName,
                              outputs.HdTextures[index]);
    }
    WriteFileVerified(configDirectory / kPackName, outputs.Pack);
}

} // namespace Oot3dNativeGame::TopScreenImport
