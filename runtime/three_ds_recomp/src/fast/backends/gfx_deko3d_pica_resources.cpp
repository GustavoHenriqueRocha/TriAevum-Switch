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
// config/texture_pack and texture_pack_scale (azahar_texture_pack_settings.h).
constexpr const char* kConfigDirectory = "sdmc:/switch/oot3dre/config";
// Pack images are big (4x a native one at the 2x cap); more than this per frame waits for the next frames.
constexpr uint64_t kReplacementBytesPerFrame = 16ULL << 20;
// The replacement lives under the native key with this mixed into the content hash.
constexpr uint64_t kReplacementKeySalt = 0x5245504C41434531ULL; // "REPLACE1"

// Bytes of the base level in the native PICA encoding (the pack hashes only these).
size_t PicaBaseLevelBytes(uint8_t format, uint32_t width, uint32_t height) {
    static constexpr uint8_t kBits[] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8};
    return format < std::size(kBits) ? static_cast<size_t>(width) * height * kBits[format] / 8U : 0U;
}

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
    mTexturePackSettings = ::Oot3d::Renderer::TexturePackSettings::Load(kConfigDirectory);
    mTexturePackRevision = ::Oot3d::Renderer::LiveTexturePackSettings().Revision.load(std::memory_order_acquire);
    mTexturePack = mTexturePackSettings.Enabled;
    if (mTexturePack) {
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().Configure(
            ::Oot3d::Renderer::MakeTexturePackConfiguration(mTexturePackSettings, kConfigDirectory));
        mPackConfigured = true;
    }
    std::ifstream menuSetting(std::string(kConfigDirectory) + "/texture_pack_menu");
    std::string menuLine;
    const bool menuSettingExists = static_cast<bool>(menuSetting);
    if (menuSettingExists) {
        std::getline(menuSetting, menuLine);
    }
    mMenuImages = ParseTexturePackMenuSetting(menuLine, menuSettingExists);
    Log("resources: texture pack %s (%s, cap %ux), menu textures as HUD images %s (config/texture_pack_menu)",
        mTexturePack ? "on" : "off", mTexturePackSettings.Folder.c_str(), mTexturePackSettings.MaxScale,
        mMenuImages ? "on" : "off");
    Log("resources: %u image / %u sampler descriptors, texture budget %llu MiB", kMaxImages, kMaxSamplers,
        static_cast<unsigned long long>(mBudgetBytes >> 20));
    return true;
}

