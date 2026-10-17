#pragma once

// GPU-side savings of the OpenGL PICA draw path that never change a pixel
// (config/gl_gpu_lean, on by default; desktop: TRIAEVUM_GL_GPU_LEAN).
//
// Mesa 20.1 nvc0 (switch-mesa) leaves Maxwell's ZCULL off for good
// (nvc0_screen.c writes ZCULL_REGION = 0x3f, "deactivate ZCULL", and
// nvc0_validate_zcull is under #if 0), so per-pixel early Z is the only
// depth rejection there is. The hardware runs it automatically unless the
// fragment program header says the shader kills pixels (any `discard` in
// the GLSL, nv50_ir usesDiscard -> hdr[0] 0x8000), writes depth
// (gl_FragDepth -> hdr[19] 0x2) or stores to memory (image stores/atomics
// -> hdr[0] bit 26). The PICA programs already keep `discard` only when the
// PICA alpha test is on and gl_FragDepth only off the raster-depth form
// (oot3d_native_pica_fragment_shader_gen.cpp), so the backend counts those
// draws (FragmentFeatures) rather than changing the programs.
//
// What this header decides, all exact by the GL specification:
//  1. Logic op COPY with blending off is the identity (s), so the logic op
//     is turned off: Mesa otherwise programs LOGIC_OP_ENABLE=1 for every
//     opaque PICA draw (the PICA's default output merger), a raster
//     operation that may read the destination.
//  2. Blending FUNC_ADD with source ONE and destination ZERO on both colour
//     and alpha is also the identity (s*1 + d*0 = s for any unorm d): off.
//  3. A draw whose fragments can change nothing (colour mask empty, a
//     blend that keeps the destination (ZERO, ONE, ADD) or the logic op
//     NOOP; and no depth write, no
//     stencil change, no image stores) is not issued.
//  4. A depth fill on a target whose PICA format has no stencil (D16/D24:
//     the frontend never enables the stencil test there, so the stencil
//     plane is never read nor written) also clears the stencil plane of the
//     D24S8 renderbuffer: a full depth/stencil clear instead of a masked one.
//
// Free of GL so it is checked offline (tests/gfx_opengl_pica_gpu_lean_tests);
// the GL enum values are mirrored here and checked against the GL headers
// with static_asserts in gfx_opengl_pica.cpp.

#include <cstdint>
#include <string_view>

