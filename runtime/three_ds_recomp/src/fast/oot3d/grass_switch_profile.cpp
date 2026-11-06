#include "fast/oot3d/grass_switch_profile.h"

#include "fast/oot3d/grass_world_placement_cache.h"

#include <algorithm>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

GrassSwitchMode ParseGrassSwitchMode(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return GrassSwitchMode::Off;
    std::string_view text = *fileText;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    size_t end = 0U;
    while (end < text.size() && !space(text[end])) ++end;
    const std::string_view word = text.substr(0U, end);
    if (word == "draw") return GrassSwitchMode::Draw;
    if (word == "0" || word == "off" || word == "false") return GrassSwitchMode::Off;
    return GrassSwitchMode::Placements;
}

std::optional<std::string_view> GrassSwitchModeWord(GrassSwitchMode mode) noexcept {
    switch (mode) {
    case GrassSwitchMode::Off: return std::nullopt;
    case GrassSwitchMode::Placements: return std::string_view("placements");
    case GrassSwitchMode::Draw: return std::string_view("draw");
    }
    return std::nullopt;
}

GrassSwitchMode ReadGrassSwitchMode(const std::filesystem::path& configDirectory) {
    std::ifstream file(configDirectory / "grass");
    if (!file) return GrassSwitchMode::Off;
    std::string text;
    std::getline(file, text);
    return ParseGrassSwitchMode(std::string_view(text));
}

bool SaveGrassSwitchMode(const std::filesystem::path& configDirectory, GrassSwitchMode mode) {
    std::error_code error;
    const auto word = GrassSwitchModeWord(mode);
    if (!word) {
        std::filesystem::remove(configDirectory / "grass", error);
        return !error;
    }
    std::filesystem::create_directories(configDirectory, error);
    std::ofstream file(configDirectory / "grass", std::ios::trunc);
    file << *word << '\n';
    return static_cast<bool>(file);
}

GrassSwitchProfile SelectGrassSwitchProfile(bool fps60) noexcept {
    GrassSwitchProfile profile;
    if (!fps60) {
        profile.DrawDistance = 2000.0F;
        profile.FadeStart = 1500.0F;
        profile.MaxVisibleInstances = 22000U;
    }
    return profile;
}

void ApplyGrassSwitchProfile(const GrassSwitchProfile& profile, InteractiveGrassSettings& settings) noexcept {
    settings.Quality = GrassQuality::Custom;
    settings.MaxInstancesPerRoom = profile.MaxAnchorsPerPlacement;
    settings.DrawDistance = profile.DrawDistance;
    settings.LodReferenceDistance = profile.DrawDistance;
    settings.CullingClusterSize = profile.ClusterSize;
    settings.FarTuftBladeCount = profile.BladesPerTuft;
    settings.MidrangeClustersEnabled = false;
    settings.MidrangeAdaptiveEnabled = false;
    auto& generation = settings.Generation;
    generation.InstancesPerSquareMeter = profile.TuftsPerSquareMeter;
    generation.MinimumSpacing = profile.MinimumSpacing;
    generation.BladeHeightMin = profile.TuftHeightMin;
    generation.BladeHeightMax = std::max(profile.TuftHeightMin, profile.TuftHeightMax);
    generation.BladeWidthMin = profile.BladeWidthMin;
    generation.BladeWidthMax = std::max(profile.BladeWidthMin, profile.BladeWidthMax);
    settings.Appearance.BladeSegments = 1U;
    settings.Appearance.TextureColorInfluence = profile.TextureColorInfluence;
}

size_t GrassRetainedBytesPerAnchor() noexcept {
    return sizeof(GrassWorldAnchor) + sizeof(GrassWorldCullingAnchor);
}

} // namespace Fast::Oot3d
