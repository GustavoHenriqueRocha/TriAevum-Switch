// config/deko3d_clear_draw (gfx_deko3d_clear_draw.h): the proofs that turn
// the frame's depth reset into fast clears, on synthetic draw states.
//
//   oot3d_deko3d_clear_draw_tests            the tests
//   oot3d_deko3d_clear_draw_tests --emit F   also writes random constant TEV
//       programs as GLSL (the generator's own expression helpers) with the
//       predicted bytes, one JSON object per line, for
//       tests/switch/test_clear_draw_tev.py (glsl_eval.py, binary32).
#include "fast/backends/gfx_deko3d_clear_draw.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "oot3d_native_pica_tev_expressions.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {
using namespace Fast::Deko3d;
namespace R = Fast::Renderer3ds;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

// ---- TEV programs -------------------------------------------------------------

// One stage's words: sources (rgb a,b,c | alpha a,b,c), modifiers, operations, scales.
std::array<uint32_t, 4> Stage(std::array<uint32_t, 3> rgb, std::array<uint32_t, 3> alpha, uint32_t colorOp,
                              uint32_t alphaOp, std::array<uint32_t, 3> rgbMod = {}, std::array<uint32_t, 3> alphaMod = {},
                              uint32_t rgbScale = 0U, uint32_t alphaScale = 0U) {
    std::array<uint32_t, 4> words{};
    for (uint32_t i = 0; i < 3U; ++i) {
        words[0] |= rgb[i] << (i * 4U) | alpha[i] << (16U + i * 4U);
        words[1] |= rgbMod[i] << (i * 4U) | alphaMod[i] << (12U + i * 4U);
    }
    words[2] = colorOp | alphaOp << 16U;
    words[3] = rgbScale | alphaScale << 16U;
    return words;
}

std::array<uint32_t, 4> PassThrough() {
    return Stage({0xF, 0xF, 0xF}, {0xF, 0xF, 0xF}, 0U, 0U);
}

PicaConstantTevInputs ConstantProgram(std::array<float, 4> constant) {
    PicaConstantTevInputs in;
    in.Stages[0] = Stage({0xE, 0xE, 0xE}, {0xE, 0xE, 0xE}, 0U, 0U);
    for (size_t s = 1; s < 6U; ++s) {
        in.Stages[s] = PassThrough();
    }
    in.Constants[0] = constant;
    return in;
}

float Byte(int value) {
    return static_cast<float>(value) / 255.0F;
}

void TestConstantTev() {
    auto in = ConstantProgram({Byte(51), Byte(102), Byte(153), Byte(255)});
    auto out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{51, 102, 153, 255}, "constant colour passes through");

    in.Stages[2] = Stage({0x0, 0xF, 0xF}, {0xF, 0xF, 0xF}, 0U, 0U);
    out = EvaluatePicaConstantTev(in);
    Require(out.Source == PicaColorSource::Primary && out.Stage == 2U, "vertex colour is not constant");

    in = ConstantProgram({Byte(10), Byte(20), Byte(30), Byte(40)});
    in.Stages[1] = Stage({0x3, 0xF, 0xF}, {0xF, 0xF, 0xF}, 2U, 0U, {}, {}); // texture0 + previous
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes[0] == 10U, "a disabled texture reads 0");
    in.TextureEnableMask = 1U;
    out = EvaluatePicaConstantTev(in);
    Require(out.Source == PicaColorSource::Texture && out.Stage == 1U, "an enabled texture is not constant");
    in.TextureEnableMask = 2U;
    Require(EvaluatePicaConstantTev(in).Proven(), "another texture unit enabled: still constant");

    in = ConstantProgram({0, 0, 0, 0});
    in.Stages[0] = Stage({0x1, 0xE, 0xE}, {0xE, 0xE, 0xE}, 2U, 0U);
    Require(EvaluatePicaConstantTev(in).Proven(), "lighting colour without lighting is 0");
    in.LightingEnabled = true;
    Require(EvaluatePicaConstantTev(in).Source == PicaColorSource::Lighting, "lighting colour with lighting");
    in.LightingEnabled = false;
    in.Stages[0] = Stage({0x6, 0xE, 0xE}, {0xE, 0xE, 0xE}, 2U, 0U);
    Require(EvaluatePicaConstantTev(in).Proven(), "procedural texture off is 0");
    in.ProcTexEnabled = true;
    Require(EvaluatePicaConstantTev(in).Source == PicaColorSource::ProcTex, "procedural texture on");

    // Add-signed of two grid values lands on a half: on the GPU it rounds
    // either way, so it is not provable.
    in = ConstantProgram({Byte(100), Byte(100), Byte(100), Byte(255)});
    in.Stages[0] = Stage({0xE, 0xE, 0xE}, {0xE, 0xE, 0xE}, 3U, 0U);
    in.Constants[0] = {Byte(100), Byte(100), Byte(100), Byte(255)};
    out = EvaluatePicaConstantTev(in);
    Require(!out.Proven() && out.Ambiguous && out.Stage == 0U, "add-signed half is ambiguous");

    // Buffer: stage 1 reads combiner_buffer = the buffer colour (stage 0 reads 0).
    in = ConstantProgram({0, 0, 0, 0});
    in.Stages[0] = Stage({0xD, 0xF, 0xF}, {0xD, 0xF, 0xF}, 0U, 0U);
    in.Stages[1] = Stage({0xD, 0xF, 0xF}, {0xD, 0xF, 0xF}, 0U, 0U);
    in.BufferColor = {Byte(7), Byte(8), Byte(9), Byte(10)};
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{7, 8, 9, 10}, "buffer colour from stage 1");
    in.Stages[1] = PassThrough();
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{0, 0, 0, 0}, "stage 0 reads an empty buffer");
    // Stage 1 writes the buffer; stage 3 reads it (one stage later than previous).
    in = ConstantProgram({Byte(1), Byte(2), Byte(3), Byte(4)});
    in.Stages[1] = Stage({0xE, 0xF, 0xF}, {0xE, 0xF, 0xF}, 0U, 0U);
    in.Constants[1] = {Byte(50), Byte(60), Byte(70), Byte(80)};
    in.Stages[2] = Stage({0xD, 0xF, 0xF}, {0xD, 0xF, 0xF}, 0U, 0U);
    in.Stages[3] = Stage({0xD, 0xF, 0xF}, {0xD, 0xF, 0xF}, 0U, 0U);
    in.BufferControl = (1U << 9U) | (1U << 13U);
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{50, 60, 70, 80}, "buffer update seen by stage 3");

    // Scale 4 clamps; in the last stage the alpha then reads exactly 255. A
    // later pass-through stage re-rounds 255/255, which may truncate to 254.
    in = ConstantProgram({Byte(100), Byte(10), Byte(0), Byte(100)});
    in.Stages[0] = Stage({0xE, 0xE, 0xE}, {0xE, 0xE, 0xE}, 0U, 0U, {}, {}, 2U, 2U);
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{255, 40, 0, 255} && !out.AlphaClamped,
            "scale 4 clamps, then passes through");
    in.Stages[0] = PassThrough();
    in.Stages[5] = Stage({0xE, 0xE, 0xE}, {0xE, 0xE, 0xE}, 0U, 0U, {}, {}, 2U, 2U);
    in.Constants[5] = {Byte(100), Byte(10), Byte(0), Byte(100)};
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{255, 40, 0, 255} && out.AlphaClamped,
            "scale 4 in the last stage clamps the alpha to exactly 1");

    // Modifiers and operations against hand values.
    in = ConstantProgram({Byte(200), Byte(100), Byte(50), Byte(30)});
    in.Constants[1] = {Byte(10), Byte(20), Byte(30), Byte(40)};
    in.Stages[1] = Stage({0xF, 0xE, 0xF}, {0xF, 0xE, 0xF}, 5U, 2U, {1U, 0U, 0U}, {2U, 0U, 0U}); // (1-prev)-c ; prev.r+c.a
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes == std::array<uint8_t, 4>{45, 135, 175, 240}, "modifiers, subtract, add");
    in.Stages[1] = Stage({0xF, 0xE, 0xF}, {0xF, 0xE, 0xF}, 7U, 0U); // dot3 rgba: alpha = colour
    out = EvaluatePicaConstantTev(in);
    Require(out.Proven() && out.Bytes[3] == out.Bytes[0], "dot3 rgba copies the colour into alpha");

    // Unsupported operand.
    in.Stages[1] = Stage({0xF, 0xE, 0xF}, {0xF, 0xE, 0xF}, 1U, 1U, {6U, 0U, 0U});
    Require(EvaluatePicaConstantTev(in).Source == PicaColorSource::Unsupported, "unsupported modifier");
}

