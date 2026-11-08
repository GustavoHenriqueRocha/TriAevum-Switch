#pragma once
// deko3d PICA resources (Switch), package 4 (docs/DEKO3D_PROGRESS.md):
// textures (RGBA8 images from the decoded PICA mips, uploaded through stream
// memory with dkCmdBufCopyBufferToImage), the lighting LUT (R32UI 256x24),
// samplers deduplicated by state, one image + one sampler descriptor array
// for the whole device, and the per-draw vertex/index/uniform streams. The
// decisions (keys, wrap/filter, vertex plan) live in the pure
// fast/backends/gfx_deko3d_pica_plan.h.
//
// Lifetime: everything a frame records reads memory that stays valid until
// that frame's slot fence; evictions (gfx_opengl_pica_residency.h policy,
// never what the last two frames used) run right after BeginFrame waited
// for the slot, so no in-flight frame can still read an evicted image.

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_plan.h"
#include "fast/backends/gfx_rendering_api.h"
#include "gfx_deko3d_device.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Fast::Deko3d {

struct PicaResourceStats {
    uint64_t TexturesCreated = 0;
    uint64_t TexturesEvicted = 0;
    uint64_t TexturesEvictedForBudget = 0; // of those: least recently used over the budget (not idle 30 s)
    uint64_t TextureBytes = 0;     // live
    uint64_t TextureUploadBytes = 0;
    uint64_t TexturesPredecoded = 0; // mips from the plan worker
    uint64_t TextureFailures = 0;    // decode/allocation failures (fallback used)
    uint64_t ShadowFallbacks = 0;    // Shadow2D textures (not in the first version)
    uint64_t UnsupportedWraps = 0;   // clamp-to-border/asymmetric (clamp to edge used)
    uint64_t LutsCreated = 0;
    uint64_t Samplers = 0;
    uint64_t VertexBytes = 0;
    uint64_t IndexBytes = 0;
    uint64_t UniformBytes = 0;
    uint64_t UniformReuses = 0;
    uint64_t StreamFailures = 0;
    uint64_t TextureReplacements = 0;    // texture pack images uploaded at their own size
    uint64_t TextureReplacementBytes = 0;
    uint64_t TextureReplacementLiveBytes = 0; // pack images resident now (part of TextureBytes)
    uint64_t TextureReplacementReuploads = 0; // pack images uploaded again (evicted since), same pack settings
    uint64_t TextureReplacementsBatched = 0;  // of TextureReplacements: uploaded at frame top (UploadReadyReplacements)
    // Menu textures (PicaTextureIsMenuArt, config/texture_pack_menu) made with a pack image: its image was
    // ready at the texture's first draw, or the native texture was drawn first (the swap in view).
    uint64_t MenuTexturesReady = 0;
    uint64_t MenuTexturesNativeFirst = 0;
};

// World textures of one scene visit (the pack runtime's NoteScene to the next
// change; SWITCH_PORT.md "File select, world textures and grass without pop-in"): pack images ready
// at the texture's first draw, natives drawn first, and when those were
// swapped for their pack image: within the scene's first kSceneStartFrames
// frames (its fade-in) or later, in view (the pop-ins left). The hold: frames
// that waited for their on-screen images, the time, and waits that ended with
// images still decoding.
struct PicaSceneTextureStats {
    uint64_t Frames = 0;
    uint64_t Ready = 0;
    uint64_t NativeFirst = 0;
    uint64_t SwapsAtStart = 0;
    uint64_t SwapsLater = 0;
    uint64_t HoldFrames = 0;
    uint64_t HoldMicros = 0;
    uint64_t HoldTimeouts = 0;
};
// The frames of a scene's start (about its fade-in at 60 FPS).
inline constexpr uint64_t kSceneStartFrames = 60U;

