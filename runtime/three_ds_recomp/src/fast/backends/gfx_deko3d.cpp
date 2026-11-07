// GfxRenderingAPI on deko3d (Switch). See gfx_deko3d_api.h and
// docs/DEKO3D_PROGRESS.md.
#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "gfx_deko3d_api.h"
#include "fast/oot3d/directional_shadows.h"
#include "fast/oot3d/graphics_settings_runtime.h"
#include "fast/oot3d/pica_rim_light.h"
#include "fast/oot3d/pica_toon_shader.h"
#include "fast/oot3d/pica_uniform_layout.h"
#include "fast/renderer3ds/pica_gl_shader_dialect.h"
#include "fast/renderer3ds/pica_surface_appearance.h"
#include "oot3d/renderer/azahar_texture_pack.h"

#include <switch.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Fast {
// gfx_opengl.cpp: the hook around the blocking window acquire (render-wait
// clock, GPU frame timer); shared by both backends.
using WindowAcquireHook = void (*)(bool after);
WindowAcquireHook GetWindowAcquireHook();

GfxRenderingAPI* CreateDeko3dRenderingApi() {
    return new GfxRenderingAPIDeko3d();
}

GfxRenderingAPIDeko3d::GfxRenderingAPIDeko3d() = default;

GfxRenderingAPIDeko3d::~GfxRenderingAPIDeko3d() {
    Shutdown();
}

const char* GfxRenderingAPIDeko3d::GetName() {
    return "deko3d";
}

int GfxRenderingAPIDeko3d::GetMaxTextureSize() {
    return 16384;
}

GfxClipParameters GfxRenderingAPIDeko3d::GetClipParameters() {
    // Origin upper-left: NDC y = -1 is the top row of every image. The
    // window is drawn like a GL framebuffer with invertY.
    const bool invertY = mCurrentFramebuffer >= 0 &&
                         static_cast<size_t>(mCurrentFramebuffer) < mFramebuffers.size() &&
                         mFramebuffers[static_cast<size_t>(mCurrentFramebuffer)].InvertY;
    return {false, invertY};
}

void GfxRenderingAPIDeko3d::Init() {
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    const uint32_t width = docked ? 1920U : 1280U;
    const uint32_t height = docked ? 1080U : 720U;
    std::string error;
    if (!mDevice.Create(width, height, &error)) {
        throw std::runtime_error(error);
    }
    Deko3d::Log("backend %s (built %s %s)", kDeko3dBuildTag, __DATE__, __TIME__);
    if (!mShaders.Init(mDevice)) {
        throw std::runtime_error("deko3d: shader code memory");
    }
    // Per-pixel surface lighting (the Modern model) needs its variants from
    // deko3d.pack; then config/lighting (off when absent) becomes the
    // graphics settings' model, which the menus and the renderer read.
    auto& graphics = Oot3d::GraphicsSettingsRuntime::Instance();
    const bool surfaceLighting = mShaders.Stats().PackEntries != 0U;
    graphics.SetCapability(Oot3d::GraphicsCapability::NativeSurfaceLighting, surfaceLighting,
                           surfaceLighting ? "" : "deko3d: shadercache/deko3d.pack is missing");
    if (const auto mode = Deko3d::LoadLightingMode("sdmc:/switch/oot3dre/config"); mode != Deko3d::LightingMode::Off) {
        graphics.Apply(Deko3d::WithLightingMode(graphics.Snapshot(), mode), false);
    }
    ApplyLightingSettings();
    if (!mResources.Init(mDevice)) {
        Deko3d::Log("init failed: descriptor/fallback memory");
        throw std::runtime_error("deko3d: descriptor/fallback memory");
    }
    mDeviceReady = true;
    InitPica();
    InitUi();
    if (mFramebuffers.empty()) {
        mFramebuffers.push_back({width, height, false, true});
    }
}

void GfxRenderingAPIDeko3d::Shutdown() {
    if (mDeviceReady) {
        LogPicaProgramCounters("shutdown");
        mDevice.WaitIdle();
        ShutdownUi();
        mVertexStages.Clear();
        mFragmentStages.Clear();
        mToonFragments.Clear();
        mPerPixelFragments.Clear();
        mPerPixelVertices.clear();
        mTargets.Shutdown();
        mPendingFills.clear();
        mBoundTarget = nullptr;
        mResources.Shutdown();
        mResourcesFrame = ~0ULL;
        mShaders.Shutdown();
        mDevice.Destroy();
        mDeviceReady = false;
    }
}

void GfxRenderingAPIDeko3d::OnResize() {
}

void GfxRenderingAPIDeko3d::StartFrame() {
    if (!mDeviceReady) {
        return;
    }
    EnsureFrame();
    mWindowBound = false;
}

