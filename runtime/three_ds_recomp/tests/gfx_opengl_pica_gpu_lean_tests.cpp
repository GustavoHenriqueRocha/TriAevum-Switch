// config/gl_gpu_lean decisions (fast/backends/gfx_opengl_pica_gpu_lean.h).

#include "fast/backends/gfx_opengl_pica_gpu_lean.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

using namespace Fast::PicaGlGpuLean;

constexpr uint32_t kSrcAlpha = 0x0302U;
constexpr uint32_t kOneMinusSrcAlpha = 0x0303U;
constexpr uint32_t kFuncSubtract = 0x800AU;
constexpr uint32_t kXor = 0x1506U;
constexpr uint32_t kReplace = 0x1E01U;

// GL per-fragment model for an RGBA8 unorm target (GL 4.6 §17.3.6/§17.3.9):
// the fragment colour is clamped to [0,1]; blending works on the converted
// values and the result is converted back with round-to-nearest; the logic
// op works on the converted integers.
uint8_t ToUnorm8(float value) {
    return static_cast<uint8_t>(
        std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

float Factor(uint32_t factor, float source) {
    switch (factor) {
    case kGlZero: return 0.0F;
    case kGlOne: return 1.0F;
    case kSrcAlpha: return source;
    case kOneMinusSrcAlpha: return 1.0F - source;
    default: return 0.0F;
    }
}

uint8_t BlendAdd(float source, uint8_t destination, uint32_t sourceFactor,
                 uint32_t destinationFactor) {
    const float s = std::clamp(source, 0.0F, 1.0F);
    const float d = static_cast<float>(destination) / 255.0F;
    return ToUnorm8(s * Factor(sourceFactor, s) +
                    d * Factor(destinationFactor, s));
}

uint8_t LogicCopy(float source, uint8_t) { return ToUnorm8(source); }
uint8_t Unblended(float source) { return ToUnorm8(source); }

TEST(GfxOpenGlPicaGpuLean, IdentityBlendAndCopyLogicOpWriteTheSourceExactly) {
    // Every destination byte against a dense sweep of fragment values,
    // including values outside [0,1] (clamped the same way on both paths).
    for (int d = 0; d < 256; ++d) {
        for (int step = -64; step <= 4096 + 64; ++step) {
            const float source = static_cast<float>(step) / 4096.0F;
            const auto destination = static_cast<uint8_t>(d);
            ASSERT_EQ(BlendAdd(source, destination, kGlOne, kGlZero),
                      Unblended(source));
            ASSERT_EQ(LogicCopy(source, destination), Unblended(source));
            ASSERT_EQ(BlendAdd(source, destination, kGlZero, kGlOne),
                      destination);
        }
    }
}

TEST(GfxOpenGlPicaGpuLean, OutputMergerKeepsEverythingButIdentities) {
    BlendState opaque;
    opaque.Enabled = false;
    opaque.LogicOp = kGlCopy;
    EXPECT_EQ(ResolveOutputMerger(opaque, true), OutputMerger::Plain);
    EXPECT_EQ(ResolveOutputMerger(opaque, false), OutputMerger::LogicOp);
    opaque.LogicOp = kXor;
    EXPECT_EQ(ResolveOutputMerger(opaque, true), OutputMerger::LogicOp);

    BlendState identity;
    identity.Enabled = true;
    EXPECT_EQ(ResolveOutputMerger(identity, true), OutputMerger::Plain);
    EXPECT_EQ(ResolveOutputMerger(identity, false), OutputMerger::Blend);

    BlendState alpha = identity;
    alpha.SourceRgb = kSrcAlpha;
    alpha.DestRgb = kOneMinusSrcAlpha;
    EXPECT_EQ(ResolveOutputMerger(alpha, true), OutputMerger::Blend);
    BlendState alphaOnly = identity;
    alphaOnly.DestAlpha = kOneMinusSrcAlpha;
    EXPECT_EQ(ResolveOutputMerger(alphaOnly, true), OutputMerger::Blend);
    BlendState subtract = identity;
    subtract.EquationRgb = kFuncSubtract;
    EXPECT_EQ(ResolveOutputMerger(subtract, true), OutputMerger::Blend);
}

TEST(GfxOpenGlPicaGpuLean, OnlyDrawsThatChangeNothingAreSkipped) {
    DrawEffects masked;
    masked.ColorWriteMask = 0U;
    EXPECT_TRUE(DrawHasNoEffect(masked));

    DrawEffects depth = masked;
    depth.DepthWrite = true;
    EXPECT_FALSE(DrawHasNoEffect(depth));

    DrawEffects stencil = masked;
    stencil.StencilTest = true;
    stencil.StencilWriteMask = 0xFFU;
    stencil.StencilPass = kReplace;
    EXPECT_FALSE(DrawHasNoEffect(stencil));
    stencil.StencilWriteMask = 0U; // ops masked out entirely
    EXPECT_TRUE(DrawHasNoEffect(stencil));
    stencil.StencilWriteMask = 0xFFU;
    stencil.StencilPass = kGlKeep;
    EXPECT_TRUE(DrawHasNoEffect(stencil));
    stencil.StencilTest = false; // ops do nothing without the test
    stencil.StencilPass = kReplace;
    EXPECT_TRUE(DrawHasNoEffect(stencil));

    DrawEffects producer = masked;
    producer.ShadowProducer = true;
    EXPECT_FALSE(DrawHasNoEffect(producer));
    DrawEffects image = masked;
    image.FragmentFeatures = kFragmentStoresImage;
    EXPECT_FALSE(DrawHasNoEffect(image));

    DrawEffects visible;
    visible.ColorWriteMask = 0xFU;
    visible.Blend.LogicOp = kGlCopy;
    EXPECT_FALSE(DrawHasNoEffect(visible));
    visible.Blend.LogicOp = kGlNoop;
    EXPECT_TRUE(DrawHasNoEffect(visible));
    visible.Blend.Enabled = true; // identity blend writes the source
    EXPECT_FALSE(DrawHasNoEffect(visible));
    visible.Blend.SourceRgb = kGlZero;
    visible.Blend.SourceAlpha = kGlZero;
    visible.Blend.DestRgb = kGlOne;
    visible.Blend.DestAlpha = kGlOne;
    EXPECT_TRUE(DrawHasNoEffect(visible));
    visible.DepthWrite = true;
    EXPECT_FALSE(DrawHasNoEffect(visible));
}

TEST(GfxOpenGlPicaGpuLean, DepthFillClearsStencilOnlyWhereNothingUsesIt) {
    EXPECT_TRUE(DepthFillClearsStencil(3U, false));
    EXPECT_TRUE(DepthFillClearsStencil(3U, true));
    EXPECT_FALSE(DepthFillClearsStencil(0U, false));
    EXPECT_FALSE(DepthFillClearsStencil(2U, false));
    EXPECT_TRUE(DepthFillClearsStencil(0U, true));
    EXPECT_TRUE(DepthFillClearsStencil(2U, true));
}

TEST(GfxOpenGlPicaGpuLean, FragmentFeaturesFollowTheProgramText) {
    EXPECT_EQ(ClassifyFragmentSource("void main(){ pica_color = vec4(1.0); }"),
              0U);
    EXPECT_EQ(ClassifyFragmentSource(
                  "int alpha_byte = 1;\nif (alpha_byte < 2) discard;\n"),
              kFragmentDiscard);
    EXPECT_EQ(ClassifyFragmentSource("bool discarded = false;"), 0U);
    EXPECT_EQ(ClassifyFragmentSource("    gl_FragDepth = pica_depth;\n"),
              kFragmentWritesDepth);
    EXPECT_EQ(ClassifyFragmentSource(
                  "imageAtomicCompSwap(pica_shadow_buffer, c, a, b);"),
              kFragmentStoresImage);
    EXPECT_EQ(ClassifyFragmentSource("imageLoad(pica_shadow_buffer, c)"), 0U);
}

} // namespace
