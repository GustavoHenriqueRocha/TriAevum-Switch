#pragma once

// The console's texture pack settings (config/ files), shared by the deko3d
// renderer (gfx_deko3d_pica_resources.cpp) and the L3+R3 Video tab
// (oot3d_video_settings_panel.cpp). Header-only and free of deko3d, checked on
// the host (oot3d_texture_pack_scale_tests).
//
// - config/texture_pack: present = HD textures on; its first line, when not
//   empty, is the pack folder (default sdmc:/switch/oot3dre/textures).
// - config/texture_pack_scale: the size cap, as a multiple of the native
//   texture (0 = no cap, 1-8); absent = 2. Pack images above it are reduced
//   on the pack worker.
// - config/texture_pack_preload: `0` turns off the scene lists
//   (config/texture_pack_scenes, azahar_scene_image_list.h) and the decode of
//   a scene's listed images when the game enters it; absent = on.
// - config/texture_pack_hold: how long, in ms, the renderer may wait in all
//   for a scene's world images while its first frames draw (`0` or `off` =
//   never); absent = 1000, at most 2500.
//
// The Video tab writes the files and publishes the values in
// LiveTexturePackSettings(); the renderer applies a new revision at the start
// of its next frame.

#include "oot3d/renderer/azahar_hud_image_list.h"
#include "oot3d/renderer/azahar_scene_image_list.h"
#include "oot3d/renderer/azahar_texture_pack.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace Oot3d::Renderer {

inline constexpr const char* kTexturePackConfigName = "texture_pack";
inline constexpr const char* kTexturePackScaleConfigName = "texture_pack_scale";
inline constexpr const char* kTexturePackDefaultFolder = "sdmc:/switch/oot3dre/textures";
// 720p handheld renders at 2.25-3x the 3DS screen: about 2x texels are enough.
inline constexpr uint32_t kTexturePackDefaultMaxScale = 2U;
inline constexpr uint32_t kTexturePackMaxScaleLimit = 8U;
inline constexpr const char* kTexturePackPreloadConfigName = "texture_pack_preload";
inline constexpr const char* kTexturePackHoldConfigName = "texture_pack_hold";
inline constexpr uint32_t kTexturePackHoldDefaultMs = 1000U;
// The hang watchdog reports a game thread stalled for 3 s.
inline constexpr uint32_t kTexturePackHoldLimitMs = 2500U;

// A whole number 0-8 (surrounding spaces and a line end allowed); anything
// else is `fallback`.
[[nodiscard]] inline uint32_t ParseTexturePackMaxScale(std::string_view text, uint32_t fallback) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1U);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1U);
    uint32_t value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size() || value > kTexturePackMaxScaleLimit)
        return fallback;
    return value;
}

// config/texture_pack_hold's first line: `0`/`off` = 0, a whole number of ms
// (at most kTexturePackHoldLimitMs), anything else the default.
[[nodiscard]] inline uint32_t ParseTexturePackHoldMs(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1U);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1U);
    if (text == "off") return 0U;
    uint32_t value = 0U;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return kTexturePackHoldDefaultMs;
    return std::min(value, kTexturePackHoldLimitMs);
}

struct TexturePackSettings {
    bool Enabled = false;
    uint32_t MaxScale = kTexturePackDefaultMaxScale;
    std::string Folder = kTexturePackDefaultFolder;
    // Read at start only (the Video tab does not write them).
    bool ScenePreload = true;
    uint32_t HoldMs = kTexturePackHoldDefaultMs;

    bool operator==(const TexturePackSettings&) const = default;

    static TexturePackSettings Load(const std::filesystem::path& config) {
        TexturePackSettings settings;
        if (std::ifstream file(config / kTexturePackConfigName); file) {
            settings.Enabled = true;
            std::string folder;
            std::getline(file, folder);
            while (!folder.empty() && (folder.back() == '\r' || folder.back() == ' ')) folder.pop_back();
            if (!folder.empty()) settings.Folder = folder;
        }
        if (std::ifstream file(config / kTexturePackScaleConfigName); file) {
            std::string text;
            std::getline(file, text);
            settings.MaxScale = ParseTexturePackMaxScale(text, kTexturePackDefaultMaxScale);
        }
        if (std::ifstream file(config / kTexturePackPreloadConfigName); file) {
            std::string text;
            std::getline(file, text);
            settings.ScenePreload = text.empty() || text.front() != '0';
        }
        if (std::ifstream file(config / kTexturePackHoldConfigName); file) {
            std::string text;
            std::getline(file, text);
            settings.HoldMs = ParseTexturePackHoldMs(text);
        }
        return settings;
    }

