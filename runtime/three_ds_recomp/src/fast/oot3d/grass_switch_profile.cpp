#include "fast/oot3d/grass_switch_profile.h"

#include "fast/oot3d/grass_world_placement_cache.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <string>

namespace Fast::Oot3d {

namespace {
// Leading word of a config file's text.
std::string_view FirstWord(std::string_view text) noexcept {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    size_t end = 0U;
    while (end < text.size() && !space(text[end])) ++end;
    return text.substr(0U, end);
}
// The menu's choice while the game runs: -1 before any.
std::atomic<int> gPublishedDistance{-1};
} // namespace

GrassSwitchMode ParseGrassSwitchMode(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return GrassSwitchMode::Off;
    const std::string_view word = FirstWord(*fileText);
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

GrassSwitchDistance ParseGrassSwitchDistance(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return GrassSwitchDistance::Near;
    const std::string_view word = FirstWord(*fileText);
    if (word == "far") return GrassSwitchDistance::Far;
    if (word == "area") return GrassSwitchDistance::Area;
    return GrassSwitchDistance::Near;
}

std::string_view GrassSwitchDistanceWord(GrassSwitchDistance distance) noexcept {
    switch (distance) {
    case GrassSwitchDistance::Near: return "near";
    case GrassSwitchDistance::Far: return "far";
    case GrassSwitchDistance::Area: return "area";
    }
    return "near";
}

GrassSwitchDistance ReadGrassSwitchDistance(const std::filesystem::path& configDirectory) {
    std::ifstream file(configDirectory / "grass_distance");
    if (!file) return GrassSwitchDistance::Near;
    std::string text;
    std::getline(file, text);
    return ParseGrassSwitchDistance(std::string_view(text));
}

bool SaveGrassSwitchDistance(const std::filesystem::path& configDirectory, GrassSwitchDistance distance) {
    std::error_code error;
    if (distance == GrassSwitchDistance::Near) {
        std::filesystem::remove(configDirectory / "grass_distance", error);
        return !error;
    }
    std::filesystem::create_directories(configDirectory, error);
    std::ofstream file(configDirectory / "grass_distance", std::ios::trunc);
    file << GrassSwitchDistanceWord(distance) << '\n';
    return static_cast<bool>(file);
}

void PublishGrassSwitchDistance(GrassSwitchDistance distance) noexcept {
    gPublishedDistance.store(static_cast<int>(distance), std::memory_order_relaxed);
}

std::optional<GrassSwitchDistance> PublishedGrassSwitchDistance() noexcept {
    const int value = gPublishedDistance.load(std::memory_order_relaxed);
    if (value < 0) return std::nullopt;
    return static_cast<GrassSwitchDistance>(value);
}

GrassSwitchProfile SelectGrassSwitchProfile(bool fps60, GrassSwitchDistance distance) noexcept {
    GrassSwitchProfile profile;
    switch (distance) {
    case GrassSwitchDistance::Near:
        if (!fps60) {
            profile.FullDensityDistance = 1600.0F;
            profile.DrawDistance = 5000.0F;
            profile.FadeStart = 4200.0F;
            profile.FillDistance = 1600.0F;
            profile.MaxVisibleInstances = 22000U;
        }
        break;
    case GrassSwitchDistance::Far:
        profile.FullDensityDistance = fps60 ? 2000.0F : 3200.0F;
        profile.DrawDistance = fps60 ? 6000.0F : 8000.0F;
        profile.FadeStart = fps60 ? 5000.0F : 6800.0F;
        profile.FillDistance = profile.FullDensityDistance;
        profile.MaxVisibleInstances = fps60 ? 16000U : 40000U;
        break;
    case GrassSwitchDistance::Area:
        profile.FullDensityDistance = kGrassWholeAreaDistance;
        profile.DrawDistance = kGrassWholeAreaDistance;
        profile.FadeStart = kGrassWholeAreaDistance;
        // No full-density band to match: the fill of Near around Link.
        profile.FillDistance = fps60 ? 1000.0F : 1600.0F;
        profile.MaxVisibleInstances = kGrassWholeAreaInstances;
        break;
    }
    return profile;
}

uint32_t GrassSwitchStreamCapacity() noexcept {
    return kGrassWholeAreaInstances;
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