void GfxRenderingAPIDeko3d::EnsureFrame() {
    mDevice.BeginFrame();
    if (mResourcesFrame != mDevice.Frame()) {
        // Right after the slot's fence wait: evictions are safe here.
        mResourcesFrame = mDevice.Frame();
        mResources.BeginFrame(mResourcesFrame);
        if (mFrameBarrier) {
            mResources.RequestInvalidate(); // config/deko3d_barrier "frame" (A/B)
        }
        mTargets.BeginFrame(mResourcesFrame);
        CollectUiRetired();
        ApplyLightingSettings();
        // A new command list: nothing is known to be bound.
        mBoundTarget = nullptr;
        // config/grass = draw: configured once after Init, before the game.
        mGrassDraws = Deko3d::GrassDrawEnabled();
    }
}

void GfxRenderingAPIDeko3d::BindWindowTarget() {
    if (!mDeviceReady) {
        return;
    }
    EnsureFrame();
    // Acquired at its first use (blocking; the work recorded so far goes to
    // the GPU first), bound whenever a PICA target took its place.
    const DkImageView& view = mDevice.AcquireWindow(GetWindowAcquireHook());
    mWindowBound = true;
    DkCmdBuf cmd = mDevice.Cmd();
    if (mBoundTarget != &mDevice) {
        const DkImageView* colors[] = {&view};
        dkCmdBufBindRenderTargets(cmd, colors, 1, nullptr);
        ApplyDkViewport(cmd, 0.0F, 0.0F, static_cast<float>(mDevice.Width()), static_cast<float>(mDevice.Height()));
        const DkScissor scissor = {0, 0, mDevice.Width(), mDevice.Height()};
        dkCmdBufSetScissors(cmd, 0, &scissor, 1);
        mBoundTarget = &mDevice;
    }
    if (mWindowClearPending) {
        mWindowClearPending = false;
        const DkScissor scissor = {0, 0, mDevice.Width(), mDevice.Height()};
        dkCmdBufSetScissors(cmd, 0, &scissor, 1);
        if (mWindowClearColor) {
            dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, mWindowClearRgba[0], mWindowClearRgba[1],
                                    mWindowClearRgba[2], mWindowClearRgba[3]);
            ++mCounters.WindowClears;
        }
    }
}

void GfxRenderingAPIDeko3d::EndFrame() {
}

void GfxRenderingAPIDeko3d::FinishRender() {
    if (!mDeviceReady) {
        return;
    }
    const Deko3d::SwapClock swapClock;
    Deko3d::GrassEndFrame(mDevice.Frame()); // config/grass: this frame's placements
    // After the window backend's pacing (SwapBuffersBegin), like the GL swap.
    if (!mDevice.FrameOpen()) {
        mDevice.BeginFrame();
    }
    if (!mWindowBound && !mWindowClearPending) {
        // Nothing drew to the window this frame: present it cleared.
        mWindowClearPending = true;
        mWindowClearColor = true;
        mWindowClearDepth = false;
        mWindowClearRgba = {mClearColor[0], mClearColor[1], mClearColor[2], mClearColor[3]};
    }
    if (!mDevice.WindowAcquired()) {
        mDevice.CountSwapAcquire();
    }
    BindWindowTarget();
    mDevice.EndFrame(GetWindowAcquireHook());
    mWindowBound = false;
    mBoundTarget = nullptr;
    mFrameDraws.EndFrame();
    if (Deko3dPicaLogDue(mDevice.Frame())) {
        LogPicaProgramCounters("frame");
    }
}

int GfxRenderingAPIDeko3d::CreateFramebuffer() {
    mFramebuffers.push_back({});
    return static_cast<int>(mFramebuffers.size() - 1U);
}

void GfxRenderingAPIDeko3d::UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t,
                                                        bool opengl_invertY, bool, bool has_depth_buffer, bool) {
    if (fb_id < 0) {
        return;
    }
    if (static_cast<size_t>(fb_id) >= mFramebuffers.size()) {
        mFramebuffers.resize(static_cast<size_t>(fb_id) + 1U);
    }
    auto& fb = mFramebuffers[static_cast<size_t>(fb_id)];
    fb.Width = width;
    fb.Height = height;
    fb.InvertY = opengl_invertY;
    fb.HasDepth = has_depth_buffer;
}

void GfxRenderingAPIDeko3d::StartDrawToFramebuffer(int fbId, float) {
    mCurrentFramebuffer = fbId;
}

