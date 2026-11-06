#pragma once

// Grass on the Switch (SWITCH_PORT.md, "Grass on deko3d"): what
// config/grass asks for and the budgets of the two frame rates. The PC preset
// is tuned for a desktop GPU; on the console the blades are tufts (a 4-vertex
// card cut into 4-5 blades in the fragment shader) at ~40 per m², single
// 1-segment blades near the camera, a short draw distance and a cap on what
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

struct GrassSwitchProfile {
    // Placement: one anchor is one tuft.
    float TuftsPerSquareMeter = 40.0F;
    float MinimumSpacing = 6.0F;
    float TuftHeightMin = 14.0F;
    float TuftHeightMax = 30.0F;
    // Width of one blade of the tuft (the card is BladesPerTuft blades wide).
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
    // Drawing, in world units (1 m = 100).
    float DrawDistance = 1200.0F;
    float FadeStart = 900.0F;
    // Tufts closer than this are drawn as their blades, one segment each.
    float SingleBladeDistance = 400.0F;
    uint32_t MaxVisibleInstances = 8000U;
};

// 60 fps (config/fps60 on): ≤8k visible, 1200 units faded from 900.
// 30 fps: the frame has 33 ms, so 22k visible and 2000 units (fade from 1500).
[[nodiscard]] GrassSwitchProfile SelectGrassSwitchProfile(bool fps60) noexcept;

// The profile's generation and distances on top of the product preset (its
// source rules and appearance stay). Quality becomes Custom.
void ApplyGrassSwitchProfile(const GrassSwitchProfile& profile, InteractiveGrassSettings& settings) noexcept;

// Retained bytes of one anchor: GrassWorldAnchor + GrassWorldCullingAnchor.
[[nodiscard]] size_t GrassRetainedBytesPerAnchor() noexcept;

} // namespace Fast::Oot3d
