// deko3d PICA path, package 5 (docs/DEKO3D_PROGRESSO.md): render targets,
// fills and clears, pipeline state, the draw, display transfers and the
// scanout. Behaviour follows the GL backend (gfx_opengl_pica.cpp,
// SubmitPicaDraw / SubmitPicaDisplayTransfer / ClearPicaRenderTarget /
// SubmitPicaMemoryFill); the pure decisions are in
// fast/backends/gfx_deko3d_pica_pipeline.h.
//
// Not here yet (documented in DEKO3D_PROGRESSO.md): Shadow2D producers
// (skipped, counted), dynamic resolution, the post-processing chain
// (FXAA/SMAA/EASU/RCAS), the widescreen menu corner passes and the
// relocated-menu nearest sampling.
#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "fast/oot3d/gl_widescreen_menu_policy.h"
#include "fast/oot3d/gl_widescreen_policy.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_draw_coverage.h"
#include "gfx_deko3d_api.h"
#include "gfx_deko3d_scanout_dksh.h"
#include "gfx_opengl_pica_scale.h"

#include <switch.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>

namespace Fast {
using WindowAcquireHook = void (*)(bool after);
WindowAcquireHook GetWindowAcquireHook();

namespace {
using namespace Fast::Deko3d;

// The pure header returns deko3d's enum values.
static_assert(DkCompareOp_Never == 1 && DkCompareOp_Always == 8 && DkCompareOp_Lequal == 4);
static_assert(DkStencilOp_Keep == 1 && DkStencilOp_DecrWrap == 8);
static_assert(DkLogicOp_Copy == kDekoLogicOpCopy && DkLogicOp_Set == 15 && DkLogicOp_Nand == 14);
static_assert(DkBlendOp_Add == 1 && DkBlendOp_Max == 5);
static_assert(DkBlendFactor_SrcAlphaSaturate == 11 && DkBlendFactor_InvConstAlpha == (4 | 0x20));
static_assert(DkPrimitive_Triangles == 4 && DkPrimitive_TriangleStrip == 5 && DkPrimitive_TriangleFan == 6);

// Keys of the built-in scanout stages in the ShaderStore (not text hashes).
constexpr uint64_t kScanoutVertexKey = 0xDE0C3D5CA0000001ULL;
constexpr uint64_t kScanoutFragmentKey = 0xDE0C3D5CA0000002ULL;

std::optional<std::string> ReadFirstLine(const char* path) {
    std::ifstream file(path);
    std::string text;
    if (!file || !std::getline(file, text)) {
        return std::nullopt;
    }
    return text;
}

// Same files and limits as the GL backend's ReadPicaGlResolutionScale.
float ReadResolutionScale() {
    const auto docked = ReadFirstLine("sdmc:/switch/oot3dre/config/resolution_scale_docked");
    const auto common = ReadFirstLine("sdmc:/switch/oot3dre/config/resolution_scale");
    const float scale = PicaGlScale::SelectResolutionScale(docked ? &*docked : nullptr, common ? &*common : nullptr,
                                                           appletGetOperationMode() == AppletOperationMode_Console);
    return PicaGlScale::FitResolutionScale(scale, 16384);
}

DkVtxAttribSize AttributeSize(PicaAttributeKind kind, uint8_t components) {
    const uint32_t index = std::clamp<uint32_t>(components, 1U, 4U) - 1U;
    static constexpr DkVtxAttribSize k32[] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32,
                                              DkVtxAttribSize_4x32};
    static constexpr DkVtxAttribSize k16[] = {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16,
                                              DkVtxAttribSize_4x16};
    static constexpr DkVtxAttribSize k8[] = {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8,
                                             DkVtxAttribSize_4x8};
    switch (kind) {
        case PicaAttributeKind::SignedByte:
        case PicaAttributeKind::UnsignedByte: return k8[index];
        case PicaAttributeKind::SignedShort: return k16[index];
        case PicaAttributeKind::Float:
        case PicaAttributeKind::Constant: break;
    }
    return k32[index];
}

// glVertexAttribPointer(normalized = GL_FALSE): integers convert to float
// unnormalized (scaled).
DkVtxAttribType AttributeType(PicaAttributeKind kind) {
    switch (kind) {
        case PicaAttributeKind::SignedByte:
        case PicaAttributeKind::SignedShort: return DkVtxAttribType_Sscaled;
        case PicaAttributeKind::UnsignedByte: return DkVtxAttribType_Uscaled;
        case PicaAttributeKind::Float:
        case PicaAttributeKind::Constant: break;
    }
    return DkVtxAttribType_Float;
}

DkScissor ToDkScissor(const PicaRect& rect, uint32_t width, uint32_t height) {
    const PicaRect clipped = ClipPicaRect(rect, width, height);
    return {static_cast<uint32_t>(clipped.X), static_cast<uint32_t>(clipped.Y), static_cast<uint32_t>(clipped.Width),
            static_cast<uint32_t>(clipped.Height)};
}

// ResolvePicaGlWidescreenPlacement of the GL backend without its diagnostic
// log (config/widescreen_menus_log) and without the corner passes.
Oot3d::GlWidescreenDrawPlacement ResolveWidescreenPlacement(const GfxNativePicaDrawView& draw,
                                                            float widescreenFactor) {
    const bool menus = Oot3d::GlWidescreenMenusEnabled();
    Oot3d::GlWidescreenDrawEvidence evidence;
    evidence.UiDomain = draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui;
    evidence.Atmosphere = draw.Composition.Layer == Renderer3ds::PicaCompositionLayer::Atmosphere;
    evidence.ViewportSpansPresentedWidth =
        draw.ViewportY <= 0.5F &&
        draw.ViewportY + draw.ViewportHeight >= static_cast<float>(draw.FramebufferHeight) - 0.5F;
    evidence.ViewportCoversTarget = draw.ViewportX <= 0.0F && draw.ViewportY <= 0.0F &&
                                    draw.ViewportX + draw.ViewportWidth >= static_cast<float>(draw.FramebufferWidth) &&
                                    draw.ViewportY + draw.ViewportHeight >= static_cast<float>(draw.FramebufferHeight);
    const bool uniformsAvailable =
        draw.VertexUniformBytes.size() >= Oot3d::kPicaPackedVertexFloatOffset + 4U * Oot3d::kPicaPackedVec4Bytes;
    if (menus && uniformsAvailable) {
        std::array<float, 16> rows{};
        std::memcpy(rows.data(), draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexFloatOffset, sizeof(rows));
        const auto projection = Oot3d::DecodeGlProjection(rows);
        evidence.Projection = projection.Shape;
        evidence.PixelAnisotropy = Oot3d::GlNativePixelAnisotropy(projection, draw.ViewportWidth,
                                                                  draw.ViewportHeight, draw.FramebufferWidth);
    }
    if (menus && !evidence.UiDomain) {
        evidence.SamplesRenderedCopy = std::any_of(draw.Textures.begin(), draw.Textures.end(), [](const auto& texture) {
            return !texture.CopyReferences.empty();
        });
    }
    if (menus) {
        evidence.Untextured = draw.Textures.empty();
        evidence.GuestScissor = draw.ScissorMode != 0U;
    }
    const bool relocatedFill = menus && Oot3d::GlWidescreenRelocatedFillCandidate(evidence);
    const bool needsCoverage = uniformsAvailable && ((evidence.UiDomain && evidence.ViewportCoversTarget) ||
                                                     (menus && Oot3d::GlWidescreenOrthographicPanelCandidate(evidence)) ||
                                                     relocatedFill);
    if (needsCoverage) {
        uint32_t booleanUniforms = 0U;
        std::memcpy(&booleanUniforms, draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexBooleanMaskOffset,
                    sizeof(booleanUniforms));
        const auto bounds = Renderer3ds::PicaDrawGeometryNdcBounds(
            draw, booleanUniforms, draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset));
        const float horizontal =
            relocatedFill ? Oot3d::GlWidescreenNativeHorizontalExtent(evidence, widescreenFactor) : 1.0F;
        evidence.GeometryCoversViewport = bounds.has_value() && bounds->Reaches({1.0F, horizontal});
    }
    auto placement = Oot3d::ResolveGlWidescreenDrawPlacement(evidence, widescreenFactor, menus);
    // The corner passes need the game side's pointer remap, which is only
    // active with the GL backend (WidescreenMenuCornersActive).
    placement.SplitCorners = false;
    return placement;
}
} // namespace

