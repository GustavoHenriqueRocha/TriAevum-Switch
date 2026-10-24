#pragma once

// Program choice of the deko3d backend for a PICA draw (package 3,
// docs/DEKO3D_PROGRESSO.md). Pure code, tested on the host
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

#include <cstddef>
#include <cstdint>
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
