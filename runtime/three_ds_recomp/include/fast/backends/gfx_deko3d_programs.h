#pragma once

// Program choice of the deko3d backend for a PICA draw (package 3,
// docs/DEKO3D_PROGRESS.md). Pure code, tested on the host
// (oot3d_deko3d_backend_tests).
//
// Each stage is resolved once per canonical source identity: the canonical
// text is translated to GLSL 4.3 (the text the GL backend gives Mesa), keyed
// with ShaderTextKey and looked up in shadercache/deko3d.pack. A miss is
// remembered too, so a draw never translates or hashes a text twice. The draw
// then uses the specialized fragment program when the pack has it, else the
// parametric (uber) program the draw carries (hybrid preparation), else it is
// skipped and counted. Nothing is ever compiled on the render thread.

#include "fast/renderer3ds/pica_shader_source_identity.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>

namespace Fast::Deko3d {

enum class PicaProgramPick : uint8_t {
    Specialized, // vertex + specialized fragment from the pack
    Uber,        // vertex + the draw's parametric fragment program
    NoVertex,    // the pack lacks the vertex program: skip
    NoFragment,  // neither fragment program is available: skip
};

[[nodiscard]] constexpr PicaProgramPick PickPicaProgram(bool vertex, bool specializedFragment,
                                                        bool uberFragment) noexcept {
    if (!vertex) {
        return PicaProgramPick::NoVertex;
    }
    if (specializedFragment) {
        return PicaProgramPick::Specialized;
    }
    return uberFragment ? PicaProgramPick::Uber : PicaProgramPick::NoFragment;
}

struct PicaProgramPickCounters {
    uint64_t Specialized = 0;
    uint64_t Uber = 0;
    uint64_t NoVertex = 0;
    uint64_t NoFragment = 0;

    void Count(PicaProgramPick pick) noexcept {
        switch (pick) {
            case PicaProgramPick::Specialized: ++Specialized; break;
            case PicaProgramPick::Uber: ++Uber; break;
            case PicaProgramPick::NoVertex: ++NoVertex; break;
            case PicaProgramPick::NoFragment: ++NoFragment; break;
        }
    }
};

// What a draw's base (native) fragment program costs, for the per-frame
// breakdown of logs/deko3d.txt: a program that spills (per-warp scratch,
// read from its DKSH), else the parametric (uber) program, else a
// specialized one. The parametric programs are the heavy native ones.
enum class PicaBaseClass : uint8_t {
    Spilling = 0,
    Parametric = 1,
    Normal = 2,
};
inline constexpr size_t kPicaBaseClasses = 3U;

[[nodiscard]] constexpr PicaBaseClass ClassifyPicaBase(bool uberFragment, uint32_t scratchBytes) noexcept {
    if (scratchBytes != 0U) {
        return PicaBaseClass::Spilling;
    }
    return uberFragment ? PicaBaseClass::Parametric : PicaBaseClass::Normal;
}

// Whether a draw's fragment program writes gl_FragDepth (which turns early
// depth testing off and, with ZCULL on, is tested by ZCULL at its raster
// depth: deko3d never sets ZCULL_TEST_MASK the way Mesa does for such
// programs). Mirrors Oot3dPicaFragmentDepthIsRasterDepth: a specialized
// program leaves depth to fixed function only for z-buffering with scale -1
// and offset 0; parametric (uber) programs always write it.
[[nodiscard]] constexpr bool PicaDrawWritesFragDepth(bool uber, bool wBuffering, float depthRange,
                                                     float nearPlane) noexcept {
    return uber || wBuffering || depthRange != -1.0F || nearPlane != 0.0F;
}

// The kind of a frame's first depth-writing draw (how the game resets depth:
// it never clears or fills it, logs15).
enum class PicaFirstDepthWrite : uint8_t {
    None,          // no draw wrote depth
    AlwaysFull,    // ALWAYS (or test off) over the whole target: a depth clear in disguise
    AlwaysPartial, // ALWAYS over part of the target
    Tested,        // a real depth test
};
inline constexpr size_t kPicaFirstDepthWriteKinds = 4U;

// Counters of one frame's issued PICA draws (counters only: a few adds per
// draw). Pixels: the draw's viewport clipped by its scissor, in target
// pixels (an upper bound of what it shades).
struct PicaFrameDraws {
    struct Class {
        uint64_t Draws = 0;
        uint64_t Lit = 0;          // drawn with a lighting variant (toon or per-pixel)
        uint64_t Pixels = 0;
        uint64_t LitPixels = 0;
    };
    std::array<Class, kPicaBaseClasses> Classes{};
    uint64_t LitOpaque = 0;        // lit draws without blending
    uint64_t LitTranslucent = 0;   // lit draws with blending
    // Per-pixel lit draws by the scene point lights their block carries
    // (0-4, kMaximumSurfacePointLights) and those with the rim light on, and
    // by the fragment variant's path (Deko3d::PerPixelPath: generic,
    // environment, baked) with their pixels.
    std::array<uint64_t, 5> LitPointLights{};
    uint64_t LitRim = 0;
    static constexpr size_t kSurfacePaths = 3U;
    std::array<uint64_t, kSurfacePaths> LitPaths{};
    std::array<uint64_t, kSurfacePaths> LitPathPixels{};
    // Depth (config/deko3d_zcull study): depth-writing draws that pass
    // every fragment (ALWAYS or test off), those of them covering the whole
    // target, and depth-tested draws whose program writes gl_FragDepth.
    uint64_t DepthAlwaysWrites = 0;
    uint64_t DepthAlwaysFull = 0;
    uint64_t FragDepthTested = 0;
    // Frames by the kind of their first depth-writing draw (EndFrame counts
    // None for a frame without one).
    std::array<uint64_t, kPicaFirstDepthWriteKinds> FirstDepthWrite{};
    bool FirstDepthSeen = false;