void GfxRenderingAPIDeko3d::InitPica() {
    float widescreen = 1.0F;
    if (Oot3d::GlWidescreenEnabled()) {
        widescreen = Oot3d::GlWidescreenFactor(mDevice.Width(), mDevice.Height());
    }
    mTargets.Init(mDevice, mResources, ReadResolutionScale(), widescreen);
    if (const auto lean = ReadFirstLine("sdmc:/switch/oot3dre/config/gl_lean")) {
        mPicaLean = PicaGlScale::ParseLeanSetting(*lean, true);
    }
    if (const auto invert = ReadFirstLine("sdmc:/switch/oot3dre/config/deko3d_cull_invert")) {
        mCullInvert = invert->empty() || (*invert)[0] != '0';
    }
    if (const auto flip = ReadFirstLine("sdmc:/switch/oot3dre/config/deko3d_flip_y")) {
        mFlipY = flip->empty() || (*flip)[0] != '0';
    }
    PicaSamplerKey linear;
    linear.MinLinear = true;
    linear.MagLinear = true;
    mLinearClampSampler = mResources.SamplerSlot(linear);
    mScanoutVertex = mShaders.Embedded(kScanoutVertexKey, std::span<const uint8_t>(kScanoutVertexDksh));
    mScanoutFragment = mShaders.Embedded(kScanoutFragmentKey, std::span<const uint8_t>(kScanoutFragmentDksh));
    Deko3d::Log("pica: scanout shaders %s, cull invert %d, flip y %d, lean %d",
                mScanoutVertex != nullptr && mScanoutFragment != nullptr ? "ready" : "MISSING",
                mCullInvert ? 1 : 0, mFlipY ? 1 : 0, mPicaLean ? 1 : 0);
}

