#pragma once

#include "oot3d_native_pica_draw_state.h"
#include "fast/renderer3ds/pica_tev_program.h"
#include "fast/renderer3ds/pica_lighting_program.h"
#include "fast/renderer3ds/pica_proctex_program.h"
#include "oot3d_native_pica_fragment_lighting_gen.h"
#include "oot3d/renderer/pica_shader_hooks.h"
#include "oot3d/renderer/pica_shader_source_identity.h"

#include <array>
#include <cstdint>
#include <string>

namespace Oot3dNativeGame {

enum class Oot3dPicaTevMode { Specialized, Parametric };

struct Oot3dPicaFragmentUniformState {
    std::array<std::array<float, 4>, 6> TevConstants{};
    std::array<float, 4> CombinerBufferColor{};
    int32_t AlphaReference = 0;
    std::array<float, 4> FogColor{};
    std::array<std::array<float, 2>, 128> FogLut{};
    std::array<float, 4> TextureLodBias{};
    Oot3dPicaFragmentLightingUniformState Lighting{};
    int32_t ShadowTextureBias = 0;
    int32_t ShadowOrthographic = 0;
    float ShadowBiasConstant = 0.0F;
    float ShadowBiasLinear = 0.0F;
    Fast::Renderer3ds::PicaTevProgram TevProgram;
    Fast::Renderer3ds::PicaLightingProgram LightingProgram;
    std::array<uint32_t, 4> FragmentControl{}; // native fog, alpha-test, texture main/config0
    Fast::Renderer3ds::PicaProcTexProgram ProcTexProgram;
};

struct Oot3dPicaGeneratedFragmentShader {
    uint64_t StateKey = 0;
    std::string Source;
    Oot3d::Renderer::PicaShaderSourceIdentity SourceIdentity;
    Oot3d::Renderer::PicaShaderHookLayout Hooks;
    Oot3dPicaFragmentUniformState Uniforms;
};

// True when a specialized program's PICA depth equals the rasterized depth
// exactly, so it leaves depth to fixed function.
bool Oot3dPicaFragmentDepthIsRasterDepth(const Oot3dPicaDecodedDrawState& state,
                                         Oot3dPicaTevMode mode = Oot3dPicaTevMode::Specialized);

uint64_t ComputeOot3dPicaFragmentShaderStateKey(
    const Oot3dPicaDrawPacket& packet,
    const Oot3dPicaDecodedDrawState& state,
    Oot3dPicaTevMode mode = Oot3dPicaTevMode::Specialized);

Oot3dPicaFragmentUniformState BuildOot3dPicaFragmentUniformState(
    const Oot3dPicaDrawPacket& packet);

// The same uniform state written into `uniforms`, which must be
// default-constructed. reuseFogTable: the converted fog table comes from
// this thread's previous call when the raw table is equal (exact).
void BuildOot3dPicaFragmentUniformStateInto(
    const Oot3dPicaDrawPacket& packet, Oot3dPicaFragmentUniformState& uniforms,
    bool reuseFogTable);

// OfflineSource emits identical code without draw-time uniforms or resident
// lighting LUTs. It rejects missing data that would be embedded into source.
bool GenerateOot3dPicaFragmentShader(
    const Oot3dPicaDrawPacket& packet,
    const Oot3dPicaDecodedDrawState& state,
    Oot3dPicaGeneratedFragmentShader& shader,
    std::string* error = nullptr,
    Oot3dPicaShaderBuildPurpose purpose = Oot3dPicaShaderBuildPurpose::RuntimeDraw,
    Oot3dPicaTevMode mode = Oot3dPicaTevMode::Specialized);

// The parametric program of one structural configuration. Its text depends
// only on native fragment lighting (with or without lookup tables), an
// integer (shadow) texture 0 and the procedural texture unit; every
// parametric draw program is one of these, with draw-specific hooks. `lightingLuts` false: the lit family whose
// decoded state disables every lookup table (no LUT texture).
bool GenerateOot3dPicaParametricBaseFragmentShader(
    bool fragmentLighting, bool integerTexture0,
    Oot3dPicaGeneratedFragmentShader& shader, std::string* error = nullptr,
    bool lightingLuts = true, bool procTex = true);

} // namespace Oot3dNativeGame