void PicaResources::Shutdown() noexcept {
    for (auto& [key, texture] : mTextures) {
        Destroy(texture);
    }
    mTextures.clear();
    for (auto& texture : mRetired) {
        Destroy(texture);
    }
    mRetired.clear();
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
    // lodBias stays 0 (dkSamplerDefaults): the fragment shaders add the PICA
    // LOD bias to their explicit LOD (textureLod(..., lod + texture_lod_bias)),
    // and the TSC bias adds to an explicit LOD too, so a bias here doubled it
    // (-3.4 for the Forest Temple's -1.7: floors shimmered). gfx_deko3d_pica_plan.h.
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

void PicaResources::Retire(Texture& texture) {
    mRetired.push_back(texture);
    texture = Texture{}; // the caller erases the entry; nothing is destroyed twice
}

void PicaResources::Forget(Texture& texture) noexcept {
    mTextureBytes -= std::min(mTextureBytes, texture.Bytes);
    if (texture.Replacement) {
        mStats.TextureReplacementLiveBytes -= std::min(mStats.TextureReplacementLiveBytes, texture.Bytes);
    }
    Destroy(texture);
    ++mStats.TexturesEvicted;
}

void PicaResources::ApplyTexturePackSettings() {
    auto& live = ::Oot3d::Renderer::LiveTexturePackSettings();
    const uint64_t revision = live.Revision.load(std::memory_order_acquire);
    if (revision == mTexturePackRevision) {
        return;
    }
    mTexturePackRevision = revision;
    auto settings = mTexturePackSettings;
    settings.Enabled = live.Enabled.load(std::memory_order_relaxed);
    settings.MaxScale = live.MaxScale.load(std::memory_order_relaxed);
    if (settings == mTexturePackSettings) {
        return;
    }
    mTexturePackSettings = settings;
    mTexturePack = settings.Enabled;
    // Every cached entry is now of an earlier epoch: pack images are released
    // (Evict) and, with the pack on, native textures resolve again (TextureHandle).
    ++mPackEpoch;
    mReleaseStalePack = true;
    mReplacementKeysUploaded.clear(); // every pack image uploads again by design
    if (settings.Enabled || mPackConfigured) {
        // Off: the runtime drops its decoded images and pending decodes.
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().Configure(
            ::Oot3d::Renderer::MakeTexturePackConfiguration(settings, kConfigDirectory));
        mPackConfigured = true;
    }
    Log("resources: texture pack %s (cap %ux)", mTexturePack ? "on" : "off", settings.MaxScale);
}

void PicaResources::BeginFrame(uint64_t frame) {
    mFrame = frame;
    mReplacementBytesThisFrame = 0U;
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
    // No barrier forced here: the frame's commands follow a dkQueueFlush,
    // whose postSubmitFlush (deko3d maxwell/gpu_base.cpp) already invalidates
    // the image, shader (code, global data, uniforms), descriptor and L2
    // caches; GPU-written images (display copy, target copies) have their
    // own Full barriers. config/deko3d_barrier "frame" brings it back.
    ApplyTexturePackSettings();
    Evict();
    UploadReadyReplacements();
}

void PicaResources::UploadReadyReplacements() {
    if (!mTexturePack) {
        return;
    }
    // Keys first: UploadReplacement inserts into mTextures (rehash).
    mReadyReplacements.clear();
    for (const auto& [key, texture] : mTextures) {
        // A native of another pack epoch is rebuilt at its next draw instead.
        if (texture.PackPending && !texture.Replacement && texture.PackEpoch == mPackEpoch &&
            texture.LastUsedFrame + 1U >= mFrame) {
            mReadyReplacements.push_back(key);
        }
    }
    auto& pack = ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance();
    for (const PicaTextureKey& key : mReadyReplacements) {
        const auto found = mTextures.find(key);
        if (found == mTextures.end()) {
            continue;
        }
        Texture& native = found->second;
        const auto polled = pack.PollOrQueue(native.PackHash, native.PackView.Width, native.PackView.Height,
                                             native.PackMenu, native.PackMenu);
        if (polled.State == ::Oot3d::Renderer::AzaharTextureResolveState::Ready && polled.Replacement != nullptr) {
            PicaTextureKey replacementKey = key;
            replacementKey.ContentHash ^= kReplacementKeySalt;
            const Renderer3ds::PicaTextureView view = native.PackView; // `native` may move on insert
            if (UploadReplacement(view, replacementKey, *polled.Replacement, native.PackMenu) != 0U) {
                // The native texture is never drawn again.
                Retire(mTextures.find(key)->second);
                mTextures.erase(key);
                ++mStats.TextureReplacementsBatched;
            }
            // 0: over this frame's upload cap (or no memory): stays pending.
        } else if (polled.State != ::Oot3d::Renderer::AzaharTextureResolveState::Pending) {
            native.PackPending = false;
        }
    }
}

void PicaResources::Evict() {
    const PicaGlResidency::Policy policy{mBudgetBytes};
    const auto idle = [&](const Texture& texture) {
        return mFrame > texture.LastUsedFrame ? mFrame - texture.LastUsedFrame : 0U;
    };
    std::erase_if(mRetired, [&](Texture& texture) {
        if (idle(texture) < policy.MinIdleFrames) {
            return false;
        }
        Forget(texture);
        return true;
    });
    if (mReleaseStalePack) {
        bool stale = false;
        for (auto entry = mTextures.begin(); entry != mTextures.end();) {
            Texture& texture = entry->second;
            if (::Oot3d::Renderer::TexturePackEntryActionFor(texture.Replacement, texture.PackEpoch, mPackEpoch,
                                                             mTexturePack) ==
                ::Oot3d::Renderer::TexturePackEntryAction::Release) {
                if (idle(texture) >= policy.MinIdleFrames) {
                    Forget(texture);
                    entry = mTextures.erase(entry);
                    continue;
                }
                stale = true;
            }
            ++entry;
        }
        mReleaseStalePack = stale;
    }
    PicaGlResidency::CollectEvictions(mTextures, mFrame, mTextureBytes, policy, mTextureEvictions);
    for (const auto& key : mTextureEvictions) {
        const auto found = mTextures.find(key);
        if (found == mTextures.end()) {
            continue;
        }
        if (idle(found->second) <= policy.MaxIdleFrames) {
            ++mStats.TexturesEvictedForBudget; // not idle for MaxIdleFrames: the budget took it
        }
        Forget(found->second);
        mTextures.erase(found);
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

DkResHandle PicaResources::TextureHandle(const Renderer3ds::PicaTextureView& view, bool uiDraw) {
    const DkResHandle fallback = dkMakeTextureHandle(kFallbackColorImage, kNearestSampler);
    const uint64_t contentHash = view.NativeContentHashAvailable ? view.NativeContentHash : HashBytes(view.NativeBytes);
    const PicaTextureKey key = MakePicaTextureKey(view, contentHash);
    PicaTextureKey replacementKey = key;
    replacementKey.ContentHash ^= kReplacementKeySalt;
    if (mTexturePack) {
        if (const auto found = mTextures.find(replacementKey); found != mTextures.end()) {
            if (found->second.PackEpoch == mPackEpoch) {
                found->second.LastUsedFrame = mFrame;
                return dkMakeTextureHandle(found->second.ImageSlot, found->second.SamplerSlot);
            }
            Retire(found->second); // made at an earlier size cap
            mTextures.erase(found);
        }
    }
    if (const auto found = mTextures.find(key); found != mTextures.end()) {
        Texture& native = found->second;
        native.LastUsedFrame = mFrame;
        if (::Oot3d::Renderer::TexturePackEntryActionFor(false, native.PackEpoch, mPackEpoch, mTexturePack) ==
            ::Oot3d::Renderer::TexturePackEntryAction::Rebuild) {
            // Made while the pack was off or at another cap: resolve it again below.
            Retire(native);
            mTextures.erase(found);
        } else {
            if (!mTexturePack) {
                native.PackPending = false;
            }
            // Still decoding when it was made: UploadReadyReplacements polls
            // it at the next frame top, with the other finished pack images,
            // so a stream of them shares one barrier instead of one per draw.
            return dkMakeTextureHandle(native.ImageSlot, native.SamplerSlot);
        }
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
        failed.PackEpoch = mPackEpoch;
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
    ::Oot3d::Renderer::AzaharTextureResolveResult pack;
    bool menu = false;
    if (mTexturePack && ClassifyPicaTexture(view) == PicaTextureUse::Color) {
        const size_t baseBytes = PicaBaseLevelBytes(view.NativeFormat, view.Width, view.Height);
        if (baseBytes != 0U && baseBytes <= view.NativeBytes.size()) {
            menu = PicaTextureIsMenuArt(view, uiDraw, mMenuImages);
            pack = ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().ResolveOrQueue({
                .Width = static_cast<uint16_t>(view.Width),
                .Height = static_cast<uint16_t>(view.Height),
                .NativeFormat = view.NativeFormat,
                .MipLevel = 0U,
                .NativeBytes = view.NativeBytes.first(baseBytes),
                .NativeRgba8 = std::span<const uint8_t>(decoded->Mips[0]),
                .UserInterface = menu,
                .Menu = menu,
            });
            if (pack.State == ::Oot3d::Renderer::AzaharTextureResolveState::Ready && pack.Replacement != nullptr) {
                if (const DkResHandle handle = UploadReplacement(view, replacementKey, *pack.Replacement, menu)) {
                    mStats.MenuTexturesReady += menu ? 1U : 0U;
                    return handle;
                }
            }
        }
    }
    const StreamSpan staging = mDevice->Stream(stagingBytes, kStagingAlignment);
    if (staging.Cpu == nullptr) {
        ++mStats.StreamFailures;
        return fallback;
    }
    Texture texture;
    texture.PackHash = pack.NativeHash;
    texture.PackPending = pack.State == ::Oot3d::Renderer::AzaharTextureResolveState::Pending ||
                          pack.State == ::Oot3d::Renderer::AzaharTextureResolveState::Ready;
    texture.PackMenu = menu && texture.PackPending;
    mStats.MenuTexturesNativeFirst += texture.PackMenu ? 1U : 0U;
    if (texture.PackPending) {
        texture.PackView = view;
        texture.PackView.NativeBytes = {};
        texture.PackView.CopyReferences = {};
    }
    texture.PackEpoch = mPackEpoch;
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

DkResHandle PicaResources::UploadReplacement(const Renderer3ds::PicaTextureView& view, const PicaTextureKey& key,
                                             const ::Oot3d::Renderer::AzaharTextureReplacement& replacement,
                                             bool menu) {
    const auto& rgba = replacement.Rgba8;
    const uint64_t baseBytes = static_cast<uint64_t>(replacement.Width) * replacement.Height * 4U;
    if (rgba == nullptr || rgba->size() < baseBytes || baseBytes > UINT32_MAX) {
        return 0U;
    }
    // Mip levels only where the game samples mips: the native chain plus the
    // levels the larger image adds on top (`extra`: 1 at 2x, 2 at 4x).
    uint32_t levels = 1U;
    uint32_t extra = 0U;
    const uint32_t nativeLevels = Oot3d::PicaTextureMipCount(view.Width, view.Height, view.MaxMipLevel);
    if (nativeLevels > 1U && !replacement.Mips.empty()) {
        while ((static_cast<uint64_t>(view.Width) << (extra + 1U)) <= replacement.Width &&
               (static_cast<uint64_t>(view.Height) << (extra + 1U)) <= replacement.Height) {
            ++extra;
        }
        levels = std::min<uint32_t>({static_cast<uint32_t>(replacement.Mips.size()) + 1U, nativeLevels + extra,
                                     kMaxPicaMipLevels});
    }
    std::array<PicaMipPlan, kMaxPicaMipLevels> mips{};
    uint32_t stagingBytes = PlanPicaRgba8Mips(replacement.Width, replacement.Height, levels, kStagingAlignment, mips);
    for (uint32_t level = 1U; level < levels; ++level) {
        if (replacement.Mips[level - 1U].size() != mips[level].Bytes) {
            levels = level; // not the chain the plan expects: stop above it
            stagingBytes = mips[level].Offset;
            break;
        }
    }
    if (stagingBytes == 0U) {
        return 0U;
    }
    if (mReplacementBytesThisFrame != 0U && mReplacementBytesThisFrame + stagingBytes > kReplacementBytesPerFrame) {
        return 0U; // the native texture is drawn this frame; the next frames upload it
    }
    const StreamSpan staging = mDevice->Stream(stagingBytes, kStagingAlignment);
    if (staging.Cpu == nullptr) {
        ++mStats.StreamFailures;
        return 0U;
    }
    Texture texture;
    if (!CreateImage(texture, DkImageFormat_RGBA8_Unorm, replacement.Width, replacement.Height, levels)) {
        return 0U;
    }
    for (uint32_t level = 0U; level < levels; ++level) {
        const auto& mip = mips[level];
        std::memcpy(static_cast<uint8_t*>(staging.Cpu) + mip.Offset,
                    level == 0U ? rgba->data() : replacement.Mips[level - 1U].data(), mip.Bytes);
        UploadLevel(texture, level, mip.Width, mip.Height, staging.Gpu + mip.Offset);
    }
    // The native sampler, its LOD range moved down by the levels the larger image adds.
    PicaSamplerKey samplerKey = MakePicaSamplerKey(view, levels);
    if (levels > 1U) {
        samplerKey.MaxLevel = static_cast<uint8_t>(std::min<uint32_t>(view.MaxMipLevel + extra, levels - 1U));
        samplerKey.MinLevel = static_cast<uint8_t>(
            view.MinMipLevel == 0U ? 0U : std::min<uint32_t>(view.MinMipLevel + extra, samplerKey.MaxLevel));
    }
    texture.SamplerSlot = SamplerSlot(samplerKey);
    DkImageView imageView;
    dkImageViewDefaults(&imageView, &texture.Image);
    imageView.mipLevelOffset = samplerKey.MinLevel;
    imageView.mipLevelCount = static_cast<uint8_t>(samplerKey.MaxLevel - samplerKey.MinLevel + 1U);
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &imageView, false, false);
    std::memcpy(&mImageDescriptors[texture.ImageSlot], &descriptor, sizeof(descriptor));
    mInvalidate = true;
    texture.LastUsedFrame = mFrame;
    texture.Replacement = true;
    texture.PackEpoch = mPackEpoch;
    mTextureBytes += texture.Bytes;
    mReplacementBytesThisFrame += stagingBytes;
    mStats.TextureBytes = mTextureBytes;
    mStats.TextureUploadBytes += stagingBytes;
    mStats.TextureReplacementBytes += stagingBytes;
    mStats.TextureReplacementLiveBytes += texture.Bytes;
    ++mStats.TextureReplacements;
    if (!mReplacementKeysUploaded.insert(key).second) {
        // Uploaded before under these settings and evicted since: the surface
        // showed its native texture again for the frames until this upload.
        ++mStats.TextureReplacementReuploads;
    }
    const auto inserted = mTextures.emplace(key, texture).first;
    // The GPU copy is the one drawn now; the worker decodes the file again if it is ever evicted. A menu
    // texture's image is a HUD image: the pack runtime keeps it (another guest address finds it at once).
    if (!menu) {
        ::Oot3d::Renderer::AzaharTexturePackRuntime::Instance().ReleaseDecoded(replacement.NativeHash,
                                                                              replacement.Generation);
    }
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
    // A draw of the game's 2D menus (`Ui` domain or layer): PicaTextureIsMenuArt.
    const bool uiDraw = draw.CompositionDomain == Renderer3ds::PicaCompositionDomain::Ui ||
                        draw.Composition.Layer == Renderer3ds::PicaCompositionLayer::Ui;
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
            case PicaTextureUse::Color: out.Textures[texture.Slot] = TextureHandle(texture, uiDraw); break;
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
    out.SurfaceLighting = DK_GPU_ADDR_INVALID;
    out.SurfaceLightingBytes = 0;
    if (!StreamUniforms(draw.VertexUniformBytes, mLastVertexUniforms, out.VertexUniforms, out.VertexUniformBytes) ||
        !StreamUniforms(draw.FragmentUniformBytes, mLastFragmentUniforms, out.FragmentUniforms,
                        out.FragmentUniformBytes)) {
        return fail("deko3d uniform stream memory");
    }
    return true;
}

bool PicaResources::StreamUniforms(std::span<const uint8_t> bytes, LastUniforms& last, DkGpuAddr& address,
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
}

bool PicaResources::PrepareSurfaceLighting(std::span<const uint8_t> bytes, PicaDrawResources& out) {
    return StreamUniforms(bytes, mLastSurfaceLighting, out.SurfaceLighting, out.SurfaceLightingBytes);
}

} // namespace Fast::Deko3d
#endif