void GfxRenderingAPIDeko3d::BindPicaTarget(PicaTarget& target) {
    if (mBoundTarget == &target) {
        return;
    }
    const DkImageView* colors[] = {&target.ColorView};
    dkCmdBufBindRenderTargets(mDevice.Cmd(), colors, 1, &target.DepthView);
    mBoundTarget = &target;
}

void GfxRenderingAPIDeko3d::FlushPicaInvalidations() {
    uint32_t flags = 0U;
    DkBarrier mode = DkBarrier_None;
    if (mResources.TakeInvalidate()) {
        // New images were written by copies (or descriptors changed) since
        // the last draw: wait for them, then drop stale cached lines. Once
        // per frame at least (BeginFrame), which also covers the uniform
        // cache over the reused stream memory.
        mode = DkBarrier_Full;
        flags |= DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader;
    }
    if (mShaders.TakeCodeWritten()) {
        flags |= DkInvalidateFlags_Shader;
    }
    if (flags != 0U) {
        dkCmdBufBarrier(mDevice.Cmd(), mode, flags);
    }
}

void GfxRenderingAPIDeko3d::ApplyPicaFill(const GfxNativePicaMemoryFillView& fill, const PicaTargetKey& key,
                                          PicaTarget& target, PicaFillCoverage coverage) {
    BindPicaTarget(target);
    DkCmdBuf cmd = mDevice.Cmd();
    const DkScissor scissor = {0, 0, target.Extent.Width, target.Extent.Height};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    if (coverage.Color) {
        const auto color = DecodePicaClearColor(fill, key.ColorFormat);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, color[0], color[1], color[2], color[3]);
        ++target.Version;
    }
    if (coverage.Depth) {
        const auto [depth, stencil] = DecodePicaClearDepthStencil(fill, key.DepthFormat);
        dkCmdBufClearDepthStencil(cmd, true, depth, PicaDepthFillClearsStencil(key.DepthFormat) ? 0xFF : 0x00,
                                  stencil);
    }
    ++mCounters.PicaFills;
    mDevice.Checkpoint("fill", fill.StartPhysicalAddress, fill.EndPhysicalAddress);
}

void GfxRenderingAPIDeko3d::ApplyPendingPicaFills(const PicaTargetKey& key, PicaTarget& target) {
    auto fill = mPendingFills.begin();
    while (fill != mPendingFills.end()) {
        if (fill->RenderTargetNamespace != key.Namespace) {
            ++fill;
            continue;
        }
        const PicaFillCoverage coverage = PicaFillCovers(*fill, key);
        if (!coverage.Color && !coverage.Depth) {
            ++fill;
            continue;
        }
        ApplyPicaFill(*fill, key, target, coverage);
        fill = mPendingFills.erase(fill);
    }
}

