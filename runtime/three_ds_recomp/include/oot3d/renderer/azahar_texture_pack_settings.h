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
// - config/texture_pack_reresolve: `0` turns off the second resolve of the
//   textures drawn while the pack's index was being built (at start, or after
//   a change in the Video tab), which then stay native until evicted (the
//   behaviour before 08/11); absent = on.
// - config/texture_pack_sizes: written by the pack worker, the pack's largest
//   world/model image scale with the pack's fingerprint (azahar_pack_scale.h):
//   the Video tab offers only the sizes the pack has.
//
// The Video tab writes the files and publishes the values in
// LiveTexturePackSettings(); the renderer applies a new revision at the start
// of its next frame.

#include "oot3d/renderer/azahar_hud_image_list.h"
#include "oot3d/renderer/azahar_pack_scale.h"
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
inline constexpr const char* kTexturePackReresolveConfigName = "texture_pack_reresolve";

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
    bool Reresolve = true;

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
        if (std::ifstream file(config / kTexturePackReresolveConfigName); file) {
            std::string text;
            std::getline(file, text);
            settings.Reresolve = text.empty() || text.front() != '0';
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
    if (!config.empty()) configuration.ImageScaleRecord = config / kTexturePackSizesName;
    return configuration;
}

// What the renderer's texture cache does with one cached entry once the pack
// settings changed (`epoch` counts the changes; an entry remembers the epoch
// it was made in) or the pack's index became ready:
// - Keep: still valid.
// - Rebuild: a native texture made without the current pack, or made while the
//   pack's index was being built (`unindexed`: the runtime had no hash for it,
//   AzaharTextureResolveResult::NativeHashAvailable false, so it could never
//   find its image) now that the index is ready: retire it and resolve it
//   again through the pack.
// - Release: a pack image of earlier settings: never drawn again; retire it
//   (its memory goes once no frame in flight can read it).
enum class TexturePackEntryAction : uint8_t { Keep, Rebuild, Release };

[[nodiscard]] constexpr TexturePackEntryAction TexturePackEntryActionFor(bool replacement, uint32_t entryEpoch,
                                                                         uint32_t epoch, bool packOn, bool unindexed,
                                                                         bool indexReady) noexcept {
    if (entryEpoch != epoch) {
        if (replacement) return TexturePackEntryAction::Release;
        return packOn ? TexturePackEntryAction::Rebuild : TexturePackEntryAction::Keep;
    }
    return !replacement && packOn && unindexed && indexReady ? TexturePackEntryAction::Rebuild
                                                             : TexturePackEntryAction::Keep;
}

// Textures one frame resolves again after a pack change (a Rebuild above, or
// a pack image of another size cap still drawn): each decodes the game's
// texture, identifies it in the pack and uploads it on the render thread.
// test.13 did all of a view's at once, at the change and again when the
// index was ready. The others keep drawing what they drew (the native
// texture, or the earlier cap's image) until a later frame has budget; pack
// images still upload within kReplacementBytesPerFrame. Turning the pack off
// is not spread: no pack image is drawn from that frame on.
inline constexpr uint32_t kTexturePackRebuildsPerFrame = 6U;

struct TexturePackRebuildBudget {
    uint32_t Left = kTexturePackRebuildsPerFrame;
    uint64_t Deferred = 0; // asked for with nothing left, all frames

    constexpr void NewFrame() noexcept { Left = kTexturePackRebuildsPerFrame; }
    [[nodiscard]] constexpr bool Take() noexcept {
        if (Left == 0U) {
            ++Deferred;
            return false;
        }
        --Left;
        return true;
    }
};

// The hold a scene's first frames get (config/texture_pack_hold): only when
// the game entered it with the pack on. A settings change never holds, also
// when the scene changed while the pack was off (test.13: HD textures turned
// on in the meadow held the frame 558 ms, perf.txt:2289, hitches.txt:1735).
[[nodiscard]] constexpr uint64_t TexturePackSceneHoldMicros(bool sceneEntered, uint32_t holdMs) noexcept {
    return sceneEntered ? static_cast<uint64_t>(holdMs) * 1000U : 0U;
}

// The HUD work one game-thread frame may do because of a change (texture
// preparation, N64UiFast3dRenderer::PrepareTextures): above it the frame
// counts as over budget in the change's line.
inline constexpr uint64_t kTexturePackChangeGamePrepareBudgetMicros = 4000U;