// ---- float32 models of the specialized program --------------------------------

// The generated GLSL in binary32 with the choices a GPU compiler may make:
// byteround's division by 255 or multiplication by its reciprocal; fma/mix
// fused or not, mix as x*(1-a)+y*a or x+(y-x)*a; dot summed in order or with
// fused steps.
struct Variant {
    bool Reciprocal = false;
    bool Fused = false;
    bool MixLerp = false;
};

float RoundByte(float x, const Variant& v) {
    const float c = std::min(std::max(x, 0.0F), 1.0F);
    volatile float scaled = c * 255.0F;
    volatile float biased = scaled + 0.5F;
    const float f = std::floor(biased);
    return v.Reciprocal ? f * (1.0F / 255.0F) : f / 255.0F;
}

std::array<uint8_t, 4> EvaluateFloat(const PicaConstantTevInputs& in, const Variant& v) {
    using V4 = std::array<float, 4>;
    V4 previous{}, buffer{}, next = in.BufferColor;
    const auto mul = [](float a, float b) {
        volatile float p = a * b;
        return static_cast<float>(p);
    };
    const auto fma = [&](float a, float b, float c) {
        if (v.Fused) {
            return std::fma(a, b, c);
        }
        volatile float s = mul(a, b) + c;
        return static_cast<float>(s);
    };
    const auto mix = [&](float x, float y, float a) {
        if (v.MixLerp) {
            volatile float d = y - x;
            return fma(d, a, x);
        }
        volatile float s = mul(x, 1.0F - a);
        return fma(y, a, s);
    };
    for (size_t stage = 0; stage < 6U; ++stage) {
        const auto& w = in.Stages[stage];
        const auto source = [&](uint32_t sel) -> V4 {
            switch (sel) {
                case 0xDU: return buffer;
                case 0xEU: return in.Constants[stage];
                case 0xFU: return previous;
                default: return V4{};
            }
        };
        std::array<uint32_t, 3> cs{w[0] & 15U, (w[0] >> 4U) & 15U, (w[0] >> 8U) & 15U};
        std::array<uint32_t, 3> as{(w[0] >> 16U) & 15U, (w[0] >> 20U) & 15U, (w[0] >> 24U) & 15U};
        if (stage == 0U) {
            for (int i = 0; i < 2; ++i) {
                if (cs[i] == 15U) cs[i] = cs[2];
                if (as[i] == 15U) as[i] = as[2];
            }
        }
        std::array<std::array<float, 3>, 3> col{};
        std::array<float, 3> alp{};
        for (uint32_t i = 0; i < 3U; ++i) {
            const V4 s = source(cs[i]);
            const uint32_t m = (w[1] >> (i * 4U)) & 15U;
            for (int c = 0; c < 3; ++c) {
                float x = s[c];
                switch (m & 14U) {
                    case 2U: x = s[3]; break;
                    case 4U: x = s[0]; break;
                    case 8U: x = s[1]; break;
                    case 12U: x = s[2]; break;
                    default: break;
                }
                col[i][c] = (m & 1U) ? 1.0F - x : x;
            }
            const V4 a = source(as[i]);
            const uint32_t am = (w[1] >> (12U + i * 4U)) & 7U;
            static constexpr int kCh[4] = {3, 0, 1, 2};
            const float x = a[kCh[am >> 1U]];
            alp[i] = (am & 1U) ? 1.0F - x : x;
        }
        const uint32_t op = w[2] & 15U;
        const uint32_t aop = (w[2] >> 16U) & 15U;
        std::array<float, 3> cr{};
        if (op == 6U || op == 7U) {
            std::array<float, 3> d0{}, d1{};
            for (int c = 0; c < 3; ++c) {
                d0[c] = col[0][c] - 0.5F;
                d1[c] = col[1][c] - 0.5F;
            }
            float dot = mul(d0[0], d1[0]);
            dot = fma(d0[1], d1[1], dot);
            dot = fma(d0[2], d1[2], dot);
            cr.fill(mul(dot, 4.0F));
        } else {
            for (int c = 0; c < 3; ++c) {
                const float a = col[0][c], b = col[1][c], k = col[2][c];
                switch (op) {
                    case 0U: cr[c] = a; break;
                    case 1U: cr[c] = mul(a, b); break;
                    case 2U: cr[c] = a + b; break;
                    case 3U: { volatile float s = a + b; cr[c] = s - 0.5F; break; }
                    case 4U: cr[c] = mix(b, a, k); break;
                    case 5U: cr[c] = a - b; break;
                    case 8U: cr[c] = fma(a, b, k); break;
                    case 9U: { volatile float s = a + b; cr[c] = mul(std::min(static_cast<float>(s), 1.0F), k); break; }
                    default: break;
                }
            }
        }
        std::array<float, 3> co{};
        for (int c = 0; c < 3; ++c) co[c] = RoundByte(cr[c], v);
        float ao = 0.0F;
        if (op == 7U) {
            ao = RoundByte(co[0], v);
        } else {
            const float a = alp[0], b = alp[1], k = alp[2];
            float r = 0.0F;
            switch (aop) {
                case 0U: r = a; break;
                case 1U: r = mul(a, b); break;
                case 2U: r = a + b; break;
                case 3U: { volatile float s = a + b; r = s - 0.5F; break; }
                case 4U: r = mix(b, a, k); break;
                case 5U: r = a - b; break;
                case 8U: r = fma(a, b, k); break;
                case 9U: { volatile float s = a + b; r = mul(std::min(static_cast<float>(s), 1.0F), k); break; }
                default: break;
            }
            ao = RoundByte(r, v);
        }
        const uint32_t cb = w[3] & 3U, ab = (w[3] >> 16U) & 3U;
        const float cscale = cb < 3U ? static_cast<float>(1U << cb) : 1.0F;
        const float ascale = ab < 3U ? static_cast<float>(1U << ab) : 1.0F;
        for (int c = 0; c < 3; ++c) previous[c] = std::min(std::max(mul(co[c], cscale), 0.0F), 1.0F);
        previous[3] = std::min(std::max(mul(ao, ascale), 0.0F), 1.0F);
        buffer = next;
        if (stage < 4U) {
            if (in.BufferControl & (1U << (8U + stage))) for (int c = 0; c < 3; ++c) next[c] = previous[c];
            if (in.BufferControl & (1U << (12U + stage))) next[3] = previous[3];
        }
    }
    // pica_color = byteround(combiner_output), stored as unorm8 (round to nearest).
    std::array<uint8_t, 4> bytes{};
    for (int c = 0; c < 4; ++c) {
        bytes[c] = static_cast<uint8_t>(std::lround(RoundByte(previous[c], v) * 255.0F));
    }
    return bytes;
}

