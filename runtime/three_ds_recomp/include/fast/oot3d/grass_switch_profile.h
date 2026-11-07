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
// - Far ("far"): full density to twice that, thinning to a farther end.
// - Area ("area"): every tuft of the area's placements, no distance
//   thinning or end; the visible cap is what the stream memory holds, the
//   nearest clusters first when it bites.
enum class GrassSwitchDistance : uint8_t { Near, Far, Area };

[[nodiscard]] GrassSwitchDistance ParseGrassSwitchDistance(std::optional<std::string_view> fileText) noexcept;
// "near", "far" or "area" (the log's `distance=` too).
[[nodiscard]] std::string_view GrassSwitchDistanceWord(GrassSwitchDistance distance) noexcept;
[[nodiscard]] GrassSwitchDistance ReadGrassSwitchDistance(const std::filesystem::path& configDirectory);
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

struct GrassSwitchProfile {
    // Placement: one anchor is one tuft.
    float TuftsPerSquareMeter = 40.0F;
    float MinimumSpacing = 6.0F;
    float TuftHeightMin = 14.0F;
    float TuftHeightMax = 30.0F;
    // Width of one blade of the tuft (the tuft is BladesPerTuft blades wide).
    float BladeWidthMin = 2.0F;
    float BladeWidthMax = 3.5F;
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
    // around Link covers the screen as the full-density band does. Near and
    // Far: FullDensityDistance; Whole area: Near's.
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
// density to 1600, thinning to 5000. Far: full density to twice that,
// thinning to 6000 (8000), at most 16k (40k). Area: kGrassWholeAreaInstances,
// no distance. Thinned tufts are drawn wider. L3+R3 > Video > FPS and
// "Grass distance" switch between them while the game runs
// (Deko3dSetGrassFrameRate, PublishGrassSwitchDistance).
[[nodiscard]] GrassSwitchProfile SelectGrassSwitchProfile(bool fps60, GrassSwitchDistance distance) noexcept;

// Instances one stream slot holds (kGrassWholeAreaInstances).
[[nodiscard]] uint32_t GrassSwitchStreamCapacity() noexcept;

// The profile's generation and distances on top of the product preset (its
// source rules and appearance stay). Quality becomes Custom.
void ApplyGrassSwitchProfile(const GrassSwitchProfile& profile, InteractiveGrassSettings& settings) noexcept;

// Retained bytes of one anchor: GrassWorldAnchor + GrassWorldCullingAnchor.
[[nodiscard]] size_t GrassRetainedBytesPerAnchor() noexcept;

} // namespace Fast::Oot3d
