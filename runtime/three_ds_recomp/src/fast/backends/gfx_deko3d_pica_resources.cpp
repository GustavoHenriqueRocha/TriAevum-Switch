// deko3d PICA resources (Switch). See gfx_deko3d_pica_resources.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_pica_resources.h"

#include "fast/oot3d/pica_texture_predecode.h"
#include "gfx_opengl_pica_residency.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

namespace Fast::Deko3d {
namespace {
constexpr uint32_t kDescriptorBytes = 32U;
constexpr uint32_t kStagingAlignment = 256U;
constexpr const char* kBudgetPath = "sdmc:/switch/oot3dre/config/gl_texture_budget_mb";

uint64_t HashBytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

DkWrapMode ToDkWrap(PicaWrap wrap) {
    switch (wrap) {
        case PicaWrap::Repeat: return DkWrapMode_Repeat;
        case PicaWrap::MirroredRepeat: return DkWrapMode_MirroredRepeat;
        case PicaWrap::ClampToEdge:
        case PicaWrap::Unsupported: break;
    }
    return DkWrapMode_ClampToEdge;
}

DkMipFilter ToDkMipFilter(PicaMipFilter filter) {
    switch (filter) {
        case PicaMipFilter::Nearest: return DkMipFilter_Nearest;
        case PicaMipFilter::Linear: return DkMipFilter_Linear;
        case PicaMipFilter::None: break;
    }
    return DkMipFilter_None;
}

PicaSamplerKey NearestClampKey() {
    return {};
}
} // namespace

bool PicaResources::Init(Device& device) {
    mDevice = &device;
    const uint32_t descriptorBytes = (kMaxImages + kMaxSamplers) * kDescriptorBytes;
    mDescriptors = device.MakeBlock(descriptorBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                    "descriptors");
    if (mDescriptors == nullptr) {
        return false;
    }
    auto* cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(mDescriptors));
    std::memset(cpu, 0, descriptorBytes);
    mImageDescriptors = reinterpret_cast<DkImageDescriptor*>(cpu);
    mSamplerDescriptors = reinterpret_cast<DkSamplerDescriptor*>(cpu + kMaxImages * kDescriptorBytes);
    mImageDescriptorsGpu = dkMemBlockGetGpuAddr(mDescriptors);
    mSamplerDescriptorsGpu = mImageDescriptorsGpu + kMaxImages * kDescriptorBytes;

    // Fallback images (filled by the first frame) and the constant vertex
    // attribute (0, 0, 0, 1).
    mFixed = device.MakeBlock(64U << 10, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "fallback images");
    if (mFixed == nullptr) {
        return false;
    }
    uint32_t offset = 0U;
    const auto place = [&](DkImage& image, DkImageFormat format) {
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, device.Handle());
        maker.format = format;
        maker.dimensions[0] = 1U;
        maker.dimensions[1] = 1U;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);
        offset = AlignUp(offset, dkImageLayoutGetAlignment(&layout));
        dkImageInitialize(&image, &layout, mFixed, offset);
        offset += static_cast<uint32_t>(dkImageLayoutGetSize(&layout));
    };
    place(mFallbackColor, DkImageFormat_RGBA8_Unorm);
    place(mFallbackInteger, DkImageFormat_R32_Uint);
    // The constant attribute every absent vertex attribute reads (GL: a
    // disabled array reads (0, 0, 0, 1)). CPU-visible: the image block above
    // has no CPU address.
    mConstant = device.MakeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                                 "constant attribute");
    if (mConstant == nullptr) {
        return false;
    }
    const float constant[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    std::memcpy(dkMemBlockGetCpuAddr(mConstant), constant, sizeof(constant));
    mConstantGpu = dkMemBlockGetGpuAddr(mConstant);
    WriteImageDescriptor(kFallbackColorImage, mFallbackColor);
    WriteImageDescriptor(kFallbackIntegerImage, mFallbackInteger);
    if (SamplerSlot(NearestClampKey()) != kNearestSampler) {
        return false;
    }

    std::ifstream budget(kBudgetPath);
    const std::string text((std::istreambuf_iterator<char>(budget)), std::istreambuf_iterator<char>());
    mBudgetBytes = PicaGlResidency::ParseBudgetBytes(text, PicaGlResidency::Policy{}.BudgetBytes);
    Log("resources: %u image / %u sampler descriptors, texture budget %llu MiB", kMaxImages, kMaxSamplers,
        static_cast<unsigned long long>(mBudgetBytes >> 20));
    return true;
}

