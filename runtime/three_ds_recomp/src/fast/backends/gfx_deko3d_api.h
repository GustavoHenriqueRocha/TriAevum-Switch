#pragma once
// GfxRenderingAPI on deko3d (Switch). Package 2 (docs/DEKO3D_PROGRESS.md):
// device, window image, clears and the present. Package 3: every PICA draw
// resolves its shaders from deko3d.pack (specialized, else parametric).
// Package 4: its textures, LUT, vertex/index streams and uniforms are
// uploaded (gfx_deko3d_pica_resources.h). Package 5: render targets, fills,
// pipeline state, the draw itself, display transfers and the scanout
// (gfx_deko3d_pica_targets.h, gfx_deko3d_pica_draw.cpp).
// Package 6: the TopScreen UI (its two Fast3D programs, RGBA8 textures) and
// ImGui (the FPS counter) drawn on the window (gfx_deko3d_ui.cpp).

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/renderer3ds/pica_program_preparation.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_pica_resources.h"
#include "gfx_deko3d_pica_targets.h"
#include "gfx_deko3d_shaders.h"
#include "fast/backends/gfx_deko3d_ui_plan.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace Fast {

struct Deko3dShaderProgram {
    uint64_t ShaderId0 = 0;
    uint64_t ShaderId1 = 0;
    uint8_t NumInputs = 0;
    bool UsedTextures[2] = {false, false};
};

struct Deko3dFramebuffer {
    uint32_t Width = 0;
    uint32_t Height = 0;
    bool InvertY = false;
    bool HasDepth = false;
};

// Every PICA call that reached the backend (logs/deko3d.txt "pica calls"):
// tells "nothing was routed here" apart from "routed but not drawn".
struct Deko3dPicaCalls {
    uint64_t Draws = 0;
    uint64_t Transfers = 0;
    uint64_t Presents = 0;
    uint64_t Fills = 0;
    uint64_t Clears = 0;
    uint64_t Resets = 0;
    uint64_t CompletionsQueued = 0;
    uint64_t CompletionsTaken = 0;
};

struct Deko3dBackendCounters {
    uint64_t PicaDrawsSkipped = 0;  // no program, no target or no effect
    uint64_t PicaDrawsPrepared = 0; // program + resources ready
    uint64_t PicaDrawsIssued = 0;   // recorded into the command buffer
    uint64_t PicaDrawsRejected = 0; // what the GL backend rejects too
    uint64_t PicaShadowProducersSkipped = 0; // FragmentOperationMode 3 (no image store yet)
    uint64_t PicaNoEffectSkipped = 0;
    uint64_t PicaTransfers = 0;
    uint64_t PicaTransfersRepeated = 0; // identical copy of an unchanged source (GL lean)
    uint64_t PicaTransfersRejected = 0;
    uint64_t PicaScanouts = 0;
    uint64_t PicaFills = 0;
    uint64_t PicaClears = 0;
    uint64_t UiDraws = 0;           // TopScreen UI triangles lists drawn
    uint64_t UiDrawsSkipped = 0;    // not on the window, unknown layout, no texture
    uint64_t UiTexturesCreated = 0;
    uint64_t UiTextureFailures = 0;
    uint64_t ImGuiFrames = 0;       // draw data with vertices (FPS counter, windows)
    uint64_t ImGuiCommands = 0;
    uint64_t ImGuiCommandsSkipped = 0; // texture other than the font atlas
    uint64_t WindowClears = 0;
    Deko3d::PicaProgramPickCounters Programs;
};

class GfxRenderingAPIDeko3d final : public GfxRenderingAPI, public Renderer3ds::PicaProgramPreparationBackend {
  public:
    // Hybrid preparation: draws carry their parametric fragment program,
    // drawn while the specialized one is not in deko3d.pack.
    void PrepareNativePicaPrograms(std::span<const Renderer3ds::PicaVertexArtifact> vertices) override;
    [[nodiscard]] bool UsesAlwaysAvailablePrograms() const override {
        return true;
    }

    GfxRenderingAPIDeko3d();
    ~GfxRenderingAPIDeko3d() override;