void GfxRenderingAPIDeko3d::ClearFramebuffer(bool color, bool depth) {
    if (mCurrentFramebuffer != 0) {
        return;
    }
    // The window image is acquired (blocking) at its first real use, as the
    // GL backend's deferred window clear does.
    mWindowClearColor = mWindowClearPending ? (mWindowClearColor || color) : color;
    mWindowClearDepth = mWindowClearPending ? (mWindowClearDepth || depth) : depth;
    if (color) {
        mWindowClearRgba = {mClearColor[0], mClearColor[1], mClearColor[2], mClearColor[3]};
    }
    mWindowClearPending = true;
}

void GfxRenderingAPIDeko3d::ClearDepthRegion(int, int, int, int) {
}

// ---- shaders and textures of the legacy/UI path (package 6) ----

void GfxRenderingAPIDeko3d::UnloadShader(ShaderProgram*) {
}

void GfxRenderingAPIDeko3d::LoadShader(ShaderProgram*) {
}

void GfxRenderingAPIDeko3d::ClearShaderCache() {
}

ShaderProgram* GfxRenderingAPIDeko3d::CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) {
    auto program = std::make_unique<Deko3dShaderProgram>();
    program->ShaderId0 = shaderId0;
    program->ShaderId1 = shaderId1;
    auto* raw = program.get();
    mShaderPrograms.push_back(std::move(program));
    return reinterpret_cast<ShaderProgram*>(raw);
}

ShaderProgram* GfxRenderingAPIDeko3d::LookupShader(uint64_t shaderId0, uint64_t shaderId1) {
    for (const auto& program : mShaderPrograms) {
        if (program->ShaderId0 == shaderId0 && program->ShaderId1 == shaderId1) {
            return reinterpret_cast<ShaderProgram*>(program.get());
        }
    }
    return nullptr;
}

void GfxRenderingAPIDeko3d::ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) {
    const auto* program = reinterpret_cast<const Deko3dShaderProgram*>(prg);
    *numInputs = program != nullptr ? program->NumInputs : 0;
    usedTextures[0] = program != nullptr && program->UsedTextures[0];
    usedTextures[1] = program != nullptr && program->UsedTextures[1];
}

void GfxRenderingAPIDeko3d::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIDeko3d::SetZmodeDecal(bool decal) {
    mCurrentZmodeDecal = decal;
}

bool GfxRenderingAPIDeko3d::SetNativeBlendState(const GfxNativeBlendState&) {
    return true;
}

bool GfxRenderingAPIDeko3d::SetNativeCullMode(GfxNativeCullMode) {
    return true;
}

void GfxRenderingAPIDeko3d::CopyFramebuffer(int, int, int, int, int, int, int, int, int, int) {
}

void GfxRenderingAPIDeko3d::ReadFramebufferToCPU(int, uint32_t width, uint32_t height, uint16_t* rgba16Buf) {
    if (rgba16Buf != nullptr) {
        std::memset(rgba16Buf, 0, static_cast<size_t>(width) * height * sizeof(uint16_t));
    }
}

void GfxRenderingAPIDeko3d::ResolveMSAAColorBuffer(int, int) {
}

std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
GfxRenderingAPIDeko3d::GetPixelDepth(int, const std::set<std::pair<float, float>>& coordinates) {
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff> result;
    for (const auto& coordinate : coordinates) {
        result[coordinate] = 0;
    }
    return result;
}

void* GfxRenderingAPIDeko3d::GetFramebufferTextureId(int) {
    return nullptr;
}

void GfxRenderingAPIDeko3d::SelectTextureFb(int) {
}

void GfxRenderingAPIDeko3d::SetTextureFilter(FilteringMode mode) {
    mFilterMode = mode;
}

FilteringMode GfxRenderingAPIDeko3d::GetTextureFilter() {
    return mFilterMode;
}

void GfxRenderingAPIDeko3d::SetSrgbMode() {
    mSrgbMode = true;
}

ImTextureID GfxRenderingAPIDeko3d::GetTextureById(int) {
    return nullptr;
}

void GfxRenderingAPIDeko3d::SetCurrentPrimDepth(float depth) {
    mCurrentPrimDepth = depth;
}

// ---- native PICA path (packages 3-4; the draw itself: gfx_deko3d_pica_draw.cpp) ----

GfxNativePicaBackendStats GfxRenderingAPIDeko3d::GetNativePicaBackendStats() const noexcept {
    GfxNativePicaBackendStats stats;
    stats.Available = mDeviceReady;
    stats.GeometryCacheEnabled = mGeometryCacheEnabled;
    stats.SkippedDraws = mCounters.PicaDrawsSkipped;
    return stats;
}

void GfxRenderingAPIDeko3d::SetNativePicaGeometryCacheEnabled(bool enabled) noexcept {
    mGeometryCacheEnabled = enabled;
}

