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
#include "fast/oot3d/graphics_settings.h"
#include "fast/renderer3ds/pica_shader_hooks.h"
#include "fast/renderer3ds/pica_surface_lighting.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace Fast::Renderer3ds {
struct PicaDrawView;
}

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

// The lighting model lives in the graphics settings (one source of truth,
// the same the scene environment tab reads): per-pixel is the Modern model
// (Effects.SurfaceLighting PerPixel, validated to PerPixelPbr), toon is
// Effects.Toon PicaMaterial. config/lighting persists it on the Switch: the
// renderer applies the file at start, the Video tab writes both.
[[nodiscard]] inline LightingMode LightingModeOf(const Oot3d::GraphicsSettings& settings) noexcept {
    if (Oot3d::UsesFullSceneLighting(settings.Effects.SurfaceLighting)) return LightingMode::PerPixel;
    if (settings.Effects.Toon == Oot3d::ToonMode::PicaMaterial) return LightingMode::Toon;
    return LightingMode::Off;
}

// `settings` with the model of `mode`. A mode needs a non-Authentic preset
// (Authentic is native presentation by definition): Custom then.
[[nodiscard]] inline Oot3d::GraphicsSettings WithLightingMode(Oot3d::GraphicsSettings settings, LightingMode mode) {
    settings.Effects.SurfaceLighting =
        mode == LightingMode::PerPixel ? Oot3d::SurfaceLightingMode::PerPixel : Oot3d::SurfaceLightingMode::Native;
    settings.Effects.Toon = mode == LightingMode::Toon ? Oot3d::ToonMode::PicaMaterial : Oot3d::ToonMode::Off;
    if (mode != LightingMode::Off && settings.Preset == Oot3d::GraphicsPreset::Authentic)
        settings.Preset = Oot3d::GraphicsPreset::Custom;
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
// The lines it puts before and after the native fog line (when the text has
// one): the environment's fog contribution.
[[nodiscard]] std::string_view PerPixelFragmentFogBefore() noexcept;
[[nodiscard]] std::string_view PerPixelFragmentFogAfter() noexcept;

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
// the block is cut to match (the selector's order: the first ones win). The
// rim (AssignPicaSurfaceRim) is kept; placed lights, GI, shadows and water go.
inline void TrimSurfaceLighting(Renderer3ds::PicaSurfaceLightingUniforms& uniforms) noexcept {
    uniforms.Control[1] = std::min(uniforms.Control[1], kMaximumSurfacePointLights);
    for (uint32_t slot = uniforms.Control[1]; slot < uniforms.ScenePoints.size(); ++slot) {
        uniforms.ScenePoints[slot] = {};
    }
    uniforms.Control[3] &= ~0xFF00U; // placed lights: none
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
    // Kept native for cost (also counted as ineligible): the base fragment
    // program spills (its variants would too), and per-pixel on a draw that
    // writes no depth (shaded again under what covers it).
    uint64_t DrawsSpillingBase = 0;
    uint64_t DrawsNoDepthWrite = 0;
};

// Why a draw keeps its native shading under per-pixel lighting (the first
// rule it fails, in the order the backend checks them).
enum class PerPixelRefusal : uint8_t {
    None = 0,
    NotSceneWorld,      // not a scene draw of a layer that takes surface lighting
    NoSurfaceProgram,   // its vertex program has no typed surface/transform hooks
    NoDepthWrite,       // PerPixelCostAdmits
    FragmentOperation,  // not the ordinary colour/depth fragment operation
    NoUniforms,         // its packed vertex uniforms are too short
    NotPerspective,     // UsesPicaPerspectiveProjection refused its projection
    NotRelightable,     // authored appearance (unlit, additive)
    NativeLights,       // its native light uniforms could not be read
    NoCamera,           // the block needs the world-to-view and has none (or a singular one)
    Block,              // BuildPicaSurfaceLightingUniforms refused it otherwise
    Count,
};

[[nodiscard]] const char* PerPixelRefusalName(PerPixelRefusal refusal) noexcept;

// Per-pixel draws of one frame kind: real (step and native) frames and
// fps60 in-between frames (PublishPicaFrameTemporalSample's Synthetic), so
// logs/deko3d.txt shows whether the in-between frames light like the real ones.
struct FrameKindLighting {
    uint64_t Samples = 0; // temporal samples published (frames of this kind)
    uint64_t Lit = 0;
    uint64_t Native = 0;
    uint64_t Ineligible = 0;
    std::array<uint64_t, static_cast<size_t>(PerPixelRefusal::Count)> Refusals{};
    PerPixelRefusal FirstRefusal = PerPixelRefusal::None;
    // The block of the first lit draw (what the variant's body branches on):
    // control (native lit, points, directionals, flags), environment
    // enabled (w), rim intensity, baked reference (w).
    struct BlockSummary {
        std::array<uint32_t, 4> Control{};
        float Environment = 0.0F;
        float Rim = 0.0F;
        float Baked = 0.0F;
    };
    std::optional<BlockSummary> FirstLit;

    void Refuse(PerPixelRefusal refusal) noexcept {
        ++Ineligible;
        ++Refusals[static_cast<size_t>(refusal)];
        if (FirstRefusal == PerPixelRefusal::None) FirstRefusal = refusal;
    }
    void Light(const Renderer3ds::PicaSurfaceLightingUniforms& block) noexcept {
        ++Lit;
        if (!FirstLit)
            FirstLit = BlockSummary{block.Control, block.EnvironmentParameters[3], block.Rim[0],
                                    block.BakedReferenceUp[3]};
    }
};

// "frames=F lit=L native=N ineligible=I first=<reason> (reason=count ...)
// block control=a/b/c/d env=E rim=R baked=B" (the first lit draw's block).
[[nodiscard]] std::string FormatFrameKindLighting(const FrameKindLighting& kind);

// The per-pixel admission of a draw before its variants are resolved
// (upstream's surfaceCandidate: scene domain, world layer, typed surface
// program; then PerPixelCostAdmits).
[[nodiscard]] PerPixelRefusal AdmitPerPixelDraw(const Renderer3ds::PicaDrawView& draw) noexcept;

// The surface block of an admitted draw (upstream's per-pixel admission and
// block, trimmed, with the rim on the draws PicaDrawReceivesRimLight admits),
// or why it keeps native shading. `view` caches the camera of the last draw
// (rebuilt when the draw's world-to-view differs).
[[nodiscard]] PerPixelRefusal BuildPerPixelSurface(const Renderer3ds::PicaDrawView& draw,
                                                   const Renderer3ds::PicaEnvironmentLighting& environment,
                                                   std::optional<Renderer3ds::PicaSurfaceLightingView>& view,
                                                   Renderer3ds::PicaSurfaceLightingUniforms& block);

// Per-pixel shading only for draws that write depth: a depth-less draw
// (translucency, effects) is shaded again under what covers it. The blend
// flag alone does not decide: the game enables blending on most solid world
// geometry, and excluding it left per-pixel lighting 0 draws (logs14: 81% of
// the draws were blended).
[[nodiscard]] constexpr bool PerPixelCostAdmits(bool depthWrite) noexcept {
    return depthWrite;
}

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
