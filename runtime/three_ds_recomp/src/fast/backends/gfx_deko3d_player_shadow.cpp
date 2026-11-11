// config/simple_shadow on deko3d (fast/oot3d/player_shadow.h; SWITCH_PORT.md,
// "Simple shadow"): the opaque draws of Link's actor in a frame are drawn
// again with his own PICA vertex program, their view rows patched to flatten
// him onto the floor along the scene's light, in a translucent black that
// darkens each pixel once (the top stencil bit, set where the shadow lands and
// zeroed again by a second pass over the same meshes; cleared first only once
// the game or a savestate may have left it set).
// They are taken only from draws that leave depth to the rasterizer and drawn
// at the frame's first draw point after them (before a translucent world draw,
// an overlay or the display transfer), with his draws' depth test and no
// depth write: the floor in front of the camera takes the shadow, anything
// standing on it hides it.
#include "fast/backends/gfx_deko3d.h"
#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/oot3d/player_shadow.h"
#include "gfx_deko3d_api.h"
#include "gfx_deko3d_player_shadow_dksh.h"
#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace Fast {
namespace {
using namespace Fast::Deko3d;
// Key of the built-in stage in the ShaderStore (not a text hash).
constexpr uint64_t kPlayerShadowFragmentKey = 0xDE0C3D5AD0500001ULL;
constexpr uint8_t kPlayerShadowStencilBit = 0x80U;
// A summary line in deko3d.txt every this many shadow frames.
constexpr uint64_t kPlayerShadowReportFrames = 1800U;
} // namespace

void GfxRenderingAPIDeko3d::InitPlayerShadow() {
    mPlayerShadow.Fragment =
        mShaders.Embedded(kPlayerShadowFragmentKey, std::span<const uint8_t>(kPlayerShadowFragmentDksh));
    Oot3d::PlayerShadowBridge::Instance().SetRendererReady(mPlayerShadow.Fragment != nullptr);
    Log("shadow: fragment program %s (%zu B)", mPlayerShadow.Fragment != nullptr ? "ready" : "MISSING",
        sizeof(kPlayerShadowFragmentDksh));
}

void GfxRenderingAPIDeko3d::ObservePlayerShadowDraw(const GfxNativePicaDrawView& draw) {
    auto& shadow = mPlayerShadow;
    const uint64_t frame = mDevice.Frame();
    if (shadow.Frame != frame) {
        shadow.Frame = frame;
        shadow.Selected = Oot3d::PlayerShadowBridge::Instance().Selected();
        shadow.Projection.reset();
        shadow.Draws.clear();
        shadow.StencilTaken = false;
    }
    if (draw.Stencil.Enabled && ((draw.Stencil.WriteMask | draw.Stencil.CompareMask) & kPlayerShadowStencilBit) != 0U) {
        shadow.StencilTaken = shadow.StencilShared = true;
        shadow.StencilClean.clear();
    }
}

void GfxRenderingAPIDeko3d::CapturePlayerShadowDraw(const GfxNativePicaDrawView& draw, const PicaProgram& program,
                                                    const PicaTarget& target, const PlayerShadowDraw& bound) {
    auto& shadow = mPlayerShadow;
    if (!shadow.Selected || shadow.Frame != mDevice.Frame()) return;
    const auto& composition = draw.Composition;
    if (!composition.NativeActorGeometry ||
        composition.NativeActorIdentity != Oot3d::PlayerShadowActorIdentity(shadow.Selected->Actor) ||
        draw.CompositionDomain != Renderer3ds::PicaCompositionDomain::Scene ||
        composition.Layer != Renderer3ds::PicaCompositionLayer::OpaqueWorld || composition.NativeProjectedShadow)
        return;
    // His depth must be the rasterizer's (the floor was tested with it; the
    // parametric program writes that same depth), his uniforms the title's
    // block and his camera known.
    const bool rasterDepth = !draw.WBuffering && draw.DepthRange == -1.0F && draw.NearPlane == 0.0F;
    if (!draw.DepthTestEnabled || !draw.DepthWriteEnabled || !rasterDepth || program.Vertex == nullptr ||
        !draw.LightBinding.WorldToView || draw.VertexUniformBytes.size() < Oot3d::kPicaPackedVertexUniformSize) {
        ++shadow.Skipped;
        return;
    }
    if (!shadow.Projection) {
        const auto light = Oot3d::PlayerShadowLightDirection(
            draw.LightBinding.SceneDirectionals.get(), shadow.Selected->FloorNormal, shadow.Selected->Height);
        shadow.Projection = Oot3d::PlayerShadowProjection(*shadow.Selected, light);
    }
    const auto bytes = static_cast<uint32_t>(draw.VertexUniformBytes.size());
    const uint32_t aligned = AlignUp(bytes, DK_UNIFORM_BUF_ALIGNMENT);
    const StreamSpan stream = mDevice.Stream(aligned, DK_UNIFORM_BUF_ALIGNMENT);
    if (stream.Cpu == nullptr) {
        ++shadow.Skipped;
        return;
    }
    auto* cpu = static_cast<uint8_t*>(stream.Cpu);
    std::memcpy(cpu, draw.VertexUniformBytes.data(), bytes);
    std::memset(cpu + bytes, 0, aligned - bytes);
    if (!Oot3d::PatchPlayerShadowViewRows(std::span<uint8_t>(cpu, bytes), *draw.LightBinding.WorldToView,
                                          *shadow.Projection)) {
        ++shadow.Skipped;
        return;
    }
    PlayerShadowDraw record = bound;
    record.Target = &target;
    record.Vertex = program.Vertex;
    record.Uniforms = stream.Gpu;
    record.UniformBytes = aligned;
    shadow.Draws.push_back(record);
}