void GfxRenderingAPIDeko3d::PrepareNativePicaPrograms(std::span<const Renderer3ds::PicaVertexArtifact> vertices) {
    // Nothing to build ahead: every stage is a precompiled DKSH loaded at its
    // first draw (a copy into code memory, no compilation).
    Deko3d::Log("programs: hybrid preparation, %zu vertex artifacts, %llu stages in the pack", vertices.size(),
                static_cast<unsigned long long>(mShaders.Stats().PackEntries));
}

const DkShader* GfxRenderingAPIDeko3d::ResolvePicaStage(Deko3d::ShaderStage stage, std::string_view canonical,
                                                        Renderer3ds::PicaShaderSourceIdentity identity) {
    if (canonical.empty()) {
        return nullptr;
    }
    if (!identity.Available()) {
        identity = Renderer3ds::IdentifyPicaShaderSource(canonical);
    }
    auto& cache = stage == Deko3d::ShaderStage::Vertex ? mVertexStages : mFragmentStages;
    return cache.Get(identity, [&]() -> const DkShader* {
        // Same text as the GL backend's BuildPicaGlProgram, so the pack made
        // from its shadercache/*.src matches.
        std::string translated;
        std::string error;
        if (!Renderer3ds::TranslatePicaShaderToOpenGl43(canonical, translated, &error)) {
            if (mTranslationFailures++ < 10U) {
                Deko3d::Log("programs: %s translation failed: %s",
                            stage == Deko3d::ShaderStage::Vertex ? "vertex" : "fragment", error.c_str());
            }
            return nullptr;
        }
        return mShaders.Find(stage, translated);
    });
}

GfxRenderingAPIDeko3d::PicaProgram GfxRenderingAPIDeko3d::ResolvePicaProgram(const GfxNativePicaDrawView& draw) {
    PicaProgram program;
    program.Vertex = ResolvePicaStage(Deko3d::ShaderStage::Vertex, draw.VertexShaderSource,
                                      draw.VertexShaderSourceIdentity);
    const DkShader* specialized = nullptr;
    const DkShader* uber = nullptr;
    if (program.Vertex != nullptr) {
        specialized = ResolvePicaStage(Deko3d::ShaderStage::Fragment, draw.FragmentShaderSource,
                                       draw.FragmentShaderSourceIdentity);
        if (specialized == nullptr) {
            uber = ResolvePicaStage(Deko3d::ShaderStage::Fragment, draw.UberFragmentShaderSource,
                                    draw.UberFragmentShaderSourceIdentity);
        }
    }
    program.Pick = Deko3d::PickPicaProgram(program.Vertex != nullptr, specialized != nullptr, uber != nullptr);
    program.Fragment = specialized != nullptr ? specialized : uber;
    if (program.Fragment != nullptr) {
        program.Base = Deko3d::ClassifyPicaBase(
            specialized == nullptr, Deko3d::ShaderStore::ResourcesOf(*program.Fragment).ScratchBytes);
    }
    mCounters.Programs.Count(program.Pick);
    if (mLightingMode != Deko3d::LightingMode::Off && program.Vertex != nullptr && program.Fragment != nullptr) {
        if (program.Base == Deko3d::PicaBaseClass::Spilling) {
            // Its variants spill too: native shading.
            ++mLightingCounters.DrawsSpillingBase;
            ++mLightingCounters.DrawsIneligible;
        } else if (specialized != nullptr) {
            // The variant of the text the native draw uses (specialized or uber).
            ApplyPicaLighting(draw, draw.FragmentShaderSource, draw.FragmentShaderSourceIdentity, program);
        } else {
            ApplyPicaLighting(draw, draw.UberFragmentShaderSource, draw.UberFragmentShaderSourceIdentity, program);
        }
    }
    return program;
}

// ---- config/lighting: toon and trimmed per-pixel variants (gfx_deko3d_lighting.h) ----

void GfxRenderingAPIDeko3d::ApplyLightingSettings() {
    // The model and, under Modern, the scene environment profile (the
    // Scene lights tab's live values; resources.o2r environments/ and the
    // user's files), as rendering sees them (preset, native override).
    auto rendering = Oot3d::GraphicsSettingsRuntime::Instance().SnapshotForRendering();
    const Deko3d::LightingMode mode = Deko3d::LightingModeOf(rendering.Value);
    if (mode != mLightingMode || mLightingRevision == 0U) {
        Deko3d::Log("lighting: mode %s (config/lighting, graphics settings revision %llu)",
                    Deko3d::LightingModeName(mode), static_cast<unsigned long long>(rendering.Revision));
    }
    mLightingRevision = rendering.Revision;
    mLightingMode = mode;
    if (mode == Deko3d::LightingMode::PerPixel) {
        mLightingEnvironment = std::move(rendering.Environment);
    }
}

