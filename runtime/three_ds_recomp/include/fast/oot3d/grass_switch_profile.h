#pragma once

// Grass on the Switch (SWITCH_PORT.md, "Grass on deko3d"): what
// config/grass asks for and the budgets of the two frame rates. The PC preset
// is tuned for a desktop GPU; on the console the blades are tufts (5 blade
// triangles side by side, facing the camera) at ~40 per m², single blades
// with their own facing near the camera, a draw distance and a cap on what
// is drawn and on what stays in memory.

#include "fast/oot3d/graphics_settings.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace Fast::Oot3d {

// config/grass: absent = Off (nothing runs). An empty file (or "placements")
// builds placements and logs them without drawing, as before; "draw" draws.
// "0", "off" and "false" are Off too.
enum class GrassSwitchMode : uint8_t { Off, Placements, Draw };

[[nodiscard]] GrassSwitchMode ParseGrassSwitchMode(std::optional<std::string_view> fileText) noexcept;
// The word written by the L3+R3 menu for a mode (nullopt: remove the file).
[[nodiscard]] std::optional<std::string_view> GrassSwitchModeWord(GrassSwitchMode mode) noexcept;
// config/grass in `configDirectory` (its first line).
[[nodiscard]] GrassSwitchMode ReadGrassSwitchMode(const std::filesystem::path& configDirectory);
// Writes config/grass (removes it for Off); false when that failed.
[[nodiscard]] bool SaveGrassSwitchMode(const std::filesystem::path& configDirectory, GrassSwitchMode mode);

// config/grass_distance (L3+R3 > Video > "Grass distance", applied at once):
// how far the grass reaches.
// - Near (absent file, "near"): full density to 1000 units at 60 fps (1600
//   at 30), then thinning with distance.
// - Area ("area"): every tuft of the area's placements, no distance
//   thinning or end; the visible cap is what the stream memory holds, the
//   nearest clusters first when it bites.
// Far ("far", full density to twice Near's) was removed after test.15: it
// cost the most in small areas (Kokiri Forest 1.43 ms at its 16k cap) and
// added nothing Whole area does not. A saved "far" reads as Area (grass far
// away, what that player chose); the file keeps its word until the option
// is changed.
enum class GrassSwitchDistance : uint8_t { Near, Area };

// `retiredFar` (optional): set when the file says the removed "far".
[[nodiscard]] GrassSwitchDistance ParseGrassSwitchDistance(std::optional<std::string_view> fileText,
                                                           bool* retiredFar = nullptr) noexcept;
// "near" or "area" (the log's `distance=` too).
[[nodiscard]] std::string_view GrassSwitchDistanceWord(GrassSwitchDistance distance) noexcept;
[[nodiscard]] GrassSwitchDistance ReadGrassSwitchDistance(const std::filesystem::path& configDirectory,
                                                          bool* retiredFar = nullptr);
// Writes config/grass_distance (removes it for Near); false when that failed.
[[nodiscard]] bool SaveGrassSwitchDistance(const std::filesystem::path& configDirectory,
                                           GrassSwitchDistance distance);
// The choice the menu made while the game runs (nullopt before any): the
// renderer applies it at its next frame. Any thread.
void PublishGrassSwitchDistance(GrassSwitchDistance distance) noexcept;
[[nodiscard]] std::optional<GrassSwitchDistance> PublishedGrassSwitchDistance() noexcept;

// config/grass_lod (L3+R3 > Video > "Grass detail", applied at once;
// SWITCH_PORT.md "Grass on deko3d: cost per tuft"): how a tuft is drawn.
// - On (absent file, any other word): each tuft's shared work (wind, bend,
//   light, colour) once per frame in a tuft pass, then its blades; far
//   tufts with 3 and then 1 wider blade (the same covered area), turning
//   into them over a distance.
// - Off ("0", "off", "false"): test.9's grass exactly (every vertex does
//   all the work, 5 blades everywhere).
[[nodiscard]] bool ParseGrassSwitchLod(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] bool ReadGrassSwitchLod(const std::filesystem::path& configDirectory);
// Writes config/grass_lod (removes it for On, writes "0" for Off); false when that failed.
[[nodiscard]] bool SaveGrassSwitchLod(const std::filesystem::path& configDirectory, bool on);
// The menu's choice while the game runs (nullopt before any). Any thread.
void PublishGrassSwitchLod(bool on) noexcept;
[[nodiscard]] std::optional<bool> PublishedGrassSwitchLod() noexcept;

