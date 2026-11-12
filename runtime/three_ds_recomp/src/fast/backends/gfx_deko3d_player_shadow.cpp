// config/simple_shadow on deko3d (fast/oot3d/player_shadow.h; SWITCH_PORT.md,
// "Simple shadow"): the opaque draws of Link's actor in a frame are drawn
// again with his own PICA vertex program, their view rows patched to flatten
// him onto the floor along the scene's light, and darken the floor in a
// translucent black, the way config/simple_shadow_method says (a 0.2.3 test;
// dkperf shadow_<method>_ms):
// - two passes: darken where the top stencil bit is clear and set it, then a
//   second pass over the same meshes zeroes it, so each pixel darkens once
//   (the bit is cleared over the whole target first only once the game or a
//   savestate may have left it set);
// - one pass: darken without the stencil, overlapping limbs darker;
// - cover: the meshes only set the bit, inside the screen rectangle of the
//   floor region his shadow can cover; one triangle over that rectangle
//   darkens where it is set and zeroes it;
// - texture: the meshes go flat into a silhouette of that region (their
//   projection rows patched too), which one quad on the floor samples, with
//   soft edges (the mod's way).
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
using Method = Oot3d::PlayerShadowMethod;
// Keys of the built-in stages in the ShaderStore (not text hashes).
constexpr uint64_t kPlayerShadowFragmentKey = 0xDE0C3D5AD0500001ULL;
constexpr uint64_t kPlayerShadowCoverVertexKey = 0xDE0C3D5AD0500002ULL;
constexpr uint64_t kPlayerShadowFloorVertexKey = 0xDE0C3D5AD0500003ULL;
constexpr uint64_t kPlayerShadowFloorFragmentKey = 0xDE0C3D5AD0500004ULL;
constexpr uint8_t kPlayerShadowStencilBit = 0x80U;
// Method texture: the silhouette's side, in texels.
constexpr uint32_t kPlayerShadowSilhouetteSize = 256U;
// A summary line in deko3d.txt every this many shadow frames.
constexpr uint64_t kPlayerShadowReportFrames = 1800U;

// player_shadow_floor_vsh.glsl and _fsh.glsl: binding 0 of both stages (std140).
struct PlayerShadowFloorUniforms {
    float Positions[4][4]; // gl_Position of the corners, a triangle strip
    float Texcoords[4][4]; // xy: the silhouette's coordinates
    float Shade[4];        // x: opacity, y, z: one texel
};
static_assert(sizeof(PlayerShadowFloorUniforms) == 144U, "the std140 block PlayerShadowFloor");

// The pixels both rectangles hold (none: zero width).
DkScissor Intersect(const DkScissor& a, const DkScissor& b) {
    const uint32_t x0 = std::max(a.x, b.x);
    const uint32_t y0 = std::max(a.y, b.y);
    const uint32_t x1 = std::min(a.x + a.width, b.x + b.width);
    const uint32_t y1 = std::min(a.y + a.height, b.y + b.height);
    return x1 > x0 && y1 > y0 ? DkScissor{x0, y0, x1 - x0, y1 - y0} : DkScissor{};
}

// The smallest rectangle holding both.
DkScissor Union(const DkScissor& a, const DkScissor& b) {
    const uint32_t x0 = std::min(a.x, b.x);
    const uint32_t y0 = std::min(a.y, b.y);
    return {x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0};
}

// Oot3d::PlayerShadowScreenRect's pixels (x, y, width, height) inside a target.
DkScissor WithinTarget(const std::array<int32_t, 4>& rect, uint32_t width, uint32_t height) {
    const auto clamp = [](int64_t value, uint32_t limit) {
        return static_cast<uint32_t>(std::clamp<int64_t>(value, 0, limit));
    };
    const uint32_t x0 = clamp(rect[0], width);
    const uint32_t y0 = clamp(rect[1], height);
    const uint32_t x1 = clamp(int64_t{rect[0]} + rect[2], width);
    const uint32_t y1 = clamp(int64_t{rect[1]} + rect[3], height);
    return x1 > x0 && y1 > y0 ? DkScissor{x0, y0, x1 - x0, y1 - y0} : DkScissor{};
}
} // namespace

