// deko3d PICA render targets and display images (Switch). See
// gfx_deko3d_pica_targets.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_pica_targets.h"

#include <algorithm>
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
} // namespace

bool PicaTargets::Init(Device& device, PicaResources& resources, float scale, float widescreenFactor) {
    mDevice = &device;
    mResources = &resources;
    mScale = std::max(scale, 1.0F);
    mWidescreenFactor = std::max(widescreenFactor, 1.0F);
    Log("targets: resolution scale %.2f, widescreen factor %.3f", static_cast<double>(mScale),
        static_cast<double>(mWidescreenFactor));
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
                                         kTopScreenPhysicalHeight);
    ImagePlacement color{MakeLayout(mDevice->Handle(), DkImageFormat_RGBA8_Unorm, kRenderFlags, target.Extent.Width,
                                    target.Extent.Height)};
    ImagePlacement depth{MakeLayout(mDevice->Handle(), DkImageFormat_Z24S8,
                                    DkImageFlags_UsageRender | DkImageFlags_HwCompression, target.Extent.Width,
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
    ImagePlacement image{MakeLayout(mDevice->Handle(), DkImageFormat_RGBA8_Unorm, kRenderFlags, allocWidth,
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

} // namespace Fast::Deko3d
#endif