    // texture_pack exists only while on (its folder line kept when not the
    // default); texture_pack_scale only away from the default. Empty on
    // success, otherwise the file that failed.
    [[nodiscard]] std::string Save(const std::filesystem::path& config) const {
        std::error_code error;
        std::filesystem::create_directories(config, error);
        if (Enabled) {
            std::ofstream file(config / kTexturePackConfigName, std::ios::trunc);
            if (Folder != kTexturePackDefaultFolder) file << Folder << '\n';
            if (!file) return kTexturePackConfigName;
        } else {
            std::filesystem::remove(config / kTexturePackConfigName, error);
            if (error) return kTexturePackConfigName;
        }
        if (MaxScale != kTexturePackDefaultMaxScale) {
            std::ofstream file(config / kTexturePackScaleConfigName, std::ios::trunc);
            file << MaxScale << '\n';
            if (!file) return kTexturePackScaleConfigName;
        } else {
            std::filesystem::remove(config / kTexturePackScaleConfigName, error);
            if (error) return kTexturePackScaleConfigName;
        }
        return {};
    }
};

// The folder as players see it on the SD card ("switch/oot3dre/textures").
[[nodiscard]] inline std::string TexturePackDisplayFolder(std::string_view folder) {
    for (const std::string_view prefix : {"sdmc:/", "sdmc:", "/"}) {
        if (folder.substr(0, prefix.size()) == prefix) {
            folder.remove_prefix(prefix.size());
            break;
        }
    }
    return std::string(folder);
}

// The pack runtime's configuration for these settings. Mipmaps are made on
// the pack worker: the renderer uploads as many levels as the native
// texture samples. The HUD's pack images are remembered in
// `config`/texture_pack_hud (azahar_hud_image_list.h) and decoded at start-up;
// each scene's world images in `config`/texture_pack_scenes
// (azahar_scene_image_list.h), decoded when the game enters the scene.
[[nodiscard]] inline AzaharTexturePackConfiguration MakeTexturePackConfiguration(const TexturePackSettings& settings,
                                                                                 const std::filesystem::path& config) {
    AzaharTexturePackConfiguration configuration;
    configuration.LoadCustomTextures = settings.Enabled;
    configuration.LoadDirectory = settings.Folder.empty() ? kTexturePackDefaultFolder : settings.Folder;
    configuration.MaxReplacementScale = settings.MaxScale;
    configuration.GenerateMipmaps = true;
    if (!config.empty()) configuration.HudImageList = config / kTexturePackHudListName;
    if (!config.empty() && settings.ScenePreload)
        configuration.SceneImageList = config / kTexturePackSceneListName;
    return configuration;
}

// What the renderer's texture cache does with one cached entry once the pack
// settings changed (`epoch` counts the changes; an entry remembers the epoch
// it was made in):
// - Keep: still valid.
// - Rebuild: a native texture made without the current pack: retire it and
//   resolve it again through the pack.
// - Release: a pack image of earlier settings: never drawn again; retire it
//   (its memory goes once no frame in flight can read it).
enum class TexturePackEntryAction : uint8_t { Keep, Rebuild, Release };

[[nodiscard]] constexpr TexturePackEntryAction TexturePackEntryActionFor(bool replacement, uint32_t entryEpoch,
                                                                         uint32_t epoch, bool packOn) noexcept {
    if (entryEpoch == epoch) return TexturePackEntryAction::Keep;
    if (replacement) return TexturePackEntryAction::Release;
    return packOn ? TexturePackEntryAction::Rebuild : TexturePackEntryAction::Keep;
}

// Published by the Video tab; read by the renderer once per frame.
struct TexturePackLiveSettings {
    std::atomic<bool> Enabled{false};
    std::atomic<uint32_t> MaxScale{kTexturePackDefaultMaxScale};
    std::atomic<uint64_t> Revision{0U};

    void Publish(const TexturePackSettings& settings) noexcept {
        Enabled.store(settings.Enabled, std::memory_order_relaxed);
        MaxScale.store(settings.MaxScale, std::memory_order_relaxed);
        Revision.fetch_add(1U, std::memory_order_release);
    }
};

inline TexturePackLiveSettings& LiveTexturePackSettings() noexcept {
    static TexturePackLiveSettings settings;
    return settings;
}

} // namespace Oot3d::Renderer