const DkShader* GfxRenderingAPIDeko3d::ResolveLightingFragment(Deko3d::LightingMode mode, std::string_view canonical,
                                                               Renderer3ds::PicaShaderSourceIdentity identity) {
    if (canonical.empty()) {
        return nullptr;
    }
    if (!identity.Available()) {
        identity = Renderer3ds::IdentifyPicaShaderSource(canonical);
    }
    auto& cache = mode == Deko3d::LightingMode::Toon ? mToonFragments : mPerPixelFragments;
    return cache.Get(identity, [&]() -> const DkShader* {
        std::string translated;
        std::string error;
        std::optional<std::string> variant;
        if (Renderer3ds::TranslatePicaShaderToOpenGl43(canonical, translated, &error)) {
            variant = Deko3d::LightingFragmentVariant(mode, translated);
        }
        return Deko3d::ResolveLightingVariant<DkShader>(
            Deko3d::ShaderStage::Fragment, variant,
            [&](uint64_t key, std::string_view text) {
                return mShaders.FindKey(Deko3d::ShaderStage::Fragment, key, text);
            },
            mLightingCounters);
    });
}

const DkShader* GfxRenderingAPIDeko3d::ResolvePerPixelVertex(const GfxNativePicaDrawView& draw) {
    auto identity = draw.VertexShaderSourceIdentity;
    if (!identity.Available()) {
        identity = Renderer3ds::IdentifyPicaShaderSource(draw.VertexShaderSource);
    }
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    for (const auto& known : mPerPixelVertices) {
        if (known.Identity == identity && known.Hooks == hooks) {
            return known.Shader;
        }
    }
    std::optional<std::string> translated;
    if (const auto variant = Deko3d::PerPixelVertexVariant(draw.VertexShaderSource, hooks)) {
        std::string text;
        std::string error;
        if (Renderer3ds::TranslatePicaShaderToOpenGl43(*variant, text, &error)) {
            translated = std::move(text);
        }
    }
    const DkShader* shader = Deko3d::ResolveLightingVariant<DkShader>(
        Deko3d::ShaderStage::Vertex, translated,
        [&](uint64_t key, std::string_view text) { return mShaders.FindKey(Deko3d::ShaderStage::Vertex, key, text); },
        mLightingCounters);
    // One title vertex program in a handful of skinning layouts: a short list.
    if (mPerPixelVertices.size() >= 64U) {
        mPerPixelVertices.clear();
    }
    mPerPixelVertices.push_back({identity, hooks, shader});
    return shader;
}

// Upstream's per-pixel admission (gfx_vulkan_pica_draw.cpp, surfaceCandidate
// and the authored-appearance test), then its block, trimmed.
bool GfxRenderingAPIDeko3d::BuildSurfaceLighting(const GfxNativePicaDrawView& draw) {
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    const auto* surface = hooks.SurfaceProgram();
    if (draw.CompositionDomain != Renderer3ds::PicaCompositionDomain::Scene ||
        !Renderer3ds::PicaCompositionReceivesSurfaceLighting(draw.Composition.Layer) ||
        draw.FragmentOperationMode != 0U || surface == nullptr || hooks.TransformProgram() == nullptr ||
        draw.VertexUniformBytes.size() < Oot3d::kPicaPackedVertexFloatOffset ||
        !Oot3d::UsesPicaPerspectiveProjection(draw.VertexUniformBytes)) {
        return false;
    }
    uint32_t booleans = 0U;
    std::memcpy(&booleans, draw.VertexUniformBytes.data() + Oot3d::kPicaPackedVertexBooleanMaskOffset,
                sizeof(booleans));
    const bool vertexLit = ((booleans >> surface->LightingBooleanUniform) & 1U) != 0U;
    const auto appearance =
        Renderer3ds::ClassifyPicaSurfaceAppearance(draw.Blend, vertexLit, draw.FragmentFeatures.FragmentLightingEnabled);
    if (appearance != Renderer3ds::PicaSurfaceAppearance::Relightable) {
        return false;
    }
    const auto* worldToView = draw.LightBinding.WorldToView ? &*draw.LightBinding.WorldToView : nullptr;
    if (worldToView != nullptr && (!mLightingView || mLightingView->WorldToView != *worldToView)) {
        mLightingView = Renderer3ds::BuildPicaSurfaceLightingView(*worldToView);
    }
    // A draw the title lights on its own keeps its native environment.
    auto isolated = mLightingEnvironment;
    isolated.Enabled = false;
    const auto uniforms = Renderer3ds::BuildPicaSurfaceLightingUniforms(
        hooks, booleans, draw.VertexUniformBytes.subspan(Oot3d::kPicaPackedVertexFloatOffset), draw.LightBinding,
        draw.Composition.IsolatedLighting ? isolated : mLightingEnvironment,
        worldToView != nullptr && mLightingView ? &*mLightingView : nullptr);
    if (!uniforms) {
        return false;
    }
    mSurfaceLighting = *uniforms;
    if (draw.Composition.IsolatedLighting || draw.FragmentFeatures.FragmentLightingEnabled) {
        mSurfaceLighting.BakedReferenceUp = {};
    }
    Deko3d::TrimSurfaceLighting(mSurfaceLighting);
    // Scene lights > Rim light, on the draws upstream gives it to.
    if (Oot3d::PicaDrawReceivesRimLight(mLightingEnvironment, draw.CompositionDomain, draw.Composition, draw.Blend,
                                        appearance, draw.DepthTestEnabled, draw.DepthWriteEnabled,
                                        draw.FragmentOperationMode)) {
        Renderer3ds::AssignPicaSurfaceRim(mSurfaceLighting, mLightingEnvironment.Rim);
    }
    return true;
}