void PicaResources::Shutdown() noexcept {
    for (auto& [key, texture] : mTextures) {
        Destroy(texture);
    }
    mTextures.clear();
    for (auto& [key, lut] : mLuts) {
        Destroy(lut);
    }
    mLuts.clear();
    mSamplers.clear();
    if (mFixed != nullptr) {
        dkMemBlockDestroy(mFixed);
        mFixed = nullptr;
    }
    if (mConstant != nullptr) {
        dkMemBlockDestroy(mConstant);
        mConstant = nullptr;
        mConstantGpu = DK_GPU_ADDR_INVALID;
    }
    if (mDescriptors != nullptr) {
        dkMemBlockDestroy(mDescriptors);
        mDescriptors = nullptr;
    }
    mDevice = nullptr;
}

void PicaResources::WriteImageDescriptor(uint32_t slot, const DkImage& image) {
    DkImageView view;
    dkImageViewDefaults(&view, &image);
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &view, false, false);
    std::memcpy(&mImageDescriptors[slot], &descriptor, sizeof(descriptor));
    mInvalidate = true;
}

uint32_t PicaResources::SamplerSlot(const PicaSamplerKey& key) {
    if (const auto found = mSamplers.find(key); found != mSamplers.end()) {
        return found->second;
    }
    const auto slot = static_cast<uint32_t>(mSamplers.size());
    if (slot >= kMaxSamplers) {
        if (mStats.TextureFailures++ < 10U) {
            Log("resources: sampler descriptors full (%u)", kMaxSamplers);
        }
        return kNearestSampler;
    }
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.minFilter = key.MinLinear ? DkFilter_Linear : DkFilter_Nearest;
    sampler.magFilter = key.MagLinear ? DkFilter_Linear : DkFilter_Nearest;
    sampler.mipFilter = ToDkMipFilter(key.Mip);
    sampler.wrapMode[0] = ToDkWrap(key.WrapS);
    sampler.wrapMode[1] = ToDkWrap(key.WrapT);
    sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
    // The image view starts at MinLevel (GL_TEXTURE_BASE_LEVEL), so the LOD
    // range is relative to it, as in GL.
    sampler.lodClampMin = 0.0F;
    sampler.lodClampMax = static_cast<float>(key.MaxLevel - key.MinLevel);
    sampler.lodBias = static_cast<float>(key.LodBiasRaw) / 256.0F;
    DkSamplerDescriptor descriptor;
    dkSamplerDescriptorInitialize(&descriptor, &sampler);
    std::memcpy(&mSamplerDescriptors[slot], &descriptor, sizeof(descriptor));
    mSamplers.emplace(key, slot);
    mStats.Samplers = mSamplers.size();
    mInvalidate = true;
    return slot;
}

bool PicaResources::CreateImage(Texture& texture, DkImageFormat format, uint32_t width, uint32_t height,
                                uint32_t levels) {
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, mDevice->Handle());
    maker.format = format;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    maker.mipLevels = levels;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint64_t size = dkImageLayoutGetSize(&layout);
    if (size > 0xFFFFFFFFULL - DK_MEMBLOCK_ALIGNMENT) {
        return false;
    }
    texture.Memory = mDevice->MakeBlock(static_cast<uint32_t>(size), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                                        "texture");
    if (texture.Memory == nullptr) {
        return false;
    }
    dkImageInitialize(&texture.Image, &layout, texture.Memory, 0U);
    texture.ImageSlot = mImageSlots.Allocate();
    if (texture.ImageSlot >= kMaxImages) {
        dkMemBlockDestroy(texture.Memory);
        texture.Memory = nullptr;
        return false;
    }
    texture.Bytes = AlignUp(static_cast<uint32_t>(size), DK_MEMBLOCK_ALIGNMENT);
    return true;
}

void PicaResources::UploadLevel(const Texture& texture, uint32_t level, uint32_t width, uint32_t height,
                                DkGpuAddr source) {
    DkImageView view;
    dkImageViewDefaults(&view, &texture.Image);
    view.mipLevelOffset = static_cast<uint8_t>(level);
    view.mipLevelCount = 1U;
    const DkCopyBuf copy = {source, 0U, 0U};
    const DkImageRect rect = {0U, 0U, 0U, width, height, 1U};
    dkCmdBufCopyBufferToImage(mDevice->Cmd(), &copy, &view, &rect, 0U);
    mDevice->Checkpoint("texture", (static_cast<uint64_t>(width) << 16U) | height, level);
}