PicaConstantTevInputs RandomConstantProgram(std::mt19937& rng, bool gridConstants) {
    static constexpr uint32_t kSources[] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xD, 0xE, 0xE, 0xF, 0xF, 0xF};
    static constexpr uint32_t kColorMods[] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x8, 0x9, 0xC, 0xD};
    static constexpr uint32_t kColorOps[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static constexpr uint32_t kAlphaOps[] = {0, 1, 2, 3, 4, 5, 8, 9};
    const auto pick = [&](const auto& table) {
        return table[std::uniform_int_distribution<size_t>(0, std::size(table) - 1U)(rng)];
    };
    PicaConstantTevInputs in;
    for (auto& stage : in.Stages) {
        // Mostly pass-through, like the game's programs.
        if (std::uniform_int_distribution<int>(0, 3)(rng) == 0) {
            stage = PassThrough();
            continue;
        }
        std::array<uint32_t, 3> rgb{pick(kSources), pick(kSources), pick(kSources)};
        std::array<uint32_t, 3> alpha{pick(kSources), pick(kSources), pick(kSources)};
        std::array<uint32_t, 3> rm{pick(kColorMods), pick(kColorMods), pick(kColorMods)};
        std::array<uint32_t, 3> am{};
        for (auto& m : am) m = std::uniform_int_distribution<uint32_t>(0, 7)(rng);
        stage = Stage(rgb, alpha, pick(kColorOps), pick(kAlphaOps), rm, am,
                      std::uniform_int_distribution<uint32_t>(0, 3)(rng), std::uniform_int_distribution<uint32_t>(0, 3)(rng));
    }
    in.BufferControl = std::uniform_int_distribution<uint32_t>(0, 255)(rng) << 8U;
    const auto value = [&] {
        return gridConstants ? Byte(std::uniform_int_distribution<int>(0, 255)(rng))
                             : std::uniform_real_distribution<float>(0.0F, 1.0F)(rng);
    };
    for (auto& constant : in.Constants) {
        for (auto& c : constant) c = value();
    }
    for (auto& c : in.BufferColor) c = value();
    return in;
}