// A texture pack change (the Video tab, or the configuration at start) from
// the frame it applies until it has taken effect on screen
// (TexturePackChangeTookEffect), then logged once: `resources: texture pack
// change took effect (...)`.
struct PicaPackChangeStats {
    bool Active = false;
    bool AtStart = false;
    bool PackOn = false;             // the settings it applied
    uint32_t MaxScale = 0;
    uint64_t Frame = 0;              // the frame it applied in
    std::chrono::steady_clock::time_point Start{};
    uint64_t IndexMicros = 0;        // to the pack's index ready (pack on; 0 = not yet)
    uint64_t ResolvedAgain = 0;      // textures retired at their draw and resolved again (earlier settings)
    uint64_t MadeWhileIndexing = 0;  // natives made while the index was being built (no hash yet)
    uint64_t ResolvedAfterIndex = 0; // of those: resolved again once the index was ready
    uint64_t Uploaded = 0;           // pack images uploaded since
    uint64_t UploadedBytes = 0;
    uint64_t Released = 0;           // pack images of earlier settings dropped
    uint64_t Deferred = 0;           // draws kept on their earlier texture: over kTexturePackRebuildsPerFrame
    uint64_t RebuildFrames = 0;      // frames that used the whole rebuild budget
    uint64_t LongestFrameStartMicros = 0; // the render thread's longest frame start (BeginFrame) since
    // Of the frame start: the longest pack settings apply (Configure), stale
    // release (Evict) and finished pack image upload (UploadReadyReplacements).
    uint64_t LongestApplyMicros = 0;
    uint64_t LongestEvictMicros = 0;
    uint64_t LongestUploadMicros = 0;
    // A change of only the size cap: pack images and natives kept (the same
    // size at the new cap), and the runtime's counters at the change.
    bool CapOnly = false;
    uint64_t KeptImages = 0;
    uint64_t KeptNatives = 0;
    ::Oot3d::Renderer::AzaharCapChangeStatus CapStatusBefore{};
};
// Retired textures and pack images of earlier settings destroyed per frame at
// most (Evict); the rest wait for the next frames, drawn by nothing.
inline constexpr uint32_t kTextureReleasesPerFrame = 12U;
// About 30 s at 60 FPS: a change still waiting then is logged as incomplete.
inline constexpr uint64_t kPackChangeReportFrames = 1800U;

// What one draw binds: texture handles for bindings 1..3 and 13, the vertex
// streams and the uniform blocks (vertex binding 0, fragment binding 4).
struct PicaDrawResources {
    std::array<DkResHandle, 3> Textures{};
    DkResHandle LightingLut = 0;
    PicaVertexPlan Vertices;
    DkGpuAddr VertexStream = DK_GPU_ADDR_INVALID;
    DkGpuAddr IndexStream = DK_GPU_ADDR_INVALID;
    uint32_t IndexBytes = 0;
    DkGpuAddr VertexUniforms = DK_GPU_ADDR_INVALID;
    uint32_t VertexUniformBytes = 0;
    DkGpuAddr FragmentUniforms = DK_GPU_ADDR_INVALID;
    uint32_t FragmentUniformBytes = 0;
    // config/lighting perpixel: the PicaSurfaceLighting block
    // (Deko3d::kSurfaceLightingBinding); invalid otherwise.
    DkGpuAddr SurfaceLighting = DK_GPU_ADDR_INVALID;
    uint32_t SurfaceLightingBytes = 0;
};

class PicaResources {
  public:
    static constexpr uint32_t kMaxImages = 4096U;
    static constexpr uint32_t kMaxSamplers = 512U;
    // Fixed image slots.
    static constexpr uint32_t kFallbackColorImage = 0U; // white RGBA8 1x1
    static constexpr uint32_t kFallbackIntegerImage = 1U; // R32UI 1x1 0xFFFFFFFF
    static constexpr uint32_t kReservedImages = 2U;
    static constexpr uint32_t kNearestSampler = 0U;

    bool Init(Device& device);
    void Shutdown() noexcept;

    // After Device::BeginFrame: binds the descriptor arrays for the frame's
    // commands and releases what the policy no longer keeps.
    void BeginFrame(uint64_t frame);

    // Everything a draw reads, recorded into the frame (uploads included).
    // False with an error on a draw the GL backend rejects too.
    bool Prepare(const GfxNativePicaDrawView& draw, PicaDrawResources& out, std::string* error);
    // The per-pixel lighting block of the draw Prepare just made ready
    // (config/lighting perpixel), deduplicated like the PICA uniforms.
    bool PrepareSurfaceLighting(std::span<const uint8_t> bytes, PicaDrawResources& out);

    // True once after new descriptors or images were written: the caller
    // issues dkCmdBufBarrier(..., DkInvalidateFlags_Descriptors |
    // DkInvalidateFlags_Image) before the next draw. Not set by BeginFrame:
    // every dkQueueFlush already invalidates the image, shader, descriptor
    // and L2 caches before the next commands (deko3d postSubmitFlush).
    bool TakeInvalidate() noexcept {
        const bool pending = mInvalidate;
        mInvalidate = false;
        return pending;
    }
    // config/deko3d_barrier "frame": the barrier at every frame's first draw
    // as before 07/11 (A/B).
    void RequestInvalidate() noexcept { mInvalidate = true; }

