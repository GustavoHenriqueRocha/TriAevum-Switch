// GfxRenderingAPI on deko3d (Switch). See gfx_deko3d_api.h and
// docs/DEKO3D_PROGRESSO.md.
#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)
#include "gfx_deko3d_api.h"

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
    mDeviceReady = true;
    if (mFramebuffers.empty()) {
        mFramebuffers.push_back({width, height, false, true});
    }
}

void GfxRenderingAPIDeko3d::Shutdown() {
    if (mDeviceReady) {
        mDevice.WaitIdle();
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
    mDevice.BeginFrame();
    mWindowBound = false;
}

void GfxRenderingAPIDeko3d::BindWindowTarget() {
    if (!mDeviceReady) {
        return;
    }
    if (!mWindowBound) {
        const DkImageView& view = mDevice.AcquireWindow(GetWindowAcquireHook());
        const DkImageView* colors[] = {&view};
        DkCmdBuf cmd = mDevice.Cmd();
        dkCmdBufBindRenderTargets(cmd, colors, 1, nullptr);
        const DkViewport viewport = {0.0F, 0.0F, static_cast<float>(mDevice.Width()),
                                     static_cast<float>(mDevice.Height()), 0.0F, 1.0F};
        dkCmdBufSetViewports(cmd, 0, &viewport, 1);
        const DkScissor scissor = {0, 0, mDevice.Width(), mDevice.Height()};
        dkCmdBufSetScissors(cmd, 0, &scissor, 1);
        mWindowBound = true;
    }
    if (mWindowClearPending) {
        mWindowClearPending = false;
        DkCmdBuf cmd = mDevice.Cmd();
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
    BindWindowTarget();
    mDevice.EndFrame(GetWindowAcquireHook());
    mWindowBound = false;
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

uint32_t GfxRenderingAPIDeko3d::NewTexture() {
    return mNextTextureId++;
}

void GfxRenderingAPIDeko3d::SelectTexture(int, uint32_t) {
}

void GfxRenderingAPIDeko3d::UploadTexture(const uint8_t*, uint32_t, uint32_t) {
}

void GfxRenderingAPIDeko3d::SetSamplerParameters(int, bool, uint32_t, uint32_t) {
}

void GfxRenderingAPIDeko3d::SetDepthTestAndMask(bool depth_test, bool z_upd) {
    mCurrentDepthTest = depth_test;
    mCurrentDepthMask = z_upd;
}

void GfxRenderingAPIDeko3d::SetZmodeDecal(bool decal) {
    mCurrentZmodeDecal = decal;
}

void GfxRenderingAPIDeko3d::SetViewport(int, int, int, int) {
}

void GfxRenderingAPIDeko3d::SetScissor(int, int, int, int) {
}

void GfxRenderingAPIDeko3d::SetUseAlpha(bool) {
}

bool GfxRenderingAPIDeko3d::SetNativeBlendState(const GfxNativeBlendState&) {
    return true;
}

bool GfxRenderingAPIDeko3d::SetNativeCullMode(GfxNativeCullMode) {
    return true;
}

bool GfxRenderingAPIDeko3d::SetOot3dNativeTransform(const float*) {
    return true;
}

void GfxRenderingAPIDeko3d::DrawTriangles(float[], size_t, size_t) {
    if (mCurrentFramebuffer == 0) {
        BindWindowTarget();
    }
    ++mCounters.UiDrawsSkipped;
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

void GfxRenderingAPIDeko3d::DeleteTexture(uint32_t) {
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

// ---- native PICA path (packages 3-5): accepted, not drawn yet ----

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

bool GfxRenderingAPIDeko3d::SubmitPicaDraw(const GfxNativePicaDrawView&, std::string*) {
    ++mCounters.PicaDrawsSkipped;
    return true;
}

bool GfxRenderingAPIDeko3d::SubmitPicaDisplayTransfer(const GfxNativePicaDisplayTransferView& transfer,
                                                      std::string*) {
    ++mCounters.PicaTransfersSkipped;
    if (transfer.Present) {
        // The scanout draws into the window (package 5): acquire it here,
        // in the frame's order.
        BindWindowTarget();
    }
    return true;
}

bool GfxRenderingAPIDeko3d::ClearPicaRenderTarget(uint64_t, uint32_t, std::string*) {
    ++mCounters.PicaClearsSkipped;
    return true;
}

bool GfxRenderingAPIDeko3d::SubmitPicaMemoryFill(const GfxNativePicaMemoryFillView&, std::string*) {
    ++mCounters.PicaFillsSkipped;
    return true;
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
    return true;
}

std::vector<uint64_t> GfxRenderingAPIDeko3d::TakePicaCompletions() {
    auto result = std::move(mCompletions);
    mCompletions.clear();
    return result;
}

bool GfxRenderingAPIDeko3d::ResetPicaState(std::string*) {
    return true;
}

} // namespace Fast

#else // !__SWITCH__

namespace Fast {
GfxRenderingAPI* CreateDeko3dRenderingApi() {
    return nullptr;
}
} // namespace Fast

#endif