void TestConstantTevAgainstFloat32() {
    std::mt19937 rng(20261107U);
    size_t proven = 0, ambiguous = 0, total = 0;
    for (int round = 0; round < 200000; ++round) {
        const auto in = RandomConstantProgram(rng, round % 4 != 0);
        const auto out = EvaluatePicaConstantTev(in);
        ++total;
        if (out.Ambiguous) {
            ++ambiguous;
            continue;
        }
        if (!out.Proven()) {
            continue; // unsupported combination
        }
        ++proven;
        for (int bits = 0; bits < 8; ++bits) {
            const Variant v{(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0};
            if (EvaluateFloat(in, v) != out.Bytes) {
                std::fprintf(stderr, "round %d variant %d: predicted %02x%02x%02x%02x\n", round, bits, out.Bytes[0],
                             out.Bytes[1], out.Bytes[2], out.Bytes[3]);
                Require(false, "a proven constant colour differs in float32");
            }
        }
    }
    std::printf("constant TEV vs float32: %zu programs, %zu proven (all 8 arithmetic variants agree), %zu ambiguous\n",
                total, proven, ambiguous);
    Require(proven > total / 2U, "most constant programs are provable");
}

// ---- alpha test ---------------------------------------------------------------

void TestAlphaTest() {
    using C = R::PicaCompareFunction;
    PicaConstantColor color;
    color.Source = PicaColorSource::Constant;
    color.Bytes = {0, 0, 0, 100};
    Require(PicaConstantAlphaTest(false, C::Never, 0, nullptr) == PicaAlphaTestOutcome::Passes, "off passes");
    Require(PicaConstantAlphaTest(true, C::Always, 0, nullptr) == PicaAlphaTestOutcome::Passes, "always passes");
    Require(PicaConstantAlphaTest(true, C::Never, 0, &color) == PicaAlphaTestOutcome::Discards, "never discards");
    Require(PicaConstantAlphaTest(true, C::Greater, 50, nullptr) == PicaAlphaTestOutcome::Unknown, "varying alpha");
    Require(PicaConstantAlphaTest(true, C::Greater, 98, &color) == PicaAlphaTestOutcome::Passes, "99 or 100 > 98");
    Require(PicaConstantAlphaTest(true, C::Greater, 99, &color) == PicaAlphaTestOutcome::Unknown,
            "99 > 99 fails, 100 passes: unknown");
    Require(PicaConstantAlphaTest(true, C::Greater, 100, &color) == PicaAlphaTestOutcome::Discards, "never > 100");
    Require(PicaConstantAlphaTest(true, C::Equal, 100, &color) == PicaAlphaTestOutcome::Unknown, "equal: truncation");
    color.Bytes[3] = 0;
    Require(PicaConstantAlphaTest(true, C::Equal, 0, &color) == PicaAlphaTestOutcome::Passes, "0 is exact");
    color.Bytes[3] = 255;
    color.AlphaClamped = true;
    Require(PicaConstantAlphaTest(true, C::GreaterOrEqual, 255, &color) == PicaAlphaTestOutcome::Passes,
            "clamped 1.0 is exact");
    color.AlphaClamped = false;
    Require(PicaConstantAlphaTest(true, C::GreaterOrEqual, 255, &color) == PicaAlphaTestOutcome::Unknown,
            "255/255 may truncate to 254");
}

// ---- synthetic draws ----------------------------------------------------------

struct SyntheticDraw {
    std::vector<uint8_t> Vertices;
    std::vector<uint8_t> VertexUniforms = std::vector<uint8_t>(Fast::Oot3d::kPicaPackedVertexUniformSize, 0U);
    std::vector<uint8_t> FragmentUniforms = std::vector<uint8_t>(Fast::Oot3d::kPicaPackedFragmentUniformSize, 0U);
    std::array<R::PicaVertexBindingView, 1> Bindings{};
    std::array<R::PicaVertexAttributeView, 1> Attributes{};
    R::PicaDrawView View;

    void SetRow(uint32_t uniform, std::array<float, 4> row) {
        std::memcpy(VertexUniforms.data() + Fast::Oot3d::kPicaPackedVertexFloatOffset + uniform * 16U, row.data(), 16U);
    }
    void SetFragmentFloat(size_t offset, float value) {
        std::memcpy(FragmentUniforms.data() + offset, &value, sizeof(value));
    }
    void SetTev(const PicaConstantTevInputs& in) {
        using namespace Fast::Oot3d;
        for (size_t s = 0; s < 6U; ++s) {
            std::memcpy(FragmentUniforms.data() + kPicaPackedFragmentTevProgramOffset + s * 16U, in.Stages[s].data(), 16U);
            std::memcpy(FragmentUniforms.data() + kPicaPackedFragmentTevConstantsOffset + s * 16U,
                        in.Constants[s].data(), 16U);
        }
        std::memcpy(FragmentUniforms.data() + kPicaPackedFragmentCombinerBufferOffset, in.BufferColor.data(), 16U);
        std::array<uint32_t, 4> control{in.BufferControl, 0U,
                                        in.TextureEnableMask | (in.ProcTexEnabled ? 1U << 10U : 0U), 0U};
        std::memcpy(FragmentUniforms.data() + kPicaPackedFragmentControlOffset, control.data(), sizeof(control));
    }
    void SetPositions(const std::vector<std::array<float, 3>>& positions) {
        Vertices.resize(positions.size() * 12U);
        std::memcpy(Vertices.data(), positions.data(), Vertices.size());
        Bindings[0].Bytes = Vertices;
        View.VertexCount = static_cast<uint32_t>(positions.size());
    }

    // Identity model/view, projection rows given (default: identity, w = 1).
    explicit SyntheticDraw(const std::vector<std::array<float, 3>>& positions) {
        auto& t = View.TemporalVertexProgram.Hooks.Transform;
        t.Operation = R::PicaVertexTransformOperation::ModelViewProjection3x4;
        t.ProjectionFirstUniform = 0U;
        t.ProjectionRowCount = 4U;
        t.ViewFirstUniform = 4U;
        t.ViewRowCount = 3U;
        t.ModelFirstUniform = 8U;
        t.ModelRowCount = 3U;
        t.PositionInputRegister = 0U;
        t.NormalInputRegister = 1U;
        for (uint32_t row = 0; row < 4U; ++row) {
            std::array<float, 4> r{};
            r[row] = 1.0F;
            SetRow(row, r);
            if (row < 3U) {
                SetRow(4U + row, r);
                SetRow(8U + row, r);
            }
        }
        Bindings[0] = {0U, 12U, false, {}};
        Attributes[0] = {0U, 0U, R::PicaVertexFormat::Float, 3U, 0U};
        View.VertexBindings = Bindings;
        View.VertexAttributes = Attributes;
        View.Topology = R::PicaTopology::TriangleStrip;
        View.DepthWriteEnabled = true;
        View.DepthTestEnabled = true;
        View.DepthCompare = R::PicaCompareFunction::Always;
        View.ColorWriteMask = 0xFU;
        SetPositions(positions);
        // Explicit depth: scale 0, offset 1 (gl_FragDepth = 1 everywhere).
        SetFragmentFloat(Fast::Oot3d::kPicaPackedFragmentDepthScaleOffset, 0.0F);
        SetFragmentFloat(Fast::Oot3d::kPicaPackedFragmentDepthOffsetOffset, 1.0F);
        SetTev(ConstantProgram({Byte(20), Byte(40), Byte(60), Byte(255)}));
        Refresh();
    }
    void Refresh() {
        View.VertexUniformBytes = VertexUniforms;
        View.FragmentUniformBytes = FragmentUniforms;
        Bindings[0].Bytes = Vertices;
    }
};

std::vector<std::array<float, 3>> Quad(float z, float extent = 1.0F) {
    // Strip order: (-,-) (+,-) (-,+) (+,+): both triangles counter-clockwise in GL winding.
    return {{-extent, -extent, z}, {extent, -extent, z}, {-extent, extent, z}, {extent, extent, z}};
}

PicaClearDrawContext ResetContext() {
    PicaClearDrawContext context;
    context.Specialized = true;
    context.WritesFragDepth = true;
    context.ViewportCoversTarget = true;
    context.ViewportWidth = 1280.0;
    context.ViewportHeight = 720.0;
    return context;
}

void TestCoverage() {
    {
        SyntheticDraw draw(Quad(-0.5F));
        const auto c = ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false);
        Require(c.Status == PicaCoverageStatus::Proven && c.Triangles == 2U, "full-screen strip quad");
        Require(c.DepthKnown && std::fabs(c.DepthMin - 0.5) < 1e-12 && std::fabs(c.DepthMax - 0.5) < 1e-12,
                "vertex depth -z/w");
    }
    {
        // Exactly on the far plane (z/w = -1): float32 may clip it.
        SyntheticDraw draw(Quad(-1.0F));
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::NearClipPlane,
                "a quad on the far plane is not provable");
        SyntheticDraw snapped(Quad(-1.000005F));
        Require(ProvePicaResetCoverage(snapped.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "beyond the far plane by less than 1e-5: snapped onto it");
        SyntheticDraw beyond(Quad(-1.0001F));
        Require(ProvePicaResetCoverage(beyond.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::NearClipPlane,
                "beyond the snap: clipped");
        SyntheticDraw front(Quad(0.25F));
        Require(ProvePicaResetCoverage(front.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::NearClipPlane,
                "in front of the near plane");
    }
    {
        SyntheticDraw draw(Quad(-0.5F, 0.9995F));
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Gap,
                "a quad short of the edge pixel centres");
        SyntheticDraw edge(Quad(-0.5F, 1.0F));
        Require(ProvePicaResetCoverage(edge.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "a quad on the viewport edges covers every pixel centre");
    }
    {
        // Bow tie: the strip's triangles overlap and leave two gaps.
        SyntheticDraw draw({{-1, -1, -0.5F}, {1, -1, -0.5F}, {1, 1, -0.5F}, {-1, 1, -0.5F}});
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Gap,
                "bow-tie strip");
        draw.View.Topology = R::PicaTopology::TriangleFan;
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "the same corners as a fan");
    }
    {
        SyntheticDraw draw({{-1, -1, -0.5F}, {1, -1, -0.5F}, {1, 1, -0.5F}, {-1, -1, -0.5F}, {1, 1, -0.5F},
                            {-1, 1, -0.5F}});
        draw.View.Topology = R::PicaTopology::TriangleList;
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "two listed triangles sharing the diagonal by position");
        SyntheticDraw big({{-1, -1, -0.5F}, {3, -1, -0.5F}, {-1, 3, -0.5F}});
        big.View.Topology = R::PicaTopology::TriangleList;
        Require(ProvePicaResetCoverage(big.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "one large triangle");
    }
    {
        SyntheticDraw draw(Quad(-0.5F));
        draw.View.CullMode = R::NativeCullMode::KeepCounterClockwise;
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Proven,
                "front-facing under culling");
        draw.View.CullMode = R::NativeCullMode::KeepClockwise;
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::Culled,
                "back-facing under culling");
        draw.View.CullMode = R::NativeCullMode::KeepCounterClockwise;
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, true).Status == PicaCoverageStatus::Culled,
                "culling with config/deko3d_cull_invert is not modelled");
    }
    {
        SyntheticDraw draw(Quad(-0.5F));
        draw.SetRow(3U, {0.0F, 0.0F, 0.0F, -1.0F});
        draw.Refresh();
        Require(ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::BehindEye,
                "w <= 0");
        SyntheticDraw none(Quad(-0.5F));
        none.View.TemporalVertexProgram.Hooks.Transform = {};
        Require(ProvePicaResetCoverage(none.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::NoTransform,
                "no typed transform");
        SyntheticDraw many(Quad(-0.5F));
        many.View.VertexCount = 20U;
        Require(ProvePicaResetCoverage(many.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::VertexCount,
                "too many vertices");
        SyntheticDraw shortData(Quad(-0.5F));
        shortData.View.VertexCount = 5U;
        Require(ProvePicaResetCoverage(shortData.View, 1280.0, 720.0, false).Status == PicaCoverageStatus::VertexData,
                "vertex outside its bytes");
    }
    {
        // A perspective projection: x, y and w scaled by 2 keep NDC; z/w from row 2.
        SyntheticDraw draw(Quad(-0.25F));
        draw.SetRow(0U, {2.0F, 0.0F, 0.0F, 0.0F});
        draw.SetRow(1U, {0.0F, 2.0F, 0.0F, 0.0F});
        draw.SetRow(3U, {0.0F, 0.0F, 0.0F, 2.0F});
        draw.Refresh();
        const auto c = ProvePicaResetCoverage(draw.View, 1280.0, 720.0, false);
        Require(c.Status == PicaCoverageStatus::Proven && std::fabs(c.DepthMin - 0.125) < 1e-12, "scaled w");
    }
}

void TestDepth() {
    std::vector<uint8_t> uniforms(Fast::Oot3d::kPicaPackedFragmentUniformSize, 0U);
    const auto set = [&](float scale, float offset, int32_t w) {
        std::memcpy(uniforms.data() + Fast::Oot3d::kPicaPackedFragmentDepthScaleOffset, &scale, 4U);
        std::memcpy(uniforms.data() + Fast::Oot3d::kPicaPackedFragmentDepthOffsetOffset, &offset, 4U);
        std::memcpy(uniforms.data() + Fast::Oot3d::kPicaPackedFragmentWBufferingOffset, &w, 4U);
    };
    set(-1.0F, 0.0F, 0);
    Require(PlanPicaResetDepth(false, uniforms).Form == PicaResetDepthForm::Raster, "raster depth");
    Require(PlanPicaResetDepth(true, uniforms).Form == PicaResetDepthForm::ExplicitVarying, "explicit, scale -1");
    set(0.0F, 1.0F, 0);
    auto d = PlanPicaResetDepth(true, uniforms);
    Require(d.Form == PicaResetDepthForm::ExplicitConstant && d.Constant == 1.0F, "scale 0: the offset");
    set(0.0F, 1.0F, 1);
    Require(PlanPicaResetDepth(true, uniforms).Form == PicaResetDepthForm::ExplicitVarying, "w-buffered offset 1");
    set(0.0F, -0.0F, 1);
    d = PlanPicaResetDepth(true, uniforms);
    Require(d.Form == PicaResetDepthForm::ExplicitConstant && d.Constant == 0.0F && !std::signbit(d.Constant),
            "w-buffered offset 0 stays 0");
    Require(PicaDrawWritesFragDepth(false, false, 0.0F, 1.0F), "scale 0 is the explicit form");
}

void TestPlan() {
    {
        SyntheticDraw draw(Quad(-0.5F));
        auto plan = PlanPicaClearDraw(draw.View, ResetContext());
        Require(plan.Action == PicaClearDrawAction::FullClear && plan.ClearDepth == 1.0F, "a constant reset");
        Require(plan.ClearColor[0] == Byte(20) && plan.ClearColor[1] == Byte(40) && plan.ClearColor[3] == 1.0F,
                "the clear colour is the bytes / 255");
        const std::string line = FormatPicaResetDraw(draw.View, plan);
        Require(line.size() < 480U && line.find("plan full_clear") != std::string::npos &&
                    line.find("constant 14283cff") != std::string::npos,
                "reset line");
    }
    {
        SyntheticDraw draw(Quad(-0.5F));
        draw.View.Blend.Enabled = true;
        draw.View.Blend.DestRgb = R::NativeBlendFactor::OneMinusSourceAlpha;
        auto plan = PlanPicaClearDraw(draw.View, ResetContext());
        Require(plan.Action == PicaClearDrawAction::DepthClear && std::string(plan.ColorWhy) == "blending",
                "blending keeps the colour draw");
        draw.View.Blend = {};
        draw.View.Blend.Enabled = true; // ONE/ZERO add replaces
        Require(PlanPicaClearDraw(draw.View, ResetContext()).Action == PicaClearDrawAction::FullClear,
                "ONE/ZERO blending replaces");
        draw.View.Blend.Enabled = false;
        draw.View.LogicOperation = R::PicaLogicOperation::Xor;
        Require(PlanPicaClearDraw(draw.View, ResetContext()).Action == PicaClearDrawAction::DepthClear, "logic op");
        draw.View.LogicOperation = R::PicaLogicOperation::Copy;
        auto context = ResetContext();
        context.StencilWrites = true;
        Require(PlanPicaClearDraw(draw.View, context).Action == PicaClearDrawAction::DepthClear, "stencil writes");
        draw.View.FragmentFeatures.FogMode = 5U;
        Require(std::string(PlanPicaClearDraw(draw.View, ResetContext()).ColorWhy) == "fog", "fog");
        draw.View.FragmentFeatures.FogMode = 0U;
        auto tev = ConstantProgram({0, 0, 0, 0});
        tev.Stages[0] = Stage({0x0, 0xE, 0xE}, {0xE, 0xE, 0xE}, 0U, 0U);
        draw.SetTev(tev);
        draw.Refresh();
        plan = PlanPicaClearDraw(draw.View, ResetContext());
        Require(plan.Action == PicaClearDrawAction::DepthClear && std::string(plan.ColorWhy) == "vertex_colour",
                "vertex colour keeps the colour draw");
        const std::string line = FormatPicaResetDraw(draw.View, plan);
        Require(line.find("plan depth_clear, colour kept (vertex_colour)") != std::string::npos, "depth-only line");
    }
    {
        SyntheticDraw draw(Quad(-0.5F));
        auto context = ResetContext();
        context.WritesFragDepth = false;
        auto plan = PlanPicaClearDraw(draw.View, context);
        Require(plan.Action == PicaClearDrawAction::Draw && std::string(plan.Why) == "raster depth", "raster reset");
        context = ResetContext();
        context.Specialized = false;
        Require(std::string(PlanPicaClearDraw(draw.View, context).Why) == "not the specialized program", "uber");
        context = ResetContext();
        context.LightingVariant = true;
        Require(std::string(PlanPicaClearDraw(draw.View, context).Why) == "lighting variant", "lit");
        draw.SetFragmentFloat(Fast::Oot3d::kPicaPackedFragmentDepthOffsetOffset, 0.5F);
        draw.Refresh();
        Require(std::string(PlanPicaClearDraw(draw.View, ResetContext()).Why) == "depth constant not 0 or 1",
                "depth 0.5");
        draw.SetFragmentFloat(Fast::Oot3d::kPicaPackedFragmentDepthOffsetOffset, 0.0F);
        draw.Refresh();
        draw.View.AlphaTestEnabled = true;
        draw.View.AlphaCompare = R::PicaCompareFunction::Greater;
        draw.View.AlphaReference = 0U;
        plan = PlanPicaClearDraw(draw.View, ResetContext());
        Require(plan.Action == PicaClearDrawAction::FullClear && plan.ClearDepth == 0.0F,
                "alpha 255 > 0 always passes (reference read from the uniforms)");
        int32_t reference = 254;
        std::memcpy(draw.FragmentUniforms.data() + Fast::Oot3d::kPicaPackedFragmentAlphaReferenceOffset, &reference, 4U);
        draw.Refresh();
        Require(std::string(PlanPicaClearDraw(draw.View, ResetContext()).Why) == "alpha test",
                "alpha 255 > 254 (the uniform's reference): 255/255 may truncate to 254");
        reference = 255;
        std::memcpy(draw.FragmentUniforms.data() + Fast::Oot3d::kPicaPackedFragmentAlphaReferenceOffset, &reference, 4U);
        draw.Refresh();
        Require(std::string(PlanPicaClearDraw(draw.View, ResetContext()).Why) == "alpha test discards",
                "alpha 255 > 255 never passes");
    }
    {
        SyntheticDraw draw(Quad(-0.5F));
        draw.View.DepthCompare = R::PicaCompareFunction::LessOrEqual;
        Require(!PlanPicaClearDraw(draw.View, ResetContext()).Reset, "a tested draw is not a reset");
        draw.View.DepthTestEnabled = false;
        Require(PlanPicaClearDraw(draw.View, ResetContext()).Reset, "test off with writes is a reset");
        draw.View.DepthWriteEnabled = false;
        Require(!PlanPicaClearDraw(draw.View, ResetContext()).Reset, "no depth write: not a reset");
        auto context = ResetContext();
        context.ViewportCoversTarget = false;
        draw.View.DepthWriteEnabled = true;
        Require(!PlanPicaClearDraw(draw.View, context).Reset, "partial viewport: not a reset");
    }
}

// ---- GLSL for tests/switch/test_clear_draw_tev.py -------------------------------

// The generator's stage loop (oot3d_native_pica_fragment_shader_gen.cpp,
// GenerateOot3dPicaFragmentShader) for the constant sources, with its
// TevExpressions helpers.
std::string ConstantSource(uint32_t source, size_t stage, const PicaConstantTevInputs& in) {
    switch (source) {
        case 0x1U: return "primary_fragment_color";
        case 0x2U: return "secondary_fragment_color";
        case 0x3U: case 0x4U: case 0x5U: case 0x6U: return "vec4(0.0)"; // disabled
        case 0xDU: return "combiner_buffer";
        case 0xEU: return "fragment_uniforms.tev_constants[" + std::to_string(stage) + "]";
        case 0xFU: return "combiner_output";
        default: (void)in; return "vec4(0.0)";
    }
}

std::string TevGlsl(const PicaConstantTevInputs& in) {
    namespace T = Oot3dNativeGame::TevExpressions;
    std::string text = "#version 450\n"
                       "layout(set=0,binding=4,std140) uniform PicaFragmentUniforms {\n"
                       "    vec4 tev_constants[6];\n"
                       "    vec4 combiner_buffer_color;\n"
                       "} fragment_uniforms;\n";
    text += T::ByteRoundHelpers;
    text += "vec4 pica_constant_tev() {\n"
            "    vec4 primary_fragment_color = vec4(0.0);\n"
            "    vec4 secondary_fragment_color = vec4(0.0);\n"
            "    vec4 combiner_buffer = vec4(0.0);\n"
            "    vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;\n"
            "    vec4 combiner_output = vec4(0.0);\n";
    bool supported = true;
    for (size_t stage = 0; stage < 6U; ++stage) {
        const auto& w = in.Stages[stage];
        std::array<uint32_t, 3> cs{w[0] & 15U, (w[0] >> 4U) & 15U, (w[0] >> 8U) & 15U};
        std::array<uint32_t, 3> as{(w[0] >> 16U) & 15U, (w[0] >> 20U) & 15U, (w[0] >> 24U) & 15U};
        if (stage == 0U) {
            for (int i = 0; i < 2; ++i) {
                if (cs[i] == 15U) cs[i] = cs[2];
                if (as[i] == 15U) as[i] = as[2];
            }
        }
        std::array<std::string, 3> cv, av;
        for (uint32_t i = 0; i < 3U; ++i) {
            cv[i] = T::ColorModifier(ConstantSource(cs[i], stage, in), static_cast<uint8_t>((w[1] >> (i * 4U)) & 15U),
                                     supported);
            av[i] = T::AlphaModifier(ConstantSource(as[i], stage, in),
                                     static_cast<uint8_t>((w[1] >> (12U + i * 4U)) & 7U), supported);
        }
        const uint8_t op = static_cast<uint8_t>(w[2] & 15U);
        const uint8_t aop = static_cast<uint8_t>((w[2] >> 16U) & 15U);
        const std::string color = T::ColorOperation(op, cv[0], cv[1], cv[2], supported);
        const std::string alpha = op == 7U ? "color_output_" + std::to_string(stage) + ".r"
                                           : T::AlphaOperation(aop, av[0], av[1], av[2], supported);
        const uint32_t cb = w[3] & 3U, ab = (w[3] >> 16U) & 3U;
        const uint32_t cscale = cb < 3U ? 1U << cb : 1U, ascale = ab < 3U ? 1U << ab : 1U;
        const std::string n = std::to_string(stage);
        text += "    precise vec3 color_output_" + n + " = byteround3(clamp(" + color + ", vec3(0.0), vec3(1.0)));\n";
        text += "    precise float alpha_output_" + n + " = byteround1(clamp(" + alpha + ", 0.0, 1.0));\n";
        text += "    combiner_output = vec4(clamp(color_output_" + n + " * " + std::to_string(cscale) +
                ".0, vec3(0.0), vec3(1.0)), clamp(alpha_output_" + n + " * " + std::to_string(ascale) +
                ".0, 0.0, 1.0));\n";
        text += "    combiner_buffer = next_combiner_buffer;\n";
        if (stage < 4U && (in.BufferControl & (1U << (8U + stage))) != 0U) {
            text += "    next_combiner_buffer.rgb = combiner_output.rgb;\n";
        }
        if (stage < 4U && (in.BufferControl & (1U << (12U + stage))) != 0U) {
            text += "    next_combiner_buffer.a = combiner_output.a;\n";
        }
    }
    text += "    return vec4(byteround3(combiner_output.rgb), byteround1(combiner_output.a));\n}\n";
    return supported ? text : std::string{};
}

void Emit(const char* path) {
    std::FILE* file = std::fopen(path, "w");
    Require(file != nullptr, "--emit: cannot open the file");
    std::mt19937 rng(7U);
    int written = 0;
    for (int round = 0; written < 400 && round < 100000; ++round) {
        const auto in = RandomConstantProgram(rng, round % 3 != 0);
        const auto out = EvaluatePicaConstantTev(in);
        if (!out.Proven()) {
            continue;
        }
        const std::string glsl = TevGlsl(in);
        Require(!glsl.empty(), "--emit: a proven program the generator's helpers reject");
        std::string escaped;
        for (const char c : glsl) {
            if (c == '\n') escaped += "\\n";
            else if (c == '"' || c == '\\') { escaped += '\\'; escaped += c; }
            else escaped += c;
        }
        std::fprintf(file, "{\"glsl\": \"%s\", \"constants\": [", escaped.c_str());
        for (size_t s = 0; s < 6U; ++s) {
            std::fprintf(file, "%s[%.9g, %.9g, %.9g, %.9g]", s == 0U ? "" : ", ", static_cast<double>(in.Constants[s][0]),
                         static_cast<double>(in.Constants[s][1]), static_cast<double>(in.Constants[s][2]),
                         static_cast<double>(in.Constants[s][3]));
        }
        std::fprintf(file, "], \"buffer\": [%.9g, %.9g, %.9g, %.9g], \"bytes\": [%u, %u, %u, %u]}\n",
                     static_cast<double>(in.BufferColor[0]), static_cast<double>(in.BufferColor[1]),
                     static_cast<double>(in.BufferColor[2]), static_cast<double>(in.BufferColor[3]), out.Bytes[0],
                     out.Bytes[1], out.Bytes[2], out.Bytes[3]);
        ++written;
    }
    std::fclose(file);
    std::printf("emitted %d programs to %s\n", written, path);
}
} // namespace

int main(int argc, char** argv) {
    TestConstantTev();
    TestConstantTevAgainstFloat32();
    TestAlphaTest();
    TestDepth();
    TestCoverage();
    TestPlan();
    if (argc == 3 && std::strcmp(argv[1], "--emit") == 0) {
        Emit(argv[2]);
    }
    std::puts("oot3d_deko3d_clear_draw_tests: ok");
    return 0;
}
