#pragma once

// The HUD images of the player's texture pack, remembered between sessions
// (config/texture_pack_hud on the Switch): one line per HUD texture the pack
// replaced, `<hash, 16 hex digits> <native width> <native height>`. The pack
// worker decodes these as soon as its index is ready, so the HUD's first draw
// already has its pack art; a HUD image decoded on first sight is appended.
// Header-only and free of the runtime, checked on the host
// (oot3d_texture_pack_scale_tests).

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace Oot3d::Renderer {

inline constexpr const char* kTexturePackHudListName = "texture_pack_hud";
// The list stops growing here (another pack's stale lines are dropped at the
// next start, see PruneHudImageList); a HUD has a few dozen textures.
inline constexpr std::size_t kHudImageListLimit = 256U;

struct HudImageEntry {
    uint64_t Hash = 0;
    uint16_t Width = 0;
    uint16_t Height = 0;

    bool operator==(const HudImageEntry&) const = default;
};

// Valid lines in file order, each hash once; malformed lines (a write cut
// short, a hand edit) are skipped, sizes 1-1024 only.
[[nodiscard]] inline std::vector<HudImageEntry> ParseHudImageList(std::string_view text) {
    std::vector<HudImageEntry> entries;
    std::unordered_set<uint64_t> seen;
    while (!text.empty() && entries.size() < kHudImageListLimit) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1U);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1U);
        const auto field = [&line](auto& value, int base) {
            while (!line.empty() && line.front() == ' ') line.remove_prefix(1U);
            const auto [next, error] = std::from_chars(line.data(), line.data() + line.size(), value, base);
            if (error != std::errc{} || next == line.data()) return false;
            line.remove_prefix(static_cast<std::size_t>(next - line.data()));
            return true;
        };
        HudImageEntry entry;
        uint32_t width = 0U, height = 0U;
        if (!field(entry.Hash, 16) || !field(width, 10) || !field(height, 10) || !line.empty() || width == 0U ||
            height == 0U || width > 1024U || height > 1024U)
            continue;
        entry.Width = static_cast<uint16_t>(width);
        entry.Height = static_cast<uint16_t>(height);
        if (seen.insert(entry.Hash).second) entries.push_back(entry);
    }
    return entries;
}

[[nodiscard]] inline std::string FormatHudImageEntry(const HudImageEntry& entry) {
    char line[48];
    std::snprintf(line, sizeof(line), "%016llX %u %u\n", static_cast<unsigned long long>(entry.Hash),
                  static_cast<unsigned>(entry.Width), static_cast<unsigned>(entry.Height));
    return line;
}

// The entries the current pack still has (`indexed(hash)`), in order; the
// file is written again only when some were dropped.
template <typename Indexed>
[[nodiscard]] std::vector<HudImageEntry> PruneHudImageList(const std::vector<HudImageEntry>& entries,
                                                           const Indexed& indexed) {
    std::vector<HudImageEntry> kept;
    kept.reserve(entries.size());
    for (const auto& entry : entries)
        if (indexed(entry.Hash)) kept.push_back(entry);
    return kept;
}

} // namespace Oot3d::Renderer