bool GfxRenderingAPIDeko3d::SubmitPicaDraw(const GfxNativePicaDrawView& draw, std::string* error) {
    const auto reject = [&](const char* why) {
        if (mCounters.PicaDrawsRejected++ < 10U) {
            Deko3d::Log("draw rejected: %s", why);
        }
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (mCalls.Draws++ == 0U) {
        Deko3d::Log("pica: first draw (ns %llu, framebuffer %ux%u colour %08x depth %08x, %u vertices)",
                    static_cast<unsigned long long>(draw.RenderTargetNamespace), draw.FramebufferWidth,
                    draw.FramebufferHeight, draw.FramebufferColorPhysicalAddress,
                    draw.FramebufferDepthPhysicalAddress, draw.VertexCount);
    }
    if (!mDeviceReady) {
        return true;
    }
    if (draw.VertexShaderSource.empty() || draw.FragmentShaderSource.empty() || draw.VertexCount == 0U ||
        draw.VertexBindings.empty() || draw.VertexAttributes.empty()) {
        return reject("PICA draw is incomplete");
    }
    if (draw.FragmentOperationMode != 0U && draw.FragmentOperationMode != 3U) {
        return reject("PICA fragment operation is unsupported");
    }
    if (draw.ScissorMode == 1U) {
        return reject("PICA exclude scissor requires shader emulation");
    }
    if (draw.ScissorMode != 0U && draw.ScissorMode != 3U) {
        return reject("invalid PICA scissor mode");
    }
    if (draw.FramebufferWidth == 0U || draw.FramebufferHeight == 0U) {
        return reject("PICA framebuffer has zero extent");
    }
    EnsureFrame();

    const PicaTargetKey key = MakePicaTargetKey(draw);
    bool created = false;
    PicaTarget* target = mTargets.Get(key, &created);
    if (created) {
        mBoundTarget = target;
    }
    if (target == nullptr) {
        ++mCounters.PicaDrawsSkipped;
        return true;
    }
    ApplyPendingPicaFills(key, *target);
    if (draw.FragmentOperationMode == 3U) {
        // Shadow2D producer: writes an R32UI image through image stores,
        // which no packed program declares yet. Its consumers read the
        // "no shadow" fallback (0xFFFFFFFF).
        ++mCounters.PicaShadowProducersSkipped;
        ++mCounters.PicaDrawsSkipped;
        return true;
    }

    const PicaProgram program = ResolvePicaProgram(draw);
    if (program.Vertex == nullptr || program.Fragment == nullptr) {
        ++mCounters.PicaDrawsSkipped;
        return true;
    }
    std::string why;
    if (!mResources.Prepare(draw, mDrawResources, &why)) {
        if (mCounters.PicaDrawsRejected++ < 10U) {
            Deko3d::Log("draw rejected: %s", why.c_str());
        }
        return true;
    }
    ++mCounters.PicaDrawsPrepared;
    mDevice.Checkpoint("prepare", mCalls.Draws, draw.VertexCount);
    if (RecordPicaDraw(draw, program, key, *target)) {
        ++mCounters.PicaDrawsIssued;
    }
    mDevice.Checkpoint("draw", mCalls.Draws, draw.VertexCount);
    return true;
}

bool GfxRenderingAPIDeko3d::RecordPicaDraw(const GfxNativePicaDrawView& draw, const PicaProgram& program,
                                           const PicaTargetKey& key, PicaTarget& target) {
    // GPU lean of the GL backend: a draw that can change no buffer is not
    // issued (blend/logic op are only "plain" when they change nothing too,
    // so only the masks decide here).
    const uint8_t colorMask = static_cast<uint8_t>(draw.ColorWriteMask & 0xFU);
    const bool stencilWrites = draw.Stencil.Enabled && draw.Stencil.WriteMask != 0U &&
                               !(draw.Stencil.Fail == Renderer3ds::PicaStencilAction::Keep &&
                                 draw.Stencil.DepthFail == Renderer3ds::PicaStencilAction::Keep &&
                                 draw.Stencil.Pass == Renderer3ds::PicaStencilAction::Keep);
    if (colorMask == 0U && !draw.DepthWriteEnabled && !stencilWrites) {
        ++mCounters.PicaNoEffectSkipped;
        return false;
    }

    DkCmdBuf cmd = mDevice.Cmd();
    FlushPicaInvalidations();
    BindPicaTarget(target);
    ++target.Version;

    const DkShader* shaders[] = {program.Vertex, program.Fragment};
    dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    const PicaDrawResources& res = mDrawResources;
    if (res.VertexUniforms != DK_GPU_ADDR_INVALID) {
        dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, res.VertexUniforms, res.VertexUniformBytes);
    }
    if (res.FragmentUniforms != DK_GPU_ADDR_INVALID) {
        dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 4, res.FragmentUniforms, res.FragmentUniformBytes);
    }
    dkCmdBufBindTextures(cmd, DkStage_Fragment, 1, res.Textures.data(), static_cast<uint32_t>(res.Textures.size()));
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 13, res.LightingLut);

    // Vertices: every binding in the draw's stream block, absent attributes
    // on the constant (0, 0, 0, 1).
    const PicaVertexPlan& plan = res.Vertices;
    std::array<DkVtxAttribState, kPicaVertexAttributeCount> attributes{};
    for (uint32_t location = 0U; location < kPicaVertexAttributeCount; ++location) {
        const PicaAttributePlan& attribute = plan.Attributes[location];
        DkVtxAttribState& state = attributes[location];
        const bool present = (plan.AttributeMask & (1U << location)) != 0U;
        state.bufferId = present ? attribute.Binding : kPicaConstantVertexBinding;
        state.isFixed = 0;
        state.offset = present ? attribute.Offset : 0U;
        state.size = present ? AttributeSize(attribute.Kind, attribute.Components) : DkVtxAttribSize_4x32;
        state.type = present ? AttributeType(attribute.Kind) : DkVtxAttribType_Float;
        state.isBgra = 0;
    }
    std::array<DkVtxBufferState, kPicaVertexAttributeCount> buffers{};
    std::array<DkBufExtents, kPicaVertexAttributeCount> extents{};
    for (uint32_t binding = 0U; binding < kPicaVertexAttributeCount; ++binding) {
        if (binding == kPicaConstantVertexBinding) {
            // Per instance: every vertex of the (single) instance reads it.
            buffers[binding] = {16U, 1U};
            extents[binding] = {mResources.ConstantAttribute(), 16U};
        } else if ((plan.BindingMask & (1U << binding)) != 0U && res.VertexStream != DK_GPU_ADDR_INVALID) {
            buffers[binding] = {plan.BindingStride[binding], plan.BindingPerInstance[binding] ? 1U : 0U};
            extents[binding] = {res.VertexStream + plan.BindingOffset[binding],
                                plan.StreamBytes - plan.BindingOffset[binding]};
        } else {
            buffers[binding] = {16U, 0U};
            extents[binding] = {mResources.ConstantAttribute(), 16U};
        }
    }
    dkCmdBufBindVtxAttribState(cmd, attributes.data(), static_cast<uint32_t>(attributes.size()));
    dkCmdBufBindVtxBufferState(cmd, buffers.data(), static_cast<uint32_t>(buffers.size()));
    dkCmdBufBindVtxBuffers(cmd, 0, extents.data(), static_cast<uint32_t>(extents.size()));

    // Output merger: blend, else the logic op (COPY = off).
    DkColorState colorState;
    dkColorStateDefaults(&colorState);
    DkBlendState blend;
    dkBlendStateDefaults(&blend);
    if (draw.Blend.Enabled) {
        dkColorStateSetBlendEnable(&colorState, 0, true);
        blend.colorBlendOp = static_cast<DkBlendOp>(DekoBlendOp(draw.Blend.EquationRgb));
        blend.alphaBlendOp = static_cast<DkBlendOp>(DekoBlendOp(draw.Blend.EquationAlpha));
        blend.srcColorBlendFactor = static_cast<DkBlendFactor>(DekoBlendFactor(draw.Blend.SourceRgb));
        blend.dstColorBlendFactor = static_cast<DkBlendFactor>(DekoBlendFactor(draw.Blend.DestRgb));
        blend.srcAlphaBlendFactor = static_cast<DkBlendFactor>(DekoBlendFactor(draw.Blend.SourceAlpha));
        blend.dstAlphaBlendFactor = static_cast<DkBlendFactor>(DekoBlendFactor(draw.Blend.DestAlpha));
        dkCmdBufBindBlendState(cmd, 0, &blend);
        dkCmdBufSetBlendConst(cmd, draw.Blend.ConstantColor[0], draw.Blend.ConstantColor[1],
                              draw.Blend.ConstantColor[2], draw.Blend.ConstantColor[3]);
    } else {
        colorState.logicOp = static_cast<DkLogicOp>(DekoLogicOp(draw.LogicOperation));
    }
    dkCmdBufBindColorState(cmd, &colorState);
    DkColorWriteState writeState;
    dkColorWriteStateDefaults(&writeState);
    dkColorWriteStateSetMask(&writeState, 0, colorMask);
    dkCmdBufBindColorWriteState(cmd, &writeState);

    DkDepthStencilState depthStencil;
    dkDepthStencilStateDefaults(&depthStencil);
    depthStencil.depthTestEnable = draw.DepthTestEnabled || draw.DepthWriteEnabled;
    depthStencil.depthWriteEnable = draw.DepthWriteEnabled;
    depthStencil.depthCompareOp =
        static_cast<DkCompareOp>(draw.DepthTestEnabled ? DekoCompareOp(draw.DepthCompare) : 8U);
    depthStencil.stencilTestEnable = draw.Stencil.Enabled;
    if (draw.Stencil.Enabled) {
        const auto fail = static_cast<DkStencilOp>(DekoStencilOp(draw.Stencil.Fail));
        const auto depthFail = static_cast<DkStencilOp>(DekoStencilOp(draw.Stencil.DepthFail));
        const auto pass = static_cast<DkStencilOp>(DekoStencilOp(draw.Stencil.Pass));
        const auto compare = static_cast<DkCompareOp>(DekoCompareOp(draw.Stencil.Compare));
        depthStencil.stencilFrontFailOp = fail;
        depthStencil.stencilFrontDepthFailOp = depthFail;
        depthStencil.stencilFrontPassOp = pass;
        depthStencil.stencilFrontCompareOp = compare;
        depthStencil.stencilBackFailOp = fail;
        depthStencil.stencilBackDepthFailOp = depthFail;
        depthStencil.stencilBackPassOp = pass;
        depthStencil.stencilBackCompareOp = compare;
        dkCmdBufSetStencil(cmd, DkFace_FrontAndBack, static_cast<uint8_t>(draw.Stencil.WriteMask),
                           static_cast<uint8_t>(draw.Stencil.Reference),
                           static_cast<uint8_t>(draw.Stencil.CompareMask));
    }
    dkCmdBufBindDepthStencilState(cmd, &depthStencil);

    // Clip at near/far like the PICA (GL disables GL_DEPTH_CLAMP here).
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.depthClampEnable = 0;
    // p6 on the console (unflipped viewports): the sky dome was culled and the
    // top scene, which OoT3D never clears, smeared. The flipped viewport
    // mirrors the window-space winding, which is what fixes the culling;
    // config/deko3d_cull_invert stays as the A/B switch.
    const PicaCullPlan cull = PlanPicaCull(draw.CullMode, draw.FramebufferFlipped, mCullInvert);
    rasterizer.cullMode = !cull.Enabled ? DkFace_None : (cull.CullFront ? DkFace_Front : DkFace_Back);
    rasterizer.frontFace = cull.FrontCcw ? DkFrontFace_CCW : DkFrontFace_CW;
    dkCmdBufBindRasterizerState(cmd, &rasterizer);

    // Viewport and scissor in host pixels of the target.
    const float scale = target.Extent.Scale;
    PicaRect viewport;
    PicaRect scissor;
    if (!target.Extent.Wide) {
        viewport = PlanPicaViewport(draw.ViewportX, draw.ViewportY, draw.ViewportWidth, draw.ViewportHeight, scale);
        scissor = PlanPicaScissor(draw.ScissorMode, draw.ScissorX1, draw.ScissorY1, draw.ScissorX2, draw.ScissorY2,
                                  scale, target.Extent.Width, target.Extent.Height);
    } else {
        // Widescreen top target (GL: PicaGlTarget::Wide): the scene stretches
        // over the widened axis, placed UI keeps a centred native canvas.
        const auto placement = ResolveWidescreenPlacement(draw, target.Extent.ScaleY / scale);
        Oot3d::GlWidescreenPresentedAxisInput axisInput;
        axisInput.ViewportY = draw.ViewportY;
        axisInput.ViewportHeight = draw.ViewportHeight;
        axisInput.GuestScissor = draw.ScissorMode == 3U;
        axisInput.ScissorY1 = draw.ScissorY1;
        axisInput.ScissorY2 = draw.ScissorY2;
        axisInput.FramebufferHeight = draw.FramebufferHeight;
        axisInput.TargetHeight = target.Extent.Height;
        axisInput.ResolutionScale = scale;
        axisInput.TargetScaleY = target.Extent.ScaleY;
        const auto axis = Oot3d::ResolveGlWidescreenPresentedAxis(placement, axisInput);
        const auto edgeX = [&](float native) { return static_cast<int32_t>(std::lround(native * scale)); };
        const int32_t viewportX0 = edgeX(draw.ViewportX);
        viewport = {viewportX0, axis.ViewportBegin,
                    std::max<int32_t>(0, edgeX(draw.ViewportX + draw.ViewportWidth) - viewportX0),
                    axis.ViewportEnd - axis.ViewportBegin};
        int32_t scissorX0 = 0;
        int32_t scissorX1 = static_cast<int32_t>(target.Extent.Width);
        if (draw.ScissorMode == 3U) {
            scissorX0 = edgeX(static_cast<float>(draw.ScissorX1));
            scissorX1 = draw.ScissorX2 >= draw.ScissorX1 ? edgeX(static_cast<float>(draw.ScissorX2) + 1.0F) : scissorX0;
        }
        scissor = {scissorX0, axis.ScissorBegin, std::max<int32_t>(0, scissorX1 - scissorX0),
                   axis.ScissorEnd - axis.ScissorBegin};
    }
    if (viewport.Width <= 0 || viewport.Height <= 0) {
        ++mCounters.PicaNoEffectSkipped;
        return false;
    }
    ApplyDkViewport(cmd, static_cast<float>(viewport.X), static_cast<float>(viewport.Y),
                    static_cast<float>(viewport.Width), static_cast<float>(viewport.Height));
    const DkScissor dkScissor = ToDkScissor(scissor, target.Extent.Width, target.Extent.Height);
    if (dkScissor.width == 0U || dkScissor.height == 0U) {
        ++mCounters.PicaNoEffectSkipped;
        return false;
    }
    dkCmdBufSetScissors(cmd, 0, &dkScissor, 1);

    const auto primitive = static_cast<DkPrimitive>(DekoPrimitive(draw.Topology));
    if (draw.Indexed) {
        const uint32_t indexSize = draw.IndicesAre16Bit ? 2U : 1U;
        if (res.IndexStream == DK_GPU_ADDR_INVALID || res.IndexBytes % indexSize != 0U) {
            if (mCounters.PicaDrawsRejected++ < 10U) {
                Deko3d::Log("draw rejected: PICA indexed draw has no/partial indices");
            }
            return false;
        }
        dkCmdBufBindIdxBuffer(cmd, draw.IndicesAre16Bit ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8, res.IndexStream);
        dkCmdBufDrawIndexed(cmd, primitive, res.IndexBytes / indexSize, 1, 0, draw.BaseVertex, 0);
    } else {
        dkCmdBufDraw(cmd, primitive, draw.VertexCount, 1, 0, 0);
    }
    (void)key;
    return true;
}