void PicaResources::Destroy(Texture& texture) noexcept {
    if (texture.Memory != nullptr) {
        dkMemBlockDestroy(texture.Memory);
        texture.Memory = nullptr;
    }
    if (texture.ImageSlot >= kReservedImages && texture.ImageSlot < kMaxImages) {
        mImageSlots.Free(texture.ImageSlot);
    }
    texture.ImageSlot = 0U;
}

void PicaResources::BeginFrame(uint64_t frame) {
    mFrame = frame;
    if (!mFallbacksUploaded) {
        // Fallback contents: white, and an empty Shadow2D texel.
        const StreamSpan staging = mDevice->Stream(2U * kStagingAlignment, kStagingAlignment);
        if (staging.Cpu != nullptr) {
            const uint32_t white = 0xFFFFFFFFU;
            std::memcpy(staging.Cpu, &white, sizeof(white));
            std::memcpy(static_cast<uint8_t*>(staging.Cpu) + kStagingAlignment, &white, sizeof(white));
            Texture color;
            color.Image = mFallbackColor;
            UploadLevel(color, 0U, 1U, 1U, staging.Gpu);
            Texture integer;
            integer.Image = mFallbackInteger;
            UploadLevel(integer, 0U, 1U, 1U, staging.Gpu + kStagingAlignment);
            mFallbacksUploaded = true;
            mInvalidate = true;
        }
    }
    DkCmdBuf cmd = mDevice->Cmd();
    dkCmdBufBindImageDescriptorSet(cmd, mImageDescriptorsGpu, kMaxImages);
    dkCmdBufBindSamplerDescriptorSet(cmd, mSamplerDescriptorsGpu, kMaxSamplers);
    mInvalidate = true;
    Evict();
}

void PicaResources::Evict() {
    const PicaGlResidency::Policy policy{mBudgetBytes};
    PicaGlResidency::CollectEvictions(mTextures, mFrame, mTextureBytes, policy, mTextureEvictions);
    for (const auto& key : mTextureEvictions) {
        const auto found = mTextures.find(key);
        if (found == mTextures.end()) {
            continue;
        }
        mTextureBytes -= std::min(mTextureBytes, found->second.Bytes);
        Destroy(found->second);
        mTextures.erase(found);
        ++mStats.TexturesEvicted;
    }
    PicaGlResidency::Policy lutPolicy;
    lutPolicy.BudgetBytes = 8ULL << 20;
    PicaGlResidency::CollectEvictions(mLuts, mFrame, mLutBytes, lutPolicy, mLutEvictions);
    for (const uint64_t key : mLutEvictions) {
        const auto found = mLuts.find(key);
        if (found == mLuts.end()) {
            continue;
        }
        mLutBytes -= std::min(mLutBytes, found->second.Bytes);
        Destroy(found->second);
        mLuts.erase(found);
    }
    mStats.TextureBytes = mTextureBytes;
}

