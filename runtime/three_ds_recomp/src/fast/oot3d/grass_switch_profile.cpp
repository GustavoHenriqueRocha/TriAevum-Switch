#include "fast/oot3d/grass_switch_profile.h"

#include "fast/oot3d/grass_world_placement_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <charconv>
#include <string>

#include "fast/oot3d/grass_scene_hold.h"

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
// The menu's grass_lod choice while the game runs: -1 before any.
std::atomic<int> gPublishedLod{-1};
// The menu's grass_fill choice while the game runs: -1 before any.
std::atomic<int> gPublishedFill{-1};

// A file that is on when absent or holding any word but "0", "off", "false".
bool ParseOnUnlessOff(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return true;
    const std::string_view word = FirstWord(*fileText);
    return !(word == "0" || word == "off" || word == "false");
}

bool ReadOnUnlessOff(const std::filesystem::path& file) {
    std::ifstream stream(file);
    if (!stream) return true;
    std::string text;
    std::getline(stream, text);
    return ParseOnUnlessOff(std::string_view(text));
}

// Removes the file for on, writes "0" for off.
bool SaveOnUnlessOff(const std::filesystem::path& configDirectory, std::string_view name, bool on) {
    std::error_code error;
    if (on) {
        std::filesystem::remove(configDirectory / name, error);
        return !error;
    }
    std::filesystem::create_directories(configDirectory, error);
    std::ofstream file(configDirectory / name, std::ios::trunc);
    file << "0\n";
    return static_cast<bool>(file);
}
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

GrassSwitchDistance ParseGrassSwitchDistance(std::optional<std::string_view> fileText, bool* retiredFar) noexcept {
    if (retiredFar != nullptr) *retiredFar = false;
    if (!fileText.has_value()) return GrassSwitchDistance::Near;
    const std::string_view word = FirstWord(*fileText);
    if (word == "area") return GrassSwitchDistance::Area;
    if (word == "far") {
        // Far was removed after test.15: grass far away, as Whole area draws it.
        if (retiredFar != nullptr) *retiredFar = true;
        return GrassSwitchDistance::Area;
    }
    return GrassSwitchDistance::Near;
}

std::string_view GrassSwitchDistanceWord(GrassSwitchDistance distance) noexcept {
    switch (distance) {
    case GrassSwitchDistance::Near: return "near";
    case GrassSwitchDistance::Area: return "area";
    }
    return "near";
}

GrassSwitchDistance ReadGrassSwitchDistance(const std::filesystem::path& configDirectory, bool* retiredFar) {
    if (retiredFar != nullptr) *retiredFar = false;
    std::ifstream file(configDirectory / "grass_distance");
    if (!file) return GrassSwitchDistance::Near;
    std::string text;
    std::getline(file, text);
    return ParseGrassSwitchDistance(std::string_view(text), retiredFar);
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

bool ParseGrassSwitchLod(std::optional<std::string_view> fileText) noexcept {
    return ParseOnUnlessOff(fileText);
}

bool ReadGrassSwitchLod(const std::filesystem::path& configDirectory) {
    return ReadOnUnlessOff(configDirectory / "grass_lod");
}

bool SaveGrassSwitchLod(const std::filesystem::path& configDirectory, bool on) {
    return SaveOnUnlessOff(configDirectory, "grass_lod", on);
}

void PublishGrassSwitchLod(bool on) noexcept {
    gPublishedLod.store(on ? 1 : 0, std::memory_order_relaxed);
}

std::optional<bool> PublishedGrassSwitchLod() noexcept {
    const int value = gPublishedLod.load(std::memory_order_relaxed);
    if (value < 0) return std::nullopt;
    return value == 1;
}

bool ParseGrassSwitchFill(std::optional<std::string_view> fileText) noexcept {
    return ParseOnUnlessOff(fileText);
}

bool ReadGrassSwitchFill(const std::filesystem::path& configDirectory) {
    return ReadOnUnlessOff(configDirectory / "grass_fill");
}

bool ParseGrassPlacementCache(std::optional<std::string_view> fileText) noexcept {
    return ParseOnUnlessOff(fileText);
}

bool ReadGrassPlacementCache(const std::filesystem::path& configDirectory) {
    return ReadOnUnlessOff(configDirectory / "grass_placement_cache");
}

bool ParseGrassPlacementReuse(std::optional<std::string_view> fileText) noexcept {
    return ParseOnUnlessOff(fileText);
}

bool ReadGrassPlacementReuse(const std::filesystem::path& configDirectory) {
    return ReadOnUnlessOff(configDirectory / "grass_placement_reuse");
}

uint32_t ParseGrassSceneHoldMs(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return kGrassSceneHoldDefaultMs;
    const std::string_view word = FirstWord(*fileText);
    if (word == "off" || word == "false") return 0U;
    uint32_t value = 0U;
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (word.empty() || error != std::errc{} || end != word.data() + word.size()) return kGrassSceneHoldDefaultMs;
    return std::min(value, kGrassSceneHoldLimitMs);
}

uint32_t ReadGrassSceneHoldMs(const std::filesystem::path& configDirectory) {
    std::ifstream file(configDirectory / "grass_hold");
    if (!file) return kGrassSceneHoldDefaultMs;
    std::string text;
    std::getline(file, text);
    return ParseGrassSceneHoldMs(std::string_view(text));
}

float ParseGrassSwitchDensity(std::optional<std::string_view> fileText) noexcept {
    if (!fileText.has_value()) return kGrassDensityDefault;
    const std::string_view word = FirstWord(*fileText);
    uint32_t value = 0U;
    const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), value);
    if (word.empty() || error != std::errc{} || end != word.data() + word.size()) return kGrassDensityDefault;
    return std::clamp(static_cast<float>(value), kGrassDensityMin, kGrassDensityMax);
}

float ReadGrassSwitchDensity(const std::filesystem::path& configDirectory) {
    std::ifstream file(configDirectory / "grass_density");
    if (!file) return kGrassDensityDefault;
    std::string text;
    std::getline(file, text);
    return ParseGrassSwitchDensity(std::string_view(text));
}

void ApplyGrassSwitchDensity(GrassSwitchProfile& profile, float tuftsPerSquareMeter) noexcept {
    const float ratio = tuftsPerSquareMeter / profile.TuftsPerSquareMeter;
    profile.TuftsPerSquareMeter = tuftsPerSquareMeter;
    profile.MinimumSpacing /= std::sqrt(ratio);
    profile.MaxVisibleInstances = static_cast<uint32_t>(std::min(
        static_cast<float>(kGrassWholeAreaInstances), std::round(static_cast<float>(profile.MaxVisibleInstances) * ratio)));
}

bool SaveGrassSwitchFill(const std::filesystem::path& configDirectory, bool fixed) {
    return SaveOnUnlessOff(configDirectory, "grass_fill", fixed);
}

void PublishGrassSwitchFill(bool fixed) noexcept {
    gPublishedFill.store(fixed ? 1 : 0, std::memory_order_relaxed);
}

std::optional<bool> PublishedGrassSwitchFill() noexcept {
    const int value = gPublishedFill.load(std::memory_order_relaxed);
    if (value < 0) return std::nullopt;
    return value == 1;
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
}

size_t GrassRetainedBytesPerAnchor() noexcept {
    return sizeof(GrassWorldAnchor) + sizeof(GrassWorldCullingAnchor);
}

} // namespace Fast::Oot3d
