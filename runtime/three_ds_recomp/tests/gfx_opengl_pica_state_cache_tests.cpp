#include "fast/backends/gfx_opengl_pica_state_cache.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace Fast::PicaGlStateCache {
namespace {

TEST(PicaGlStateCache, DisabledIssuesEveryCall) {
    State state;
    state.Enabled = false;
    EXPECT_TRUE(state.Change(state.DepthTest, true));
    EXPECT_TRUE(state.Change(state.DepthTest, true));
    EXPECT_TRUE(state.BindFramebuffer(7U));
    EXPECT_TRUE(state.BindFramebuffer(7U));
    bool activate = false;
    EXPECT_TRUE(state.BindTexture(1U, 5U, &activate));
    EXPECT_TRUE(activate);
    EXPECT_TRUE(state.BindTexture(1U, 5U, &activate));
    EXPECT_TRUE(activate);
    // 2 toggles + 2 x (bind + draw buffer) + 2 x (active + bind).
    EXPECT_EQ(state.Count.Issued, 10U);
    EXPECT_EQ(state.Count.Skipped, 0U);
}

TEST(PicaGlStateCache, DropsOnlyRepeatedValues) {
    State state;
    EXPECT_TRUE(state.Change(state.DepthFunc, 0x0201U));
    EXPECT_FALSE(state.Change(state.DepthFunc, 0x0201U));
    EXPECT_TRUE(state.Change(state.DepthFunc, 0x0203U));
    const std::array<int32_t, 4> box{0, 0, 800, 480};
    EXPECT_TRUE(state.Change(state.Viewport, box));
    EXPECT_FALSE(state.Change(state.Viewport, box));
    EXPECT_TRUE(state.Change(state.Viewport, std::array<int32_t, 4>{0, 0, 800, 481}));
    EXPECT_TRUE(state.UseProgram(3U));
    EXPECT_FALSE(state.UseProgram(3U));
    EXPECT_EQ(state.Count.ProgramBinds, 1U);
    // Blend constants compare by bits: -0 and +0 differ, as GL may keep them.
    EXPECT_TRUE(state.Change(state.BlendColor,
                             std::array<uint32_t, 4>{FloatBits(0.0F), 0U, 0U, 0U}));
    EXPECT_TRUE(state.Change(state.BlendColor,
                             std::array<uint32_t, 4>{FloatBits(-0.0F), 0U, 0U, 0U}));
}

TEST(PicaGlStateCache, InvalidateForgetsValuesButKeepsCounters) {
    State state;
    EXPECT_TRUE(state.BindVertexArray(2U));
    EXPECT_FALSE(state.BindVertexArray(2U));
    const auto issued = state.Count.Issued;
    state.Invalidate();
    EXPECT_TRUE(state.Enabled);
    EXPECT_EQ(state.Count.Issued, issued);
    EXPECT_TRUE(state.BindVertexArray(2U));
    state.Enabled = false;
    state.Invalidate();
    EXPECT_FALSE(state.Enabled);
}

TEST(PicaGlStateCache, TextureUnitsTrackTheActiveUnit) {
    State state;
    bool activate = false;
    EXPECT_TRUE(state.BindTexture(1U, 10U, &activate));
    EXPECT_TRUE(activate);
    EXPECT_TRUE(state.BindTexture(2U, 11U, &activate));
    EXPECT_TRUE(activate);
    EXPECT_FALSE(state.BindTexture(1U, 10U, &activate));
    EXPECT_FALSE(activate);
    // Unit 2 is still active: a new texture there needs no glActiveTexture.
    EXPECT_TRUE(state.BindTexture(2U, 12U, &activate));
    EXPECT_FALSE(activate);
    // A helper created a texture on the active unit (2): only it is unknown.
    state.ForgetActiveTextureBinding();
    EXPECT_TRUE(state.BindTexture(2U, 12U, &activate));
    EXPECT_FALSE(activate);
    EXPECT_FALSE(state.BindTexture(1U, 10U, &activate));
    // With the active unit unknown every unit is forgotten.
    state.ActiveUnit.Reset();
    state.ForgetActiveTextureBinding();
    EXPECT_TRUE(state.BindTexture(1U, 10U, &activate));
    EXPECT_TRUE(activate);
}

TEST(PicaGlStateCache, ClearStateForgetsFramebufferScissorAndMasks) {
    State state;
    EXPECT_TRUE(state.BindFramebuffer(4U));
    EXPECT_TRUE(state.Change(state.ScissorTest, true));
    EXPECT_TRUE(state.Change(state.ColorMask, uint8_t{0xFU}));
    EXPECT_TRUE(state.Change(state.DepthMask, true));
    EXPECT_TRUE(state.Change(state.StencilMask, 0xFFU));
    EXPECT_TRUE(state.Change(state.DepthTest, true));
    state.ForgetClearState();
    EXPECT_TRUE(state.BindFramebuffer(4U));
    EXPECT_TRUE(state.Change(state.ScissorTest, true));
    EXPECT_TRUE(state.Change(state.ColorMask, uint8_t{0xFU}));
    EXPECT_TRUE(state.Change(state.DepthMask, true));
    EXPECT_TRUE(state.Change(state.StencilMask, 0xFFU));
    EXPECT_FALSE(state.Change(state.DepthTest, true));
}

TEST(PicaGlStateCache, ParsesSwitches) {
    EXPECT_TRUE(ParseSwitch(nullptr, true));
    EXPECT_FALSE(ParseSwitch(nullptr, false));
    EXPECT_TRUE(ParseSwitch("", true));
    EXPECT_FALSE(ParseSwitch("0", true));
    EXPECT_FALSE(ParseSwitch(" no\n", true));
    EXPECT_TRUE(ParseSwitch("1", false));
    EXPECT_TRUE(ParseSwitch("\tyes", false));
    EXPECT_FALSE(ParseSwitch("x", false));
}

// The fixed-function calls of SubmitPicaDraw for one draw, in the same
// order and with the same conditions (gfx_opengl_pica.cpp), driven by a
// synthetic frame. Used to estimate calls per draw with and without the
// cache; the console counts the real ones (config/gl_stats).
struct SyntheticDraw {
    uint32_t Framebuffer = 1U;
    uint32_t Program = 1U;
    std::array<uint32_t, 3> Textures{};
    uint32_t Lut = 100U;
    bool Blend = false;
    bool Stencil = false;
    bool Cull = true;
};

void Simulate(State& gl, const SyntheticDraw& draw) {
    (void)gl.BindFramebuffer(draw.Framebuffer);
    (void)gl.BindVertexArray(9U);
    (void)gl.UseProgram(draw.Program);
    gl.ForgetActiveTextureBinding();
    bool activate = false;
    for (uint32_t slot = 0U; slot < 3U; ++slot) {
        (void)gl.BindTexture(slot + 1U, draw.Textures[slot], &activate);
    }
    (void)gl.BindTexture(13U, draw.Lut, &activate);
    (void)gl.Change(gl.ColorMask, uint8_t{0xFU});
    if (draw.Blend) {
        (void)gl.Change(gl.Blend, true);
        (void)gl.Change(gl.ColorLogicOp, false);
        (void)gl.Change(gl.BlendEquation, std::array<uint32_t, 2>{1U, 1U});
        (void)gl.Change(gl.BlendFunc, std::array<uint32_t, 4>{1U, 2U, 1U, 2U});
        (void)gl.Change(gl.BlendColor, std::array<uint32_t, 4>{});
    } else {
        (void)gl.Change(gl.Blend, false);
        (void)gl.Change(gl.ColorLogicOp, true);
        (void)gl.Change(gl.LogicOp, 3U);
    }
    (void)gl.Change(gl.DepthTest, true);
    (void)gl.Change(gl.DepthFunc, draw.Blend ? 1U : 2U);
    (void)gl.Change(gl.DepthMask, !draw.Blend);
    if (draw.Stencil) {
        (void)gl.Change(gl.StencilTest, true);
        (void)gl.Change(gl.StencilFunc, std::array<uint32_t, 3>{1U, 2U, 3U});
        (void)gl.Change(gl.StencilMask, 0xFFU);
        (void)gl.Change(gl.StencilOp, std::array<uint32_t, 3>{1U, 1U, 1U});
    } else {
        (void)gl.Change(gl.StencilTest, false);
    }
    if (draw.Cull) {
        (void)gl.Change(gl.CullFace, true);
        (void)gl.Change(gl.FrontFace, 1U);
        (void)gl.Change(gl.CullFaceMode, 2U);
    } else {
        (void)gl.Change(gl.CullFace, false);
    }
    (void)gl.Change(gl.Viewport, std::array<int32_t, 4>{0, 0, 800, 480});
    (void)gl.Change(gl.ScissorTest, true);
    (void)gl.Change(gl.Scissor, std::array<int32_t, 4>{0, 0, 800, 480});
}

std::vector<SyntheticDraw> SyntheticFrame() {
    // 400 draws: 24 materials (program + textures) in runs of 1-6 draws,
    // a quarter blended (effects, foliage), a few stenciled and cull-less.
    std::vector<SyntheticDraw> frame;
    uint32_t seed = 12345U;
    const auto next = [&] {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 8U;
    };
    while (frame.size() < 400U) {
        SyntheticDraw draw;
        const uint32_t material = next() % 24U;
        draw.Program = 1U + material % 16U;
        draw.Textures = {200U + material, material % 3U == 0U ? 300U + material : 1U, 1U};
        draw.Lut = 100U + material % 4U;
        draw.Blend = frame.size() > 300U || next() % 8U == 0U;
        draw.Stencil = next() % 40U == 0U;
        draw.Cull = next() % 10U != 0U;
        const uint32_t run = 1U + next() % 6U;
        for (uint32_t i = 0U; i < run && frame.size() < 400U; ++i) {
            frame.push_back(draw);
        }
    }
    return frame;
}

TEST(PicaGlStateCache, SyntheticFrameCallsPerDraw) {
    const auto frame = SyntheticFrame();
    State before;
    before.Enabled = false;
    State after;
    for (const auto& draw : frame) {
        Simulate(before, draw);
        Simulate(after, draw);
    }
    // Without the cache the legacy VAO was rebound after every draw.
    before.Count.Issued += frame.size();
    const double perDrawBefore =
        static_cast<double>(before.Count.Issued) / frame.size();
    const double perDrawAfter =
        static_cast<double>(after.Count.Issued) / frame.size();
    std::printf("synthetic frame: %zu draws, state calls per draw %.2f -> %.2f "
                "(program binds %llu -> %llu, texture binds %llu -> %llu)\n",
                frame.size(), perDrawBefore, perDrawAfter,
                static_cast<unsigned long long>(before.Count.ProgramBinds),
                static_cast<unsigned long long>(after.Count.ProgramBinds),
                static_cast<unsigned long long>(before.Count.TextureBinds),
                static_cast<unsigned long long>(after.Count.TextureBinds));
    EXPECT_GT(perDrawBefore, 25.0);
    EXPECT_LT(perDrawAfter, perDrawBefore / 3.0);
}

// GL_DEPTH_CLAMP as the backend drives it: Init enables it for the legacy
// path; every PICA draw must run with it off (near/far clipping, as the PICA)
// and every legacy draw with it on, whatever the state cache forgets.
TEST(PicaGlDepthClamp, PicaDrawsClipAndLegacyDrawsKeepTheClamp) {
    bool glDepthClamp = true; // GfxRenderingAPIOGL::Init
    DepthClamp clamp;
    State gl;
    uint32_t calls = 0U;
    const auto picaDraw = [&] {
        gl.Invalidate(); // PicaGlStateMayBeStale after legacy work
        if (clamp.EnterPica()) {
            glDepthClamp = false;
            ++calls;
        }
        EXPECT_FALSE(glDepthClamp) << "PICA draw without near-plane clipping";
    };
    const auto legacyDraw = [&] {
        if (clamp.LeavePica()) {
            glDepthClamp = true;
            ++calls;
        }
        EXPECT_TRUE(glDepthClamp);
    };
    legacyDraw();
    EXPECT_EQ(calls, 0U);
    // A frame: 300 PICA draws, then the HUD (legacy), twice.
    for (int frame = 0; frame < 2; ++frame) {
        for (int draw = 0; draw < 300; ++draw) picaDraw();
        for (int draw = 0; draw < 20; ++draw) legacyDraw();
    }
    // One disable and one enable per PICA -> legacy switch, not per draw.
    EXPECT_EQ(calls, 4U);
    EXPECT_FALSE(clamp.LeavePica());
    EXPECT_TRUE(clamp.EnterPica());
    EXPECT_FALSE(clamp.EnterPica());
}

} // namespace
} // namespace Fast::PicaGlStateCache
