#include "fast/backends/gfx_opengl_pica_scale.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace Fast::PicaGlScale {
namespace {

TEST(PicaGlScale, ParsesIntegerScalesAsBefore) {
    EXPECT_EQ(ParseResolutionScale("1"), 1.0F);
    EXPECT_EQ(ParseResolutionScale("2"), 2.0F);
    EXPECT_EQ(ParseResolutionScale("3\n"), 3.0F);
    EXPECT_EQ(ParseResolutionScale("  2 \r\n"), 2.0F);
    // The former parser: two digits or garbage meant 1, above 3 clamped.
    EXPECT_EQ(ParseResolutionScale("10"), 1.0F);
    EXPECT_EQ(ParseResolutionScale("7"), 3.0F);
    EXPECT_EQ(ParseResolutionScale(""), 1.0F);
    EXPECT_EQ(ParseResolutionScale("x"), 1.0F);
    EXPECT_EQ(ParseResolutionScale("0"), 1.0F);
    EXPECT_EQ(ParseResolutionScale("2x"), 2.0F);
}

TEST(PicaGlScale, ParsesFractionalScalesInQuarterSteps) {
    EXPECT_EQ(ParseResolutionScale("2.5"), 2.5F);
    EXPECT_EQ(ParseResolutionScale("2,5"), 2.5F);
    EXPECT_EQ(ParseResolutionScale("2.25"), 2.25F);
    EXPECT_EQ(ParseResolutionScale("2.75\n"), 2.75F);
    EXPECT_EQ(ParseResolutionScale("2.6"), 2.5F);
    EXPECT_EQ(ParseResolutionScale("2.9"), 3.0F);
    EXPECT_EQ(ParseResolutionScale("3.5"), 3.0F);
    EXPECT_EQ(ParseResolutionScale("1.1"), 1.0F);
    EXPECT_FALSE(IsIntegerScale(2.5F));
    EXPECT_TRUE(IsIntegerScale(3.0F));
}

TEST(PicaGlScale, IntegerEdgesAreTheFormerProducts) {
    for (const float scale : {1.0F, 2.0F, 3.0F}) {
        for (int64_t native = -8; native <= 1024; ++native) {
            EXPECT_EQ(ScaledEdge(native, scale),
                      static_cast<int32_t>(native * static_cast<int64_t>(scale)));
        }
        EXPECT_EQ(ScaledExtent(400U, scale), 400U * static_cast<uint32_t>(scale));
    }
}

TEST(PicaGlScale, FractionalRectanglesTileWithoutGapsOrOverlap) {
    // Two native rectangles sharing an edge share the same host edge, so the
    // host widths of a split always add up to the host width of the whole.
    const float scale = 2.5F;
    for (int64_t x = 0; x < 64; ++x) {
        for (int64_t split = x; split < 64; ++split) {
            const int32_t whole = ScaledEdge(64, scale) - ScaledEdge(x, scale);
            const int32_t left = ScaledEdge(split, scale) - ScaledEdge(x, scale);
            const int32_t right = ScaledEdge(64, scale) - ScaledEdge(split, scale);
            EXPECT_EQ(left + right, whole);
            EXPECT_GE(left, 0);
        }
    }
    // The 3DS screens at 2.5x.
    EXPECT_EQ(ScaledExtent(240U, scale), 600U);
    EXPECT_EQ(ScaledExtent(400U, scale), 1000U);
    EXPECT_EQ(ScaledExtent(320U, scale), 800U);
    EXPECT_EQ(ScaledExtent(1U, scale), 3U); // never zero for a non-empty extent
}

TEST(PicaGlScale, FitsTheDriverLimit) {
    EXPECT_EQ(FitResolutionScale(3.0F, 16384), 3.0F);
    EXPECT_EQ(FitResolutionScale(3.0F, 2048), 2.0F);
    EXPECT_EQ(FitResolutionScale(2.5F, 2048), 2.0F);
    EXPECT_EQ(FitResolutionScale(2.5F, 4096), 2.5F);
    EXPECT_EQ(FitResolutionScale(2.0F, 1024), 1.0F);
    EXPECT_EQ(FitResolutionScale(1.0F, 512), 1.0F);
}

TEST(PicaGlScale, LeanSettingDefaultsOn) {
    EXPECT_TRUE(ParseLeanSetting("", true));
    EXPECT_TRUE(ParseLeanSetting("1\n", true));
    EXPECT_FALSE(ParseLeanSetting("0", true));
    EXPECT_TRUE(ParseLeanSetting(" off", true)); // 'o' is not a switch: fallback
    EXPECT_FALSE(ParseLeanSetting("n", true));
    EXPECT_TRUE(ParseLeanSetting("?", true));
}

TEST(PicaGlScale, ScanoutCoverage) {
    EXPECT_TRUE(ScanoutCoversWindow(0, 0, 1280, 720, 1280, 720));
    // 5:3 pillarbox (widescreen off): the bars still need the clear.
    EXPECT_FALSE(ScanoutCoversWindow(40, 0, 1200, 720, 1280, 720));
    EXPECT_FALSE(ScanoutCoversWindow(0, 1, 1280, 719, 1280, 720));
}

TEST(PicaGlScale, DisplayCopyIdentity) {
    DisplayCopyIdentity none;
    EXPECT_FALSE(none.Valid());
    const DisplayCopyIdentity a{1U, 7U, 720, 1280, 0x2600U};
    DisplayCopyIdentity b = a;
    EXPECT_TRUE(a.Valid());
    EXPECT_EQ(a, b);
    b.SourceVersion = 8U; // the source was drawn since
    EXPECT_NE(a, b);
    b = a;
    b.SourceTarget = 2U; // another target with the same version number
    EXPECT_NE(a, b);
    b = a;
    b.Filter = 0x2601U;
    EXPECT_NE(a, b);
}

} // namespace
} // namespace Fast::PicaGlScale
