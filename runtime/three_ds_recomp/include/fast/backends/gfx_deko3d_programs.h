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
    // (0-4, kMaximumSurfacePointLights) and those with the rim light on.
    std::array<uint64_t, 5> LitPointLights{};
    uint64_t LitRim = 0;

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
    void CountSurfaceLighting(uint32_t pointLights, bool rim) noexcept {
        ++LitPointLights[std::min<size_t>(pointLights, LitPointLights.size() - 1U)];
        LitRim += rim ? 1U : 0U;
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

// "opaque=N translucent=N points 0/1/2/3/4=a/b/c/d/e rim=N" of the lit draws.
[[nodiscard]] inline std::string FormatPicaLitDraws(const PicaFrameDraws& draws, uint64_t frames) {
    const double divisor = frames == 0U ? 1.0 : static_cast<double>(frames);
    const auto per = [&](uint64_t value) { return static_cast<double>(value) / divisor; };
    char text[192];
    std::snprintf(text, sizeof(text), "opaque=%.0f translucent=%.0f points 0/1/2/3/4=%.0f/%.0f/%.0f/%.0f/%.0f rim=%.0f",
                  per(draws.LitOpaque), per(draws.LitTranslucent), per(draws.LitPointLights[0]),
                  per(draws.LitPointLights[1]), per(draws.LitPointLights[2]), per(draws.LitPointLights[3]),
                  per(draws.LitPointLights[4]), per(draws.LitRim));
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