namespace Fast::PicaGlGpuLean {

inline constexpr uint32_t kGlZero = 0x0000U;
inline constexpr uint32_t kGlOne = 0x0001U;
inline constexpr uint32_t kGlFuncAdd = 0x8006U;
inline constexpr uint32_t kGlCopy = 0x1503U;
inline constexpr uint32_t kGlNoop = 0x1505U;
inline constexpr uint32_t kGlKeep = 0x1E00U;

// Fragment program properties that disable early Z on nvc0 (see above).
enum FragmentFeature : uint8_t {
    kFragmentDiscard = 1U << 0U,
    kFragmentWritesDepth = 1U << 1U,
    kFragmentStoresImage = 1U << 2U,
};

[[nodiscard]] inline bool IsIdentifierChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

// True when `word` appears in `source` as a whole identifier.
[[nodiscard]] inline bool ContainsWord(std::string_view source,
                                       std::string_view word) noexcept {
    size_t position = source.find(word);
    while (position != std::string_view::npos) {
        const bool before =
            position == 0U || !IsIdentifierChar(source[position - 1U]);
        const size_t end = position + word.size();
        const bool after = end >= source.size() || !IsIdentifierChar(source[end]);
        if (before && after) {
            return true;
        }
        position = source.find(word, position + 1U);
    }
    return false;
}

// Conservative: a word inside a comment or a dead branch still counts (the
// GLSL compiler may or may not drop it); only used for counters.
[[nodiscard]] inline uint8_t ClassifyFragmentSource(
    std::string_view source) noexcept {
    uint8_t features = 0U;
    if (ContainsWord(source, "discard")) {
        features |= kFragmentDiscard;
    }
    if (ContainsWord(source, "gl_FragDepth")) {
        features |= kFragmentWritesDepth;
    }
    if (source.find("imageStore") != std::string_view::npos ||
        source.find("imageAtomic") != std::string_view::npos) {
        features |= kFragmentStoresImage;
    }
    return features;
}

struct BlendState {
    bool Enabled = false;
    uint32_t EquationRgb = kGlFuncAdd;
    uint32_t EquationAlpha = kGlFuncAdd;
    uint32_t SourceRgb = kGlOne;
    uint32_t DestRgb = kGlZero;
    uint32_t SourceAlpha = kGlOne;
    uint32_t DestAlpha = kGlZero;
    uint32_t LogicOp = kGlCopy; // GL logic op used when blending is off
};

enum class OutputMerger : uint8_t {
    Blend,   // GL_BLEND on, logic op off
    LogicOp, // GL_BLEND off, GL_COLOR_LOGIC_OP on
    Plain,   // both off: the fragment colour is written as is
};

[[nodiscard]] inline bool BlendIsIdentity(const BlendState& blend) noexcept {
    return blend.EquationRgb == kGlFuncAdd &&
           blend.EquationAlpha == kGlFuncAdd && blend.SourceRgb == kGlOne &&
           blend.SourceAlpha == kGlOne && blend.DestRgb == kGlZero &&
           blend.DestAlpha == kGlZero;
}

[[nodiscard]] inline bool BlendKeepsDestination(
    const BlendState& blend) noexcept {
    return blend.EquationRgb == kGlFuncAdd &&
           blend.EquationAlpha == kGlFuncAdd && blend.SourceRgb == kGlZero &&
           blend.SourceAlpha == kGlZero && blend.DestRgb == kGlOne &&
           blend.DestAlpha == kGlOne;
}

// The previous path: blending on -> Blend, off -> LogicOp (always enabled).
[[nodiscard]] inline OutputMerger ResolveOutputMerger(
    const BlendState& blend, bool lean) noexcept {
    if (blend.Enabled) {
        return lean && BlendIsIdentity(blend) ? OutputMerger::Plain
                                              : OutputMerger::Blend;
    }
    return lean && blend.LogicOp == kGlCopy ? OutputMerger::Plain
                                            : OutputMerger::LogicOp;
}

struct DrawEffects {
    bool ShadowProducer = false; // writes the Shadow2D image
    uint8_t ColorWriteMask = 0U; // RGBA bits as sent to glColorMask
    BlendState Blend;
    bool DepthWrite = false;
    bool StencilTest = false;
    uint32_t StencilWriteMask = 0U;
    uint32_t StencilFail = kGlKeep;
    uint32_t StencilDepthFail = kGlKeep;
    uint32_t StencilPass = kGlKeep;
    uint8_t FragmentFeatures = 0U;
};

// True when no fragment of the draw can change any buffer or image.
[[nodiscard]] inline bool DrawHasNoEffect(const DrawEffects& draw) noexcept {
    if (draw.ShadowProducer ||
        (draw.FragmentFeatures & kFragmentStoresImage) != 0U) {
        return false;
    }
    if (draw.DepthWrite) {
        return false;
    }
    if (draw.StencilTest && (draw.StencilWriteMask & 0xFFU) != 0U &&
        (draw.StencilFail != kGlKeep || draw.StencilDepthFail != kGlKeep ||
         draw.StencilPass != kGlKeep)) {
        return false;
    }
    if ((draw.ColorWriteMask & 0xFU) == 0U) {
        return true;
    }
    // Blending that keeps the destination (d = s*0 + d*1 = d), or the
    // logic op NOOP with blending off.
    return draw.Blend.Enabled ? BlendKeepsDestination(draw.Blend)
                              : draw.Blend.LogicOp == kGlNoop;
}

// PICA depth formats: 0 = D16, 1 = reserved, 2 = D24, 3 = D24S8.
[[nodiscard]] inline bool DepthFillClearsStencil(uint32_t picaDepthFormat,
                                                 bool lean) noexcept {
    return picaDepthFormat == 3U || lean;
}

// Bits for the glperf counters (GlPerf::CountPica).
enum CountBit : uint32_t {
    kCountFragmentDiscard = 1U << 0U,
    kCountFragmentDepth = 1U << 1U,
    kCountFragmentImage = 1U << 2U,
    kCountBlended = 1U << 3U,
    kCountLogicCopyOff = 1U << 4U,
    kCountBlendIdentityOff = 1U << 5U,
    kCountNoEffectSkipped = 1U << 6U,
    kCountFullDepthClear = 1U << 7U,
};

} // namespace Fast::PicaGlGpuLean