void GfxRenderingAPIDeko3d::DrawPlayerShadow(PicaTarget& target) {
    auto& shadow = mPlayerShadow;
    if (shadow.Frame != mDevice.Frame() || shadow.Draws.empty()) return;
    // This target's draws, once; another target's wait for its own point.
    std::vector<PlayerShadowDraw> draws;
    std::erase_if(shadow.Draws, [&](const PlayerShadowDraw& draw) {
        if (draw.Target != &target) return false;
        draws.push_back(draw);
        return true;
    });
    if (draws.empty() || shadow.Fragment == nullptr || !shadow.Selected) return;
    // A frame whose own draws use the top stencil bit keeps it: the shadow is
    // drawn without it (overlapping limbs darker) rather than not at all, the
    // foot shadows being hidden already.
    const bool stencil = !shadow.StencilTaken;
    if (!stencil && shadow.StencilFrames++ == 0U)
        Log("shadow: a draw uses the top stencil bit, drawn without it (overlaps darker) in such frames");
    const float opacity = std::clamp(shadow.Selected->Opacity, 0.0F, 1.0F);
    if (opacity <= 0.0F) return;
    const StreamSpan color = mDevice.Stream(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    if (color.Cpu == nullptr) return;
    const float rgba[4] = {0.0F, 0.0F, 0.0F, opacity};
    std::memcpy(color.Cpu, rgba, sizeof(rgba));

    DkCmdBuf cmd = mDevice.Cmd();
    BindPicaTarget(target);
    ++target.Version;
    if (stencil && shadow.StencilShared && shadow.StencilClean.insert(&target).second) {
        const DkScissor whole{0, 0, target.Extent.Width, target.Extent.Height};
        dkCmdBufSetScissors(cmd, 0, &whole, 1);
        dkCmdBufClearDepthStencil(cmd, false, 0.0F, kPlayerShadowStencilBit, 0U);
        ++shadow.FullClears;
    }

    DkColorState colorState;
    dkColorStateDefaults(&colorState);
    dkColorStateSetBlendEnable(&colorState, 0, true);
    dkCmdBufBindColorState(cmd, &colorState);
    DkBlendState blend;
    dkBlendStateDefaults(&blend);
    blend.colorBlendOp = DkBlendOp_Add;
    blend.srcColorBlendFactor = DkBlendFactor_SrcAlpha;
    blend.dstColorBlendFactor = DkBlendFactor_InvSrcAlpha;
    blend.alphaBlendOp = DkBlendOp_Add;
    blend.srcAlphaBlendFactor = DkBlendFactor_Zero;
    blend.dstAlphaBlendFactor = DkBlendFactor_One;
    dkCmdBufBindBlendState(cmd, 0, &blend);
    DkColorWriteState writeState;
    dkColorWriteStateDefaults(&writeState);
    dkColorWriteStateSetMask(&writeState, 0, DkColorMask_RGB);
    dkCmdBufBindColorWriteState(cmd, &writeState);
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.depthClampEnable = 0;
    rasterizer.cullMode = DkFace_None; // flattening turns some triangles over
    dkCmdBufBindRasterizerState(cmd, &rasterizer);
    dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, kPlayerShadowStencilBit, kPlayerShadowStencilBit,
                       kPlayerShadowStencilBit);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, color.Gpu, DK_UNIFORM_BUF_ALIGNMENT);

    // Every mesh with his depth test; `pass` writes the bit where `compare` lets the pixel through.
    const auto drawMeshes = [&](DkStencilOp pass, DkCompareOp compare) {
        for (const PlayerShadowDraw& draw : draws) {
            DkDepthStencilState depthStencil;
            dkDepthStencilStateDefaults(&depthStencil);
            depthStencil.depthTestEnable = 1;
            depthStencil.depthWriteEnable = 0;
            depthStencil.depthCompareOp = static_cast<DkCompareOp>(draw.DepthCompare);
            depthStencil.stencilTestEnable = stencil ? 1 : 0;
            depthStencil.stencilFrontFailOp = depthStencil.stencilBackFailOp = DkStencilOp_Keep;
            depthStencil.stencilFrontDepthFailOp = depthStencil.stencilBackDepthFailOp = DkStencilOp_Keep;
            depthStencil.stencilFrontPassOp = depthStencil.stencilBackPassOp = pass;
            depthStencil.stencilFrontCompareOp = depthStencil.stencilBackCompareOp = compare;
            dkCmdBufBindDepthStencilState(cmd, &depthStencil);
            const DkShader* shaders[] = {draw.Vertex, shadow.Fragment};
            dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, draw.Uniforms, draw.UniformBytes);
            dkCmdBufBindVtxAttribState(cmd, draw.Attributes.data(), static_cast<uint32_t>(draw.Attributes.size()));
            dkCmdBufBindVtxBufferState(cmd, draw.Buffers.data(), static_cast<uint32_t>(draw.Buffers.size()));
            dkCmdBufBindVtxBuffers(cmd, 0, draw.Extents.data(), static_cast<uint32_t>(draw.Extents.size()));
            ApplyDkViewport(cmd, static_cast<float>(draw.Viewport.X), static_cast<float>(draw.Viewport.Y),
                            static_cast<float>(draw.Viewport.Width), static_cast<float>(draw.Viewport.Height));
            dkCmdBufSetScissors(cmd, 0, &draw.Scissor, 1);
            if (draw.Indexed) {
                dkCmdBufBindIdxBuffer(cmd, draw.Indices16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8,
                                      draw.IndexStream);
                dkCmdBufDrawIndexed(cmd, draw.Primitive, draw.IndexCount, 1, 0, draw.BaseVertex, 0);
            } else {
                dkCmdBufDraw(cmd, draw.Primitive, draw.VertexCount, 1, 0, 0);
            }
        }
    };
    // Drawn where the bit is clear, then set: each pixel darkens once.
    drawMeshes(DkStencilOp_Replace, DkCompareOp_NotEqual);
    if (stencil) {
        // The same meshes without colour put the bit back to zero on exactly
        // the pixels the shadow set: clearing one stencil bit over the whole
        // target rewrites all of its depth and stencil.
        DkColorWriteState noColor;
        dkColorWriteStateDefaults(&noColor);
        dkColorWriteStateSetMask(&noColor, 0, 0);
        dkCmdBufBindColorWriteState(cmd, &noColor);
        drawMeshes(DkStencilOp_Zero, DkCompareOp_Always);
    }
    mDevice.Checkpoint("shadow");
    shadow.Meshes += draws.size();
    if (shadow.Frames++ % kPlayerShadowReportFrames == 0U) {
        Log("shadow: frame %llu %zu meshes opacity %.2f height %.0f | %llu frames, %llu meshes, %llu skipped, "
            "%llu frames without the stencil bit, %llu full stencil clears",
            static_cast<unsigned long long>(mDevice.Frame()), draws.size(), static_cast<double>(opacity),
            static_cast<double>(shadow.Selected->Height), static_cast<unsigned long long>(shadow.Frames),
            static_cast<unsigned long long>(shadow.Meshes), static_cast<unsigned long long>(shadow.Skipped),
            static_cast<unsigned long long>(shadow.StencilFrames),
            static_cast<unsigned long long>(shadow.FullClears));
    }
}

} // namespace Fast
#endif
