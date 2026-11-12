#pragma once

// The largest size a texture pack's world and model images have, for
// L3+R3 > Video > "HD texture size" (oot3d_video_settings_panel.cpp). The
// setting (config/texture_pack_scale) is a cap that only reduces
// (CappedReplacementSize), so a size above the pack's largest image changes
// nothing: the menu offers 1x up to it, and No limit only when an image is
// larger than 4x. HUD and menu art (the pack's UI and MAPS folders) is capped
// at kUiTextureMaxScale apart and left out.
//
// The pack worker measures it once the index is ready, only while it has
// nothing else to do (azahar_texture_pack.cpp): it lists the pack (names and
// sizes: its fingerprint), then reads the PNG headers of the indexed images
// whose Azahar name gives their native size. The result is remembered in
// config/texture_pack_sizes with the fingerprint, so a later start with the
// same pack only lists it. Header-only and free of the runtime, checked on the
// host (oot3d_texture_pack_scale_tests).

#include "oot3d/renderer/azahar_texture_scale.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3d::Renderer {

inline constexpr const char* kTexturePackSizesName = "texture_pack_sizes";
// The largest cap the Video tab offers by name; a pack with larger images
// also gets No limit.
inline constexpr uint32_t kTexturePackLargestNamedScale = 4U;

// Width and height from the first 24 bytes of a PNG (signature, then the IHDR
// chunk); nullopt for anything else.
[[nodiscard]] inline std::optional<TextureSize> ReadPngSize(std::span<const uint8_t> bytes) noexcept {
    constexpr std::array<uint8_t, 16> kStart{0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU,
                                             0x00U, 0x00U, 0x00U, 0x0DU, 'I',   'H',   'D',   'R'};
    if (bytes.size() < 24U || !std::equal(kStart.begin(), kStart.end(), bytes.begin())) return std::nullopt;
    const auto big32 = [&](size_t at) {
        return (static_cast<uint32_t>(bytes[at]) << 24U) | (static_cast<uint32_t>(bytes[at + 1U]) << 16U) |
               (static_cast<uint32_t>(bytes[at + 2U]) << 8U) | static_cast<uint32_t>(bytes[at + 3U]);
    };
    const TextureSize size{big32(16U), big32(20U)};
    if (size.Width == 0U || size.Height == 0U) return std::nullopt;
    return size;
}

// The same from a file: one unbuffered read of 24 bytes, no decode.
[[nodiscard]] inline std::optional<TextureSize> ReadPngFileSize(const std::filesystem::path& path) {
    std::FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) return std::nullopt;
    std::setvbuf(file, nullptr, _IONBF, 0);
    std::array<uint8_t, 24> header{};
    const size_t read = std::fread(header.data(), 1U, header.size(), file);
    std::fclose(file);
    return ReadPngSize(std::span<const uint8_t>(header.data(), read));
}