    void CountDepth(bool testEnabled, bool writes, bool always, bool fullTarget, bool fragDepth) noexcept {
        if (writes && always) {
            ++DepthAlwaysWrites;
            DepthAlwaysFull += fullTarget ? 1U : 0U;
        }
        if (testEnabled && !always && fragDepth) {
            ++FragDepthTested;
        }
        if (writes && !FirstDepthSeen) {
            FirstDepthSeen = true;
            const auto kind = !always ? PicaFirstDepthWrite::Tested
                                      : (fullTarget ? PicaFirstDepthWrite::AlwaysFull : PicaFirstDepthWrite::AlwaysPartial);
            ++FirstDepthWrite[static_cast<size_t>(kind)];
        }
    }
    void Count(PicaBaseClass base, bool lit, bool translucent, uint64_t pixels) noexcept {
        auto& slot = Classes[static_cast<size_t>(base)];
        ++slot.Draws;
        slot.Pixels += pixels;
        if (lit) {
            ++slot.Lit;
            slot.LitPixels += pixels;
            ++(translucent ? LitTranslucent : LitOpaque);
        }
    }
    void CountSurfaceLighting(uint32_t pointLights, bool rim, size_t path, uint64_t pixels) noexcept {
        ++LitPointLights[std::min<size_t>(pointLights, LitPointLights.size() - 1U)];
        LitRim += rim ? 1U : 0U;
        path = std::min(path, kSurfacePaths - 1U);
        ++LitPaths[path];
        LitPathPixels[path] += pixels;
    }
    PicaFrameDraws& operator+=(const PicaFrameDraws& other) noexcept {
        for (size_t i = 0; i < Classes.size(); ++i) {
            Classes[i].Draws += other.Classes[i].Draws;
            Classes[i].Lit += other.Classes[i].Lit;
            Classes[i].Pixels += other.Classes[i].Pixels;
            Classes[i].LitPixels += other.Classes[i].LitPixels;
        }
        LitOpaque += other.LitOpaque;
        LitTranslucent += other.LitTranslucent;
        for (size_t i = 0; i < LitPointLights.size(); ++i) {
            LitPointLights[i] += other.LitPointLights[i];
        }
        LitRim += other.LitRim;
        for (size_t i = 0; i < kSurfacePaths; ++i) {
            LitPaths[i] += other.LitPaths[i];
            LitPathPixels[i] += other.LitPathPixels[i];
        }
        DepthAlwaysWrites += other.DepthAlwaysWrites;
        DepthAlwaysFull += other.DepthAlwaysFull;
        FragDepthTested += other.FragDepthTested;
        for (size_t i = 0; i < FirstDepthWrite.size(); ++i) {
            FirstDepthWrite[i] += other.FirstDepthWrite[i];
        }
        return *this;
    }
};

// The frame being counted, the last complete frame, and the sum over the
// frames since the last log line (the line prints the last frame and the
// window's average per frame).
struct PicaFrameDrawWindow {
    PicaFrameDraws Current;
    PicaFrameDraws Last;
    PicaFrameDraws Sum;
    uint64_t Frames = 0;

