#pragma once
// deko3d PICA render targets and display images (Switch), package 5
// (docs/DEKO3D_PROGRESS.md). A target is the GL backend's PicaGlTarget: a
// colour image (RGBA8) and a depth/stencil image (Z24S8) per PICA target key,
// at the resolution scale (a top-screen target widened by widescreen and,
// by default, at one sample per presented line: PicaTargetSamplingX). A
// display image is PicaGlDisplay: the RGBA8 destination of a display
// transfer, sampled by the scanout. The decisions live in the pure
// fast/backends/gfx_deko3d_pica_pipeline.h.
//
// Lifetime: targets live until Reset/Shutdown (as in GL). A display image
// whose extent changes is retired and destroyed only after the frames in
// flight that may still read it are done.

#if defined(__SWITCH__)
#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/backends/gfx_deko3d_pica_snapshot.h"
#include "gfx_deko3d_device.h"
#include "gfx_deko3d_pica_resources.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Fast::Deko3d {

// GL keeps its targets in a std::map: a fill goes to the first target that
// covers it in this order (the scene before a blur target sharing its depth).
inline bool PicaTargetKeyOrder(const PicaTargetKey& left, const PicaTargetKey& right) {
    const auto order = [](const PicaTargetKey& key) {
        return std::tie(key.Namespace, key.ColorAddress, key.DepthAddress, key.Width, key.Height, key.ColorFormat,
                        key.DepthFormat);
    };
    return order(left) < order(right);
}

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
    // Order of the last colour write by a draw, clear or fill
    // (PicaTargets::NextWriteSerial; 0 = only the creation clear): which of
    // the target and a transfer at the same address wrote last
    // (gfx_deko3d_pica_feedback.h).
    uint64_t WriteSerial = 0;
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
    // The last transfer into it (feedback textures): its order, output format
    // and whether the output has a texture's tiled layout.
    uint64_t WriteSerial = 0;
    uint8_t ColorFormat = 0;
    bool TextureLayout = false;
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
    // the top-screen target (config/widescreen and the window shape); lines:
    // config/deko3d_line_samples (the top target's sampling, PicaTargetSamplingX).
    bool Init(Device& device, PicaResources& resources, float scale, float widescreenFactor,
              TopScreenLineSamples lines);
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
    // The GPU writes this address: a target's colour buffer or a transfer output
    // (the guest bytes there never see those writes).
    bool GpuWritten(uint64_t targetNamespace, uint32_t address) const;
    // In GL's key order (PicaTargetKeyOrder): the unordered map's order once
    // gave the scene's depth clear to a blur target (MM3D Lost Woods).
    template <typename F> void ForEach(F&& visit) {
        std::vector<std::pair<const PicaTargetKey*, PicaTarget*>> ordered;
        ordered.reserve(mTargets.size());
        for (auto& [key, target] : mTargets) {
            ordered.emplace_back(&key, &target);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right) { return PicaTargetKeyOrder(*left.first, *right.first); });
        for (const auto& [key, target] : ordered) {
            visit(*key, *target);
        }
    }
    [[nodiscard]] size_t Count() const noexcept { return mTargets.size(); }

    // Savestates (gfx_deko3d_pica_snapshot.h), at an idle boundary only:
    // the copy engine moves a target's drawn extent between its block-linear
    // images and CPU memory (colour RGBA8 rows, depth/stencil Z24S8 words).
    // Each call records into the open frame, submits it and waits for the GPU;
    // its staging memory is freed before it returns. False (and *error) when
    // memory runs out or the queue fails.
    bool ReadTarget(const PicaTarget& target, std::vector<uint8_t>& rgba8, std::vector<uint32_t>& depthStencil,
                    std::string* error);
    bool WriteTarget(PicaTarget& target, std::span<const uint8_t> rgba8, std::span<const uint32_t> depthStencil,
                     std::string* error);
    // A raw TextureCopy's source (gfx_deko3d_pica_copies.h): the colour rows
    // EncodePicaFramebuffer samples for a nativeHeight-row framebuffer, row r
    // of `rgba8` (RGBA8, the target's width) for window row r. Records into the
    // open frame, submits it and waits for the GPU like ReadTarget, but keeps
    // its staging memory for the next copy.
    bool ReadCopyRows(const PicaTarget& target, uint32_t nativeHeight, bool bottomRowFirst,
                      std::vector<uint8_t>& rgba8, std::string* error);
    // The Z24S8 word layout the copy engine reads and writes, probed once on a
    // small compressed target like the real ones: a clear read back gives the
    // layout, a written pattern read back proves writes reach the image. Leaves
    // the probe bound: the caller rebinds before drawing.
    std::optional<DepthStencilWordLayout> DepthStencilLayout(std::string* error);

    // Dynamic resolution at the frame's first draw into an eligible target
    // (GL: ApplyPicaGlTargetScaleAtDraw): moves the drawn extent to the
    // requested scale and clears the colour (what the frame leaves undrawn is
    // black, not pixels at the other scale). True when the extent changed.
    bool ApplyDynamicScale(const PicaTargetKey& key, PicaTarget& target);

    PicaDisplay* GetDisplay(uint64_t targetNamespace, uint32_t outputAddress, uint32_t width, uint32_t height,
                            uint32_t allocWidth, uint32_t allocHeight);
    // The display image a transfer wrote at this address; nullptr when none.
    PicaDisplay* FindDisplay(uint64_t targetNamespace, uint32_t outputAddress);
    // Feedback textures (gfx_deko3d_pica_feedback.h): the image a texture at
    // this address reads, the GPU surface copied in texture orientation. Same
    // lifetime as display images (retired when its extent changes).
    PicaDisplay* GetFeedbackImage(uint64_t targetNamespace, uint32_t address, uint32_t width, uint32_t height,
                                  uint32_t allocWidth, uint32_t allocHeight);
    // Monotonic order of GPU colour writes (targets and transfers).
    uint64_t NextWriteSerial() noexcept { return ++mWriteSerial; }

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
    PicaDisplay* GetImage(std::map<std::pair<uint64_t, uint32_t>, PicaDisplay>& images, const char* what,
                          uint64_t targetNamespace, uint32_t address, uint32_t width, uint32_t height,
                          uint32_t allocWidth, uint32_t allocHeight);

    Device* mDevice = nullptr;
    PicaResources* mResources = nullptr;
    float mScale = 1.0F;
    float mWidescreenFactor = 1.0F;
    TopScreenLineSamples mLines = TopScreenLineSamples::One;
    uint64_t mNextId = 1;
    uint64_t mFrame = 0;
    std::unordered_map<PicaTargetKey, PicaTarget, PicaTargetKeyHash> mTargets;
    std::map<std::pair<uint64_t, uint32_t>, PicaDisplay> mDisplays;
    std::map<std::pair<uint64_t, uint32_t>, PicaDisplay> mFeedback;
    uint64_t mWriteSerial = 0;
    std::vector<Retired> mRetired;
    std::optional<DepthStencilWordLayout> mDepthStencilLayout;
    PicaTargetStats mStats{};
    DkMemBlock mCopyStaging = nullptr; // ReadCopyRows; freed by Reset
    uint32_t mCopyStagingBytes = 0;
};

} // namespace Fast::Deko3d
#endif
