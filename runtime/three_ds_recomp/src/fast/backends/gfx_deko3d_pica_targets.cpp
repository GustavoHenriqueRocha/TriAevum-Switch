// deko3d PICA render targets and display images (Switch). See
// gfx_deko3d_pica_targets.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_pica_targets.h"

#include "fast/backends/gl_pica_dynamic_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>

namespace Fast::Deko3d {
namespace {

struct ImagePlacement {
    DkImageLayout Layout{};
    uint32_t Offset = 0;
};

DkImageLayout MakeLayout(DkDevice device, DkImageFormat format, uint32_t flags, uint32_t width, uint32_t height) {
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = flags;
    maker.format = format;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    return layout;
}

// Lays images out one after the other in a block; false when it overflows.
bool Place(std::initializer_list<ImagePlacement*> images, uint64_t& total) {
    uint64_t offset = 0U;
    for (ImagePlacement* image : images) {
        const uint32_t alignment = dkImageLayoutGetAlignment(&image->Layout);
        offset = (offset + alignment - 1U) / alignment * alignment;
        image->Offset = static_cast<uint32_t>(offset);
        offset += dkImageLayoutGetSize(&image->Layout);
        if (offset > 0x7FFFFFFFULL) {
            return false;
        }
    }
    total = offset;
    return true;
}

constexpr uint32_t kRenderFlags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
constexpr uint32_t kDepthFlags = DkImageFlags_UsageRender | DkImageFlags_HwCompression;

// Scene colour and display images: compressed unless config/deko3d_compression
// says "0" (Device::ColorCompression). Their readers are GPU engines only:
// the 2D blit into the display image, the scanout's sampling, the copy
// engine of save states (as for the compressed depth).
uint32_t ColorFlags(const Device& device) {
    return kRenderFlags | (device.ColorCompression() ? static_cast<uint32_t>(DkImageFlags_HwCompression) : 0U);
}

// Savestate staging: written or read by the CPU once, between two waits for
// the GPU (every queue flush also flushes and invalidates the GPU's L2).
constexpr uint32_t kStagingFlags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
constexpr uint32_t kStagingAlignment = 256U;

bool Fail(std::string* error, const std::string& message) {
    Log("savestate: %s", message.c_str());
    if (error != nullptr) {
        *error = message;
    }
    return false;
}

// Hands what the frame recorded to the GPU and waits for all of it.
bool SubmitAndWait(Device& device, std::string* error) {
    device.Submit();
    device.WaitIdle();
    if (dkQueueIsInErrorState(device.Queue())) {
        return Fail(error, "the deko3d queue failed during a savestate copy");
    }
    return true;
}
} // namespace

bool PicaTargets::Init(Device& device, PicaResources& resources, float scale, float widescreenFactor,
                       TopScreenLineSamples lines) {
    mDevice = &device;
    mResources = &resources;
    mScale = std::max(scale, 1.0F);
    mWidescreenFactor = std::max(widescreenFactor, 1.0F);
    mLines = lines;
    const float topSampling = PicaTargetSamplingX(kTopScreenPhysicalWidth, kTopScreenPhysicalHeight, mScale, mLines);
    Log("targets: resolution scale %.2f, widescreen factor %.3f, top screen %s sample%s per presented line "
        "(config/deko3d_line_samples %u)",
        static_cast<double>(mScale), static_cast<double>(mWidescreenFactor), topSampling < 1.0F ? "one" : "two",
        topSampling < 1.0F ? "" : "s", lines == TopScreenLineSamples::Two ? 2U : 1U);
    // Dynamic resolution lowers the configured scale, never raises it (no
    // SMAA/FXAA here, whose images would have the source's extent).
    PicaGlDynamicScale::gState.MaxScale = mScale;
    PicaGlDynamicScale::gState.Supported = mScale > 1.0F;
    return true;
}

void PicaTargets::DestroyTarget(PicaTarget& target) noexcept {
    if (target.Memory != nullptr) {
        dkMemBlockDestroy(target.Memory);
        target.Memory = nullptr;
    }
}

void PicaTargets::Reset() noexcept {
    for (auto& [key, target] : mTargets) {
        DestroyTarget(target);
    }
    mTargets.clear();
    for (auto& [key, display] : mDisplays) {
        if (display.Memory != nullptr) {
            dkMemBlockDestroy(display.Memory);
        }
        if (mResources != nullptr) {
            mResources->FreeImageSlot(display.ImageSlot);
        }
    }
    mDisplays.clear();
    for (const Retired& retired : mRetired) {
        dkMemBlockDestroy(retired.Memory);
        if (mResources != nullptr) {
            mResources->FreeImageSlot(retired.ImageSlot);
        }
    }
    mRetired.clear();
    mStats.TargetBytes = 0;
}

void PicaTargets::Shutdown() noexcept {
    Reset();
    mDevice = nullptr;
    mResources = nullptr;
}

void PicaTargets::BeginFrame(uint64_t frame) {
    mFrame = frame;
    // BeginFrame waited for the slot of frame - kFramesInFlight: anything
    // retired at or before it is no longer read.
    auto keep = mRetired.begin();
    for (auto it = mRetired.begin(); it != mRetired.end(); ++it) {
        if (it->Frame + kFramesInFlight <= frame) {
            dkMemBlockDestroy(it->Memory);
            mResources->FreeImageSlot(it->ImageSlot);
        } else {
            *keep++ = *it;
        }
    }
    mRetired.erase(keep, mRetired.end());
}

PicaTarget* PicaTargets::Get(const PicaTargetKey& key, bool* created) {
    if (created != nullptr) {
        *created = false;
    }
    if (const auto found = mTargets.find(key); found != mTargets.end()) {
        return &found->second;
    }
    PicaTarget target;
    target.Extent = PlanPicaTargetExtent(key.Width, key.Height, mScale, mWidescreenFactor, kTopScreenPhysicalWidth,
                                         kTopScreenPhysicalHeight,
                                         PicaTargetSamplingX(key.Width, key.Height, mScale, mLines));
    ImagePlacement color{MakeLayout(mDevice->Handle(), DkImageFormat_RGBA8_Unorm, ColorFlags(*mDevice), target.Extent.Width,
                                    target.Extent.Height)};
    ImagePlacement depth{MakeLayout(mDevice->Handle(), DkImageFormat_Z24S8, kDepthFlags, target.Extent.Width,
                                    target.Extent.Height)};
    uint64_t bytes = 0U;
    if (!Place({&color, &depth}, bytes) ||
        (target.Memory = mDevice->MakeBlock(static_cast<uint32_t>(bytes),
                                            DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "render target")) ==
            nullptr) {
        if (mStats.Failures++ < 10U) {
            Log("targets: no memory for a %ux%u target (%llu MiB live)", target.Extent.Width, target.Extent.Height,
                static_cast<unsigned long long>(mStats.TargetBytes >> 20));
        }
        return nullptr;
    }
    target.Alloc = target.Extent;
    target.DynamicEligible = key.Width == kTopScreenPhysicalWidth && key.Height == kTopScreenPhysicalHeight;
    target.Id = mNextId++;
    mStats.TargetBytes += bytes;
    ++mStats.TargetsCreated;
    auto& stored = mTargets.emplace(key, target).first->second;
    // Images and views only after the copy into the map: a DkImageView keeps a
    // pointer to its DkImage, and views built on the local pointed at the stack
    // (p5/p5b: Data Abort in dkCmdBufBindRenderTargets on the first draw).
    dkImageInitialize(&stored.Color, &color.Layout, stored.Memory, color.Offset);
    dkImageInitialize(&stored.DepthStencil, &depth.Layout, stored.Memory, depth.Offset);
    dkImageViewDefaults(&stored.ColorView, &stored.Color);
    dkImageViewDefaults(&stored.DepthView, &stored.DepthStencil);
    // GL: a new target starts cleared (colour 0, depth 1, stencil 0).
    DkCmdBuf cmd = mDevice->Cmd();
    const DkImageView* colors[] = {&stored.ColorView};
    dkCmdBufBindRenderTargets(cmd, colors, 1, &stored.DepthView);
    const DkScissor scissor = {0, 0, stored.Extent.Width, stored.Extent.Height};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0F, 0.0F, 0.0F, 0.0F);
    dkCmdBufClearDepthStencil(cmd, true, 1.0F, 0xFF, 0);
    ++stored.Version;
    if (mStats.TargetsCreated <= 32U) {
        Log("targets: %ux%u (native %ux%u, ns %llu, colour %08x, depth %08x%s), %llu KiB", stored.Extent.Width,
            stored.Extent.Height, key.Width, key.Height, static_cast<unsigned long long>(key.Namespace),
            key.ColorAddress, key.DepthAddress, stored.Extent.Wide ? ", wide" : "",
            static_cast<unsigned long long>(bytes >> 10));
    }
    if (created != nullptr) {
        *created = true;
    }
    return &stored;
}