    void EndFrame() noexcept {
        if (!Current.FirstDepthSeen) {
            ++Current.FirstDepthWrite[static_cast<size_t>(PicaFirstDepthWrite::None)];
        }
        Last = Current;
        Sum += Current;
        ++Frames;
        Current = {};
    }
    void StartWindow() noexcept {
        Sum = {};
        Frames = 0;
    }
};

// "spill=D/L px=M/M parametric=... normal=..." for a frame (frames = 1) or a
// window's average (frames > 1): draws/lit draws and pixels/lit pixels in
// millions per frame.
[[nodiscard]] inline std::string FormatPicaClassDraws(const PicaFrameDraws& draws, uint64_t frames) {
    static constexpr const char* kNames[kPicaBaseClasses] = {"spill", "parametric", "normal"};
    const double divisor = frames == 0U ? 1.0 : static_cast<double>(frames);
    std::string text;
    char part[128];
    for (size_t i = 0; i < kPicaBaseClasses; ++i) {
        const auto& c = draws.Classes[i];
        std::snprintf(part, sizeof(part), "%s%s=%.0f/%.0f px=%.2f/%.2fM", i == 0U ? "" : " ", kNames[i],
                      static_cast<double>(c.Draws) / divisor, static_cast<double>(c.Lit) / divisor,
                      static_cast<double>(c.Pixels) / divisor * 1.0e-6,
                      static_cast<double>(c.LitPixels) / divisor * 1.0e-6);
        text += part;
    }
    return text;
}

// "opaque=N translucent=N points 0/1/2/3/4=a/b/c/d/e rim=N paths g/e/b=a/b/c px=a/b/cM" of the lit
// draws (paths: per-pixel variants generic/environment/baked, draws and viewport-in-scissor pixels).
[[nodiscard]] inline std::string FormatPicaLitDraws(const PicaFrameDraws& draws, uint64_t frames) {
    const double divisor = frames == 0U ? 1.0 : static_cast<double>(frames);
    const auto per = [&](uint64_t value) { return static_cast<double>(value) / divisor; };
    const auto mpx = [&](uint64_t value) { return static_cast<double>(value) / divisor * 1.0e-6; };
    char text[288];
    std::snprintf(text, sizeof(text),
                  "opaque=%.0f translucent=%.0f points 0/1/2/3/4=%.0f/%.0f/%.0f/%.0f/%.0f rim=%.0f "
                  "paths g/e/b=%.0f/%.0f/%.0f px=%.2f/%.2f/%.2fM",
                  per(draws.LitOpaque), per(draws.LitTranslucent), per(draws.LitPointLights[0]),
                  per(draws.LitPointLights[1]), per(draws.LitPointLights[2]), per(draws.LitPointLights[3]),
                  per(draws.LitPointLights[4]), per(draws.LitRim), per(draws.LitPaths[0]), per(draws.LitPaths[1]),
                  per(draws.LitPaths[2]), mpx(draws.LitPathPixels[0]), mpx(draws.LitPathPixels[1]),
                  mpx(draws.LitPathPixels[2]));
    return text;
}

// "always=N full=N fragdepth=N first none/full/partial/tested=a/b/c/d": the
// depth counters per frame; `first` counts frames (fractions in a window).
[[nodiscard]] inline std::string FormatPicaDepthDraws(const PicaFrameDraws& draws, uint64_t frames) {
    const double divisor = frames == 0U ? 1.0 : static_cast<double>(frames);
    const auto per = [&](uint64_t value) { return static_cast<double>(value) / divisor; };
    char text[160];
    std::snprintf(text, sizeof(text), "always=%.0f full=%.0f fragdepth=%.0f first none/full/partial/tested=%.2f/%.2f/%.2f/%.2f",
                  per(draws.DepthAlwaysWrites), per(draws.DepthAlwaysFull), per(draws.FragDepthTested),
                  per(draws.FirstDepthWrite[0]), per(draws.FirstDepthWrite[1]), per(draws.FirstDepthWrite[2]),
                  per(draws.FirstDepthWrite[3]));
    return text;
}

struct PicaShaderIdentityHash {
    size_t operator()(const Renderer3ds::PicaShaderSourceIdentity& identity) const noexcept {
        return static_cast<size_t>(identity.Id ^ (identity.SecondaryHash * 0x9E3779B97F4A7C15ULL) ^ identity.Size);
    }
};

// Stage resolutions by canonical source identity. T is the shader object
// (DkShader on the console); nullptr results are cached as misses.
template <typename T> class PicaStageCache {
  public:
    // resolve() runs only the first time an identity is seen.
    template <typename Resolve>
    const T* Get(const Renderer3ds::PicaShaderSourceIdentity& identity, Resolve&& resolve) {
        if (const auto found = mStages.find(identity); found != mStages.end()) {
            return found->second;
        }
        ++mResolutions;
        const T* shader = resolve();
        mStages.emplace(identity, shader);
        return shader;
    }

    void Clear() noexcept {
        mStages.clear();
    }

    [[nodiscard]] size_t Size() const noexcept {
        return mStages.size();
    }
    [[nodiscard]] uint64_t Resolutions() const noexcept {
        return mResolutions;
    }

  private:
    std::unordered_map<Renderer3ds::PicaShaderSourceIdentity, const T*, PicaShaderIdentityHash> mStages;
    uint64_t mResolutions = 0;
};

} // namespace Fast::Deko3d
