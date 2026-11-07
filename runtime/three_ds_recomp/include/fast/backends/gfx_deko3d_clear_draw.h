#pragma once

// config/deko3d_clear_draw (off by default): the frame's depth reset, a draw
// with depth test ALWAYS over the whole target (the game never clears or
// fills depth: `fills=0 clears=0`, SWITCH_PORT.md "Depth reset as fast
// clears"), replaced by fast clears where that is provably exact:
//
//   - depth-only: the draw provably writes one depth, 0 or 1, at every pixel
//     of the target -> dkCmdBufClearDepthStencil to it, the draw kept with
//     depth writes off;
//   - full: its colour is provably one constant too -> the colour is cleared
//     to it (under the draw's colour mask) and the draw is skipped.
//
// Pure code, tested on the host (oot3d_deko3d_clear_draw_tests,
// tests/switch/test_clear_draw_tev.py). Everything not provable keeps the
// draw as it is; the reason is reported (`reset draw:` in logs/deko3d.txt).
//
// What "provable" rests on:
//   - Depth: the explicit form of the specialized fragment program
//     (oot3d_native_pica_fragment_shader_gen.cpp) writes
//     gl_FragDepth = -gl_FragCoord.z * depth_scale + depth_offset, divided by
//     gl_FragCoord.w with w-buffering. Scale 0 makes it the offset at every
//     fragment (w-buffered: only offset 0). The raster form (scale -1,
//     offset 0) writes the rasterized depth, which the CPU cannot reproduce
//     bit for bit: never converted. Only 0 and 1 are cleared to (exact in
//     every depth format and conversion; also deko3d's fast ZCULL clear).
//   - Colour: every TEV input the specialized program reads is a constant
//     (TEV constant, buffer colour, a disabled texture/lighting/procedural
//     source = 0). The six stages are evaluated in double precision and each
//     8-bit rounding must sit at least kPicaTevRoundMargin (in 1/255 units)
//     away from a rounding boundary, so float32 GPU arithmetic (any order,
//     fused or not, division or reciprocal) rounds to the same bytes.
//   - Coverage: the frontend's typed model-view-projection transform of the
//     vertex program (the one PicaDrawGeometryNdcBounds uses) projects the
//     positions; one triangle or two triangles forming a convex quad must
//     contain every pixel centre of the viewport, no vertex may lie near a
//     clip plane (where float32 and double could disagree on clipping), and
//     with face culling every covering triangle must be front-facing by the
//     GL rule that PlanPicaCull reproduces (not with config/deko3d_cull_invert).
//   - No fragment may be discarded (alpha test off, ALWAYS, or provably
//     passing on the constant alpha), no fog, no fragment lighting, no
//     lighting variant (Toon/Per-pixel), a specialized program; for the
//     colour clear also no blending (or ONE/ZERO add), logic op COPY, no
//     stencil writes.

#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_render_backend.h"
#include "fast/renderer3ds/pica_vertex_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>

namespace Fast::Deko3d {

// ---- constant TEV -------------------------------------------------------------

// Minimum distance of a scaled TEV value (value * 255) from a rounding
// boundary (n + 0.5). float32 errors of one stage stay below 1e-3 of it
// (values <= 4, a few operations of 2^-24 relative error each).
inline constexpr double kPicaTevRoundMargin = 0.01;

enum class PicaColorSource : uint8_t {
    Constant,    // every input read is a constant
    Primary,     // the vertex colour (source 0)
    Lighting,    // fragment lighting colours (sources 1, 2) with lighting on
    Texture,     // an enabled texture unit (sources 3-5)
    ProcTex,     // the procedural texture (source 6) when enabled
    Unsupported, // a source/modifier/operation the specialized program rejects
};

[[nodiscard]] constexpr const char* PicaColorSourceName(PicaColorSource source) noexcept {
    switch (source) {
        case PicaColorSource::Constant: return "constant";
        case PicaColorSource::Primary: return "vertex_colour";
        case PicaColorSource::Lighting: return "lighting";
        case PicaColorSource::Texture: return "texture";
        case PicaColorSource::ProcTex: return "proctex";
        case PicaColorSource::Unsupported: return "unsupported";
    }
    return "?";
}

// The specialized program's TEV inputs, from the packed fragment uniform
// block (pica_uniform_layout.h): the decoded program (sources, modifiers,
// operations, scales per stage; buffer-update bits), the constants and the
// buffer colour, and fragment_control (0x0E0, 0x104, 0x080, 0x083).
struct PicaConstantTevInputs {
    std::array<std::array<uint32_t, 4>, 6> Stages{};
    uint32_t BufferControl = 0U; // register 0x0E0 & 0xFF00
    std::array<std::array<float, 4>, 6> Constants{};
    std::array<float, 4> BufferColor{};
    uint8_t TextureEnableMask = 0U; // register 0x080 bits 0-2
    bool ProcTexEnabled = false;    // register 0x080 bit 10
    bool LightingEnabled = false;
};

struct PicaConstantColor {
    PicaColorSource Source = PicaColorSource::Unsupported;
    bool Ambiguous = false; // constant, but a rounding sits on a boundary
    uint8_t Stage = 0U;     // where Source / Ambiguous was found
    // combiner_output in bytes (exact multiples of 1/255 on the GPU, up to
    // its float rounding): what pica_color stores.
    std::array<uint8_t, 4> Bytes{};
    // combiner_output.a * scale went above 1 and was clamped to exactly 1.0
    // (the alpha test then reads 255 exactly).
    bool AlphaClamped = false;

