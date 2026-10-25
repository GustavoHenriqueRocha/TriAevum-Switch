#pragma once
// deko3d PICA render targets and display images (Switch), package 5
// (docs/DEKO3D_PROGRESSO.md). A target is the GL backend's PicaGlTarget: a
// colour image (RGBA8) and a depth/stencil image (Z24S8) per PICA target key,
// at the resolution scale (a top-screen target widened by widescreen). A
// display image is PicaGlDisplay: the RGBA8 destination of a display
// transfer, sampled by the scanout. The decisions live in the pure
// fast/backends/gfx_deko3d_pica_pipeline.h.
//
// Lifetime: targets live until Reset/Shutdown (as in GL). A display image
// whose extent changes is retired and destroyed only after the frames in
// flight that may still read it are done.

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_pica_resources.h"

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Fast::Deko3d {

struct PicaTarget {
    DkMemBlock Memory = nullptr;
    DkImage Color{};
    DkImage DepthStencil{};
    DkImageView ColorView{};
    DkImageView DepthView{};
    // Drawn extent; below Alloc while dynamic resolution lowers the scale
    // (the bottom-left sub-rectangle of the same images).
    PicaTargetExtent Extent;
    PicaTargetExtent Alloc;
    // Dynamic resolution: the top-screen scene target; FrameSerial its scale
    // was last looked at (gl_pica_dynamic_scale.h).
    bool DynamicEligible = false;
    uint32_t ScaleSerial = 0;
    // Unique per created target and bumped by every write to Color: the
    // identity of the content a display copy read (GL: Id/Version).
    uint64_t Id = 0;
    uint64_t Version = 0;
};

struct PicaDisplayCopy {
    uint64_t Target = 0;
    uint64_t Version = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    bool Linear = false;

    [[nodiscard]] bool Valid() const noexcept { return Target != 0U; }
    bool operator==(const PicaDisplayCopy&) const = default;
};

struct PicaDisplay {
    DkMemBlock Memory = nullptr;
    DkImage Image{};
    DkImageView View{};
    uint32_t ImageSlot = 0;
    uint32_t Width = 0; // native output extent
    uint32_t Height = 0;
    uint32_t AllocWidth = 0;
    uint32_t AllocHeight = 0;
    uint32_t ContentWidth = 0;
    uint32_t ContentHeight = 0;
    PicaDisplayCopy LastCopy;
};

struct PicaTargetStats {
    uint64_t TargetsCreated = 0;
    uint64_t TargetBytes = 0;
    uint64_t DisplaysCreated = 0;
    uint64_t Failures = 0;
};

class PicaTargets {
  public:
    // scale: config/resolution_scale(_docked); widescreenFactor: > 1 widens
    // the top-screen target (config/widescreen and the window shape).
    bool Init(Device& device, PicaResources& resources, float scale, float widescreenFactor);
    void Shutdown() noexcept;
    // Every target and display image (after the GPU is idle).
    void Reset() noexcept;
    // After Device::BeginFrame: destroys what the frames in flight released.
    void BeginFrame(uint64_t frame);

    // The target of a key, created (and cleared like GL: colour 0, depth 1,
    // stencil 0) on first use; nullptr when the memory ran out. *created is
    // set for a new target (its clear is recorded, the target is bound).
    PicaTarget* Get(const PicaTargetKey& key, bool* created);
    // The target a transfer or clear names: the smallest key (GL's std::map
    // order) of the namespace with that colour address.
    std::pair<const PicaTargetKey*, PicaTarget*> FindColor(uint64_t targetNamespace, uint32_t colorAddress);
    template <typename F> void ForEach(F&& visit) {
        for (auto& [key, target] : mTargets) {
            visit(key, target);
        }
    }

    // Dynamic resolution at the frame's first draw into an eligible target
    // (GL: ApplyPicaGlTargetScaleAtDraw): moves the drawn extent to the
    // requested scale and clears the colour (what the frame leaves undrawn is
    // black, not pixels at the other scale). True when the extent changed.
    bool ApplyDynamicScale(const PicaTargetKey& key, PicaTarget& target);

    PicaDisplay* GetDisplay(uint64_t targetNamespace, uint32_t outputAddress, uint32_t width, uint32_t height,
                            uint32_t allocWidth, uint32_t allocHeight);

    [[nodiscard]] float Scale() const noexcept { return mScale; }
    [[nodiscard]] float WidescreenFactor() const noexcept { return mWidescreenFactor; }
    [[nodiscard]] const PicaTargetStats& Stats() const noexcept { return mStats; }

  private:
    struct Retired {
        DkMemBlock Memory = nullptr;
        uint32_t ImageSlot = 0;
        uint64_t Frame = 0;
    };
    void DestroyTarget(PicaTarget& target) noexcept;

    Device* mDevice = nullptr;
    PicaResources* mResources = nullptr;
    float mScale = 1.0F;
    float mWidescreenFactor = 1.0F;
    uint64_t mNextId = 1;
    uint64_t mFrame = 0;
    std::unordered_map<PicaTargetKey, PicaTarget, PicaTargetKeyHash> mTargets;
    std::map<std::pair<uint64_t, uint32_t>, PicaDisplay> mDisplays;
    std::vector<Retired> mRetired;
    PicaTargetStats mStats{};
};

} // namespace Fast::Deko3d
#endif