    const char* GetName() override;
    int GetMaxTextureSize() override;
    GfxClipParameters GetClipParameters() override;
    void UnloadShader(ShaderProgram* oldPrg) override;
    void LoadShader(ShaderProgram* newPrg) override;
    void ClearShaderCache() override;
    ShaderProgram* CreateAndLoadNewShader(uint64_t shaderId0, uint64_t shaderId1) override;
    ShaderProgram* LookupShader(uint64_t shaderId0, uint64_t shaderId1) override;
    void ShaderGetInfo(ShaderProgram* prg, uint8_t* numInputs, bool usedTextures[2]) override;
    uint32_t NewTexture() override;
    void SelectTexture(int tile, uint32_t textureId) override;
    void UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) override;
    void SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) override;
    void SetDepthTestAndMask(bool depth_test, bool z_upd) override;
    void SetZmodeDecal(bool decal) override;
    void SetViewport(int x, int y, int width, int height) override;
    void SetScissor(int x, int y, int width, int height) override;
    void SetUseAlpha(bool useAlpha) override;
    bool SetNativeBlendState(const GfxNativeBlendState& state) override;
    bool SetNativeCullMode(GfxNativeCullMode mode) override;
    bool SetNativeSamplerParameters(int sampler, const GfxNativeSamplerState& state) override;
    bool SetOot3dNativeTransform(const float* row_major_matrix) override;
    void DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) override;
    void Init() override;
    void Shutdown() override;
    void OnResize() override;
    void StartFrame() override;
    void EndFrame() override;
    void FinishRender() override;
    int CreateFramebuffer() override;
    void UpdateFramebufferParameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                     bool opengl_invertY, bool render_target, bool has_depth_buffer,
                                     bool can_extract_depth) override;
    GfxNativePicaBackendStats GetNativePicaBackendStats() const noexcept override;
    void SetNativePicaGeometryCacheEnabled(bool enabled) noexcept override;

    bool SubmitPicaDraw(const GfxNativePicaDrawView& draw, std::string* error = nullptr) override;
    bool SubmitPicaDisplayTransfer(const GfxNativePicaDisplayTransferView& transfer,
                                   std::string* error = nullptr) override;
    bool ClearPicaRenderTarget(uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress,
                               std::string* error = nullptr) override;
    bool SubmitPicaMemoryFill(const GfxNativePicaMemoryFillView& fill, std::string* error = nullptr) override;
    bool QueuePicaCompletion(uint64_t completionId, std::string* error = nullptr) override;
    std::vector<uint64_t> TakePicaCompletions() override;
    bool ResetPicaState(std::string* error = nullptr) override;
    // Savestates (gfx_deko3d_pica_snapshot.h): every PICA target's colour,
    // depth and stencil at the guest framebuffer extent, and back at this
    // renderer's scale. Called with the render thread drained; nothing of it
    // runs in a frame without a save or a load.
    bool CapturePicaColorTargets(std::vector<GfxNativePicaRenderTargetColorSnapshot>& snapshots,
                                 std::string* error = nullptr) override;
    bool RestorePicaColorTargets(std::span<const GfxNativePicaRenderTargetColorSnapshot> snapshots,
                                 std::string* error = nullptr) override;
    // The native namespace's replay stays skipped: a savestate keeps whatever
    // that namespace holds (what was drawn there for guest read-backs); the
    // presented scene lives in the visual namespace, which no savestate keeps.
    [[nodiscard]] bool PicaNativeNamespaceObservable() const override {
        return false;
    }

    void StartDrawToFramebuffer(int fbId, float noiseScale) override;
    void CopyFramebuffer(int fbDstId, int fbSrcId, int srcX0, int srcY0, int srcX1, int srcY1, int dstX0, int dstY0,
                         int dstX1, int dstY1) override;
    void ClearFramebuffer(bool color, bool depth) override;
    void ClearDepthRegion(int x, int y, int w, int h) override;
    void ReadFramebufferToCPU(int fbId, uint32_t width, uint32_t height, uint16_t* rgba16Buf) override;
    void ResolveMSAAColorBuffer(int fbIdTarger, int fbIdSrc) override;
    std::unordered_map<std::pair<float, float>, uint16_t, hash_pair_ff>
    GetPixelDepth(int fb_id, const std::set<std::pair<float, float>>& coordinates) override;
    void* GetFramebufferTextureId(int fbId) override;
    void SelectTextureFb(int fbId) override;
    void DeleteTexture(uint32_t texId) override;
    void SetTextureFilter(FilteringMode mode) override;
    FilteringMode GetTextureFilter() override;
    void SetSrgbMode() override;
    ImTextureID GetTextureById(int id) override;
    void SetCurrentPrimDepth(float depth) override;

    // ImGui draw data on the window, in renderer order (Fast3dGui routes a
    // copy here on the render thread). Only the font atlas is sampled.
    void RenderImGui(const ImDrawData* data);

    [[nodiscard]] const Deko3dBackendCounters& Counters() const noexcept {
        return mCounters;
    }

  private:
    // Binds the window image as the render target (acquiring it) and does
    // the clear left pending by the frame-begin ClearFramebuffer.
    void BindWindowTarget();

    struct PicaProgram {
        Deko3d::PicaProgramPick Pick = Deko3d::PicaProgramPick::NoVertex;
        const DkShader* Vertex = nullptr;
        const DkShader* Fragment = nullptr;
        // config/lighting perpixel: the draw binds mSurfaceLighting at
        // Deko3d::kSurfaceLightingBinding.
        bool SurfaceLighting = false;
    };
    // The shaders of a draw (package 3); no draw ever compiles.
    PicaProgram ResolvePicaProgram(const GfxNativePicaDrawView& draw);
    const DkShader* ResolvePicaStage(Deko3d::ShaderStage stage, std::string_view canonical,
                                     Renderer3ds::PicaShaderSourceIdentity identity);
    // config/lighting (gfx_deko3d_lighting.h): applies a new mode at the
    // start of a frame, swaps an eligible draw's shaders for the mode's
    // variants when the pack has them, and logs the mode and counters.
    void ApplyLightingSettings();
    void ApplyPicaLighting(const GfxNativePicaDrawView& draw, std::string_view fragmentSource,
                           Renderer3ds::PicaShaderSourceIdentity fragmentIdentity, PicaProgram& program);
    const DkShader* ResolveLightingFragment(Deko3d::LightingMode mode, std::string_view canonical,
                                            Renderer3ds::PicaShaderSourceIdentity identity);
    const DkShader* ResolvePerPixelVertex(const GfxNativePicaDrawView& draw);
    bool BuildSurfaceLighting(const GfxNativePicaDrawView& draw);
    void LogLightingCounters(const char* when);
    void LogPicaProgramCounters(const char* when);
    // Package 5 (gfx_deko3d_pica_draw.cpp).
    void InitPica();
    void BindPicaTarget(Deko3d::PicaTarget& target);
    void ApplyPendingPicaFills(const Deko3d::PicaTargetKey& key, Deko3d::PicaTarget& target);
    void ApplyPicaFill(const GfxNativePicaMemoryFillView& fill, const Deko3d::PicaTargetKey& key,
                       Deko3d::PicaTarget& target, Deko3d::PicaFillCoverage coverage);
    // Barriers before the next draw: uploads/descriptors/shader code.
    void FlushPicaInvalidations();
    bool RecordPicaDraw(const GfxNativePicaDrawView& draw, const PicaProgram& program,
                        const Deko3d::PicaTargetKey& key, Deko3d::PicaTarget& target);
    void DrawPicaScanout(const GfxNativePicaDisplayTransferView& transfer, const Deko3d::PicaDisplay& display,
                         const Deko3d::PicaTransferPlan& plan, float allocScale);
    // Opens the device frame (if needed) and starts the resources' frame
    // once per device frame.
    void EnsureFrame();
    // Package 6 (gfx_deko3d_ui.cpp).
    struct UiTexture {
        DkMemBlock Memory = nullptr;
        DkImage Image{};
        uint32_t ImageSlot = Deko3d::PicaResources::kMaxImages;
        uint32_t Width = 0;
        uint32_t Height = 0;
        Deko3d::PicaSamplerKey Sampler{};
    };
    struct UiRetired {
        DkMemBlock Memory = nullptr;
        uint32_t ImageSlot = Deko3d::PicaResources::kMaxImages;
        uint64_t Frame = 0;
    };
    void InitUi();
    void ShutdownUi();
    // Frees what the GPU can no longer read (BeginFrame of a device frame).
    void CollectUiRetired();
    void RetireUi(DkMemBlock memory, uint32_t imageSlot);
    UiTexture* SelectedUiTexture();
    bool UploadUiTexture(UiTexture& texture, const uint8_t* rgba8, uint32_t width, uint32_t height);
    // Records one triangle list on the window (vertices in the UI layout).
    bool RecordUiDraw(const float* vertices, size_t floats, size_t vertexCount, Deko3d::UiVertexLayout layout,
                      const UiTexture* texture, const Deko3d::UiRect& viewport, const Deko3d::UiRect& scissor,
                      float clipYScale, bool blend, const float* transform);
    void LogUiCounters(const char* when);

    Deko3d::Device mDevice;
    Deko3d::ShaderStore mShaders;
    Deko3d::PicaResources mResources;
    uint64_t mResourcesFrame = ~0ULL;
    Deko3d::PicaDrawResources mDrawResources;
    Deko3d::PicaTargets mTargets;
    std::vector<GfxNativePicaMemoryFillView> mPendingFills;
    // What dkCmdBufBindRenderTargets holds: a PICA target, the window, or
    // nothing known (frame start, after a target was created).
    const void* mBoundTarget = nullptr;
    const DkShader* mScanoutVertex = nullptr;
    const DkShader* mScanoutFragment = nullptr;
    uint32_t mLinearClampSampler = 0;
    bool mCullInvert = false;    // config/deko3d_cull_invert
    // config/deko3d_barrier (diagnosis): the barrier after new images, as
    // words "full|primitives|fragments|none image descriptors shader l2".
    DkBarrier mImageBarrierMode = DkBarrier_Full;
    uint32_t mImageBarrierFlags = DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors | DkInvalidateFlags_Shader;
    bool mFlipY = true;          // config/deko3d_flip_y ("0": the p6 orientation, upside down)
    // Every viewport goes through here: deko3d 0.5.0 always flips clip-space
    // y in the viewport (y up, like GL) while OriginUpperLeft stores row 0 at
    // the top, so NDC -1 landed on the last row. A NegativeY viewport swizzle
    // undoes it before the viewport (p6b's negative-height viewport made the
    // hardware viewport clip rect garbage: black screen).
    void ApplyDkViewport(DkCmdBuf cmd, float x, float y, float width, float height) const noexcept {
        const DkViewport viewport = {x, y, width, height, 0.0F, 1.0F};
        dkCmdBufSetViewports(cmd, 0, &viewport, 1);
        const DkViewportSwizzle swizzle = {DkSwizzle_PositiveX, Deko3d::ClipYSwizzleNegates(mFlipY)
                                                                    ? DkSwizzle_NegativeY
                                                                    : DkSwizzle_PositiveY,
                                           DkSwizzle_PositiveZ, DkSwizzle_PositiveW};
        dkCmdBufSetViewportSwizzles(cmd, 0, &swizzle, 1);
    }
    bool mPicaLean = true;       // config/gl_lean (identical display copies skipped)
    Deko3d::PicaStageCache<DkShader> mVertexStages;
    Deko3d::PicaStageCache<DkShader> mFragmentStages;
    uint64_t mTranslationFailures = 0;
    // config/lighting: the mode in use, the live settings revision applied,
    // the variants per native fragment identity (one cache per mode) and per
    // vertex identity and hooks, the frame's environment profile (per-pixel)
    // and the block of the draw being prepared.
    Deko3d::LightingMode mLightingMode = Deko3d::LightingMode::Off;
    uint64_t mLightingRevision = 0;
    Deko3d::LightingCounters mLightingCounters{};
    Deko3d::PicaStageCache<DkShader> mToonFragments;
    Deko3d::PicaStageCache<DkShader> mPerPixelFragments;
    struct PerPixelVertex {
        Renderer3ds::PicaShaderSourceIdentity Identity;
        Renderer3ds::PicaVertexShaderHookLayout Hooks;
        const DkShader* Shader = nullptr;
    };
    std::vector<PerPixelVertex> mPerPixelVertices;
    Renderer3ds::PicaEnvironmentLighting mLightingEnvironment{};
    std::optional<Renderer3ds::PicaSurfaceLightingView> mLightingView;
    Renderer3ds::PicaSurfaceLightingUniforms mSurfaceLighting{};
    bool mDeviceReady = false;
    std::vector<Deko3dFramebuffer> mFramebuffers;
    std::vector<std::unique_ptr<Deko3dShaderProgram>> mShaderPrograms;
    uint32_t mNextTextureId = 1;
    std::unordered_map<uint32_t, UiTexture> mUiTextures;
    std::vector<UiRetired> mUiRetired;
    std::array<uint32_t, 2> mUiSelected{};
    int mUiActiveTile = 0;
    std::array<float, 16> mUiTransform{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                       0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    std::array<int, 4> mUiViewport{};
    std::array<int, 4> mUiScissor{};
    bool mUiViewportSet = false;
    bool mUiScissorSet = false;
    bool mUiUseAlpha = false;
    const DkShader* mUiSolidVertex = nullptr;
    const DkShader* mUiSolidFragment = nullptr;
    const DkShader* mUiTexturedVertex = nullptr;
    const DkShader* mUiTexturedFragment = nullptr;
    uint32_t mImGuiFontTexture = 0;
    int mCurrentFramebuffer = 0;
    bool mWindowBound = false;
    bool mWindowClearPending = false;
    bool mWindowClearColor = false;
    bool mWindowClearDepth = false;
    std::array<float, 4> mWindowClearRgba{};
    FilteringMode mFilterMode = FILTER_THREE_POINT;
    bool mGeometryCacheEnabled = true;
    std::vector<uint64_t> mCompletions;
    Deko3dBackendCounters mCounters{};
    Deko3dPicaCalls mCalls{};

    // config/grass = draw (gfx_deko3d_grass.cpp). mGrassDraws is false in
    // every other case, and then nothing below is touched.
    bool mGrassDraws = false;
    // The opaque world draw of the frame that last reached a target: where
    // and how the grass draws (viewport and scissor with dynamic resolution
    // and widescreen applied, the scene's depth compare).
    struct GrassScene {
        const Deko3d::PicaTarget* Target = nullptr;
        Deko3d::PicaRect Viewport;
        DkScissor Scissor{};
        uint8_t DepthCompare = 0;
        uint64_t Frame = ~0ULL;
    };
    GrassScene mGrassScene;
    struct GrassDrawn {
        const Deko3d::PicaTarget* Target = nullptr;
        uint64_t Frame = ~0ULL;
    };
    GrassDrawn mGrassDrawn;
    void RememberGrassScene(const Deko3d::PicaTarget& target, const Deko3d::PicaRect& viewport,
                            const DkScissor& scissor, uint8_t depthCompare);
    // Draws the culled tufts of this frame's sources into `target`, once per
    // target and frame, if its opaque world was drawn this frame.
    void DrawGrass(const Deko3d::PicaTargetKey& key, Deko3d::PicaTarget& target);
};

namespace Deko3d {
// gfx_deko3d_grass.cpp: a scene draw of the frame (opaque world draws become
// grass sources) and the end of the frame (placements, the next cull).
void GrassObserveDraw(const GfxNativePicaDrawView& draw, uint64_t frame);
void GrassEndFrame(uint64_t frame);
// config/grass = draw and the preset loaded (EnsureFrame reads it per frame).
[[nodiscard]] bool GrassDrawEnabled();
} // namespace Deko3d

} // namespace Fast
#endif