std::pair<const PicaTargetKey*, PicaTarget*> PicaTargets::FindColor(uint64_t targetNamespace, uint32_t colorAddress) {
    const PicaTargetKey* bestKey = nullptr;
    PicaTarget* best = nullptr;
    const auto order = [](const PicaTargetKey& key) {
        return std::tie(key.Namespace, key.ColorAddress, key.DepthAddress, key.Width, key.Height, key.ColorFormat,
                        key.DepthFormat);
    };
    for (auto& [key, target] : mTargets) {
        if (key.Namespace != targetNamespace || key.ColorAddress != colorAddress) {
            continue;
        }
        if (bestKey == nullptr || order(key) < order(*bestKey)) {
            bestKey = &key;
            best = &target;
        }
    }
    return {bestKey, best};
}

bool PicaTargets::ApplyDynamicScale(const PicaTargetKey& key, PicaTarget& target) {
    if (!target.DynamicEligible) {
        return false;
    }
    const auto& dynamic = PicaGlDynamicScale::gState;
    float desired = target.Alloc.Scale;
    if (dynamic.Supported && dynamic.Requested > 0.0F) {
        desired = std::clamp(std::round(dynamic.Requested * 4.0F) / 4.0F, 1.0F, target.Alloc.Scale);
    }
    PicaGlDynamicScale::TargetPhase phase{target.Extent.Scale, target.ScaleSerial};
    const bool moved = PicaGlDynamicScale::ScaleAtDraw(phase, dynamic.FrameSerial, desired);
    target.ScaleSerial = phase.Serial;
    if (!moved) {
        return false;
    }
    target.Extent = PlanPicaDynamicExtent(key.Width, key.Height, desired, target.Alloc);
    DkCmdBuf cmd = mDevice->Cmd();
    const DkImageView* colors[] = {&target.ColorView};
    dkCmdBufBindRenderTargets(cmd, colors, 1, &target.DepthView);
    const DkScissor scissor = {0, 0, target.Alloc.Width, target.Alloc.Height};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0F, 0.0F, 0.0F, 1.0F);
    ++target.Version;
    PicaGlDynamicScale::gState.Applied.fetch_add(1U, std::memory_order_relaxed);
    PicaGlDynamicScale::gState.AppliedAtDraw.fetch_add(1U, std::memory_order_relaxed);
    return true;
}