bool GfxRenderingAPIDeko3d::SubmitPicaDisplayTransfer(const GfxNativePicaDisplayTransferView& transfer,
                                                      std::string* error) {
    const auto reject = [&](const char* why) {
        if (mCounters.PicaTransfersRejected++ < 10U) {
            Deko3d::Log("transfer rejected: %s", why);
        }
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    const uint64_t transferCall = mCalls.Transfers++;
    if (transfer.Present) {
        ++mCalls.Presents;
    }
    if (transferCall == 0U || (transfer.Present && mCalls.Presents == 1U)) {
        Deko3d::Log("pica: first %s (ns %llu, %08x %ux%u -> %08x %ux%u, flags %08x, mode %u)",
                    transfer.Present ? "presented transfer" : "transfer",
                    static_cast<unsigned long long>(transfer.RenderTargetNamespace), transfer.InputPhysicalAddress,
                    transfer.InputWidth, transfer.InputHeight, transfer.OutputPhysicalAddress, transfer.OutputWidth,
                    transfer.OutputHeight, transfer.Flags, static_cast<unsigned>(transfer.PresentationMode));
    }
    if (!mDeviceReady) {
        return true;
    }
    EnsureFrame();
    const auto [key, source] = mTargets.FindColor(transfer.RenderTargetNamespace, transfer.InputPhysicalAddress);
    if (source == nullptr) {
        if (transfer.Present) {
            BindWindowTarget(); // the window still shows the clear
        }
        return reject("PICA display transfer source is missing");
    }
    const PicaTransferPlan plan = PlanPicaTransfer(transfer, key->Width, key->Height, source->Extent.Scale,
                                                   source->Extent.ScaleY, source->Extent.Width,
                                                   source->Extent.Height);
    if (!plan.Valid) {
        if (transfer.Present) {
            BindWindowTarget();
        }
        return reject(plan.Error);
    }
    PicaDisplay* display = mTargets.GetDisplay(transfer.RenderTargetNamespace, transfer.OutputPhysicalAddress,
                                               plan.OutputWidth, plan.OutputHeight, plan.AllocWidth,
                                               plan.AllocHeight);
    if (display == nullptr) {
        if (transfer.Present) {
            BindWindowTarget();
        }
        return reject("deko3d display image memory");
    }
    const PicaDisplayCopy copy{source->Id, source->Version, plan.SourceWidth, plan.SourceHeight, plan.Linear};
    DkCmdBuf cmd = mDevice.Cmd();
    if (mPicaLean && display->LastCopy.Valid() && display->LastCopy == copy) {
        // A presentation re-submits the transfer its sample already ran.
        ++mCounters.PicaTransfersRepeated;
    } else {
        // The source's fragments must be in memory before the 2D engine
        // reads them; the copy must land before the scanout samples it and
        // before later draws write the source again.
        dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
        const DkImageRect sourceRect = {0, 0, 0, plan.SourceWidth, plan.SourceHeight, 1};
        const DkImageRect destRect = {0, 0, 0, plan.ContentWidth, plan.ContentHeight, 1};
        dkCmdBufBlitImage(cmd, &source->ColorView, &sourceRect, &display->View, &destRect,
                          plan.Linear ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest, 0);
        dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image);
        display->LastCopy = copy;
        mDevice.Checkpoint("transfer blit", plan.SourceWidth, plan.SourceHeight);
        ++mCounters.PicaTransfers;
    }
    display->ContentWidth = plan.ContentWidth;
    display->ContentHeight = plan.ContentHeight;
    if (transfer.Present) {
        DrawPicaScanout(transfer, *display, plan, source->Extent.Scale);
        mDevice.Checkpoint("scanout", transfer.OutputPhysicalAddress, 0);
    }
    return true;
}