DkResHandle PicaResources::TextureHandle(const Renderer3ds::PicaTextureView& view) {
    const DkResHandle fallback = dkMakeTextureHandle(kFallbackColorImage, kNearestSampler);
    const uint64_t contentHash = view.NativeContentHashAvailable ? view.NativeContentHash : HashBytes(view.NativeBytes);
    const PicaTextureKey key = MakePicaTextureKey(view, contentHash);
    if (const auto found = mTextures.find(key); found != mTextures.end()) {
        found->second.LastUsedFrame = mFrame;
        return dkMakeTextureHandle(found->second.ImageSlot, found->second.SamplerSlot);
    }
    const uint32_t levels = Oot3d::PicaTextureMipCount(view.Width, view.Height, view.MaxMipLevel);
    std::array<PicaMipPlan, kMaxPicaMipLevels> mips{};
    const uint32_t stagingBytes = PlanPicaRgba8Mips(view.Width, view.Height, levels, kStagingAlignment, mips);
    // A texture that cannot decode is remembered as the fallback (no new
    // decode attempt every draw); it leaves with the usual idle eviction.
    const auto rememberFallback = [&] {
        Texture failed;
        failed.ImageSlot = kFallbackColorImage;
        failed.SamplerSlot = kNearestSampler;
        failed.LastUsedFrame = mFrame;
        mTextures.emplace(key, failed);
        return fallback;
    };
    if (stagingBytes == 0U) {
        ++mStats.TextureFailures;
        return rememberFallback();
    }
    auto decoded = Oot3d::SharedPicaPredecodeCache().Take(
        {contentHash, view.Width, view.Height, view.NativeFormat, static_cast<uint8_t>(levels)});
    if (decoded != nullptr) {
        ++mStats.TexturesPredecoded;
    } else {
        decoded = Oot3d::DecodePicaTextureMips(view.NativeFormat, view.Width, view.Height, view.MaxMipLevel,
                                               view.NativeBytes);
    }
    if (decoded == nullptr || decoded->Mips.size() != levels) {
        if (mStats.TextureFailures++ < 10U) {
            Log("resources: texture %ux%u format %u did not decode", view.Width, view.Height, view.NativeFormat);
        }
        return rememberFallback();
    }
    const StreamSpan staging = mDevice->Stream(stagingBytes, kStagingAlignment);
    if (staging.Cpu == nullptr) {
        ++mStats.StreamFailures;
        return fallback;
    }
    Texture texture;
    if (!CreateImage(texture, DkImageFormat_RGBA8_Unorm, view.Width, view.Height, levels)) {
        if (mStats.TextureFailures++ < 10U) {
            Log("resources: no memory/slot for a %ux%u texture (%llu MiB live)", view.Width, view.Height,
                static_cast<unsigned long long>(mTextureBytes >> 20));
        }
        return fallback;
    }
    for (uint32_t level = 0U; level < levels; ++level) {
        const auto& mip = mips[level];
        const auto& bytes = decoded->Mips[level];
        std::memcpy(static_cast<uint8_t*>(staging.Cpu) + mip.Offset, bytes.data(),
                    std::min<size_t>(bytes.size(), mip.Bytes));
        UploadLevel(texture, level, mip.Width, mip.Height, staging.Gpu + mip.Offset);
    }
    PicaSamplerKey samplerKey = MakePicaSamplerKey(view, levels);
    if (samplerKey.WrapS == PicaWrap::Unsupported || samplerKey.WrapT == PicaWrap::Unsupported) {
        ++mStats.UnsupportedWraps;
    }
    texture.SamplerSlot = SamplerSlot(samplerKey);
    // The view starts at GL_TEXTURE_BASE_LEVEL and ends at GL_TEXTURE_MAX_LEVEL.
    DkImageView imageView;
    dkImageViewDefaults(&imageView, &texture.Image);
    imageView.mipLevelOffset = samplerKey.MinLevel;
    imageView.mipLevelCount = static_cast<uint8_t>(samplerKey.MaxLevel - samplerKey.MinLevel + 1U);
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &imageView, false, false);
    std::memcpy(&mImageDescriptors[texture.ImageSlot], &descriptor, sizeof(descriptor));
    mInvalidate = true;
    texture.LastUsedFrame = mFrame;
    mTextureBytes += texture.Bytes;
    mStats.TextureBytes = mTextureBytes;
    mStats.TextureUploadBytes += stagingBytes;
    ++mStats.TexturesCreated;
    const auto inserted = mTextures.emplace(key, texture).first;
    return dkMakeTextureHandle(inserted->second.ImageSlot, inserted->second.SamplerSlot);
}

DkResHandle PicaResources::LightingLutHandle(const Renderer3ds::PicaLightingLutView& lut) {
    const DkResHandle fallback = dkMakeTextureHandle(kFallbackIntegerImage, kNearestSampler);
    if (!lut.Valid()) {
        return fallback;
    }
    if (const auto found = mLuts.find(lut.ContentHash); found != mLuts.end()) {
        found->second.LastUsedFrame = mFrame;
        return dkMakeTextureHandle(found->second.ImageSlot, kNearestSampler);
    }
    const uint32_t bytes = static_cast<uint32_t>(lut.PackedEntries.size_bytes());
    const StreamSpan staging = mDevice->Stream(bytes, kStagingAlignment);
    if (staging.Cpu == nullptr) {
        ++mStats.StreamFailures;
        return fallback;
    }
    Texture texture;
    if (!CreateImage(texture, DkImageFormat_R32_Uint, 256U, 24U, 1U)) {
        ++mStats.TextureFailures;
        return fallback;
    }
    std::memcpy(staging.Cpu, lut.PackedEntries.data(), bytes);
    UploadLevel(texture, 0U, 256U, 24U, staging.Gpu);
    WriteImageDescriptor(texture.ImageSlot, texture.Image);
    texture.SamplerSlot = kNearestSampler;
    texture.LastUsedFrame = mFrame;
    mLutBytes += texture.Bytes;
    ++mStats.LutsCreated;
    const auto inserted = mLuts.emplace(lut.ContentHash, texture).first;
    return dkMakeTextureHandle(inserted->second.ImageSlot, kNearestSampler);
}