void GfxRenderingAPIDeko3d::InitPlayerShadow() {
    auto& shadow = mPlayerShadow;
    shadow.Fragment =
        mShaders.Embedded(kPlayerShadowFragmentKey, std::span<const uint8_t>(kPlayerShadowFragmentDksh));
    shadow.CoverVertex =
        mShaders.Embedded(kPlayerShadowCoverVertexKey, std::span<const uint8_t>(kPlayerShadowCoverVertexDksh));
    shadow.FloorVertex =
        mShaders.Embedded(kPlayerShadowFloorVertexKey, std::span<const uint8_t>(kPlayerShadowFloorVertexDksh));
    shadow.FloorFragment =
        mShaders.Embedded(kPlayerShadowFloorFragmentKey, std::span<const uint8_t>(kPlayerShadowFloorFragmentDksh));
    // Method texture's silhouette: drawn, then sampled (its red channel);
    // uncompressed, its own block.
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, mDevice.Handle());
    maker.flags = DkImageFlags_UsageRender;
    maker.format = DkImageFormat_RGBA8_Unorm;
    maker.dimensions[0] = kPlayerShadowSilhouetteSize;
    maker.dimensions[1] = kPlayerShadowSilhouetteSize;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint32_t slot = mResources.AllocateImageSlot();
    if (slot < PicaResources::kMaxImages) {
        shadow.SilhouetteMemory = mDevice.MakeBlock(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)),
                                                    DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                                                    "shadow silhouette");
        if (shadow.SilhouetteMemory != nullptr) {
            dkImageInitialize(&shadow.SilhouetteImage, &layout, shadow.SilhouetteMemory, 0U);
            dkImageViewDefaults(&shadow.SilhouetteView, &shadow.SilhouetteImage);
            mResources.WriteImageDescriptor(slot, shadow.SilhouetteImage);
            shadow.SilhouetteSlot = slot;
        } else {
            mResources.FreeImageSlot(slot);
        }
    }
    Oot3d::PlayerShadowBridge::Instance().SetRendererReady(mPlayerShadow.Fragment != nullptr);
    Log("shadow: fragment program %s (%zu B); cover %s; texture %s (silhouette %ux%u %s)",
        shadow.Fragment != nullptr ? "ready" : "MISSING", sizeof(kPlayerShadowFragmentDksh),
        shadow.CoverVertex != nullptr ? "ready" : "MISSING",
        shadow.FloorVertex != nullptr && shadow.FloorFragment != nullptr ? "ready" : "MISSING",
        kPlayerShadowSilhouetteSize, kPlayerShadowSilhouetteSize,
        shadow.SilhouetteMemory != nullptr ? "ready" : "MISSING");
}

void GfxRenderingAPIDeko3d::ObservePlayerShadowDraw(const GfxNativePicaDrawView& draw) {
    auto& shadow = mPlayerShadow;
    const uint64_t frame = mDevice.Frame();
    if (shadow.Frame != frame) {
        auto& bridge = Oot3d::PlayerShadowBridge::Instance();
        shadow.Frame = frame;
        shadow.Selected = bridge.Selected();
        shadow.Method = bridge.Method();
        shadow.Projection.reset();
        shadow.Region.reset();
        shadow.WorldToClip.reset();
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
        if (shadow.Method == Method::Cover || shadow.Method == Method::Texture) {
            shadow.Region = Oot3d::PlayerShadowFloorRegion(*shadow.Selected, light);
            shadow.WorldToClip =
                Oot3d::PlayerShadowWorldToClip(draw.VertexUniformBytes, *draw.LightBinding.WorldToView);
        }
    }
    const auto bytes = static_cast<uint32_t>(draw.VertexUniformBytes.size());
    const uint32_t aligned = AlignUp(bytes, DK_UNIFORM_BUF_ALIGNMENT);
    // The block with the flattening view rows; for the silhouette, its
    // projection rows too.
    const auto patched = [&](bool silhouette) -> DkGpuAddr {
        const StreamSpan stream = mDevice.Stream(aligned, DK_UNIFORM_BUF_ALIGNMENT);
        if (stream.Cpu == nullptr) return DK_GPU_ADDR_INVALID;
        auto* cpu = static_cast<uint8_t*>(stream.Cpu);
        std::memcpy(cpu, draw.VertexUniformBytes.data(), bytes);
        std::memset(cpu + bytes, 0, aligned - bytes);
        const std::span<uint8_t> block(cpu, bytes);
        if (!Oot3d::PatchPlayerShadowViewRows(block, *draw.LightBinding.WorldToView, *shadow.Projection) ||
            (silhouette &&
             !Oot3d::PatchPlayerShadowSilhouetteRows(block, *draw.LightBinding.WorldToView, *shadow.Region)))
            return DK_GPU_ADDR_INVALID;
        return stream.Gpu;
    };
    PlayerShadowDraw record = bound;
    record.Target = &target;
    record.Vertex = program.Vertex;
    record.Uniforms = patched(false);
    record.UniformBytes = aligned;
    if (shadow.Method == Method::Texture && shadow.Region) record.SilhouetteUniforms = patched(true);
    if (record.Uniforms == DK_GPU_ADDR_INVALID ||
        (shadow.Method == Method::Texture && shadow.Region && record.SilhouetteUniforms == DK_GPU_ADDR_INVALID)) {
        ++shadow.Skipped;
        return;
    }
    shadow.Draws.push_back(record);
}