void GfxRenderingAPIDeko3d::DrawPicaScanout(const GfxNativePicaDisplayTransferView& transfer,
                                            const PicaDisplay& display, const PicaTransferPlan& plan,
                                            float allocScale) {
    BindWindowTarget();
    if (mScanoutVertex == nullptr || mScanoutFragment == nullptr) {
        return;
    }
    DkCmdBuf cmd = mDevice.Cmd();
    const uint32_t windowWidth = mDevice.Width();
    const uint32_t windowHeight = mDevice.Height();
    const PicaScanoutPlacement placement =
        PlanPicaScanout(plan.OutputWidth, plan.OutputHeight, display.AllocHeight, allocScale, windowWidth, windowHeight);
    const bool overlay = transfer.PresentationMode == GfxNativePicaPresentationMode::AlphaOverlay;
    const DkScissor whole = {0, 0, windowWidth, windowHeight};
    dkCmdBufSetScissors(cmd, 0, &whole, 1);
    if (!overlay && !(mPicaLean && placement.CoversWindow)) {
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, mClearColor[0], mClearColor[1], mClearColor[2],
                                mClearColor[3]);
    }
    FlushPicaInvalidations();

    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rasterizer);
    DkDepthStencilState depthStencil;
    dkDepthStencilStateDefaults(&depthStencil);
    depthStencil.depthTestEnable = 0;
    depthStencil.depthWriteEnable = 0;
    depthStencil.stencilTestEnable = 0;
    dkCmdBufBindDepthStencilState(cmd, &depthStencil);
    DkColorState colorState;
    dkColorStateDefaults(&colorState);
    if (overlay) {
        dkColorStateSetBlendEnable(&colorState, 0, true);
        DkBlendState blend;
        dkBlendStateDefaults(&blend);
        dkBlendStateSetOps(&blend, DkBlendOp_Add, DkBlendOp_Add);
        dkBlendStateSetFactors(&blend, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_SrcAlpha,
                               DkBlendFactor_InvSrcAlpha);
        dkCmdBufBindBlendState(cmd, 0, &blend);
    }
    dkCmdBufBindColorState(cmd, &colorState);
    DkColorWriteState writeState;
    dkColorWriteStateDefaults(&writeState);
    dkCmdBufBindColorWriteState(cmd, &writeState);

    ApplyDkViewport(cmd, static_cast<float>(placement.Viewport.X), static_cast<float>(placement.Viewport.Y),
                    static_cast<float>(placement.Viewport.Width), static_cast<float>(placement.Viewport.Height));

    const StreamSpan params = mDevice.Stream(DK_UNIFORM_BUF_ALIGNMENT, DK_UNIFORM_BUF_ALIGNMENT);
    if (params.Cpu == nullptr) {
        return;
    }
    const float values[4] = {
        plan.Flip ? 1.0F : 0.0F, 0.0F,
        display.AllocWidth != 0U ? static_cast<float>(display.ContentWidth) / static_cast<float>(display.AllocWidth)
                                 : 1.0F,
        display.AllocHeight != 0U ? static_cast<float>(display.ContentHeight) / static_cast<float>(display.AllocHeight)
                                  : 1.0F};
    std::memcpy(params.Cpu, values, sizeof(values));
    const DkShader* shaders[] = {mScanoutVertex, mScanoutFragment};
    dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Fragment, 0, params.Gpu, DK_UNIFORM_BUF_ALIGNMENT);
    dkCmdBufBindTexture(cmd, DkStage_Fragment, 0, dkMakeTextureHandle(display.ImageSlot, mLinearClampSampler));
    dkCmdBufBindVtxAttribState(cmd, nullptr, 0);
    dkCmdBufBindVtxBufferState(cmd, nullptr, 0);
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
    ++mCounters.PicaScanouts;
}