// config/grass_fill (L3+R3 > Video > "Grass near Link", applied at once;
// SWITCH_PORT.md "Grass on deko3d: nothing grows near Link"): how the fill
// tufts near the camera come and go (GrassStreamSettings::FillFixed).
// - Fixed (absent file, any other word): the same fill tufts at full size
//   within kGrassFixedFillNear wherever the camera is; past it they narrow
//   away, and thinned tufts widen in: nothing grows out of the ground.
// - Off ("0", "off", "false"): test.9's fill, growing out of the ground over
//   a quarter of its distance, and thinned tufts growing in.
[[nodiscard]] bool ParseGrassSwitchFill(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] bool ReadGrassSwitchFill(const std::filesystem::path& configDirectory);
// Writes config/grass_fill (removes it for fixed, writes "0" for test.9's); false when that failed.
[[nodiscard]] bool SaveGrassSwitchFill(const std::filesystem::path& configDirectory, bool fixed);
// The menu's choice while the game runs (nullopt before any). Any thread.
void PublishGrassSwitchFill(bool fixed) noexcept;
[[nodiscard]] std::optional<bool> PublishedGrassSwitchFill() noexcept;

// config/grass_placement_cache (read at start; SWITCH_PORT.md "File select,
// world textures and grass without pop-in"): built placements are kept on the SD card
// (shadercache/grass_placements_v1, GrassPersistentPlacementCache, the PC's
// format) and a ground seen in an earlier visit or session loads its
// placement instead of building it again.
// - On (absent file, any other word).
// - Off ("0", "off", "false"): every placement is built, as before.
[[nodiscard]] bool ParseGrassPlacementCache(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] bool ReadGrassPlacementCache(const std::filesystem::path& configDirectory);
// The SD card's share: at most this much and this many placements (the least
// recently used go first). Hyrule Field's ground is ~8 MiB.
inline constexpr uint64_t kGrassPlacementCacheBytes = 256ULL << 20;
inline constexpr size_t kGrassPlacementCacheEntries = 96U;
// config/grass_placement_reuse (read at start; SWITCH_PORT.md "Grass at a
// scene's start"): a stored ground loads although the camera of its first
// frame rounded its transform differently, or it sits at other guest
// addresses (GrassPlacementDiskMatch); its tufts are drawn with the
// transform they were made with.
// - On (absent file, any other word).
// - Off ("0", "off", "false"): exact matching (the PC's); a different
//   first camera builds the ground again.
[[nodiscard]] bool ParseGrassPlacementReuse(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] bool ReadGrassPlacementReuse(const std::filesystem::path& configDirectory);
// config/grass_hold (read at start; grass_scene_hold.h): ms per scene the
// render thread may wait for the scene's first grass. Absent or unreadable:
// kGrassSceneHoldDefaultMs; "0"/"off": never; at most kGrassSceneHoldLimitMs.
[[nodiscard]] uint32_t ParseGrassSceneHoldMs(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] uint32_t ReadGrassSceneHoldMs(const std::filesystem::path& configDirectory);
// config/grass_density (read at start): tufts per square metre, for the PC's
// blade size (A/B within one build). Absent or unreadable:
// kGrassDensityDefault; clamped to [kGrassDensityMin, kGrassDensityMax].
inline constexpr float kGrassDensityDefault = 80.0F;
inline constexpr float kGrassDensityMin = 40.0F;
inline constexpr float kGrassDensityMax = 200.0F;
[[nodiscard]] float ParseGrassSwitchDensity(std::optional<std::string_view> fileText) noexcept;
[[nodiscard]] float ReadGrassSwitchDensity(const std::filesystem::path& configDirectory);