void GfxRenderingAPIDeko3d::DrawPlayerShadowMesh(DkCmdBuf cmd, const PlayerShadowDraw& draw) {
    dkCmdBufBindVtxAttribState(cmd, draw.Attributes.data(), static_cast<uint32_t>(draw.Attributes.size()));
    dkCmdBufBindVtxBufferState(cmd, draw.Buffers.data(), static_cast<uint32_t>(draw.Buffers.size()));
    dkCmdBufBindVtxBuffers(cmd, 0, draw.Extents.data(), static_cast<uint32_t>(draw.Extents.size()));
    if (draw.Indexed) {
        dkCmdBufBindIdxBuffer(cmd, draw.Indices16 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8, draw.IndexStream);
        dkCmdBufDrawIndexed(cmd, draw.Primitive, draw.IndexCount, 1, 0, draw.BaseVertex, 0);
    } else {
        dkCmdBufDraw(cmd, draw.Primitive, draw.VertexCount, 1, 0, 0);
    }
}

void GfxRenderingAPIDeko3d::DrawPlayerShadowSilhouette(const std::vector<PlayerShadowDraw>& draws) {
    auto& shadow = mPlayerShadow;
    DkCmdBuf cmd = mDevice.Cmd();
    FlushPicaInvalidations();
    // Drawn again in the same frame (a second target): only once the first
    // quad has read it.
    if (shadow.SilhouetteFrame == mDevice.Frame()) dkCmdBufBarrier(cmd, DkBarrier_Fragments, 0);
    shadow.SilhouetteFrame = mDevice.Frame();
    const DkImageView* colors[] = {&shadow.SilhouetteView};
    dkCmdBufBindRenderTargets(cmd, colors, 1, nullptr);
    mBoundTarget = nullptr; // the scene target is bound again after
    constexpr uint32_t size = kPlayerShadowSilhouetteSize;
    const DkScissor whole{0, 0, size, size};
    dkCmdBufSetScissors(cmd, 0, &whole, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0F, 0.0F, 0.0F, 0.0F);
    const StreamSpan white = mDevice.Stream(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    if (white.Cpu != nullptr) {
        const float rgba[4] = {1.0F, 1.0F, 1.0F, 1.0F};
        std::memcpy(white.Cpu, rgba, sizeof(rgba));
        // Every covered texel white: no blend, depth or stencil.
        DkColorState colorState;
        dkColorStateDefaults(&colorState);
        dkCmdBufBindColorState(cmd, &colorState);
        DkColorWriteState writeState;
        dkColorWriteStateDefaults(&writeState);
        dkCmdBufBindColorWriteState(cmd, &writeState);
        DkDepthStencilState depthStencil;
        dkDepthStencilStateDefaults(&depthStencil);
        depthStencil.depthTestEnable = 0;
        depthStencil.depthWriteEnable = 0;
        depthStencil.stencilTestEnable = 0;
        dkCmdBufBindDepthStencilState(cmd, &depthStencil);
        DkRasterizerState rasterizer;
        dkRasterizerStateDefaults(&rasterizer);
        rasterizer.depthClampEnable = 0;
        rasterizer.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmd, &rasterizer);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, white.Gpu, DK_UNIFORM_BUF_ALIGNMENT);
        ApplyDkViewport(cmd, 0.0F, 0.0F, static_cast<float>(size), static_cast<float>(size));
        // A texel of border stays clear: the sampler clamps to the edge.
        const DkScissor inner{1, 1, size - 2U, size - 2U};
        dkCmdBufSetScissors(cmd, 0, &inner, 1);
        for (const PlayerShadowDraw& draw : draws) {
            const DkShader* shaders[] = {draw.Vertex, shadow.Fragment};
            dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, draw.SilhouetteUniforms, draw.UniformBytes);
            DrawPlayerShadowMesh(cmd, draw);
        }
    }
    // Drawn before the floor quad samples it.
    dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
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
    const float opacity = std::clamp(shadow.Selected->Opacity, 0.0F, 1.0F);
    if (opacity <= 0.0F) return;
    // Cover and texture need the floor region and his camera, and their
    // programs (texture its silhouette too): without them, two passes.
    Method method = shadow.Method;
    const bool regionKnown = shadow.Region.has_value() && shadow.WorldToClip.has_value();
    if ((method == Method::Cover && (!regionKnown || shadow.CoverVertex == nullptr)) ||
        (method == Method::Texture && (!regionKnown || shadow.FloorVertex == nullptr ||
                                       shadow.FloorFragment == nullptr || shadow.SilhouetteMemory == nullptr)))
        method = Method::TwoPass;
    // A frame whose own draws use the top stencil bit keeps it: the methods
    // that borrow it draw in one pass (overlapping limbs darker) rather than
    // not at all, the foot shadows being hidden already.
    if (shadow.StencilTaken && (method == Method::TwoPass || method == Method::Cover)) {
        if (shadow.StencilFrames++ == 0U)
            Log("shadow: a draw uses the top stencil bit, drawn in one pass (overlaps darker) in such frames");
        method = Method::OnePass;
    }
    const bool stencil = method == Method::TwoPass || method == Method::Cover;
    const StreamSpan color = mDevice.Stream(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    if (color.Cpu == nullptr) return;
    const float rgba[4] = {0.0F, 0.0F, 0.0F, opacity};
    std::memcpy(color.Cpu, rgba, sizeof(rgba));

    DkCmdBuf cmd = mDevice.Cmd();
    mDevice.MarkShadowStart();
    if (method == Method::Texture) DrawPlayerShadowSilhouette(draws);
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
    DkColorWriteState rgb;
    dkColorWriteStateDefaults(&rgb);
    dkColorWriteStateSetMask(&rgb, 0, DkColorMask_RGB);
    DkColorWriteState noColor;
    dkColorWriteStateDefaults(&noColor);
    dkColorWriteStateSetMask(&noColor, 0, 0);
    dkCmdBufBindColorWriteState(cmd, &rgb);
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.depthClampEnable = 0;
    rasterizer.cullMode = DkFace_None; // flattening turns some triangles over
    dkCmdBufBindRasterizerState(cmd, &rasterizer);
    dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, kPlayerShadowStencilBit, kPlayerShadowStencilBit,
                       kPlayerShadowStencilBit);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, color.Gpu, DK_UNIFORM_BUF_ALIGNMENT);

    // No depth write; with the stencil test, `pass` writes the bit where
    // `compare` lets the pixel through.
    const auto bindDepthStencil = [&](bool depthTest, DkCompareOp depthCompare, bool stencilTest, DkStencilOp pass,
                                      DkCompareOp compare) {
        DkDepthStencilState depthStencil;
        dkDepthStencilStateDefaults(&depthStencil);
        depthStencil.depthTestEnable = depthTest ? 1 : 0;
        depthStencil.depthWriteEnable = 0;
        depthStencil.depthCompareOp = depthCompare;
        depthStencil.stencilTestEnable = stencilTest ? 1 : 0;
        depthStencil.stencilFrontFailOp = depthStencil.stencilBackFailOp = DkStencilOp_Keep;
        depthStencil.stencilFrontDepthFailOp = depthStencil.stencilBackDepthFailOp = DkStencilOp_Keep;
        depthStencil.stencilFrontPassOp = depthStencil.stencilBackPassOp = pass;
        depthStencil.stencilFrontCompareOp = depthStencil.stencilBackCompareOp = compare;
        dkCmdBufBindDepthStencilState(cmd, &depthStencil);
    };
    // Every mesh with his depth test, inside `clip` if there is one.
    const auto drawMeshes = [&](bool stencilTest, DkStencilOp pass, DkCompareOp compare,
                                const std::optional<DkScissor>& clip) {
        for (const PlayerShadowDraw& draw : draws) {
            const DkScissor scissor = clip ? Intersect(draw.Scissor, *clip) : draw.Scissor;
            if (scissor.width == 0U || scissor.height == 0U) continue;
            bindDepthStencil(true, static_cast<DkCompareOp>(draw.DepthCompare), stencilTest, pass, compare);
            const DkShader* shaders[] = {draw.Vertex, shadow.Fragment};
            dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
            dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, draw.Uniforms, draw.UniformBytes);
            ApplyDkViewport(cmd, static_cast<float>(draw.Viewport.X), static_cast<float>(draw.Viewport.Y),
                            static_cast<float>(draw.Viewport.Width), static_cast<float>(draw.Viewport.Height));
            dkCmdBufSetScissors(cmd, 0, &scissor, 1);
            DrawPlayerShadowMesh(cmd, draw);
        }
    };
    const PlayerShadowDraw& first = draws.front();
    switch (method) {
    case Method::TwoPass:
        // Drawn where the bit is clear, then set: each pixel darkens once.
        drawMeshes(true, DkStencilOp_Replace, DkCompareOp_NotEqual, std::nullopt);
        // The same meshes without colour put the bit back to zero on exactly
        // the pixels the shadow set: clearing one stencil bit over the whole
        // target rewrites all of its depth and stencil.
        dkCmdBufBindColorWriteState(cmd, &noColor);
        drawMeshes(true, DkStencilOp_Zero, DkCompareOp_Always, std::nullopt);
        break;
    case Method::OnePass:
        drawMeshes(false, DkStencilOp_Keep, DkCompareOp_Always, std::nullopt);
        break;
    case Method::Cover: {
        // The meshes mark only inside the region's screen rectangle (his
        // draws' scissors when a corner is behind the camera), so the cover
        // over it zeroes every bit they set.
        std::optional<DkScissor> clip;
        const std::array<float, 4> viewport{
            static_cast<float>(first.Viewport.X), static_cast<float>(first.Viewport.Y),
            static_cast<float>(first.Viewport.Width), static_cast<float>(first.Viewport.Height)};
        if (const auto rect = Oot3d::PlayerShadowScreenRect(*shadow.WorldToClip, *shadow.Region, viewport,
                                                            Deko3d::ClipYSwizzleNegates(mFlipY)))
            clip = WithinTarget(*rect, target.Extent.Width, target.Extent.Height);
        std::optional<DkScissor> cover;
        for (const PlayerShadowDraw& draw : draws) {
            const DkScissor marked = clip ? Intersect(draw.Scissor, *clip) : draw.Scissor;
            if (marked.width != 0U && marked.height != 0U) cover = cover ? Union(*cover, marked) : marked;
        }
        if (!cover) break; // off screen
        dkCmdBufBindColorWriteState(cmd, &noColor);
        drawMeshes(true, DkStencilOp_Replace, DkCompareOp_Always, clip);
        // One triangle over the rectangle: darkened where the bit is set, which it zeroes.
        dkCmdBufBindColorWriteState(cmd, &rgb);
        bindDepthStencil(false, DkCompareOp_Always, true, DkStencilOp_Zero, DkCompareOp_Equal);
        const DkShader* shaders[] = {shadow.CoverVertex, shadow.Fragment};
        dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
        dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
        dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
        ApplyDkViewport(cmd, static_cast<float>(cover->x), static_cast<float>(cover->y),
                        static_cast<float>(cover->width), static_cast<float>(cover->height));
        dkCmdBufSetScissors(cmd, 0, &*cover, 1);
        dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
        break;
    }
    case Method::Texture: {
        // The floor region's quad samples the silhouette, with his depth test.
        PlayerShadowFloorUniforms floor{};
        const auto corners = shadow.Region->Corners();
        // The silhouette was drawn with the scene's NDC-to-row rule, so its
        // rows follow the swizzle.
        const bool minusOneFirst = Deko3d::ClipYSwizzleNegates(mFlipY);
        for (size_t corner = 0; corner < corners.size(); ++corner) {
            const auto position = Oot3d::PlayerShadowGlPosition(*shadow.WorldToClip, corners[corner]);
            std::copy(position.begin(), position.end(), floor.Positions[corner]);
            const float across = (corner & 2U) != 0U ? 1.0F : 0.0F;
            floor.Texcoords[corner][0] = (corner & 1U) != 0U ? 1.0F : 0.0F;
            floor.Texcoords[corner][1] = minusOneFirst ? across : 1.0F - across;
        }
        floor.Shade[0] = opacity;
        floor.Shade[1] = floor.Shade[2] = 1.0F / static_cast<float>(kPlayerShadowSilhouetteSize);
        const uint32_t floorBytes = AlignUp(static_cast<uint32_t>(sizeof(floor)), DK_UNIFORM_BUF_ALIGNMENT);
        const StreamSpan uniforms = mDevice.Stream(floorBytes, DK_UNIFORM_BUF_ALIGNMENT);
        if (uniforms.Cpu == nullptr) break;
        std::memcpy(uniforms.Cpu, &floor, sizeof(floor));
        bindDepthStencil(true, static_cast<DkCompareOp>(first.DepthCompare), false, DkStencilOp_Keep,
                         DkCompareOp_Always);
        const DkShader* shaders[] = {shadow.FloorVertex, shadow.FloorFragment};
        dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, uniforms.Gpu, floorBytes);
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, uniforms.Gpu, floorBytes);
        dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(shadow.SilhouetteSlot, mLinearClampSampler));
        dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
        dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
        ApplyDkViewport(cmd, static_cast<float>(first.Viewport.X), static_cast<float>(first.Viewport.Y),
                        static_cast<float>(first.Viewport.Width), static_cast<float>(first.Viewport.Height));
        dkCmdBufSetScissors(cmd, 0, &first.Scissor, 1);
        dkCmdBufDraw(cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        break;
    }
    }
    mDevice.MarkShadowEnd(static_cast<uint32_t>(method));
    mDevice.Checkpoint("shadow");
    shadow.Meshes += draws.size();
    ++shadow.MethodFrames[static_cast<size_t>(method)];
    if (shadow.Frames++ % kPlayerShadowReportFrames == 0U) {
        const std::string_view word = Oot3d::PlayerShadowMethodWord(method);
        const auto& drawn = shadow.MethodFrames;
        Log("shadow: frame %llu %.*s, %zu meshes opacity %.2f height %.0f | %llu frames (two_pass %llu, one_pass "
            "%llu, cover %llu, texture %llu), %llu meshes, %llu skipped, %llu frames without the stencil bit, %llu "
            "full stencil clears",
            static_cast<unsigned long long>(mDevice.Frame()), static_cast<int>(word.size()), word.data(),
            draws.size(), static_cast<double>(opacity), static_cast<double>(shadow.Selected->Height),
            static_cast<unsigned long long>(shadow.Frames), static_cast<unsigned long long>(drawn[0]),
            static_cast<unsigned long long>(drawn[1]), static_cast<unsigned long long>(drawn[2]),
            static_cast<unsigned long long>(drawn[3]), static_cast<unsigned long long>(shadow.Meshes),
            static_cast<unsigned long long>(shadow.Skipped), static_cast<unsigned long long>(shadow.StencilFrames),
            static_cast<unsigned long long>(shadow.FullClears));
    }
}

} // namespace Fast
#endif