PicaDisplay* PicaTargets::GetDisplay(uint64_t targetNamespace, uint32_t outputAddress, uint32_t width,
                                     uint32_t height, uint32_t allocWidth, uint32_t allocHeight) {
    auto [found, inserted] = mDisplays.try_emplace({targetNamespace, outputAddress});
    PicaDisplay& display = found->second;
    if (!inserted && display.Memory != nullptr && display.Width == width && display.Height == height &&
        display.AllocWidth == allocWidth && display.AllocHeight == allocHeight) {
        return &display;
    }
    if (display.Memory != nullptr) {
        mRetired.push_back({display.Memory, display.ImageSlot, mFrame});
        display = PicaDisplay{};
    }
    ImagePlacement image{MakeLayout(mDevice->Handle(), DkImageFormat_RGBA8_Unorm, ColorFlags(*mDevice), allocWidth,
                                    allocHeight)};
    uint64_t bytes = 0U;
    const uint32_t slot = mResources->AllocateImageSlot();
    if (slot >= PicaResources::kMaxImages || !Place({&image}, bytes) ||
        (display.Memory = mDevice->MakeBlock(static_cast<uint32_t>(bytes),
                                             DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "display image")) ==
            nullptr) {
        if (slot < PicaResources::kMaxImages) {
            mResources->FreeImageSlot(slot);
        }
        ++mStats.Failures;
        mDisplays.erase(found);
        return nullptr;
    }
    dkImageInitialize(&display.Image, &image.Layout, display.Memory, image.Offset);
    dkImageViewDefaults(&display.View, &display.Image);
    display.ImageSlot = slot;
    mResources->WriteImageDescriptor(slot, display.Image);
    display.Width = width;
    display.Height = height;
    display.AllocWidth = allocWidth;
    display.AllocHeight = allocHeight;
    ++mStats.DisplaysCreated;
    Log("targets: display image %ux%u (native %ux%u, ns %llu, output %08x)", allocWidth, allocHeight, width, height,
        static_cast<unsigned long long>(targetNamespace), outputAddress);
    return &display;
}

