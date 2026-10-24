#pragma once
// deko3d PICA resources (Switch), package 4 (docs/DEKO3D_PROGRESSO.md):
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

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace Fast::Deko3d {

struct PicaResourceStats {
    uint64_t TexturesCreated = 0;
    uint64_t TexturesEvicted = 0;
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
};

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

    // True once after new descriptors or images were written: the caller
    // issues dkCmdBufBarrier(..., DkInvalidateFlags_Descriptors |
    // DkInvalidateFlags_Image) before the next draw.
    bool TakeInvalidate() noexcept {
        const bool pending = mInvalidate;
        mInvalidate = false;
        return pending;
    }

    [[nodiscard]] const PicaResourceStats& Stats() const noexcept { return mStats; }
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
    };

    struct LastUniforms {
        uint64_t Frame = ~0ULL;
        DkGpuAddr Address = DK_GPU_ADDR_INVALID;
        std::vector<uint8_t> Bytes;
    };

    DkResHandle TextureHandle(const Renderer3ds::PicaTextureView& texture);
    DkResHandle LightingLutHandle(const Renderer3ds::PicaLightingLutView& lut);
    // A sampled 2D image with its own memory block, its descriptor in `slot`.
    bool CreateImage(Texture& texture, DkImageFormat format, uint32_t width, uint32_t height, uint32_t levels);
    void UploadLevel(const Texture& texture, uint32_t level, uint32_t width, uint32_t height, DkGpuAddr source);
    void Destroy(Texture& texture) noexcept;
    void Evict();

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
    PicaResourceStats mStats{};
};

} // namespace Fast::Deko3d
#endif