    [[nodiscard]] const PicaResourceStats& Stats() const noexcept { return mStats; }
    // The current scene's `world pack` line (frame statistics); nothing
    // without the pack or before the first scene.
    void LogSceneProgress() {
        if (mTexturePack && mScene.Frames != 0U) LogScene(mScene, "now");
    }
    [[nodiscard]] DkGpuAddr ConstantAttribute() const noexcept { return mConstantGpu; }

    // Image descriptors owned by others (display images of package 5).
    // Allocate returns kMaxImages when full; Free only once no frame in
    // flight can read the slot.
    uint32_t AllocateImageSlot() noexcept { return mImageSlots.Allocate(); }
    void FreeImageSlot(uint32_t slot) noexcept {
        if (slot >= kReservedImages && slot < kMaxImages) {
            mImageSlots.Free(slot);
        }
    }
    void WriteImageDescriptor(uint32_t slot, const DkImage& image);
    // The descriptor slot of a sampler state (deduplicated).
    uint32_t SamplerSlot(const PicaSamplerKey& key);

  private:
    struct Texture {
        DkMemBlock Memory = nullptr;
        DkImage Image{};
        uint32_t ImageSlot = 0;
        uint32_t SamplerSlot = 0;
        uint64_t Bytes = 0;
        uint64_t LastUsedFrame = 0;
        uint64_t PackHash = 0;          // texture pack identity (while decoding, and of a pack image)
        bool PackPending = false;
        // Made while the pack's index was being built (no hash, PackHash 0;
        // config/texture_pack_reresolve): made again at its first draw once the
        // index is ready (TexturePackEntryActionFor).
        bool PackUnindexed = false;
        bool PackMenu = false;          // resolved as a HUD image (PicaTextureIsMenuArt)
        bool Replacement = false;       // a texture pack image (under the replacement key)
        uint32_t PackEpoch = 0;         // mPackEpoch when made
        uint64_t SceneSerial = 0;       // a pack image: the scene visit that last drew it
        // While PackPending: the view it was made from (spans empty), so
        // BeginFrame can upload the finished pack image for it.
        Renderer3ds::PicaTextureView PackView{};
        // A pack image: its PNG's size before the cap (TexturePackCapChangesImage).
        ::Oot3d::Renderer::TextureSize PackSource{};
    };

    struct LastUniforms {
        uint64_t Frame = ~0ULL;
        DkGpuAddr Address = DK_GPU_ADDR_INVALID;
        std::vector<uint8_t> Bytes;
    };

    // A uniform block in this frame's stream; one equal to `last` (this
    // frame) is bound again instead of copied.
    bool StreamUniforms(std::span<const uint8_t> bytes, LastUniforms& last, DkGpuAddr& address, uint32_t& size);
    DkResHandle TextureHandle(const Renderer3ds::PicaTextureView& texture, bool menuDraw);
    DkResHandle LightingLutHandle(const Renderer3ds::PicaLightingLutView& lut);
    // A sampled 2D image with its own memory block, its descriptor in `slot`.
    bool CreateImage(Texture& texture, DkImageFormat format, uint32_t width, uint32_t height, uint32_t levels);
    void UploadLevel(const Texture& texture, uint32_t level, uint32_t width, uint32_t height, DkGpuAddr source);
    void Destroy(Texture& texture) noexcept;
    // The texture pack image (larger than the native one) under the replacement key; 0 when not now.
    // `menu`: a HUD image (kept by the pack runtime), not released after the upload.
    DkResHandle UploadReplacement(const Renderer3ds::PicaTextureView& view, const PicaTextureKey& key,
                                  const ::Oot3d::Renderer::AzaharTextureReplacement& replacement, bool menu);
    // Applies a texture pack change published by the Video tab (BeginFrame).
    void ApplyTexturePackSettings();
    // Takes an entry out of the cache; its memory goes in Evict once no
    // frame in flight can read it.
    void Retire(Texture& texture);
    // Destroys an entry already out of mTextures and takes it off the totals.
    void Forget(Texture& texture) noexcept;
    void Evict();
    // Pack images the worker finished for natives drawn last frame, all
    // uploaded at frame top (one barrier at the first draw for all).
    void UploadReadyReplacements();
    std::vector<PicaTextureKey> mReadyReplacements;
    // At a scene change (the pack runtime's SceneSerial): logs the visit that
    // ended. In the scene's first frames (config/texture_pack_hold), waits
    // for the pack images of the world textures drawn native last frame.
    void BeginScene();
    // Follows the runtime's scene `serial`: the visit before is logged and a
    // new one starts, with the hold budget only when the game entered it with
    // the pack on (`entered`; TexturePackSceneHoldMicros).
    void FollowScene(uint64_t serial, bool entered);
    // The change in mPackChange: counted, and logged once it took effect
    // (TrackPackChange, at each frame's start), was superseded or ran out.
    // Its log names the textures drawn last frame (PackOnScreen).
    struct PackOnScreen {
        uint64_t PackImages = 0;
        uint64_t Natives = 0;
        uint64_t Waiting = 0; // natives of the current settings still waiting for their pack image
    };
    void StartPackChange(bool atStart);
    void TrackPackChange();
    [[nodiscard]] PackOnScreen CountPackOnScreen() const;
    void LogPackChange(const char* outcome, const PackOnScreen& onScreen);
    void LogScene(const PicaSceneTextureStats& stats, const char* when);
    // The pack's image sizes (AzaharTexturePackRuntime::ImageScaleStatus),
    // logged once per new result (`resources: texture pack sizes`).
    void LogPackSizes();
    uint64_t mPackSizesSerial = 0;
    std::vector<uint64_t> mHoldHashes;
    uint64_t mSceneSerial = 0;
    uint64_t mHoldLeftMicros = 0;
    PicaSceneTextureStats mScene{};
    // Resolves left this frame after a pack change (TextureHandle).
    ::Oot3d::Renderer::TexturePackRebuildBudget mPackRebuilds{};