bool GfxRenderingAPIDeko3d::ClearPicaRenderTarget(uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
                                                  std::string* error) {
    if (mCalls.Clears++ == 0U) {
        Deko3d::Log("pica: first render-target clear (ns %llu, colour %08x)",
                    static_cast<unsigned long long>(renderTargetNamespace), colorPhysicalAddress);
    }
    if (colorPhysicalAddress == 0U) {
        if (error != nullptr) {
            *error = "PICA render-target clear is incomplete";
        }
        return false;
    }
    if (!mDeviceReady) {
        return true;
    }
    EnsureFrame();
    mTargets.ForEach([&](const PicaTargetKey& key, PicaTarget& target) {
        if (key.Namespace != renderTargetNamespace || key.ColorAddress != colorPhysicalAddress) {
            return;
        }
        BindPicaTarget(target);
        DkCmdBuf cmd = mDevice.Cmd();
        const DkScissor scissor = {0, 0, target.Extent.Width, target.Extent.Height};
        dkCmdBufSetScissors(cmd, 0, &scissor, 1);
        dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0F, 0.0F, 0.0F, 0.0F);
        dkCmdBufClearDepthStencil(cmd, true, 1.0F, 0xFF, 0);
        ++target.Version;
        ++mCounters.PicaClears;
        mDevice.Checkpoint("clear", key.ColorAddress, key.DepthAddress);
    });
    return true;
}

