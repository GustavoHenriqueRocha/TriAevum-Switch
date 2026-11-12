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
#include "fast/backends/gfx_deko3d_clear_draw.h"
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/renderer3ds/pica_program_preparation.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_pica_resources.h"
#include "gfx_deko3d_pica_targets.h"
#include "gfx_deko3d_shaders.h"
#include "fast/backends/gfx_deko3d_ui_plan.h"
#include "fast/oot3d/grass_draw_point.h"
#include "fast/oot3d/player_shadow.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
    uint64_t PicaTextureCopies = 0;       // raw copies out of a GPU target, captured (gfx_deko3d_pica_copies.h)
    uint64_t PicaTextureCopiesReused = 0; // a replay of a captured copy
    uint64_t PicaTextureCopiesFailed = 0; // completed without bytes (logged)
    double PicaTextureCopyMilliseconds = 0.0;    // the captures' time, summed
    double PicaTextureCopyMaxMilliseconds = 0.0; // the longest capture
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
    // deko3d keeps no GPU image per raw copy, only each version's bytes
    // (gfx_deko3d_pica_copies.h): the set says which versions draws may still sample.
    bool SetPinnedPicaTextureCopies(std::span<const Renderer3ds::PicaTextureCopyReference> copies,
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
    // fps60: which presented frames are synthetic in-between samples (grass
    // draws the last real frame's placements in them; gfx_deko3d_grass.cpp).
    bool PublishPicaFrameTemporalSample(const Renderer3ds::PicaFrameTemporalSample& sample) override;

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
        // The class of the base (native) fragment program (per-frame
        // breakdown, and a spilling base gets no lighting variant).
        Deko3d::PicaBaseClass Base = Deko3d::PicaBaseClass::Normal;
        // config/lighting: drawn with the mode's variant.
        bool Lit = false;
        // config/lighting perpixel: the draw binds mSurfaceLighting at
        // Deko3d::kSurfaceLightingBinding.
        bool SurfaceLighting = false;
        // The per-pixel fragment variant's path (Deko3d::ClassifyPerPixelSurface).
        Deko3d::PerPixelPath SurfacePath = Deko3d::PerPixelPath::Generic;
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
    // Toon, or per-pixel of `path` (one cache per mode and path).
    const DkShader* ResolveLightingFragment(Deko3d::LightingMode mode, std::string_view canonical,
                                            Renderer3ds::PicaShaderSourceIdentity identity,
                                            Deko3d::PerPixelPath path = Deko3d::PerPixelPath::Generic);
    const DkShader* ResolvePerPixelVertex(const GfxNativePicaDrawView& draw);
    void LogLightingCounters(const char* when);
    // A temporal sample opens (PublishPicaFrameTemporalSample): its frame
    // kind, and the one-frame lighting transients of the samples before it.
    void BeginLightingSample(bool inBetween);
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
    // `reset draw:` line of a depth reset's plan (config/deko3d_clear_draw).
    void LogPicaResetDraw(const GfxNativePicaDrawView& draw, const Deko3d::PicaClearDrawPlan& plan);
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
    // config/deko3d_clear_draw (off by default, gfx_deko3d_clear_draw.h): the
    // frame's depth reset (a full-target ALWAYS draw) turned into fast clears
    // where that is provably exact.
    bool mClearDraw = false;
    // `reset draw:` lines (logs/deko3d.txt): when the plan changes (at most
    // kResetLineLimit), and at the first reset after each periodic log.
    static constexpr uint32_t kResetLineLimit = 64U;
    uint64_t mResetSignature = ~0ULL;
    uint32_t mResetLines = 0U;
    bool mResetLineDue = false;
    // config/deko3d_barrier (diagnosis): the barrier after new images, as
    // words "full|primitives|fragments|none image descriptors shader l2
    // frame". Default Full + Image|Descriptors: uploads and descriptor
    // writes touch neither shader code, global memory nor uniforms (those
    // are fresh stream memory per draw; new code sets its own Shader flag
    // through ShaderStore::TakeCodeWritten). Full stays: the copy engine's
    // writes must land before the 3D engine samples them.
    DkBarrier mImageBarrierMode = DkBarrier_Full;
    uint32_t mImageBarrierFlags = DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors;
    // "frame": also the barrier at each frame's first draw (before 07/11).
    bool mFrameBarrier = false;
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
    // Issued draws by base program class (logs/deko3d.txt, per frame).
    Deko3d::PicaFrameDrawWindow mFrameDraws;
    // config/lighting: the mode in use, the live settings revision applied,
    // the variants per native fragment identity (one cache per mode, and per
    // per-pixel path) and per vertex identity and hooks, the frame's
    // environment profile (per-pixel) and the block of the draw being prepared.
    Deko3d::LightingMode mLightingMode = Deko3d::LightingMode::Off;
    uint64_t mLightingRevision = 0;
    Deko3d::LightingCounters mLightingCounters{};
    Deko3d::PicaStageCache<DkShader> mToonFragments;
    std::array<Deko3d::PicaStageCache<DkShader>, static_cast<size_t>(Deko3d::PerPixelPath::Count)> mPerPixelFragments;
    struct PerPixelVertex {
        Renderer3ds::PicaShaderSourceIdentity Identity;
        Renderer3ds::PicaVertexShaderHookLayout Hooks;
        const DkShader* Shader = nullptr;
    };
    std::vector<PerPixelVertex> mPerPixelVertices;
    Renderer3ds::PicaEnvironmentLighting mLightingEnvironment{};
    std::optional<Renderer3ds::PicaSurfaceLightingView> mLightingView;
    Renderer3ds::PicaSurfaceLightingUniforms mSurfaceLighting{};
    // Per-pixel draws by frame kind (logs/deko3d.txt "lighting real" and
    // "lighting in-between"):
    // the temporal sample the draws belong to (PublishPicaFrameTemporalSample)
    // is an fps60 in-between frame, or a real one.
    bool mInBetweenSample = false;
    Deko3d::FrameKindLighting mRealLighting{};
    Deko3d::FrameKindLighting mInBetweenLighting{};
    // Scene environment changes logged (on/off, scope, rim), at most 64.
    uint32_t mEnvironmentChangesLogged = 0;
    // One-frame per-pixel transients of the presented samples
    // ("lighting transient:" lines, at most 48).
    Deko3d::LightingTransientWatch mLightingWatch;
    uint32_t mTransientsLogged = 0;
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
    // The opaque world draw of the frame that last reached a target: how the
    // grass draws (viewport and scissor with dynamic resolution and
    // widescreen applied, the scene's depth compare). Its target and frame are
    // mGrassPoints' Scene.
    struct GrassScene {
        Deko3d::PicaRect Viewport;
        DkScissor Scissor{};
        uint8_t DepthCompare = 0;
    };
    GrassScene mGrassScene;
    // Where the grass goes in the frame: before a translucent world draw, an
    // overlay (HUD, minimap, title card) or the display transfer, whichever
    // comes first (grass_draw_point.h).
    Oot3d::GrassDrawPointGate<const Deko3d::PicaTarget*> mGrassPoints;
    void RememberGrassScene(const Deko3d::PicaTarget& target, const Deko3d::PicaRect& viewport,
                            const DkScissor& scissor, uint8_t depthCompare);
    // Draws the culled tufts of this frame's sources into `target`, once per
    // target and frame, if its opaque world and sources were drawn this frame.
    void DrawGrass(const Deko3d::PicaTargetKey& key, Deko3d::PicaTarget& target, Oot3d::GrassDrawPoint point);

    // config/simple_shadow (gfx_deko3d_player_shadow.cpp): Link's opaque draws
    // of the frame, drawn again flattened onto the floor at the frame's first
    // draw point after them (the grass's points), the way
    // config/simple_shadow_method says.
    struct PlayerShadowDraw {
        const Deko3d::PicaTarget* Target = nullptr; // the frame's draws may go to several
        const DkShader* Vertex = nullptr;
        DkGpuAddr Uniforms = DK_GPU_ADDR_INVALID; // his uniforms with the flattening view rows
        // Method texture: the same with the silhouette's projection rows.
        DkGpuAddr SilhouetteUniforms = DK_GPU_ADDR_INVALID;
        uint32_t UniformBytes = 0;
        std::array<DkVtxAttribState, Deko3d::kPicaVertexAttributeCount> Attributes{};
        std::array<DkVtxBufferState, Deko3d::kPicaVertexAttributeCount> Buffers{};
        std::array<DkBufExtents, Deko3d::kPicaVertexAttributeCount> Extents{};
        DkPrimitive Primitive = DkPrimitive_Triangles;
        bool Indexed = false;
        bool Indices16 = false;
        DkGpuAddr IndexStream = DK_GPU_ADDR_INVALID;
        uint32_t IndexCount = 0;
        int32_t BaseVertex = 0;
        uint32_t VertexCount = 0;
        Deko3d::PicaRect Viewport;
        DkScissor Scissor{};
        uint8_t DepthCompare = 0;
    };
    struct PlayerShadowState {
        // Of Selected, Method, Projection, Region, WorldToClip, Draws and StencilTaken.
        uint64_t Frame = ~uint64_t{0};
        std::optional<Oot3d::PlayerShadowFrame> Selected;
        // The bridge's, at the frame's first draw.
        Oot3d::PlayerShadowMethod Method = Oot3d::PlayerShadowMethod::TwoPass;
        // One light and floor for all of the frame's meshes (taken at its first).
        std::optional<std::array<float, 16>> Projection;
        // Methods cover and texture: the floor region and the camera of his first draw.
        std::optional<Oot3d::PlayerShadowRegion> Region;
        std::optional<std::array<float, 16>> WorldToClip;
        std::vector<PlayerShadowDraw> Draws;
        // A draw of the frame tests or writes the top stencil bit, which two
        // passes and cover borrow: that frame they draw in one pass.
        bool StencilTaken = false;
        // Whether that bit can be set outside the shadow. Targets start with
        // it zero and the shadow's second pass leaves it so; once a game draw
        // uses it or a savestate restores stencil, a pass clears it over its
        // whole target once, and the target counts as clean again.
        bool StencilShared = false;
        std::unordered_set<const Deko3d::PicaTarget*> StencilClean; // since the last share
        uint64_t FullClears = 0;
        const DkShader* Fragment = nullptr;
        const DkShader* CoverVertex = nullptr; // method cover
        const DkShader* FloorVertex = nullptr; // method texture
        const DkShader* FloorFragment = nullptr;
        // Method texture: the silhouette, drawn and then sampled at each draw point.
        DkMemBlock SilhouetteMemory = nullptr;
        DkImage SilhouetteImage{};
        DkImageView SilhouetteView{};
        uint32_t SilhouetteSlot = 0;
        uint64_t SilhouetteFrame = ~uint64_t{0}; // the last frame that drew it
        uint64_t Frames = 0;
        uint64_t Meshes = 0;
        uint64_t Skipped = 0;
        uint64_t StencilFrames = 0;
        std::array<uint64_t, Oot3d::kPlayerShadowMethodCount> MethodFrames{}; // drawn with each method
    };
    PlayerShadowState mPlayerShadow;
    // InitPica: the programs and the silhouette, and the bridge told whether
    // the shadow can be drawn.
    void InitPlayerShadow();
    // Every PICA draw, after EnsureFrame: the frame's shadow and stencil use.
    void ObservePlayerShadowDraw(const GfxNativePicaDrawView& draw);
    // RecordPicaDraw, with the state it bound, before the draw call.
    void CapturePlayerShadowDraw(const GfxNativePicaDrawView& draw, const PicaProgram& program,
                                 const Deko3d::PicaTarget& target, const PlayerShadowDraw& bound);
    // At a draw point into `target`: the captured draws, once per frame.
    void DrawPlayerShadow(Deko3d::PicaTarget& target);
    // Method texture, at a draw point: the draws flat into the silhouette,
    // whose target stays bound (the caller binds the scene target again).
    void DrawPlayerShadowSilhouette(const std::vector<PlayerShadowDraw>& draws);
    // A captured draw's vertex input and draw call.
    static void DrawPlayerShadowMesh(DkCmdBuf cmd, const PlayerShadowDraw& draw);
    // SubmitPicaDisplayTransfer of a raw TextureCopy (TextureCopyBytes != 0).
    bool SubmitPicaTextureCopy(const GfxNativePicaDisplayTransferView& transfer, std::string* error);
    // The copy's destination bytes, or why it has none.
    const char* CapturePicaTextureCopy(const GfxNativePicaDisplayTransferView& transfer,
                                       Deko3d::PicaCopyStore::Version& version);
    std::vector<uint8_t> mCopyRgba;    // a raw copy's source rows read back
};

namespace Deko3d {
// gfx_deko3d_grass.cpp: a scene draw of the frame (opaque world draws become
// grass sources) and the end of the frame (placements, the next cull).
void GrassObserveDraw(const GfxNativePicaDrawView& draw, uint64_t frame);
void GrassEndFrame(uint64_t frame);
// Frame start (EnsureFrame, after the texture hold): config/grass_hold's
// wait for a scene's first grass (grass_scene_hold.h).
void GrassBeginFrame();
// The sample the next draws of `frame` belong to: synthetic (an fps60
// in-between frame) or real.
void GrassNoteSample(bool synthetic, uint64_t frame);
// config/grass = draw and the preset loaded (EnsureFrame reads it per frame).
[[nodiscard]] bool GrassDrawEnabled();
// Placements requested and not made yet (config/grass = draw; render
// thread): entry work for the pacing governor (Deko3dEntryWorkPending).
[[nodiscard]] bool GrassWorkPending();
} // namespace Deko3d

} // namespace Fast
#endif