// The game thread during a pack change, from the change to its `took
// effect` line: its longest frame (one UI submission to the next, waits for
// the render thread included) and its longest UI texture preparation (the HUD
// textures resolved again, N64UiFast3dRenderer::PrepareTextures). The
// renderer begins and ends it; the game thread observes each frame.
class TexturePackChangeGameWatch {
  public:
    struct Result {
        uint64_t Frames = 0;
        uint64_t LongestFrameMicros = 0;
        uint64_t LongestPrepareMicros = 0;
        uint64_t PrepareOverBudget = 0; // frames over kTexturePackChangeGamePrepareBudgetMicros
        uint64_t Resolves = 0;          // HUD textures resolved (the provider's resolve calls)
        uint64_t Decodes = 0;           // of those, decoded (not in its identity cache)
        // Audio concealments (the output queue ran dry: repeated sound) from
        // the change's first game-thread frame to its last.
        uint64_t ConcealEvents = 0;
    };

    void Begin() noexcept {
        mFrames.store(0U, std::memory_order_relaxed);
        mLongestFrame.store(0U, std::memory_order_relaxed);
        mLongestPrepare.store(0U, std::memory_order_relaxed);
        mOverBudget.store(0U, std::memory_order_relaxed);
        mResolves.store(0U, std::memory_order_relaxed);
        mDecodes.store(0U, std::memory_order_relaxed);
        mFirstConceal.store(kUnknown, std::memory_order_relaxed);
        mLastConceal.store(kUnknown, std::memory_order_relaxed);
        mActive.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool Active() const noexcept { return mActive.load(std::memory_order_acquire); }
    // One game-thread frame: from the previous one's UI submission, its HUD
    // texture preparation, the HUD textures it resolved and decoded, and the
    // audio stream's concealments so far (a running total).
    void Observe(uint64_t frameMicros, uint64_t prepareMicros, uint64_t resolves = 0U, uint64_t decodes = 0U,
                 uint64_t concealEventsTotal = kUnknown) noexcept {
        if (!mActive.load(std::memory_order_acquire)) return;
        mFrames.fetch_add(1U, std::memory_order_relaxed);
        Raise(mLongestFrame, frameMicros);
        Raise(mLongestPrepare, prepareMicros);
        if (prepareMicros > kTexturePackChangeGamePrepareBudgetMicros) mOverBudget.fetch_add(1U, std::memory_order_relaxed);
        mResolves.fetch_add(resolves, std::memory_order_relaxed);
        mDecodes.fetch_add(decodes, std::memory_order_relaxed);
        if (concealEventsTotal != kUnknown) {
            uint64_t expected = kUnknown;
            mFirstConceal.compare_exchange_strong(expected, concealEventsTotal, std::memory_order_relaxed);
            mLastConceal.store(concealEventsTotal, std::memory_order_relaxed);
        }
    }
    Result End() noexcept {
        mActive.store(false, std::memory_order_release);
        const uint64_t first = mFirstConceal.load(std::memory_order_relaxed);
        const uint64_t last = mLastConceal.load(std::memory_order_relaxed);
        return {mFrames.load(std::memory_order_relaxed),    mLongestFrame.load(std::memory_order_relaxed),
                mLongestPrepare.load(std::memory_order_relaxed), mOverBudget.load(std::memory_order_relaxed),
                mResolves.load(std::memory_order_relaxed),  mDecodes.load(std::memory_order_relaxed),
                first == kUnknown || last < first ? 0U : last - first};
    }

  private:
    static void Raise(std::atomic<uint64_t>& longest, uint64_t value) noexcept {
        uint64_t current = longest.load(std::memory_order_relaxed);
        while (value > current && !longest.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
        }
    }
    static constexpr uint64_t kUnknown = ~0ULL;
    std::atomic<bool> mActive{false};
    std::atomic<uint64_t> mFrames{0U};
    std::atomic<uint64_t> mLongestFrame{0U};
    std::atomic<uint64_t> mLongestPrepare{0U};
    std::atomic<uint64_t> mOverBudget{0U};
    std::atomic<uint64_t> mResolves{0U};
    std::atomic<uint64_t> mDecodes{0U};
    std::atomic<uint64_t> mFirstConceal{kUnknown};
    std::atomic<uint64_t> mLastConceal{kUnknown};
};

inline TexturePackChangeGameWatch& LiveTexturePackChangeGameWatch() noexcept {
    static TexturePackChangeGameWatch watch;
    return watch;
}

// A pack change (or the configuration at start) has taken effect on screen
// once no pack image of earlier settings is left and, with the pack on, its
// index is ready and none of the textures drawn last frame still waits for its
// pack image (decoding, or made while the index was being built). The
// renderer logs it once (`texture pack change took effect`).
[[nodiscard]] constexpr bool TexturePackChangeTookEffect(bool packOn, bool indexReady, uint64_t waitingOnScreen,
                                                         bool earlierPackImagesLeft) noexcept {
    return !earlierPackImagesLeft && (!packOn || (indexReady && waitingOnScreen == 0U));
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
