#pragma once

// The world images of the player's texture pack, remembered per scene between
// sessions (config/texture_pack_scenes on the Switch): one line per pack image
// a scene's textures asked for, `<scene, hex> <hash, 16 hex digits> <native
// width> <native height>`, in the order the scene first drew them. When the
// game enters a scene (AzaharTexturePackRuntime::NoteScene) the pack worker
// decodes that scene's listed images, so they are ready when its first frames
// draw instead of popping in after them; each image a scene asks for and its
// list does not have yet is appended. Header-only and free of the runtime,
// checked on the host (oot3d_texture_pack_scale_tests).

#include "oot3d/renderer/azahar_hud_image_list.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Oot3d::Renderer {

inline constexpr const char* kTexturePackSceneListName = "texture_pack_scenes";
// Per scene and in all: a dungeon with all its rooms draws a few hundred
// textures, the game has ~110 scenes. Lines past either limit are not read.
inline constexpr std::size_t kSceneImageListPerScene = 512U;
inline constexpr std::size_t kSceneImageListLimit = 16384U;
// The scene numbers the game uses (its scene table); anything else is a
// damaged line.
inline constexpr uint32_t kSceneImageListMaxScene = 0xFFU;

struct SceneImageEntry {
    uint16_t Scene = 0;
    HudImageEntry Image;

    bool operator==(const SceneImageEntry&) const = default;
};

// Valid lines in file order, each (scene, hash) once; malformed lines (a write
// cut short, a hand edit) are skipped, sizes 1-1024 only.
[[nodiscard]] inline std::vector<SceneImageEntry> ParseSceneImageList(std::string_view text) {
    std::vector<SceneImageEntry> entries;
    std::unordered_map<uint16_t, std::unordered_set<uint64_t>> seen;
    while (!text.empty() && entries.size() < kSceneImageListLimit) {
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
        uint32_t scene = 0U, width = 0U, height = 0U;
        uint64_t hash = 0U;
        if (!field(scene, 16) || !field(hash, 16) || !field(width, 10) || !field(height, 10) || !line.empty() ||
            scene > kSceneImageListMaxScene || width == 0U || height == 0U || width > 1024U || height > 1024U)
            continue;
        auto& sceneSeen = seen[static_cast<uint16_t>(scene)];
        if (sceneSeen.size() >= kSceneImageListPerScene || !sceneSeen.insert(hash).second) continue;
        entries.push_back({static_cast<uint16_t>(scene),
                           {hash, static_cast<uint16_t>(width), static_cast<uint16_t>(height)}});
    }
    return entries;
}

[[nodiscard]] inline std::string FormatSceneImageEntry(const SceneImageEntry& entry) {
    char line[56];
    std::snprintf(line, sizeof(line), "%02X %016llX %u %u\n", static_cast<unsigned>(entry.Scene),
                  static_cast<unsigned long long>(entry.Image.Hash), static_cast<unsigned>(entry.Image.Width),
                  static_cast<unsigned>(entry.Image.Height));
    return line;
}

// The entries the current pack still has (`indexed(hash)`), in order.
template <typename Indexed>
[[nodiscard]] std::vector<SceneImageEntry> PruneSceneImageList(const std::vector<SceneImageEntry>& entries,
                                                               const Indexed& indexed) {
    std::vector<SceneImageEntry> kept;
    kept.reserve(entries.size());
    for (const auto& entry : entries)
        if (indexed(entry.Image.Hash)) kept.push_back(entry);
    return kept;
}

} // namespace Oot3d::Renderer