bool PicaTargets::ReadTarget(const PicaTarget& target, std::vector<uint8_t>& rgba8,
                             std::vector<uint32_t>& depthStencil, std::string* error) {
    const uint32_t width = target.Extent.Width;
    const uint32_t height = target.Extent.Height;
    const uint64_t bytes64 = static_cast<uint64_t>(width) * height * 4U;
    if (bytes64 == 0U || bytes64 > 0x3FFFFFFFULL) {
        return Fail(error, "a render target has an invalid extent");
    }
    const auto bytes = static_cast<uint32_t>(bytes64);
    const uint32_t depthOffset = AlignUp(bytes, kStagingAlignment);
    DkMemBlock staging = mDevice->MakeBlock(depthOffset + bytes, kStagingFlags, "savestate read-back");
    if (staging == nullptr) {
        return Fail(error, "no memory to read back a " + std::to_string(width) + "x" + std::to_string(height) +
                               " render target");
    }
    const DkGpuAddr gpu = dkMemBlockGetGpuAddr(staging);
    DkCmdBuf cmd = mDevice->Cmd();
    // The target's fragments are in memory before the copy engine reads them;
    // the copies are in memory before the CPU does.
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    const DkImageRect rect = {0U, 0U, 0U, width, height, 1U};
    const DkCopyBuf color = {gpu, 0U, 0U};
    dkCmdBufCopyImageToBuffer(cmd, &target.ColorView, &rect, &color, 0U);
    const DkCopyBuf depth = {gpu + depthOffset, 0U, 0U};
    dkCmdBufCopyImageToBuffer(cmd, &target.DepthView, &rect, &depth, 0U);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    const bool done = SubmitAndWait(*mDevice, error);
    if (done) {
        // One pass over the uncached block into cached memory.
        const auto* cpu = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(staging));
        rgba8.resize(bytes);
        std::memcpy(rgba8.data(), cpu, bytes);
        depthStencil.resize(bytes / 4U);
        std::memcpy(depthStencil.data(), cpu + depthOffset, bytes);
    }
    dkMemBlockDestroy(staging);
    return done;
}