void GfxRenderingAPIDeko3d::ApplyPicaLighting(const GfxNativePicaDrawView& draw, std::string_view fragmentSource,
                                              Renderer3ds::PicaShaderSourceIdentity fragmentIdentity,
                                              PicaProgram& program) {
    if (mLightingMode == Deko3d::LightingMode::Toon) {
        Oot3d::PicaToonDrawInfo info;
        info.CompositionDomain = draw.CompositionDomain;
        info.DepthTestEnabled = draw.DepthTestEnabled;
        info.DepthWriteEnabled = draw.DepthWriteEnabled;
        info.BlendEnabled = draw.Blend.Enabled;
        info.ColorWriteMask = draw.ColorWriteMask;
        if (Oot3d::ClassifyPicaToonDraw(info, Oot3d::ToonMode::PicaMaterial) != Oot3d::PicaToonEligibility::Eligible) {
            ++mLightingCounters.DrawsIneligible;
            return;
        }
        const DkShader* fragment = ResolveLightingFragment(Deko3d::LightingMode::Toon, fragmentSource, fragmentIdentity);
        const auto choice = Deko3d::ChooseLightingProgram<DkShader>(program.Vertex, program.Fragment, program.Vertex,
                                                                    fragment, mLightingCounters);
        program.Vertex = choice.Vertex;
        program.Fragment = choice.Fragment;
        program.Lit = choice.Lit;
        return;
    }
    // Per-pixel: both variants, then the block (only for a draw that uses them).
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    if (draw.CompositionDomain != Renderer3ds::PicaCompositionDomain::Scene ||
        !Renderer3ds::PicaCompositionReceivesSurfaceLighting(draw.Composition.Layer) ||
        hooks.SurfaceProgram() == nullptr || hooks.TransformProgram() == nullptr) {
        ++mLightingCounters.DrawsIneligible;
        return;
    }
    if (!Deko3d::PerPixelCostAdmits(draw.DepthWriteEnabled)) {
        ++mLightingCounters.DrawsNoDepthWrite;
        ++mLightingCounters.DrawsIneligible;
        return;
    }
    const DkShader* vertex = ResolvePerPixelVertex(draw);
    const DkShader* fragment =
        vertex != nullptr ? ResolveLightingFragment(Deko3d::LightingMode::PerPixel, fragmentSource, fragmentIdentity)
                          : nullptr;
    if (vertex != nullptr && fragment != nullptr && !BuildSurfaceLighting(draw)) {
        ++mLightingCounters.DrawsIneligible;
        return;
    }
    const auto choice =
        Deko3d::ChooseLightingProgram<DkShader>(program.Vertex, program.Fragment, vertex, fragment, mLightingCounters);
    program.Vertex = choice.Vertex;
    program.Fragment = choice.Fragment;
    program.Lit = choice.Lit;
    program.SurfaceLighting = choice.Lit;
}

void GfxRenderingAPIDeko3d::LogLightingCounters(const char* when) {
    const auto& c = mLightingCounters;
    if (mLightingMode == Deko3d::LightingMode::Off && c.VariantsFound == 0U && c.VariantsMissing == 0U &&
        c.VariantsUnsupported == 0U) {
        return;
    }
    // The GPU cost: dkperf busy_ms in logs/perf.txt (same windows), against
    // a run with the mode off. Per frame: the lit draws of the last frame and
    // the window's average (points: per-pixel draws by scene point lights).
    const auto& frames = mFrameDraws;
    Deko3d::Log("lighting (%s): mode=%s variants found=%llu missing=%llu unsupported=%llu; draws lit=%llu "
                "native=%llu ineligible=%llu (spilling base=%llu no depth write=%llu); per frame lit %s | avg of %llu %s",
                when, Deko3d::LightingModeName(mLightingMode), static_cast<unsigned long long>(c.VariantsFound),
                static_cast<unsigned long long>(c.VariantsMissing),
                static_cast<unsigned long long>(c.VariantsUnsupported), static_cast<unsigned long long>(c.DrawsLit),
                static_cast<unsigned long long>(c.DrawsNative), static_cast<unsigned long long>(c.DrawsIneligible),
                static_cast<unsigned long long>(c.DrawsSpillingBase), static_cast<unsigned long long>(c.DrawsNoDepthWrite),
                Deko3d::FormatPicaLitDraws(frames.Last, 1U).c_str(), static_cast<unsigned long long>(frames.Frames),
                Deko3d::FormatPicaLitDraws(frames.Sum, frames.Frames).c_str());
}