    [[nodiscard]] bool Proven() const noexcept { return Source == PicaColorSource::Constant && !Ambiguous; }
};

[[nodiscard]] inline bool ReadPicaConstantTevInputs(std::span<const uint8_t> uniforms, bool lightingEnabled,
                                                    PicaConstantTevInputs& inputs) noexcept {
    using namespace Fast::Oot3d;
    if (uniforms.size() < kPicaPackedFragmentUniformSize) {
        return false;
    }
    for (size_t stage = 0; stage < 6U; ++stage) {
        std::memcpy(inputs.Stages[stage].data(), uniforms.data() + kPicaPackedFragmentTevProgramOffset + stage * 16U,
                    16U);
        std::memcpy(inputs.Constants[stage].data(),
                    uniforms.data() + kPicaPackedFragmentTevConstantsOffset + stage * kPicaPackedVec4Bytes,
                    kPicaPackedVec4Bytes);
    }
    std::array<uint32_t, 4> control{};
    std::memcpy(control.data(), uniforms.data() + kPicaPackedFragmentControlOffset, sizeof(control));
    std::memcpy(inputs.BufferColor.data(), uniforms.data() + kPicaPackedFragmentCombinerBufferOffset,
                kPicaPackedVec4Bytes);
    inputs.BufferControl = control[0] & 0xFF00U;
    inputs.TextureEnableMask = static_cast<uint8_t>(control[2] & 7U);
    inputs.ProcTexEnabled = (control[2] & (1U << 10U)) != 0U;
    inputs.LightingEnabled = lightingEnabled;
    return true;
}

namespace ClearDrawDetail {
using Rgba = std::array<double, 4>;

struct Value {
    Rgba V{};
    PicaColorSource Source = PicaColorSource::Constant;
};

// 8-bit rounding of the generator's byteround(clamp(x, 0, 1)); false when x
// sits within the margin of a rounding boundary.
[[nodiscard]] inline bool ByteRound(double x, double& byte) noexcept {
    if (!std::isfinite(x)) {
        return false;
    }
    const double scaled = std::clamp(x, 0.0, 1.0) * 255.0;
    if (std::fabs(scaled - std::floor(scaled) - 0.5) < kPicaTevRoundMargin) {
        return false;
    }
    byte = std::floor(scaled + 0.5);
    return true;
}

[[nodiscard]] inline uint8_t InputMask(uint32_t operation) noexcept {
    switch (operation) {
        case 0U: return 1U;
        case 1U: case 2U: case 3U: case 5U: case 6U: case 7U: return 3U;
        case 4U: case 8U: case 9U: return 7U;
        default: return 0U;
    }
}
} // namespace ClearDrawDetail

// The specialized program's six TEV stages on constant inputs
// (SourceExpression, ColorModifier, AlphaModifier, ColorOperation,
// AlphaOperation and the stage epilogue of the generator).
[[nodiscard]] inline PicaConstantColor EvaluatePicaConstantTev(const PicaConstantTevInputs& in) noexcept {
    using namespace ClearDrawDetail;
    PicaConstantColor result;
    Value previous;  // combiner_output (vec4(0.0) before stage 0)
    Value buffer;    // combiner_buffer (vec4(0.0) before stage 0)
    Value nextBuffer;
    for (size_t c = 0; c < 4U; ++c) {
        nextBuffer.V[c] = in.BufferColor[c];
    }
    const auto fail = [&](PicaColorSource source, size_t stage) {
        result.Source = source;
        result.Stage = static_cast<uint8_t>(stage);
        return result;
    };
    for (size_t stage = 0; stage < 6U; ++stage) {
        const auto& words = in.Stages[stage];
        const auto source = [&](uint32_t selector, Value& out) {
            out = Value{};
            switch (selector) {
                case 0x0U: out.Source = PicaColorSource::Primary; return true;
                case 0x1U:
                case 0x2U:
                    out.Source = in.LightingEnabled ? PicaColorSource::Lighting : PicaColorSource::Constant;
                    return true;
                case 0x3U:
                case 0x4U:
                case 0x5U:
                    if ((in.TextureEnableMask & (1U << (selector - 3U))) != 0U) {
                        out.Source = PicaColorSource::Texture;
                    }
                    return true;
                case 0x6U:
                    out.Source = in.ProcTexEnabled ? PicaColorSource::ProcTex : PicaColorSource::Constant;
                    return true;
                case 0xDU: out = buffer; return true;
                case 0xEU:
                    for (size_t c = 0; c < 4U; ++c) {
                        out.V[c] = in.Constants[stage][c];
                    }
                    return true;
                case 0xFU: out = previous; return true;
                default: return false;
            }
        };
        std::array<uint32_t, 3> colorSources{words[0] & 0xFU, (words[0] >> 4U) & 0xFU, (words[0] >> 8U) & 0xFU};
        std::array<uint32_t, 3> alphaSources{(words[0] >> 16U) & 0xFU, (words[0] >> 20U) & 0xFU,
                                             (words[0] >> 24U) & 0xFU};
        if (stage == 0U) {
            for (size_t input = 0; input < 2U; ++input) {
                if (colorSources[input] == 0xFU) {
                    colorSources[input] = colorSources[2];
                }
                if (alphaSources[input] == 0xFU) {
                    alphaSources[input] = alphaSources[2];
                }
            }
        }
        const uint32_t colorOp = words[2] & 0xFU;
        const uint32_t alphaOp = (words[2] >> 16U) & 0xFU;
        const uint8_t colorMask = InputMask(colorOp);
        const uint8_t alphaMask = colorOp == 7U ? 0U : InputMask(alphaOp);
        if (colorMask == 0U || (colorOp != 7U && (alphaMask == 0U || alphaOp == 6U || alphaOp == 7U))) {
            return fail(PicaColorSource::Unsupported, stage);
        }
        std::array<std::array<double, 3>, 3> color{};
        std::array<double, 3> alpha{};
        for (size_t input = 0; input < 3U; ++input) {
            if ((colorMask & (1U << input)) != 0U) {
                Value value;
                if (!source(colorSources[input], value)) {
                    return fail(PicaColorSource::Unsupported, stage);
                }
                if (value.Source != PicaColorSource::Constant) {
                    return fail(value.Source, stage);
                }
                const uint32_t modifier = (words[1] >> (input * 4U)) & 0xFU;
                size_t channel = 0;
                bool broadcast = true;
                switch (modifier & 0xEU) {
                    case 0x0U: broadcast = false; break;
                    case 0x2U: channel = 3; break;
                    case 0x4U: channel = 0; break;
                    case 0x8U: channel = 1; break;
                    case 0xCU: channel = 2; break;
                    default: return fail(PicaColorSource::Unsupported, stage);
                }
                for (size_t c = 0; c < 3U; ++c) {
                    const double v = broadcast ? value.V[channel] : value.V[c];
                    color[input][c] = (modifier & 1U) != 0U ? 1.0 - v : v;
                }
            }
            if ((alphaMask & (1U << input)) != 0U) {
                Value value;
                if (!source(alphaSources[input], value)) {
                    return fail(PicaColorSource::Unsupported, stage);
                }
                if (value.Source != PicaColorSource::Constant) {
                    return fail(value.Source, stage);
                }
                const uint32_t modifier = (words[1] >> (12U + input * 4U)) & 7U;
                static constexpr size_t kChannel[4] = {3, 0, 1, 2};
                const double v = value.V[kChannel[modifier >> 1U]];
                alpha[input] = (modifier & 1U) != 0U ? 1.0 - v : v;
            }
        }
        std::array<double, 3> colorResult{};
        if (colorOp == 6U || colorOp == 7U) {
            double dot = 0.0;
            for (size_t c = 0; c < 3U; ++c) {
                dot += (color[0][c] - 0.5) * (color[1][c] - 0.5);
            }
            colorResult.fill(dot * 4.0);
        } else {
            for (size_t c = 0; c < 3U; ++c) {
                const double a = color[0][c], b = color[1][c], k = color[2][c];
                switch (colorOp) {
                    case 0U: colorResult[c] = a; break;
                    case 1U: colorResult[c] = a * b; break;
                    case 2U: colorResult[c] = a + b; break;
                    case 3U: colorResult[c] = a + b - 0.5; break;
                    case 4U: colorResult[c] = b * (1.0 - k) + a * k; break;
                    case 5U: colorResult[c] = a - b; break;
                    case 8U: colorResult[c] = a * b + k; break;
                    case 9U: colorResult[c] = std::min(a + b, 1.0) * k; break;
                    default: return fail(PicaColorSource::Unsupported, stage);
                }
            }
        }
        std::array<double, 3> colorBytes{};
        for (size_t c = 0; c < 3U; ++c) {
            if (!ByteRound(colorResult[c], colorBytes[c])) {
                result.Source = PicaColorSource::Constant;
                result.Ambiguous = true;
                result.Stage = static_cast<uint8_t>(stage);
                return result;
            }
        }
        double alphaByte = 0.0;
        if (colorOp == 7U) {
            alphaByte = colorBytes[0];
        } else {
            const double a = alpha[0], b = alpha[1], k = alpha[2];
            double value = 0.0;
            switch (alphaOp) {
                case 0U: value = a; break;
                case 1U: value = a * b; break;
                case 2U: value = a + b; break;
                case 3U: value = a + b - 0.5; break;
                case 4U: value = b * (1.0 - k) + a * k; break;
                case 5U: value = a - b; break;
                case 8U: value = a * b + k; break;
                case 9U: value = std::min(a + b, 1.0) * k; break;
                default: return fail(PicaColorSource::Unsupported, stage);
            }
            if (!ByteRound(value, alphaByte)) {
                result.Source = PicaColorSource::Constant;
                result.Ambiguous = true;
                result.Stage = static_cast<uint8_t>(stage);
                return result;
            }
        }
        const uint32_t colorScaleBits = words[3] & 3U;
        const uint32_t alphaScaleBits = (words[3] >> 16U) & 3U;
        const double colorScale = colorScaleBits < 3U ? static_cast<double>(1U << colorScaleBits) : 1.0;
        const double alphaScale = alphaScaleBits < 3U ? static_cast<double>(1U << alphaScaleBits) : 1.0;
        previous = Value{};
        for (size_t c = 0; c < 3U; ++c) {
            previous.V[c] = std::min(colorBytes[c] * colorScale, 255.0) / 255.0;
        }
        previous.V[3] = std::min(alphaByte * alphaScale, 255.0) / 255.0;
        result.AlphaClamped = alphaByte * alphaScale > 255.0;
        // combiner_buffer = next_combiner_buffer, then this stage's updates.
        buffer = nextBuffer;
        if (stage < 4U) {
            if ((in.BufferControl & (1U << (8U + stage))) != 0U) {
                for (size_t c = 0; c < 3U; ++c) {
                    nextBuffer.V[c] = previous.V[c];
                }
            }
            if ((in.BufferControl & (1U << (12U + stage))) != 0U) {
                nextBuffer.V[3] = previous.V[3];
            }
        }
    }
    result.Source = PicaColorSource::Constant;
    for (size_t c = 0; c < 4U; ++c) {
        result.Bytes[c] = static_cast<uint8_t>(std::lround(previous.V[c] * 255.0));
    }
    return result;
}

// ---- alpha test -----------------------------------------------------------

enum class PicaAlphaTestOutcome : uint8_t {
    Passes,      // no fragment is discarded
    Discards,    // every fragment is discarded
    Unknown,     // depends on the fragment or on GPU rounding
};

// The generator's `int alpha_byte = int(combiner_output.a * 255.0);
// if (<AlphaDiscardCondition(function)>) discard;`. combiner_output.a is
// byte/255 up to a float rounding, so the truncation gives byte or byte - 1
// (exactly byte for 0 and for a value clamped to 1.0).
[[nodiscard]] inline PicaAlphaTestOutcome PicaConstantAlphaTest(bool enabled, Renderer3ds::PicaCompareFunction function,
                                                                int32_t reference, const PicaConstantColor* color) noexcept {
    using Compare = Renderer3ds::PicaCompareFunction;
    if (!enabled || function == Compare::Always) {
        return PicaAlphaTestOutcome::Passes;
    }
    if (function == Compare::Never) {
        return PicaAlphaTestOutcome::Discards;
    }
    if (color == nullptr || !color->Proven()) {
        return PicaAlphaTestOutcome::Unknown;
    }
    const int32_t byte = color->Bytes[3];
    const bool exact = byte == 0 || color->AlphaClamped;
    const auto discards = [&](int32_t value) {
        switch (function) {
            case Compare::Equal: return value != reference;
            case Compare::NotEqual: return value == reference;
            case Compare::Less: return value >= reference;
            case Compare::LessOrEqual: return value > reference;
            case Compare::Greater: return value <= reference;
            case Compare::GreaterOrEqual: return value < reference;
            default: return false;
        }
    };
    const bool high = discards(byte);
    const bool low = exact ? high : discards(byte - 1);
    if (high != low) {
        return PicaAlphaTestOutcome::Unknown;
    }
    return high ? PicaAlphaTestOutcome::Discards : PicaAlphaTestOutcome::Passes;
}

// ---- depth --------------------------------------------------------------------

enum class PicaResetDepthForm : uint8_t {
    Raster,           // gl_FragCoord.z (fixed function): not reproducible on the CPU
    ExplicitConstant, // gl_FragDepth = depth_offset at every fragment
    ExplicitVarying,  // gl_FragDepth follows the raster depth
};

struct PicaResetDepth {
    PicaResetDepthForm Form = PicaResetDepthForm::Raster;
    float Scale = -1.0F;
    float Offset = 0.0F;
    bool WBuffering = false;
    float Constant = 0.0F; // ExplicitConstant only
};

[[nodiscard]] inline PicaResetDepth PlanPicaResetDepth(bool programWritesFragDepth,
                                                       std::span<const uint8_t> fragmentUniforms) noexcept {
    using namespace Fast::Oot3d;
    PicaResetDepth depth;
    if (fragmentUniforms.size() >= kPicaPackedFragmentBaseUniformSize) {
        int32_t wBuffering = 0;
        std::memcpy(&depth.Scale, fragmentUniforms.data() + kPicaPackedFragmentDepthScaleOffset, sizeof(float));
        std::memcpy(&depth.Offset, fragmentUniforms.data() + kPicaPackedFragmentDepthOffsetOffset, sizeof(float));
        std::memcpy(&wBuffering, fragmentUniforms.data() + kPicaPackedFragmentWBufferingOffset, sizeof(wBuffering));
        depth.WBuffering = wBuffering != 0;
    } else if (programWritesFragDepth) {
        depth.Form = PicaResetDepthForm::ExplicitVarying; // no uniforms: unknown
        return depth;
    }
    if (!programWritesFragDepth) {
        depth.Form = PicaResetDepthForm::Raster;
        return depth;
    }
    // -z * 0 + offset = offset for every finite raster z; with w-buffering
    // it is divided by gl_FragCoord.w (> 0, finite): constant only for 0.
    const bool constant = depth.Scale == 0.0F && std::isfinite(depth.Offset) &&
                          (!depth.WBuffering || depth.Offset == 0.0F);
    depth.Form = constant ? PicaResetDepthForm::ExplicitConstant : PicaResetDepthForm::ExplicitVarying;
    depth.Constant = constant ? (depth.Offset == 0.0F ? 0.0F : depth.Offset) : 0.0F;
    return depth;
}

// ---- coverage -----------------------------------------------------------------

enum class PicaCoverageStatus : uint8_t {
    Proven,
    NoTransform,     // no typed model-view-projection transform
    Skinned,         // the skeleton program is enabled
    PositionFormat,  // positions are not 2-4 floats of a per-vertex binding
    VertexCount,     // none, or more than kPicaResetMaxVertices
    VertexData,      // index or vertex outside its bytes, non-finite
    BehindEye,       // w <= 0
    NearClipPlane,   // a vertex at or beyond a near/far plane (within rounding)
    Topology,        // geometry-shader topology
    Culled,          // culling with config/deko3d_cull_invert, or covering triangles culled
    Gap,             // no triangle or convex quad contains every pixel centre
};

[[nodiscard]] constexpr const char* PicaCoverageStatusName(PicaCoverageStatus status) noexcept {
    switch (status) {
        case PicaCoverageStatus::Proven: return "proven";
        case PicaCoverageStatus::NoTransform: return "no_typed_transform";
        case PicaCoverageStatus::Skinned: return "skinned";
        case PicaCoverageStatus::PositionFormat: return "position_format";
        case PicaCoverageStatus::VertexCount: return "vertex_count";
        case PicaCoverageStatus::VertexData: return "vertex_data";
        case PicaCoverageStatus::BehindEye: return "behind_eye";
        case PicaCoverageStatus::NearClipPlane: return "on_clip_plane";
        case PicaCoverageStatus::Topology: return "topology";
        case PicaCoverageStatus::Culled: return "culled";
        case PicaCoverageStatus::Gap: return "gap";
    }
    return "?";
}

inline constexpr uint32_t kPicaResetMaxVertices = 16U;

struct PicaResetCoverage {
    PicaCoverageStatus Status = PicaCoverageStatus::NoTransform;
    uint32_t Vertices = 0U;
    // Raster (window) depth of the vertices, -z/w of the PICA clip position
    // (the GL vertex wrapper's gl_Position.z = -2z - w), and NDC x/y bounds.
    double DepthMin = 0.0;
    double DepthMax = 0.0;
    std::array<double, 2> NdcMin{};
    std::array<double, 2> NdcMax{};
    uint32_t Triangles = 0U;     // non-degenerate, not culled
    bool DepthKnown = false;
};

namespace ClearDrawDetail {
struct Point {
    double X = 0.0;
    double Y = 0.0;
};

[[nodiscard]] inline double Cross(const Point& a, const Point& b, const Point& c) noexcept {
    return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
}

// p strictly inside the convex polygon (counter-clockwise), at least `margin`
// away from every edge.
[[nodiscard]] inline bool InsideConvex(std::span<const Point> polygon, const Point& p, double margin) noexcept {
    for (size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1U) % polygon.size()];
        const double length = std::hypot(b.X - a.X, b.Y - a.Y);
        if (!(length > 0.0) || Cross(a, b, p) / length < margin) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool ContainsSquare(std::span<const Point> polygon, const std::array<double, 2>& half,
                                         double margin) noexcept {
    for (const double sx : {-1.0, 1.0}) {
        for (const double sy : {-1.0, 1.0}) {
            if (!InsideConvex(polygon, {sx * half[0], sy * half[1]}, margin)) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] inline bool SamePoint(const Point& a, const Point& b) noexcept {
    return a.X == b.X && a.Y == b.Y;
}
} // namespace ClearDrawDetail

// viewportWidth/Height: the viewport in target pixels (pixel centres are half
// a pixel inside its edges). Culling follows the draw's NativeCullMode;
// cullInvert: config/deko3d_cull_invert (then culled draws are not proven).
[[nodiscard]] inline PicaResetCoverage ProvePicaResetCoverage(const Renderer3ds::PicaDrawView& draw, double viewportWidth,
                                                              double viewportHeight, bool cullInvert) noexcept {
    using namespace ClearDrawDetail;
    using namespace Renderer3ds::VertexGeometry;
    using Renderer3ds::ShadowMatrix;
    PicaResetCoverage coverage;
    const auto finish = [&](PicaCoverageStatus status) {
        coverage.Status = status;
        return coverage;
    };
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    const auto* transform = hooks.TransformProgram();
    if (transform == nullptr) {
        return finish(PicaCoverageStatus::NoTransform);
    }
    if (draw.VertexUniformBytes.size() < Oot3d::kPicaPackedVertexFloatOffset) {
        return finish(PicaCoverageStatus::NoTransform);
    }
    uint32_t booleans = 0U;
    std::memcpy(&booleans, draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexBooleanMaskOffset, sizeof(booleans));
    const auto floats = draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset);
    if (const auto* skeleton = hooks.SkeletonProgram();
        skeleton != nullptr && skeleton->EnableBooleanUniform < 32U &&
        (booleans & (1U << skeleton->EnableBooleanUniform)) != 0U) {
        return finish(PicaCoverageStatus::Skinned);
    }
    ShadowMatrix view;
    ShadowMatrix model;
    std::array<std::array<float, 4>, 4> projection{};
    if (!UniformMatrix(floats, transform->ViewFirstUniform, view) ||
        !UniformMatrix(floats, transform->ModelFirstUniform, model) ||
        floats.size() < (size_t{transform->ProjectionFirstUniform} + 4U) * sizeof(Vec4)) {
        return finish(PicaCoverageStatus::NoTransform);
    }
    for (size_t row = 0; row < 4U; ++row) {
        std::memcpy(projection[row].data(), floats.data() + (transform->ProjectionFirstUniform + row) * sizeof(Vec4),
                    sizeof(Vec4));
        for (const float value : projection[row]) {
            if (!std::isfinite(value)) {
                return finish(PicaCoverageStatus::NoTransform);
            }
        }
    }
    const auto attribute = std::find_if(draw.VertexAttributes.begin(), draw.VertexAttributes.end(),
                                        [&](const auto& item) { return item.Location == transform->PositionInputRegister; });
    if (attribute == draw.VertexAttributes.end() || attribute->Format != Renderer3ds::PicaVertexFormat::Float ||
        attribute->ComponentCount < 2U || attribute->ComponentCount > 4U) {
        return finish(PicaCoverageStatus::PositionFormat);
    }
    const auto binding = std::find_if(draw.VertexBindings.begin(), draw.VertexBindings.end(),
                                      [&](const auto& item) { return item.Binding == attribute->Binding; });
    if (binding == draw.VertexBindings.end() || binding->PerInstance || binding->ByteStride == 0U) {
        return finish(PicaCoverageStatus::PositionFormat);
    }
    if (draw.Topology == Renderer3ds::PicaTopology::GeometryShader) {
        return finish(PicaCoverageStatus::Topology);
    }
    const size_t indexBytes = draw.IndicesAre16Bit ? 2U : 1U;
    const size_t count =
        draw.Indexed ? std::min<size_t>(draw.VertexCount, draw.IndexBytes.size() / indexBytes) : draw.VertexCount;
    coverage.Vertices = static_cast<uint32_t>(count);
    if (count < 3U || count > kPicaResetMaxVertices) {
        return finish(PicaCoverageStatus::VertexCount);
    }
    // Every vertex in double precision with a bound of the float32 error the
    // GPU can make: the model and view rows (3x4, w forced to 1 as the typed
    // transform does), then the projection rows. Each dot product may err by
    // kDotError of the sum of its terms' magnitudes plus its inputs' errors
    // (four products and three sums of 2^-24 each, doubled for margin).
    constexpr double kUnit = 1.0 / 16777216.0;
    constexpr double kDotError = 16.0 * kUnit;
    std::array<Point, kPicaResetMaxVertices> points{};
    double ndcError = 0.0;
    coverage.DepthMin = 1.0e30;
    coverage.DepthMax = -1.0e30;
    coverage.NdcMin = {1.0e30, 1.0e30};
    coverage.NdcMax = {-1.0e30, -1.0e30};
    bool clipped = false;
    for (size_t item = 0; item < count; ++item) {
        uint32_t index = static_cast<uint32_t>(item);
        if (draw.Indexed) {
            if (draw.IndicesAre16Bit) {
                uint16_t value = 0;
                std::memcpy(&value, draw.IndexBytes.data() + item * 2U, sizeof(value));
                index = value;
            } else {
                index = draw.IndexBytes[item];
            }
        }
        const int64_t vertex = int64_t{index} + draw.BaseVertex;
        const uint64_t offset = static_cast<uint64_t>(vertex) * binding->ByteStride + attribute->ByteOffset;
        const size_t bytes = size_t{attribute->ComponentCount} * sizeof(float);
        if (vertex < 0 || offset > binding->Bytes.size() || bytes > binding->Bytes.size() - offset) {
            return finish(PicaCoverageStatus::VertexData);
        }
        Vec4 position{0.0F, 0.0F, 0.0F, 1.0F};
        std::memcpy(position.data(), binding->Bytes.data() + offset, bytes);
        for (const float value : position) {
            if (!std::isfinite(value)) {
                return finish(PicaCoverageStatus::VertexData);
            }
        }
        struct Bounded {
            std::array<double, 4> Value{0.0, 0.0, 0.0, 1.0};
            std::array<double, 4> Error{};
        };
        const auto rows3 = [&](const ShadowMatrix& matrix, const Bounded& point) {
            Bounded out;
            for (size_t row = 0; row < 3U; ++row) {
                double sum = 0.0;
                double magnitude = 0.0;
                double carried = 0.0;
                for (size_t col = 0; col < 4U; ++col) {
                    const double coefficient = matrix[col * 4U + row];
                    sum += coefficient * point.Value[col];
                    magnitude += std::fabs(coefficient * point.Value[col]);
                    carried += std::fabs(coefficient) * point.Error[col];
                }
                out.Value[row] = sum;
                out.Error[row] = carried + kDotError * magnitude;
            }
            return out;
        };
        Bounded object;
        object.Value = {position[0], position[1], position[2], 1.0};
        const Bounded eye = rows3(view, rows3(model, object));
        std::array<double, 4> clip{};
        std::array<double, 4> clipError{};
        for (size_t row = 0; row < 4U; ++row) {
            double sum = 0.0;
            double magnitude = 0.0;
            double carried = 0.0;
            for (size_t col = 0; col < 4U; ++col) {
                const double coefficient = projection[row][col];
                sum += coefficient * eye.Value[col];
                magnitude += std::fabs(coefficient * eye.Value[col]);
                carried += std::fabs(coefficient) * eye.Error[col];
            }
            clip[row] = sum;
            clipError[row] = carried + kDotError * magnitude;
        }
        const double w = clip[3];
        if (!std::isfinite(w) || !(w - clipError[3] > 1.0e-6)) {
            return finish(PicaCoverageStatus::BehindEye);
        }
        const auto ratioError = [&](size_t row) {
            const double ratio = clip[row] / w;
            return (clipError[row] + std::fabs(ratio) * clipError[3]) / (w - clipError[3]) +
                   4.0 * kUnit * std::fabs(ratio);
        };
        // PICA clip z/w = q, inside for [-1, 0] (the GL vertex wrapper's
        // gl_Position.z = -2z - w). The wrapper snaps q in (-1.00001, -1) to
        // -1 and q in (0, 1e-6) to 0, so a vertex is inside for sure when the
        // GPU's q is anywhere in (-1, 1e-6) or (-1.00001, -1); exactly -1 is
        // not sure (z one ulp beyond -w can still divide to -1.0).
        const double q = clip[2] / w;
        const double qError = ratioError(2U) + 4.0 * kUnit;
        const bool interior = q - qError > -1.0 && q + qError < 1.0e-6;
        const bool snapped = q + qError < -1.0 && q - qError > -1.00001;
        clipped = clipped || !(interior || snapped);
        coverage.DepthMin = std::min(coverage.DepthMin, -q);
        coverage.DepthMax = std::max(coverage.DepthMax, -q);
        coverage.DepthKnown = true;
        points[item] = {clip[0] / w, clip[1] / w};
        ndcError = std::max({ndcError, ratioError(0U), ratioError(1U)});
        coverage.NdcMin[0] = std::min(coverage.NdcMin[0], points[item].X);
        coverage.NdcMin[1] = std::min(coverage.NdcMin[1], points[item].Y);
        coverage.NdcMax[0] = std::max(coverage.NdcMax[0], points[item].X);
        coverage.NdcMax[1] = std::max(coverage.NdcMax[1], points[item].Y);
    }
    if (clipped) {
        return finish(PicaCoverageStatus::NearClipPlane);
    }
    const bool cull = draw.CullMode != Renderer3ds::NativeCullMode::KeepAll;
    if (cull && cullInvert) {
        return finish(PicaCoverageStatus::Culled);
    }
    // Triangles in GL winding order (odd strip triangles swap their first two).
    std::array<std::array<Point, 3>, kPicaResetMaxVertices> triangles{};
    size_t triangleCount = 0;
    bool culledAny = false;
    const auto add = [&](size_t a, size_t b, size_t c) {
        std::array<Point, 3> triangle{points[a], points[b], points[c]};
        const double area = Cross(triangle[0], triangle[1], triangle[2]);
        if (!(std::fabs(area) > 1.0e-9)) {
            return;
        }
        // GL (PlanPicaCull's model): kept when its NDC winding is the kept one.
        if (cull && (area > 0.0) != (draw.CullMode == Renderer3ds::NativeCullMode::KeepCounterClockwise)) {
            culledAny = true;
            return;
        }
        if (area < 0.0) {
            std::swap(triangle[1], triangle[2]);
        }
        triangles[triangleCount++] = triangle;
    };
    switch (draw.Topology) {
        case Renderer3ds::PicaTopology::TriangleList:
            for (size_t i = 0; i + 2U < count; i += 3U) {
                add(i, i + 1U, i + 2U);
            }
            break;
        case Renderer3ds::PicaTopology::TriangleStrip:
            for (size_t i = 0; i + 2U < count; ++i) {
                if ((i & 1U) == 0U) {
                    add(i, i + 1U, i + 2U);
                } else {
                    add(i + 1U, i, i + 2U);
                }
            }
            break;
        case Renderer3ds::PicaTopology::TriangleFan:
            for (size_t i = 1; i + 1U < count; ++i) {
                add(0U, i, i + 1U);
            }
            break;
        case Renderer3ds::PicaTopology::GeometryShader: return finish(PicaCoverageStatus::Topology);
    }
    coverage.Triangles = static_cast<uint32_t>(triangleCount);
    // Pixel centres closest to the viewport edges are half a pixel (1/size in
    // NDC) inside; the square checked stops a quarter pixel inside the edges
    // (sub-pixel snapping moves a vertex by 1/512 pixel), and its corners
    // must clear every covering edge by four times the vertices' float32
    // error bound (at least 1e-5).
    if (!(viewportWidth >= 1.0) || !(viewportHeight >= 1.0)) {
        return finish(PicaCoverageStatus::Gap);
    }
    const std::array<double, 2> half{1.0 - 0.5 / viewportWidth, 1.0 - 0.5 / viewportHeight};
    const double kEdgeMargin = std::max(1.0e-5, 4.0 * ndcError);
    for (size_t i = 0; i < triangleCount; ++i) {
        if (ContainsSquare(triangles[i], half, kEdgeMargin)) {
            return finish(PicaCoverageStatus::Proven);
        }
    }
    for (size_t i = 0; i < triangleCount; ++i) {
        for (size_t j = i + 1U; j < triangleCount; ++j) {
            // Two triangles sharing an edge whose far vertices lie on either
            // side of it: their union is the quad (o_i, s0, o_j, s1).
            const auto& t = triangles[i];
            const auto& u = triangles[j];
            int shared = 0;
            std::array<Point, 2> edge{};
            Point otherT{};
            Point otherU{};
            for (const Point& p : t) {
                const bool inU = SamePoint(p, u[0]) || SamePoint(p, u[1]) || SamePoint(p, u[2]);
                if (inU && shared < 2) {
                    edge[shared++] = p;
                } else if (!inU) {
                    otherT = p;
                }
            }
            if (shared != 2) {
                continue;
            }
            for (const Point& p : u) {
                if (!SamePoint(p, edge[0]) && !SamePoint(p, edge[1])) {
                    otherU = p;
                }
            }
            std::array<Point, 4> quad{otherT, edge[0], otherU, edge[1]};
            double sign = 0.0;
            bool convex = true;
            for (size_t k = 0; k < 4U; ++k) {
                const double turn = Cross(quad[k], quad[(k + 1U) % 4U], quad[(k + 2U) % 4U]);
                if (!(std::fabs(turn) > 1.0e-9) || (sign != 0.0 && (turn > 0.0) != (sign > 0.0))) {
                    convex = false;
                    break;
                }
                sign = turn;
            }
            if (!convex) {
                continue;
            }
            if (sign < 0.0) {
                std::swap(quad[1], quad[3]);
            }
            if (ContainsSquare(quad, half, kEdgeMargin)) {
                return finish(PicaCoverageStatus::Proven);
            }
        }
    }
    return finish(culledAny ? PicaCoverageStatus::Culled : PicaCoverageStatus::Gap);
}

// ---- the plan -----------------------------------------------------------------

enum class PicaClearDrawAction : uint8_t {
    Draw,       // as recorded (not a reset, or not provable)
    DepthClear, // clear depth, draw with depth writes off
    FullClear,  // clear colour (masked) and depth, skip the draw
};

[[nodiscard]] constexpr const char* PicaClearDrawActionName(PicaClearDrawAction action) noexcept {
    switch (action) {
        case PicaClearDrawAction::Draw: return "draw";
        case PicaClearDrawAction::DepthClear: return "depth_clear";
        case PicaClearDrawAction::FullClear: return "full_clear";
    }
    return "?";
}

// What the backend knows about the draw besides the view.
struct PicaClearDrawContext {
    bool Specialized = false;          // the specialized program from the pack (not the uber)
    bool LightingVariant = false;      // Toon/Per-pixel variant (program.Lit)
    bool WritesFragDepth = false;      // PicaDrawWritesFragDepth
    bool ViewportCoversTarget = false; // viewport and scissor contain the whole target extent
    double ViewportWidth = 0.0;        // target pixels
    double ViewportHeight = 0.0;
    bool CullInvert = false;           // config/deko3d_cull_invert
    bool StencilWrites = false;        // RecordPicaDraw's stencilWrites
};

struct PicaClearDrawPlan {
    PicaClearDrawAction Action = PicaClearDrawAction::Draw;
    bool Reset = false; // depth write, ALWAYS (or test off), over the whole target
    const char* Why = "not a reset";  // what kept the draw (first failed condition)
    const char* ColorWhy = "";        // what kept the colour (DepthClear)
    PicaResetDepth Depth;
    PicaResetCoverage Coverage;
    PicaConstantColor Color;
    PicaAlphaTestOutcome AlphaTest = PicaAlphaTestOutcome::Unknown;
    bool Fog = false;
    bool Lighting = false;
    float ClearDepth = 1.0F;
    std::array<float, 4> ClearColor{};
    uint8_t ColorMask = 0U;
};

// Whether the draw is a depth reset: writes depth, passes every depth test,
// covers the target (viewport and scissor; the geometry is checked later).
[[nodiscard]] inline bool PicaDrawIsDepthReset(const Renderer3ds::PicaDrawView& draw, bool viewportCoversTarget) noexcept {
    return draw.DepthWriteEnabled &&
           (!draw.DepthTestEnabled || draw.DepthCompare == Renderer3ds::PicaCompareFunction::Always) &&
           viewportCoversTarget;
}

[[nodiscard]] inline PicaClearDrawPlan PlanPicaClearDraw(const Renderer3ds::PicaDrawView& draw,
                                                         const PicaClearDrawContext& context) noexcept {
    PicaClearDrawPlan plan;
    plan.ColorMask = static_cast<uint8_t>(draw.ColorWriteMask & 0xFU);
    plan.Reset = PicaDrawIsDepthReset(draw, context.ViewportCoversTarget);
    if (!plan.Reset) {
        return plan;
    }
    plan.Fog = draw.FragmentFeatures.FogMode == 5U;
    plan.Lighting = draw.FragmentFeatures.FragmentLightingEnabled;
    plan.Depth = PlanPicaResetDepth(context.WritesFragDepth, draw.FragmentUniformBytes);
    plan.Coverage = ProvePicaResetCoverage(draw, context.ViewportWidth, context.ViewportHeight, context.CullInvert);
    PicaConstantTevInputs tev;
    const bool tevKnown = ReadPicaConstantTevInputs(draw.FragmentUniformBytes, plan.Lighting, tev);
    if (tevKnown) {
        plan.Color = EvaluatePicaConstantTev(tev);
    }
    int32_t reference = draw.AlphaReference;
    if (draw.FragmentUniformBytes.size() >= Oot3d::kPicaPackedFragmentBaseUniformSize) {
        std::memcpy(&reference, draw.FragmentUniformBytes.data() + Oot3d::kPicaPackedFragmentAlphaReferenceOffset,
                    sizeof(reference));
    }
    plan.AlphaTest = PicaConstantAlphaTest(draw.AlphaTestEnabled, draw.AlphaCompare, reference,
                                           tevKnown ? &plan.Color : nullptr);
    const auto keep = [&](const char* why) {
        plan.Why = why;
        return plan;
    };
    if (!context.Specialized) {
        return keep("not the specialized program");
    }
    if (context.LightingVariant) {
        return keep("lighting variant");
    }
    if (draw.FragmentOperationMode != 0U) {
        return keep("fragment operation mode");
    }
    if (plan.Depth.Form == PicaResetDepthForm::Raster) {
        return keep("raster depth");
    }
    if (plan.Depth.Form != PicaResetDepthForm::ExplicitConstant) {
        return keep("depth follows the raster");
    }
    if (plan.Depth.Constant != 0.0F && plan.Depth.Constant != 1.0F) {
        return keep("depth constant not 0 or 1");
    }
    if (plan.AlphaTest != PicaAlphaTestOutcome::Passes) {
        return keep(plan.AlphaTest == PicaAlphaTestOutcome::Discards ? "alpha test discards" : "alpha test");
    }
    if (plan.Coverage.Status != PicaCoverageStatus::Proven) {
        return keep(PicaCoverageStatusName(plan.Coverage.Status));
    }
    plan.Action = PicaClearDrawAction::DepthClear;
    plan.ClearDepth = plan.Depth.Constant;
    plan.Why = "";
    const auto colorKeep = [&](const char* why) {
        plan.ColorWhy = why;
        return plan;
    };
    if (context.StencilWrites) {
        return colorKeep("stencil writes");
    }
    if (plan.ColorMask == 0U) {
        // Writes depth only: the depth clear is all of it.
        plan.Action = PicaClearDrawAction::FullClear;
        return plan;
    }
    if (!tevKnown) {
        return colorKeep("no uniforms");
    }
    if (plan.Fog) {
        return colorKeep("fog");
    }
    if (!plan.Color.Proven()) {
        return colorKeep(plan.Color.Ambiguous ? "rounding boundary" : PicaColorSourceName(plan.Color.Source));
    }
    if (!Renderer3ds::PicaBlendReplacesDestination(draw.Blend)) {
        return colorKeep("blending");
    }
    if (!draw.Blend.Enabled && draw.LogicOperation != Renderer3ds::PicaLogicOperation::Copy) {
        return colorKeep("logic op");
    }
    plan.Action = PicaClearDrawAction::FullClear;
    for (size_t c = 0; c < 4U; ++c) {
        plan.ClearColor[c] = static_cast<float>(plan.Color.Bytes[c]) / 255.0F;
    }
    return plan;
}

// The `reset draw:` line of logs/deko3d.txt (after "reset draw: "; under
// 440 characters, Log cuts at 511): vertices, topology, culling and the
// coverage proof; depth form (raster, or gl_FragDepth with its scale/offset)
// and the raster depth range of the vertices (-z/w); colour source, mask,
// blending, logic op, fog, lighting, alpha test, stencil; the plan and what
// kept the draw or its colour.
[[nodiscard]] inline std::string FormatPicaResetDraw(const Renderer3ds::PicaDrawView& draw,
                                                     const PicaClearDrawPlan& plan) {
    const auto& c = plan.Coverage;
    char depth[80];
    switch (plan.Depth.Form) {
        case PicaResetDepthForm::Raster: std::snprintf(depth, sizeof(depth), "raster"); break;
        case PicaResetDepthForm::ExplicitConstant:
            std::snprintf(depth, sizeof(depth), "frag %.9g", static_cast<double>(plan.Depth.Constant));
            break;
        case PicaResetDepthForm::ExplicitVarying:
            std::snprintf(depth, sizeof(depth), "frag scale %.9g offset %.9g w %d",
                          static_cast<double>(plan.Depth.Scale), static_cast<double>(plan.Depth.Offset),
                          plan.Depth.WBuffering ? 1 : 0);
            break;
    }
    char vertices[112];
    if (c.DepthKnown) {
        std::snprintf(vertices, sizeof(vertices), "z %.9g..%.9g x %.5g..%.5g y %.5g..%.5g", c.DepthMin, c.DepthMax,
                      c.NdcMin[0], c.NdcMax[0], c.NdcMin[1], c.NdcMax[1]);
    } else {
        std::snprintf(vertices, sizeof(vertices), "z unknown");
    }
    char colour[64];
    if (plan.Color.Proven()) {
        std::snprintf(colour, sizeof(colour), "constant %02x%02x%02x%02x", plan.Color.Bytes[0], plan.Color.Bytes[1],
                      plan.Color.Bytes[2], plan.Color.Bytes[3]);
    } else if (plan.Color.Ambiguous) {
        std::snprintf(colour, sizeof(colour), "constant, rounding boundary at stage %u", plan.Color.Stage);
    } else {
        std::snprintf(colour, sizeof(colour), "%s at stage %u", PicaColorSourceName(plan.Color.Source),
                      plan.Color.Stage);
    }
    static constexpr const char* kTopology[] = {"list", "strip", "fan", "geometry"};
    static constexpr const char* kCull[] = {"none", "keep_cw", "keep_ccw"};
    static constexpr const char* kAlpha[] = {"passes", "discards", "unknown"};
    char line[448];
    std::snprintf(line, sizeof(line),
                  "%u vertices %s (%u triangles) cull %s coverage %s; depth %s test %s, %s; colour %s mask %x "
                  "blend %d logic %u fog %d lighting %d alpha_test %d/%u %s stencil %d; plan %s",
                  c.Vertices, kTopology[static_cast<size_t>(draw.Topology) & 3U], c.Triangles,
                  kCull[std::min<size_t>(static_cast<size_t>(draw.CullMode), 2U)], PicaCoverageStatusName(c.Status),
                  depth, draw.DepthTestEnabled ? "always" : "off", vertices, colour, plan.ColorMask,
                  draw.Blend.Enabled ? 1 : 0, static_cast<unsigned>(draw.LogicOperation), plan.Fog ? 1 : 0,
                  plan.Lighting ? 1 : 0, draw.AlphaTestEnabled ? 1 : 0, static_cast<unsigned>(draw.AlphaCompare),
                  kAlpha[static_cast<size_t>(plan.AlphaTest)], draw.Stencil.Enabled ? 1 : 0,
                  PicaClearDrawActionName(plan.Action));
    std::string text = line;
    if (plan.Why[0] != '\0') {
        text += " (";
        text += plan.Why;
        text += ')';
    } else if (plan.Action == PicaClearDrawAction::DepthClear && plan.ColorWhy[0] != '\0') {
        text += ", colour kept (";
        text += plan.ColorWhy;
        text += ')';
    }
    return text;
}

} // namespace Fast::Deko3d