struct GrassSwitchProfile {
    // Placement: one anchor is one tuft.
    float TuftsPerSquareMeter = 40.0F;
    float MinimumSpacing = 6.0F;
    // The PC preset's blade (graphics_settings_persistence.cpp, "Grass"
    // "Generation"; its HeightScale 1.15 applies on top as on the PC). The
    // tufts were 14-30 x 2-3.5 to cover with fewer of them: tall and sparse
    // next to the PC's (12/11); config/grass_density gives more tufts instead.
    float TuftHeightMin = 7.68F;
    float TuftHeightMax = 12.51F;
    // Width of one blade of the tuft (the tuft is BladesPerTuft blades wide).
    float BladeWidthMin = 1.8F;
    float BladeWidthMax = 1.94F;
    uint8_t BladesPerTuft = 5U;
    // How much of the blade colour comes from the ground texture under the
    // tuft (the rest from the preset's root and tip colours).
    float TextureColorInfluence = 0.85F;
    float ClusterSize = 300.0F;
    // A cap per placement request; a cap that bites thins evenly
    // (ThinGrassAnchorsToLimit), it never cuts the index buffer short.
    uint32_t MaxAnchorsPerPlacement = 96000U;
    // Placements kept in memory (anchors + culling copies + clusters): the
    // least recently drawn go first, so what stays is what the camera sees.
    size_t RetainedBytes = 12U * 1024U * 1024U;
    // Drawing, in world units (1 m = 100). Full density up to
    // FullDensityDistance, then thinning with distance (about the same
    // number of tufts per screen area) to none at DrawDistance; tufts shrink
    // into the ground from FadeStart. Whole area: all three are
    // kGrassWholeAreaDistance (every tuft at full density and height).
    float FullDensityDistance = 1000.0F;
    float DrawDistance = 4000.0F;
    float FadeStart = 3300.0F;
    // Tufts closer than this are drawn as their blades, one segment each;
    // they turn into tufts over the next half of it (grass_vsh.glsl).
    float SingleBladeDistance = 400.0F;
    // Fill near the camera (GrassStreamFillMultiplier): FillDistance /
    // distance tufts per placement tuft, within FillRadius of it, so the grass
    // around Link covers the screen as the full-density band does. Near:
    // FullDensityDistance; Whole area: Near's.
    float FillDistance = 1000.0F;
    float FillRadius = 18.0F;
    uint32_t MaxVisibleInstances = 8000U;
};

// Instances one stream slot holds: the Whole area cap, the largest of every
// rate and distance, so neither an FPS nor a distance change needs new
// stream memory (4 slots of 32 bytes each: 8 MiB).
inline constexpr uint32_t kGrassWholeAreaInstances = 65536U;
// Whole area's distances (1 km, past any area of the game).
inline constexpr float kGrassWholeAreaDistance = 100000.0F;

// Near: 60 fps (60 FPS presented) ≤8k visible, full density to 1000 units,
// thinning to 4000; 30 fps: the frame has 33 ms, so 22k visible, full
// density to 1600, thinning to 5000. Area: kGrassWholeAreaInstances, no
// distance. Thinned tufts are drawn wider. L3+R3 > Video > FPS and
// "Grass distance" switch between them while the game runs
// (Deko3dSetGrassFrameRate, PublishGrassSwitchDistance).
[[nodiscard]] GrassSwitchProfile SelectGrassSwitchProfile(bool fps60, GrassSwitchDistance distance) noexcept;

// Instances one stream slot holds (kGrassWholeAreaInstances).
[[nodiscard]] uint32_t GrassSwitchStreamCapacity() noexcept;

// The profile's generation and distances on top of the product preset (its
// source rules and appearance stay). Quality becomes Custom.
void ApplyGrassSwitchProfile(const GrassSwitchProfile& profile, InteractiveGrassSettings& settings) noexcept;
// config/grass_density on a selected profile: that many tufts per square
// metre, the spacing shrinking as 1 / sqrt(density) and the visible cap
// growing with it (at most kGrassWholeAreaInstances); retained memory stays.
void ApplyGrassSwitchDensity(GrassSwitchProfile& profile, float tuftsPerSquareMeter) noexcept;

// Retained bytes of one anchor: GrassWorldAnchor + GrassWorldCullingAnchor.
[[nodiscard]] size_t GrassRetainedBytesPerAnchor() noexcept;

} // namespace Fast::Oot3d