    Device* mDevice = nullptr;
    DkMemBlock mDescriptors = nullptr; // image descriptors, then sampler descriptors
    DkImageDescriptor* mImageDescriptors = nullptr;
    DkSamplerDescriptor* mSamplerDescriptors = nullptr;
    DkGpuAddr mImageDescriptorsGpu = DK_GPU_ADDR_INVALID;
    DkGpuAddr mSamplerDescriptorsGpu = DK_GPU_ADDR_INVALID;
    DkMemBlock mFixed = nullptr; // fallback images
    DkImage mFallbackColor{};
    DkImage mFallbackInteger{};
    DkMemBlock mConstant = nullptr; // (0, 0, 0, 1) floats, CPU-visible
    DkGpuAddr mConstantGpu = DK_GPU_ADDR_INVALID;
    DescriptorSlots<kMaxImages> mImageSlots{kReservedImages};
    std::unordered_map<PicaSamplerKey, uint32_t, PicaSamplerKeyHash> mSamplers;
    bool mTexturePack = false;                 // HD textures on (config/texture_pack, Video tab)
    ::Oot3d::Renderer::TexturePackSettings mTexturePackSettings;
    uint64_t mTexturePackRevision = 0;         // LiveTexturePackSettings revision applied
    uint32_t mPackEpoch = 1;                   // grows on every pack change (TexturePackEntryActionFor)
    bool mReleaseStalePack = false;            // pack images of earlier settings may still be cached
    bool mPackConfigured = false;              // the pack runtime was configured (the pack was on once)
    bool mPackIndexReady = false;              // the pack's index was ready at this frame's start
    PicaPackChangeStats mPackChange{};
    bool mMenuImages = true;                   // config/texture_pack_menu (ParseTexturePackMenuSetting)
    std::vector<Texture> mRetired;
    // Replacement keys uploaded under the current pack settings (TextureReplacementReuploads).
    std::unordered_set<PicaTextureKey, PicaTextureKeyHash> mReplacementKeysUploaded;
    uint64_t mReplacementBytesThisFrame = 0;   // pack uploads are spread over frames
    std::unordered_map<PicaTextureKey, Texture, PicaTextureKeyHash> mTextures;
    std::unordered_map<uint64_t, Texture> mLuts;
    std::vector<PicaTextureKey> mTextureEvictions;
    std::vector<uint64_t> mLutEvictions;
    uint64_t mTextureBytes = 0;
    uint64_t mLutBytes = 0;
    uint64_t mFrame = 0;
    uint64_t mBudgetBytes = 0;
    bool mInvalidate = false;
    bool mFallbacksUploaded = false;
    LastUniforms mLastVertexUniforms;
    LastUniforms mLastFragmentUniforms;
    LastUniforms mLastSurfaceLighting;
    PicaResourceStats mStats{};
};

} // namespace Fast::Deko3d
#endif
