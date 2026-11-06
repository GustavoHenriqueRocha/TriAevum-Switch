#pragma once

// Optional lighting of the deko3d backend (Switch), off by default
// (config/lighting, L3+R3 > Video > "Lighting"):
//
// - Toon: upstream's Toon "PICA material" banding (pica_toon_shader.cpp,
//   default ToonStyleSettings, constants baked in, no extra uniform block) as
//   a text mutation of each PICA fragment shader. Fragment stage only.
// - PerPixel: a trimmed copy of upstream's per-pixel surface lighting
//   (pica_surface_lighting.cpp): native lights, ambient, the sun (scene and
//   environment directionals), at most four scene point lights and the baked
//   surface branch; no placed lights, water, rim, GI or shadows. The fragment
//   reads upstream's 2992-byte PicaSurfaceLighting block, bound at
//   kSurfaceLightingBinding instead of upstream's 16 (deko3d has 0-15); the
//   vertex stage gets upstream's surface normal and tint outputs
//   (BuildPicaVertexSurfaceShader, locations 9 and 10).
//
// Nothing is compiled on the console: a variant is the deterministic mutation
// of the GLSL 4.3 text the native draw would use, looked up by its own
// ShaderTextKey in deko3d.pack. The pack tool applies the same fragment
// mutation offline to every known text (oot3d_deko3d_lighting_variants,
// tools/oot3d/deko3d_shader_pack.py --lighting-variants). A variant missing
// from the pack is recorded like any missing stage (logs/deko3d_shaders/) and
// that draw keeps its native shaders. With the mode off nothing here runs and
// the native keys are untouched.
//
// Pure code, tested on the host (oot3d_deko3d_lighting_tests).

#include "fast/backends/gfx_deko3d_shader_pack.h"
#include "fast/renderer3ds/pica_shader_hooks.h"
#include "fast/renderer3ds/pica_surface_lighting.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Fast::Deko3d {

enum class LightingMode : uint8_t {
    Off = 0,
    Toon = 1,
    PerPixel = 2,
};

// sdmc:/switch/oot3dre/config/lighting: first word `toon` or `perpixel`;
// absent, empty or anything else = off.
inline constexpr const char* kLightingConfigName = "lighting";