// The native size in an Azahar file name, tex1_<W>x<H>_<hash>_<format>_mip0.png
// (as the index reads it: an optional _mip<N> part, any extension). nullopt
// for another mip level (the index keeps level 0 of a texture when the pack
// has it) or any other name, such as a file only pack.json's `textures` map
// names.
[[nodiscard]] inline std::optional<TextureSize> AzaharNativeSizeFromName(std::string_view name) noexcept {
    if (const size_t dot = name.rfind('.'); dot != std::string_view::npos) name = name.substr(0, dot);
    constexpr std::string_view kPrefix = "tex1_";
    if (name.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
    name.remove_prefix(kPrefix.size());
    const auto number = [&](uint32_t& value) {
        const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), value);
        if (error != std::errc{} || end == name.data()) return false;
        name.remove_prefix(static_cast<size_t>(end - name.data()));
        return true;
    };
    const auto take = [&](char expected) {
        if (name.empty() || name.front() != expected) return false;
        name.remove_prefix(1U);
        return true;
    };
    TextureSize size;
    if (!number(size.Width) || !take('x') || !number(size.Height) || !take('_')) return std::nullopt;
    const size_t hashEnd = name.find('_');
    if (hashEnd == 0U || hashEnd == std::string_view::npos ||
        !std::all_of(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(hashEnd),
                     [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
        return std::nullopt;
    name.remove_prefix(hashEnd + 1U);
    uint32_t format = 0U;
    if (!number(format)) return std::nullopt;
    if (!name.empty()) {
        constexpr std::string_view kMip = "_mip";
        uint32_t level = 0U;
        if (name.substr(0, kMip.size()) != kMip) return std::nullopt;
        name.remove_prefix(kMip.size());
        if (!number(level) || !name.empty() || level != 0U) return std::nullopt;
    }
    if (size.Width == 0U || size.Height == 0U) return std::nullopt;
    return size;
}

// HUD, menu and map art: below a UI or MAPS folder at the pack's top (any
// case), the parts shrink_texture_pack.py also treats as the HUD.
[[nodiscard]] inline bool IsHudPackPath(const std::filesystem::path& relative) {
    auto part = relative.begin();
    if (part == relative.end() || std::next(part) == relative.end()) return false;
    std::string top = part->string();
    std::transform(top.begin(), top.end(), top.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return top == "UI" || top == "MAPS";
}

// The smallest cap that keeps `image` whole for a native texture of `native`
// (CappedReplacementSize(native, image, cap) == image), at least 1.
[[nodiscard]] constexpr uint32_t ImageScaleOf(TextureSize native, TextureSize image) noexcept {
    if (native.Width == 0U || native.Height == 0U) return 0U;
    const uint32_t width = (image.Width + native.Width - 1U) / native.Width;
    const uint32_t height = (image.Height + native.Height - 1U) / native.Height;
    return std::max({1U, width, height});
}

// The images to measure: each file the index uses once, without HUD art and
// without the files whose name gives no native size.
struct PackScaleFile {
    std::filesystem::path Path;
    TextureSize Native;
};
struct PackScalePlan {
    std::vector<PackScaleFile> Files;
    size_t Hud = 0;
    size_t Unnamed = 0;
};

[[nodiscard]] inline PackScalePlan PlanPackScaleScan(const std::filesystem::path& root,
                                                     std::vector<std::filesystem::path> indexed) {
    std::sort(indexed.begin(), indexed.end());
    indexed.erase(std::unique(indexed.begin(), indexed.end()), indexed.end());
    PackScalePlan plan;
    for (auto& path : indexed) {
        if (IsHudPackPath(path.lexically_relative(root))) {
            ++plan.Hud;
        } else if (const auto native = AzaharNativeSizeFromName(path.filename().string())) {
            plan.Files.push_back({std::move(path), *native});
        } else {
            ++plan.Unnamed;
        }
    }
    return plan;
}

// Every file of the pack folder by its path below it and its size, in any
// order: a pack copied again, shrunk to other sizes or with files added,
// removed or replaced lists otherwise.
struct PackFingerprint {
    uint64_t Files = 0;
    uint64_t Bytes = 0;
    uint64_t Sum = 0;

    bool operator==(const PackFingerprint&) const = default;

    void Add(std::string_view relative, uint64_t size) noexcept {
        uint64_t hash = 0xCBF29CE484222325ULL;
        for (const char c : relative) hash = (hash ^ static_cast<uint8_t>(c == '\\' ? '/' : c)) * 0x100000001B3ULL;
        hash ^= size * 0x9E3779B97F4A7C15ULL;
        hash = (hash ^ (hash >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        hash = (hash ^ (hash >> 27U)) * 0x94D049BB133111EBULL;
        Sum += hash ^ (hash >> 31U);
        ++Files;
        Bytes += size;
    }
};

// config/texture_pack_sizes: one line, `<sum hex> <files> <bytes> <largest>
// <measured>`; largest 0 = no world or model image measured.
struct PackScaleRecord {
    PackFingerprint Fingerprint;
    uint32_t Largest = 0;
    uint64_t Measured = 0;

    bool operator==(const PackScaleRecord&) const = default;
};

[[nodiscard]] inline std::string FormatPackScaleRecord(const PackScaleRecord& record) {
    char line[128];
    std::snprintf(line, sizeof(line), "%016llx %llu %llu %u %llu\n",
                  static_cast<unsigned long long>(record.Fingerprint.Sum),
                  static_cast<unsigned long long>(record.Fingerprint.Files),
                  static_cast<unsigned long long>(record.Fingerprint.Bytes), record.Largest,
                  static_cast<unsigned long long>(record.Measured));
    return line;
}

[[nodiscard]] inline std::optional<PackScaleRecord> ParsePackScaleRecord(std::string_view text) noexcept {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.remove_suffix(1U);
    PackScaleRecord record;
    const auto field = [&](auto& value, int base) {
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
        if (error != std::errc{} || end == text.data()) return false;
        text.remove_prefix(static_cast<size_t>(end - text.data()));
        return true;
    };
    const auto space = [&] {
        if (text.empty() || text.front() != ' ') return false;
        text.remove_prefix(1U);
        return true;
    };
    if (!field(record.Fingerprint.Sum, 16) || !space() || !field(record.Fingerprint.Files, 10) || !space() ||
        !field(record.Fingerprint.Bytes, 10) || !space() || !field(record.Largest, 10) || !space() ||
        !field(record.Measured, 10) || !text.empty())
        return std::nullopt;
    return record;
}

// The "HD texture size" choices for a cap (config/texture_pack_scale, 0 = no
// cap) and the pack's largest world/model image (0 = not known: nothing
// measured or remembered, or nothing measurable), and the one shown as
// selected (-1: a cap the menu does not name, "Custom"). Known: 1x up to the
// largest (labelled as such), and No limit only above 4x; a cap at or above
// the largest selects it without the setting being written.
struct TexturePackScaleChoice {
    uint32_t Scale = 0;
    std::string Label;
};
struct TexturePackScaleMenu {
    std::vector<TexturePackScaleChoice> Choices;
    int Selected = -1;
};

[[nodiscard]] inline TexturePackScaleMenu MakeTexturePackScaleMenu(uint32_t cap, uint32_t largest) {
    constexpr std::array<std::string_view, 4> kNamed{"1x (original size)", "2x (default)", "3x",
                                                     "4x (full 1080p pack)"};
    TexturePackScaleMenu menu;
    const uint32_t named = largest == 0U ? kTexturePackLargestNamedScale
                                         : std::min(largest, kTexturePackLargestNamedScale);
    for (uint32_t scale = 1U; scale <= named; ++scale) {
        std::string label(kNamed[scale - 1U]);
        if (largest != 0U && scale == largest) {
            label = std::to_string(scale) + "x (" + (scale == 1U ? "original size, " : scale == 2U ? "default, " : "") +
                    "the largest in this pack)";
        }
        menu.Choices.push_back({scale, std::move(label)});
    }
    if (largest == 0U || largest > kTexturePackLargestNamedScale) {
        menu.Choices.push_back(
            {0U, largest == 0U ? "No limit" : "No limit (up to " + std::to_string(largest) + "x in this pack)"});
    }
    // What the cap draws: at or above the largest, the same as the largest.
    uint32_t shown = cap;
    if (largest != 0U && (cap == 0U || cap >= largest))
        shown = largest > kTexturePackLargestNamedScale ? 0U : largest;
    for (size_t index = 0; index < menu.Choices.size(); ++index)
        if (menu.Choices[index].Scale == shown) menu.Selected = static_cast<int>(index);
    return menu;
}

// The last measure remembered in config/texture_pack_sizes (absent, too long
// or damaged: none). Read by the Video tab, which has no pack worker of its own.
[[nodiscard]] inline std::optional<PackScaleRecord> ReadPackScaleRecord(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return std::nullopt;
    std::string text;
    char buffer[129];
    input.read(buffer, sizeof(buffer));
    text.assign(buffer, static_cast<size_t>(input.gcount()));
    if (text.size() >= sizeof(buffer)) return std::nullopt;
    return ParsePackScaleRecord(text);
}

// The largest size the menu offers: this start's measure when the pack worker
// has one, else the one remembered from the last measure (HD textures off, or
// on and not measured yet), else 0 (every size). Without the remembered one the
// menu offered 3x and 4x with HD textures off and dropped them once they were
// turned on (user report, 11/09). A pack changed since the last measure is
// measured again when HD textures are on; until then the remembered sizes show.
[[nodiscard]] inline uint32_t TexturePackMenuLargest(uint32_t measured,
                                                     const std::optional<PackScaleRecord>& remembered) noexcept {
    if (measured != 0U) return measured;
    return remembered ? remembered->Largest : 0U;
}

} // namespace Oot3d::Renderer