bool PicaTargets::WriteTarget(PicaTarget& target, std::span<const uint8_t> rgba8,
                              std::span<const uint32_t> depthStencil, std::string* error) {
    const uint32_t width = target.Extent.Width;
    const uint32_t height = target.Extent.Height;
    const uint64_t bytes64 = static_cast<uint64_t>(width) * height * 4U;
    if (bytes64 == 0U || bytes64 > 0x3FFFFFFFULL || rgba8.size() != bytes64 ||
        depthStencil.size() * 4U != bytes64) {
        return Fail(error, "restored pixels do not match the render target's extent");
    }
    const auto bytes = static_cast<uint32_t>(bytes64);
    const uint32_t depthOffset = AlignUp(bytes, kStagingAlignment);
    DkMemBlock staging = mDevice->MakeBlock(depthOffset + bytes, kStagingFlags, "savestate upload");
    if (staging == nullptr) {
        return Fail(error, "no memory to restore a " + std::to_string(width) + "x" + std::to_string(height) +
                               " render target");
    }
    auto* cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(staging));
    std::memcpy(cpu, rgba8.data(), bytes);
    std::memcpy(cpu + depthOffset, depthStencil.data(), bytes);
    const DkGpuAddr gpu = dkMemBlockGetGpuAddr(staging);
    DkCmdBuf cmd = mDevice->Cmd();
    // The target's creation clear is done (and nothing stale of the staging
    // memory is cached) before the copy engine overwrites it.
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    const DkImageRect rect = {0U, 0U, 0U, width, height, 1U};
    const DkCopyBuf color = {gpu, 0U, 0U};
    dkCmdBufCopyBufferToImage(cmd, &color, &target.ColorView, &rect, 0U);
    const DkCopyBuf depth = {gpu + depthOffset, 0U, 0U};
    dkCmdBufCopyBufferToImage(cmd, &depth, &target.DepthView, &rect, 0U);
    // Later draws and copies see the restored images, and Zcull forgets the
    // creation clear (it would cull against depth 1 where the copy wrote less).
    const DkImageView* colors[] = {&target.ColorView};
    dkCmdBufBindRenderTargets(cmd, colors, 1, &target.DepthView);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Zcull);
    ++target.Version;
    const bool done = SubmitAndWait(*mDevice, error);
    dkMemBlockDestroy(staging);
    return done;
}