void GfxRenderingAPIDeko3d::LogPicaProgramCounters(const char* when) {
    const auto& programs = mCounters.Programs;
    const auto& shaders = mShaders.Stats();
    // Per frame, by base fragment program class: draws/lit draws and
    // viewport-in-scissor pixels/lit pixels (millions), the last frame and
    // the average since the previous line.
    const auto& frames = mFrameDraws;
    Deko3d::Log("programs (%s): draws specialized=%llu uber=%llu no_vertex=%llu no_fragment=%llu; "
                "stages vertex=%zu fragment=%zu loaded=%llu missing=%llu code=%llu KiB; per frame %s | avg of %llu %s",
                when, static_cast<unsigned long long>(programs.Specialized),
                static_cast<unsigned long long>(programs.Uber), static_cast<unsigned long long>(programs.NoVertex),
                static_cast<unsigned long long>(programs.NoFragment), mVertexStages.Size(), mFragmentStages.Size(),
                static_cast<unsigned long long>(shaders.Loaded), static_cast<unsigned long long>(shaders.Missing),
                static_cast<unsigned long long>(shaders.CodeBytes >> 10),
                Deko3d::FormatPicaClassDraws(frames.Last, 1U).c_str(), static_cast<unsigned long long>(frames.Frames),
                Deko3d::FormatPicaClassDraws(frames.Sum, frames.Frames).c_str());
    // How the game resets depth (it never clears it) and the draws ZCULL
    // could mis-test (gl_FragDepth): the ZCULL study, SWITCH_PORT.md.
    Deko3d::Log("depth (%s): zcull=%d per frame %s | avg of %llu %s", when, mDevice.Zcull() ? 1 : 0,
                Deko3d::FormatPicaDepthDraws(frames.Last, 1U).c_str(), static_cast<unsigned long long>(frames.Frames),
                Deko3d::FormatPicaDepthDraws(frames.Sum, frames.Frames).c_str());
    LogLightingCounters(when);
    mFrameDraws.StartWindow();
    const auto& resources = mResources.Stats();
    Deko3d::Log("resources (%s): draws prepared=%llu rejected=%llu; textures created=%llu evicted=%llu "
                "(budget %llu) live=%llu MiB predecoded=%llu failures=%llu shadow_fallbacks=%llu "
                "unsupported_wraps=%llu luts=%llu samplers=%llu; pack images=%llu (again %llu, at frame top %llu) "
                "uploaded=%llu MiB "
                "live=%llu MiB; "
                "streamed vertex=%llu index=%llu uniform=%llu KiB, uniform reuses=%llu, "
                "stream failures=%llu",
                when, static_cast<unsigned long long>(mCounters.PicaDrawsPrepared),
                static_cast<unsigned long long>(mCounters.PicaDrawsRejected),
                static_cast<unsigned long long>(resources.TexturesCreated),
                static_cast<unsigned long long>(resources.TexturesEvicted),
                static_cast<unsigned long long>(resources.TexturesEvictedForBudget),
                static_cast<unsigned long long>(resources.TextureBytes >> 20),
                static_cast<unsigned long long>(resources.TexturesPredecoded),
                static_cast<unsigned long long>(resources.TextureFailures),
                static_cast<unsigned long long>(resources.ShadowFallbacks),
                static_cast<unsigned long long>(resources.UnsupportedWraps),
                static_cast<unsigned long long>(resources.LutsCreated),
                static_cast<unsigned long long>(resources.Samplers),
                static_cast<unsigned long long>(resources.TextureReplacements),
                static_cast<unsigned long long>(resources.TextureReplacementReuploads),
                static_cast<unsigned long long>(resources.TextureReplacementsBatched),
                static_cast<unsigned long long>(resources.TextureReplacementBytes >> 20),
                static_cast<unsigned long long>(resources.TextureReplacementLiveBytes >> 20),
                static_cast<unsigned long long>(resources.VertexBytes >> 10),
                static_cast<unsigned long long>(resources.IndexBytes >> 10),
                static_cast<unsigned long long>(resources.UniformBytes >> 10),
                static_cast<unsigned long long>(resources.UniformReuses),
                static_cast<unsigned long long>(resources.StreamFailures));
    // The HUD's pack images (TopScreen HUD, drawn by the UI renderer): those
    // decoded at start-up from config/texture_pack_hud and the worker time
    // they took, and those decoded on first sight (drawn late: the longest
    // wait). Only with a texture pack whose HUD images were seen.
    if (const auto hud = ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().HudStatus();
        hud.Listed != 0U || hud.OnSightDecodes != 0U) {
        Deko3d::Log("hud pack (%s): listed=%zu preloaded=%zu/%zu in %llu ms (%llu KiB); first sight=%zu "
                    "(wait max %llu ms)",
                    when, hud.Listed, hud.PreloadDecoded, hud.PreloadQueued,
                    static_cast<unsigned long long>(hud.PreloadMicros / 1000U),
                    static_cast<unsigned long long>(hud.PreloadBytes >> 10), hud.OnSightDecodes,
                    static_cast<unsigned long long>(hud.OnSightWaitMaxMicros / 1000U));
    }
    const auto& targets = mTargets.Stats();
    Deko3d::Log("pica calls (%s): draws=%llu transfers=%llu presents=%llu fills=%llu clears=%llu resets=%llu "
                "completions queued=%llu taken=%llu",
                when, static_cast<unsigned long long>(mCalls.Draws), static_cast<unsigned long long>(mCalls.Transfers),
                static_cast<unsigned long long>(mCalls.Presents), static_cast<unsigned long long>(mCalls.Fills),
                static_cast<unsigned long long>(mCalls.Clears), static_cast<unsigned long long>(mCalls.Resets),
                static_cast<unsigned long long>(mCalls.CompletionsQueued),
                static_cast<unsigned long long>(mCalls.CompletionsTaken));
    Deko3d::Log("pica (%s): draws issued=%llu skipped=%llu shadow_producers=%llu no_effect=%llu; transfers=%llu "
                "repeated=%llu rejected=%llu scanouts=%llu fills=%llu clears=%llu; targets=%llu (%llu MiB) "
                "displays=%llu failures=%llu",
                when, static_cast<unsigned long long>(mCounters.PicaDrawsIssued),
                static_cast<unsigned long long>(mCounters.PicaDrawsSkipped),
                static_cast<unsigned long long>(mCounters.PicaShadowProducersSkipped),
                static_cast<unsigned long long>(mCounters.PicaNoEffectSkipped),
                static_cast<unsigned long long>(mCounters.PicaTransfers),
                static_cast<unsigned long long>(mCounters.PicaTransfersRepeated),
                static_cast<unsigned long long>(mCounters.PicaTransfersRejected),
                static_cast<unsigned long long>(mCounters.PicaScanouts),
                static_cast<unsigned long long>(mCounters.PicaFills),
                static_cast<unsigned long long>(mCounters.PicaClears),
                static_cast<unsigned long long>(targets.TargetsCreated),
                static_cast<unsigned long long>(targets.TargetBytes >> 20),
                static_cast<unsigned long long>(targets.DisplaysCreated),
                static_cast<unsigned long long>(targets.Failures));
    LogUiCounters(when);
}

