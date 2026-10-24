#pragma once
// GfxRenderingAPI on deko3d (Switch). Package 2 (docs/DEKO3D_PROGRESSO.md):
// device, window image, clears and the present. Package 3: every PICA draw
// resolves its shaders from deko3d.pack (specialized, else parametric).
// Package 4: its textures, LUT, vertex/index streams and uniforms are
// uploaded (gfx_deko3d_pica_resources.h).
// Drawing, the UI and the rest come in later packages; until then their calls
// are accepted and counted, so the game runs headless on this backend.

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_programs.h"
#include "fast/backends/gfx_rendering_api.h"
#include "fast/renderer3ds/pica_program_preparation.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_pica_resources.h"
#include "gfx_deko3d_shaders.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

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

struct Deko3dBackendCounters {
    uint64_t PicaDrawsSkipped = 0;
    uint64_t PicaDrawsPrepared = 0; // program + resources ready (drawn from package 5 on)
    uint64_t PicaDrawsRejected = 0; // what the GL backend rejects too
    uint64_t PicaTransfersSkipped = 0;
    uint64_t PicaFillsSkipped = 0;
    uint64_t PicaClearsSkipped = 0;
    uint64_t UiDrawsSkipped = 0;
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
    };
    // The shaders of a draw (package 3); no draw ever compiles.
    PicaProgram ResolvePicaProgram(const GfxNativePicaDrawView& draw);
    const DkShader* ResolvePicaStage(Deko3d::ShaderStage stage, std::string_view canonical,
                                     Renderer3ds::PicaShaderSourceIdentity identity);
    void LogPicaProgramCounters(const char* when);
    // Opens the device frame (if needed) and starts the resources' frame
    // once per device frame.
    void EnsureFrame();

    Deko3d::Device mDevice;
    Deko3d::ShaderStore mShaders;
    Deko3d::PicaResources mResources;
    uint64_t mResourcesFrame = ~0ULL;
    Deko3d::PicaDrawResources mDrawResources;
    Deko3d::PicaStageCache<DkShader> mVertexStages;
    Deko3d::PicaStageCache<DkShader> mFragmentStages;
    uint64_t mTranslationFailures = 0;
    bool mDeviceReady = false;
    std::vector<Deko3dFramebuffer> mFramebuffers;
    std::vector<std::unique_ptr<Deko3dShaderProgram>> mShaderPrograms;
    uint32_t mNextTextureId = 1;
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
};

} // namespace Fast
#endif