std::optional<DepthStencilWordLayout> PicaTargets::DepthStencilLayout(std::string* error) {
    if (mDepthStencilLayout.has_value()) {
        return mDepthStencilLayout;
    }
    constexpr uint32_t kSize = 256U;
    constexpr uint32_t kWords = kSize * kSize;
    constexpr uint32_t kBytes = kWords * 4U;
    ImagePlacement color{MakeLayout(mDevice->Handle(), DkImageFormat_RGBA8_Unorm, kRenderFlags, kSize, kSize)};
    ImagePlacement depth{MakeLayout(mDevice->Handle(), DkImageFormat_Z24S8, kDepthFlags, kSize, kSize)};
    uint64_t imageBytes = 0U;
    DkMemBlock images = nullptr;
    if (Place({&color, &depth}, imageBytes)) {
        images = mDevice->MakeBlock(static_cast<uint32_t>(imageBytes), DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                                    "depth probe");
    }
    // [cleared read-back | written pattern | pattern read-back]
    DkMemBlock staging = images != nullptr ? mDevice->MakeBlock(3U * kBytes, kStagingFlags, "depth probe staging")
                                           : nullptr;
    if (staging == nullptr) {
        if (images != nullptr) {
            dkMemBlockDestroy(images);
        }
        Fail(error, "no memory for the depth read-back probe");
        return std::nullopt;
    }
    DkImage colorImage;
    DkImage depthImage;
    dkImageInitialize(&colorImage, &color.Layout, images, color.Offset);
    dkImageInitialize(&depthImage, &depth.Layout, images, depth.Offset);
    DkImageView colorView;
    DkImageView depthView;
    dkImageViewDefaults(&colorView, &colorImage);
    dkImageViewDefaults(&depthView, &depthImage);
    auto* cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(staging));
    std::vector<uint32_t> pattern(kWords);
    for (uint32_t index = 0; index < kWords; ++index) {
        pattern[index] = (index + 1U) * 0x9E3779B9U;
    }
    std::memcpy(cpu + kBytes, pattern.data(), kBytes);
    const DkGpuAddr gpu = dkMemBlockGetGpuAddr(staging);
    const DkImageRect rect = {0U, 0U, 0U, kSize, kSize, 1U};
    const DkCopyBuf cleared = {gpu, 0U, 0U};
    const DkCopyBuf written = {gpu + kBytes, 0U, 0U};
    const DkCopyBuf reread = {gpu + 2U * kBytes, 0U, 0U};
    DkCmdBuf cmd = mDevice->Cmd();
    const DkImageView* colors[] = {&colorView};
    dkCmdBufBindRenderTargets(cmd, colors, 1, &depthView);
    const DkScissor scissor = {0U, 0U, kSize, kSize};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmd, 0, DkColorMask_RGBA, 0.0F, 0.0F, 0.0F, 0.0F);
    dkCmdBufClearDepthStencil(cmd, true, kProbeDepth, 0xFF, kProbeStencil);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache);
    dkCmdBufCopyImageToBuffer(cmd, &depthView, &rect, &cleared, 0U);
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    dkCmdBufCopyBufferToImage(cmd, &written, &depthView, &rect, 0U);
    dkCmdBufBarrier(cmd, DkBarrier_Full, 0);
    dkCmdBufCopyImageToBuffer(cmd, &depthView, &rect, &reread, 0U);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Zcull | DkInvalidateFlags_L2Cache);
    bool done = SubmitAndWait(*mDevice, error);
    std::vector<uint32_t> words(2U * kWords);
    if (done) {
        std::memcpy(words.data(), cpu, kBytes);
        std::memcpy(words.data() + kWords, cpu + 2U * kBytes, kBytes);
    }
    dkMemBlockDestroy(staging);
    dkMemBlockDestroy(images);
    if (!done) {
        return std::nullopt;
    }
    const std::span<const uint32_t> clearedWords(words.data(), kWords);
    const std::span<const uint32_t> rereadWords(words.data() + kWords, kWords);
    const auto layout = IdentifyDepthStencilWordLayout(clearedWords.front());
    char detail[160];
    if (!layout.has_value() ||
        !std::all_of(clearedWords.begin(), clearedWords.end(), [&](uint32_t word) { return word == clearedWords.front(); })) {
        std::snprintf(detail, sizeof(detail), "depth read-back unrecognised: a clear to %.2f/%02x read %08x",
                      static_cast<double>(kProbeDepth), kProbeStencil, clearedWords.front());
        Fail(error, detail);
        return std::nullopt;
    }
    const auto mismatch = std::mismatch(rereadWords.begin(), rereadWords.end(), pattern.begin());
    if (mismatch.first != rereadWords.end()) {
        std::snprintf(detail, sizeof(detail), "depth write does not round-trip: wrote %08x, read %08x",
                      *mismatch.second, *mismatch.first);
        Fail(error, detail);
        return std::nullopt;
    }
    Log("savestate: Z24S8 words read as %s (a clear to %.2f/%02x read %08x), writes round-trip",
        *layout == DepthStencilWordLayout::DepthHigh ? "depth << 8 | stencil" : "stencil << 24 | depth",
        static_cast<double>(kProbeDepth), kProbeStencil, clearedWords.front());
    mDepthStencilLayout = layout;
    return layout;
}

} // namespace Fast::Deko3d
#endif