bool GfxRenderingAPIDeko3d::SubmitPicaMemoryFill(const GfxNativePicaMemoryFillView& fill, std::string* error) {
    if (mCalls.Fills++ == 0U) {
        Deko3d::Log("pica: first memory fill (ns %llu, %08x-%08x, control %04x)",
                    static_cast<unsigned long long>(fill.RenderTargetNamespace), fill.StartPhysicalAddress,
                    fill.EndPhysicalAddress, fill.Control);
    }
    if (fill.StartPhysicalAddress == 0U || fill.EndPhysicalAddress <= fill.StartPhysicalAddress ||
        (fill.Control & 1U) == 0U) {
        if (error != nullptr) {
            *error = "PICA memory fill is incomplete";
        }
        return false;
    }
    if (!mDeviceReady) {
        return true;
    }
    EnsureFrame();
    const auto previous = std::find_if(mPendingFills.begin(), mPendingFills.end(), [&](const auto& pending) {
        return pending.RenderTargetNamespace == fill.RenderTargetNamespace &&
               pending.StartPhysicalAddress == fill.StartPhysicalAddress &&
               pending.EndPhysicalAddress == fill.EndPhysicalAddress;
    });
    if (previous != mPendingFills.end()) {
        *previous = fill;
    } else {
        mPendingFills.push_back(fill);
    }
    mTargets.ForEach([&](const PicaTargetKey& key, PicaTarget& target) { ApplyPendingPicaFills(key, target); });
    return true;
}

bool GfxRenderingAPIDeko3d::ResetPicaState(std::string*) {
    Deko3d::Log("pica: reset %llu", static_cast<unsigned long long>(++mCalls.Resets));
    if (!mDeviceReady) {
        return true;
    }
    // Like the GL backend's re-init: targets and display images start over.
    if (mDevice.FrameOpen()) {
        mDevice.Submit();
    }
    mDevice.WaitIdle();
    mTargets.Reset();
    mPendingFills.clear();
    mBoundTarget = nullptr;
    return true;
}

} // namespace Fast
#endif