bool PicaResources::Prepare(const GfxNativePicaDrawView& draw, PicaDrawResources& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };
    if (!PlanPicaVertices(draw.VertexBindings, draw.VertexAttributes, out.Vertices)) {
        return fail(out.Vertices.Error);
    }
    // Textures first: an unsupported one rejects the draw before any upload.
    out.Textures.fill(dkMakeTextureHandle(kFallbackColorImage, kNearestSampler));
    for (const auto& texture : draw.Textures) {
        if (texture.Slot >= out.Textures.size()) {
            return fail("PICA texture slot is invalid");
        }
        switch (ClassifyPicaTexture(texture)) {
            case PicaTextureUse::Unsupported: return fail("PICA texture type is unsupported");
            case PicaTextureUse::Shadow2D:
                ++mStats.ShadowFallbacks;
                out.Textures[texture.Slot] = dkMakeTextureHandle(kFallbackIntegerImage, kNearestSampler);
                break;
            case PicaTextureUse::Color: out.Textures[texture.Slot] = TextureHandle(texture); break;
        }
    }
    out.LightingLut = draw.LightingLut.PackedEntries.empty()
                          ? dkMakeTextureHandle(kFallbackIntegerImage, kNearestSampler)
                          : LightingLutHandle(draw.LightingLut);

    // Vertex streams: all bindings in one block of this frame.
    if (out.Vertices.StreamBytes != 0U) {
        const StreamSpan stream = mDevice->Stream(out.Vertices.StreamBytes, kPicaVertexStreamAlignment);
        if (stream.Cpu == nullptr) {
            ++mStats.StreamFailures;
            return fail("deko3d vertex stream memory");
        }
        for (const auto& binding : draw.VertexBindings) {
            std::memcpy(static_cast<uint8_t*>(stream.Cpu) + out.Vertices.BindingOffset[binding.Binding],
                        binding.Bytes.data(), binding.Bytes.size());
        }
        out.VertexStream = stream.Gpu;
        mStats.VertexBytes += out.Vertices.StreamBytes;
    }
    if (draw.Indexed && !draw.IndexBytes.empty()) {
        const StreamSpan stream = mDevice->Stream(static_cast<uint32_t>(draw.IndexBytes.size()), 4U);
        if (stream.Cpu == nullptr) {
            ++mStats.StreamFailures;
            return fail("deko3d index stream memory");
        }
        std::memcpy(stream.Cpu, draw.IndexBytes.data(), draw.IndexBytes.size());
        out.IndexStream = stream.Gpu;
        out.IndexBytes = static_cast<uint32_t>(draw.IndexBytes.size());
        mStats.IndexBytes += out.IndexBytes;
    }

    // Uniform blocks; a block equal to the stage's previous one this frame is
    // bound again instead of copied (draws of one object share matrices).
    const auto uniforms = [&](std::span<const uint8_t> bytes, LastUniforms& last, DkGpuAddr& address,
                              uint32_t& size) {
        if (bytes.empty()) {
            return true;
        }
        size = PicaUniformBindBytes(bytes.size());
        if (last.Frame == mFrame && last.Address != DK_GPU_ADDR_INVALID && last.Bytes.size() == bytes.size() &&
            std::memcmp(last.Bytes.data(), bytes.data(), bytes.size()) == 0) {
            address = last.Address;
            ++mStats.UniformReuses;
            return true;
        }
        const StreamSpan stream = mDevice->Stream(size, kDekoUniformAlignment);
        if (stream.Cpu == nullptr) {
            ++mStats.StreamFailures;
            return false;
        }
        const size_t copy = std::min<size_t>(bytes.size(), size);
        std::memcpy(stream.Cpu, bytes.data(), copy);
        address = stream.Gpu;
        last.Frame = mFrame;
        last.Address = stream.Gpu;
        last.Bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(copy));
        mStats.UniformBytes += size;
        return true;
    };
    if (!uniforms(draw.VertexUniformBytes, mLastVertexUniforms, out.VertexUniforms, out.VertexUniformBytes) ||
        !uniforms(draw.FragmentUniformBytes, mLastFragmentUniforms, out.FragmentUniforms,
                  out.FragmentUniformBytes)) {
        return fail("deko3d uniform stream memory");
    }
    return true;
}

} // namespace Fast::Deko3d
#endif