bool GfxRenderingAPIDeko3d::QueuePicaCompletion(uint64_t completionId, std::string* error) {
    if (completionId == 0U) {
        if (error != nullptr) {
            *error = "PICA completion identity is invalid";
        }
        return false;
    }
    // Every guest span a submission reads is copied at submit time (as in
    // the GL backend): the completion acknowledges that copy.
    mCompletions.push_back(completionId);
    if (mCalls.CompletionsQueued++ == 0U) {
        Deko3d::Log("pica: first completion queued (%llu)", static_cast<unsigned long long>(completionId));
    }
    return true;
}

std::vector<uint64_t> GfxRenderingAPIDeko3d::TakePicaCompletions() {
    auto result = std::move(mCompletions);
    mCompletions.clear();
    if (!result.empty() && mCalls.CompletionsTaken == 0U) {
        Deko3d::Log("pica: first completions taken (%zu)", result.size());
    }
    mCalls.CompletionsTaken += result.size();
    return result;
}

} // namespace Fast

#else // !__SWITCH__

namespace Fast {
GfxRenderingAPI* CreateDeko3dRenderingApi() {
    return nullptr;
}
void Deko3dRenderImGuiDrawData(const ImDrawData*) {
}
std::string Deko3dPerfReport(unsigned long long) {
    return {};
}
void Deko3dTagFrame(uint8_t, double) {
}
bool Deko3dTakeGpuFrame(Deko3dGpuFrame&) {
    return false;
}
Deko3dSwapTimes Deko3dLastSwapTimes() {
    return {};
}
} // namespace Fast

#endif