[[nodiscard]] inline LightingMode ParseLightingMode(std::string_view text) noexcept {
    const size_t end = text.find_first_of("\r\n");
    if (end != std::string_view::npos) {
        text = text.substr(0, end);
    }
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1U);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1U);
    std::string word(text);
    std::transform(word.begin(), word.end(), word.begin(),
                   [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    if (word == "toon") return LightingMode::Toon;
    if (word == "perpixel" || word == "per-pixel" || word == "per_pixel") return LightingMode::PerPixel;
    return LightingMode::Off;
}

[[nodiscard]] constexpr const char* LightingModeName(LightingMode mode) noexcept {
    switch (mode) {
        case LightingMode::Toon: return "toon";
        case LightingMode::PerPixel: return "perpixel";
        case LightingMode::Off: break;
    }
    return "off";
}

[[nodiscard]] inline LightingMode LoadLightingMode(const std::filesystem::path& config) {
    std::ifstream file(config / kLightingConfigName);
    if (!file) return LightingMode::Off;
    std::string line;
    std::getline(file, line);
    return ParseLightingMode(line);
}

// The file exists only while a mode is on. Empty on success, otherwise the
// file name that failed.
[[nodiscard]] inline std::string SaveLightingMode(const std::filesystem::path& config, LightingMode mode) {
    std::error_code error;
    if (mode == LightingMode::Off) {
        std::filesystem::remove(config / kLightingConfigName, error);
        return error ? kLightingConfigName : "";
    }
    std::filesystem::create_directories(config, error);
    std::ofstream file(config / kLightingConfigName, std::ios::trunc);
    file << LightingModeName(mode) << '\n';
    return file ? "" : kLightingConfigName;
}

// Published by the Video tab (and by the renderer's start-up read of the
// file); the renderer applies a new revision at the start of its next frame.
struct LightingLiveSettings {
    std::atomic<uint8_t> Mode{static_cast<uint8_t>(LightingMode::Off)};
    std::atomic<uint64_t> Revision{0U};

    void Publish(LightingMode mode) noexcept {
        Mode.store(static_cast<uint8_t>(mode), std::memory_order_relaxed);
        Revision.fetch_add(1U, std::memory_order_release);
    }
    [[nodiscard]] LightingMode Current() const noexcept {
        return static_cast<LightingMode>(Mode.load(std::memory_order_relaxed));
    }
};

inline LightingLiveSettings& LiveLightingSettings() noexcept {
    static LightingLiveSettings settings;
    return settings;
}

// Fragment uniform slots in use: 4 (PICA fragment uniforms). Upstream binds
// PicaSurfaceLighting at 16, past deko3d's 16 slots per stage.
inline constexpr uint32_t kUpstreamSurfaceLightingBinding = 16U;
inline constexpr uint32_t kSurfaceLightingBinding = 5U;
inline constexpr uint32_t kSurfaceLightingBytes = sizeof(Renderer3ds::PicaSurfaceLightingUniforms);
static_assert(kSurfaceLightingBytes == 2992U);
// Scene point lights the trimmed shader reads (upstream: 32).
inline constexpr uint32_t kMaximumSurfacePointLights = 4U;

// Markers the mutations leave in a variant (and test for: a text that already
// has one is its own variant).
inline constexpr std::string_view kToonVariantMarker = "oot3d_toon_parameters()";
inline constexpr std::string_view kPerPixelVariantMarker = "// OOT3D_DEKO3D_PER_PIXEL";

// The GLSL the per-pixel fragment variant inserts before main().
[[nodiscard]] std::string_view PerPixelFragmentDeclarations() noexcept;
// The lines it inserts after the lighting extension point.
[[nodiscard]] std::string_view PerPixelFragmentBody() noexcept;

// The variant of a GLSL 4.3 PICA fragment text, or nothing when the text has
// no place for it (and for LightingMode::Off). A text that already is this
// mode's variant is returned unchanged (idempotent).
[[nodiscard]] std::optional<std::string> ToonFragmentVariant(std::string_view text);
[[nodiscard]] std::optional<std::string> PerPixelFragmentVariant(std::string_view text);
[[nodiscard]] std::optional<std::string> LightingFragmentVariant(LightingMode mode, std::string_view text);

// The per-pixel vertex variant of a canonical (pre-translation) PICA vertex
// text: the surface normal and tint outputs at the hooks' main-body end. The
// caller translates it to GLSL 4.3 like the native text.
[[nodiscard]] std::optional<std::string> PerPixelVertexVariant(std::string_view canonical,
                                                               const Renderer3ds::PicaVertexShaderHookLayout& hooks);

// The trimmed shader reads at most kMaximumSurfacePointLights scene points;
// the block is cut to match (the selector's order: the first ones win).
inline void TrimSurfaceLighting(Renderer3ds::PicaSurfaceLightingUniforms& uniforms) noexcept {
    uniforms.Control[1] = std::min(uniforms.Control[1], kMaximumSurfacePointLights);
    for (uint32_t slot = uniforms.Control[1]; slot < uniforms.ScenePoints.size(); ++slot) {
        uniforms.ScenePoints[slot] = {};
    }
    uniforms.Control[3] &= ~0xFF00U; // placed lights: none
    uniforms.Rim = uniforms.RimColor = uniforms.RimDirection = {};
    uniforms.GlobalIlluminationSources = {};
    uniforms.PointShadow = uniforms.PointShadowAtlas = {};
    uniforms.WaterViewTranslation = {};
}

// Variants resolved (once per distinct shader) and draws drawn with them;
// logs/deko3d.txt "lighting" lines.
struct LightingCounters {
    uint64_t VariantsFound = 0;       // in the pack
    uint64_t VariantsMissing = 0;     // not in the pack: recorded, native shaders used
    uint64_t VariantsUnsupported = 0; // the text has no place for the mutation
    uint64_t DrawsLit = 0;            // drawn with the variant
    uint64_t DrawsNative = 0;         // eligible, drawn native (missing/unsupported variant)
    uint64_t DrawsIneligible = 0;     // not a draw the mode changes
};

// One variant stage, looked up by the key of its own text. `lookup(key,
// text)` returns the shader or nullptr (the store then records the text as
// missing, like any stage the pack lacks). Unsupported texts are never
// looked up. Runs once per distinct native stage (the caller caches it).
template <typename T, typename Lookup>
const T* ResolveLightingVariant(ShaderStage stage, const std::optional<std::string>& variant, Lookup&& lookup,
                                LightingCounters& counters) {
    if (!variant) {
        ++counters.VariantsUnsupported;
        return nullptr;
    }
    const T* shader = lookup(ShaderTextKey(stage, *variant), std::string_view(*variant));
    ++(shader != nullptr ? counters.VariantsFound : counters.VariantsMissing);
    return shader;
}

// The shaders of a draw the mode applies to: the variants when every one it
// needs is there (toon has no vertex variant: pass the native vertex), else
// both native shaders. Never a mixed pair: the per-pixel fragment reads
// outputs only its vertex variant writes.
template <typename T> struct LightingProgramChoice {
    const T* Vertex = nullptr;
    const T* Fragment = nullptr;
    bool Lit = false;
};

template <typename T>
[[nodiscard]] LightingProgramChoice<T> ChooseLightingProgram(const T* nativeVertex, const T* nativeFragment,
                                                             const T* variantVertex, const T* variantFragment,
                                                             LightingCounters& counters) noexcept {
    if (variantVertex != nullptr && variantFragment != nullptr) {
        ++counters.DrawsLit;
        return {variantVertex, variantFragment, true};
    }
    ++counters.DrawsNative;
    return {nativeVertex, nativeFragment, false};
}

} // namespace Fast::Deko3d
