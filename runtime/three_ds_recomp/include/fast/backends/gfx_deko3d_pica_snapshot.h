#pragma once
// Savestates on deko3d (Switch): the CPU side of CapturePicaColorTargets and
// RestorePicaColorTargets (same contract as the Vulkan backend's,
// gfx_vulkan_pica_state.cpp). Pure, tested on the host
// (oot3d_deko3d_backend_tests).
//
// A target is read back and written through CPU memory by the copy engine:
// its colour as RGBA8 rows and its depth/stencil (Z24S8) as one 32-bit word
// per pixel, both at the target's drawn extent. A snapshot holds them at the
// guest framebuffer extent (kPicaGuestExtentRenderScalePermille): colour
// area-averaged (ResamplePicaRgba8), depth and stencil nearest
// (ResamplePicaNearest), exactly as Vulkan resamples them.

#include "fast/backends/gfx_deko3d_pica_pipeline.h"
#include "fast/renderer3ds/pica_guest_extent_snapshot.h"
#include "fast/renderer3ds/pica_render_backend.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace Fast::Deko3d {

// Where the 24 depth bits sit in a Z24S8 word read by the copy engine. The
// device probes it once (a clear to kProbeDepth/kProbeStencil read back), so
// a wrong guess about the hardware layout fails the save instead of storing
// swapped depth and stencil.
enum class DepthStencilWordLayout : uint8_t {
    DepthHigh, // depth << 8 | stencil
    DepthLow,  // stencil << 24 | depth
};

inline constexpr float kDepth24Max = 16777215.0F;
inline constexpr float kProbeDepth = 0.25F;
inline constexpr uint8_t kProbeStencil = 0x5AU;

[[nodiscard]] inline uint32_t Depth24(float depth) noexcept {
    return static_cast<uint32_t>(std::lround(std::clamp(depth, 0.0F, 1.0F) * kDepth24Max));
}

[[nodiscard]] inline uint32_t PackDepthStencilWord(float depth, uint8_t stencil,
                                                   DepthStencilWordLayout layout) noexcept {
    const uint32_t depth24 = Depth24(depth);
    return layout == DepthStencilWordLayout::DepthHigh ? (depth24 << 8U) | stencil
                                                       : (static_cast<uint32_t>(stencil) << 24U) | depth24;
}

[[nodiscard]] inline uint32_t WordDepth24(uint32_t word, DepthStencilWordLayout layout) noexcept {
    return layout == DepthStencilWordLayout::DepthHigh ? word >> 8U : word & 0x00FFFFFFU;
}

[[nodiscard]] inline uint8_t WordStencil(uint32_t word, DepthStencilWordLayout layout) noexcept {
    return static_cast<uint8_t>(layout == DepthStencilWordLayout::DepthHigh ? word & 0xFFU : word >> 24U);
}

// The layout under which `word` holds the probe clear (the hardware may round
// the 24-bit depth either way); nullopt when neither does.
[[nodiscard]] inline std::optional<DepthStencilWordLayout> IdentifyDepthStencilWordLayout(uint32_t word) noexcept {
    const uint32_t expected = Depth24(kProbeDepth);
    for (const auto layout : {DepthStencilWordLayout::DepthHigh, DepthStencilWordLayout::DepthLow}) {
        const uint32_t depth = WordDepth24(word, layout);
        const uint32_t distance = depth > expected ? depth - expected : expected - depth;
        if (WordStencil(word, layout) == kProbeStencil && distance <= 1U) {
            return layout;
        }
    }
    return std::nullopt;
}

// Snapshot order: PicaTargets::FindColor's (GL's std::map order).
[[nodiscard]] inline bool PicaTargetKeyLess(const PicaTargetKey& left, const PicaTargetKey& right) noexcept {
    return std::tie(left.Namespace, left.ColorAddress, left.DepthAddress, left.Width, left.Height, left.ColorFormat,
                    left.DepthFormat) < std::tie(right.Namespace, right.ColorAddress, right.DepthAddress,
                                                 right.Width, right.Height, right.ColorFormat, right.DepthFormat);
}

[[nodiscard]] inline PicaTargetKey PicaTargetKeyOf(const Renderer3ds::PicaRenderTargetColorSnapshot& snapshot) noexcept {
    return {snapshot.RenderTargetNamespace, snapshot.ColorPhysicalAddress, snapshot.DepthPhysicalAddress,
            snapshot.FramebufferWidth,      snapshot.FramebufferHeight,    snapshot.FramebufferColorFormat,
            snapshot.FramebufferDepthFormat};
}

// One target read back at its drawn extent (width x height): `rgba8` rows,
// `depthStencil` Z24S8 words. deko3d has neither W-buffer consumers nor
// scene effects, so both flags stay false (a Vulkan restore sets W-buffering
// again at the target's next draw).
[[nodiscard]] inline Renderer3ds::PicaRenderTargetColorSnapshot MakeGuestExtentSnapshot(
    const PicaTargetKey& key, uint32_t width, uint32_t height, std::span<const uint8_t> rgba8,
    std::span<const uint32_t> depthStencil, DepthStencilWordLayout layout) {
    Renderer3ds::PicaRenderTargetColorSnapshot snapshot;
    snapshot.RenderTargetNamespace = key.Namespace;
    snapshot.ColorPhysicalAddress = key.ColorAddress;
    snapshot.DepthPhysicalAddress = key.DepthAddress;
    snapshot.FramebufferWidth = key.Width;
    snapshot.FramebufferHeight = key.Height;
    snapshot.FramebufferColorFormat = key.ColorFormat;
    snapshot.FramebufferDepthFormat = key.DepthFormat;
    snapshot.RenderScalePermille = Renderer3ds::kPicaGuestExtentRenderScalePermille;
    snapshot.ImageWidth = key.Width;
    snapshot.ImageHeight = key.Height;
    snapshot.SampleCount = 1U;
    snapshot.WBuffering = false;
    snapshot.SceneEffectsResolved = false;
    snapshot.ColorRgba8 = Renderer3ds::ResamplePicaRgba8(rgba8, width, height, key.Width, key.Height);
    // Nearest on the packed word picks the same source pixel for depth and
    // stencil that two separate nearest resamples would.
    const auto words = Renderer3ds::ResamplePicaNearest<uint32_t>(depthStencil, width, height, key.Width, key.Height);
    snapshot.DepthValues.resize(words.size());
    snapshot.StencilValues.resize(words.size());
    for (size_t index = 0; index < words.size(); ++index) {
        snapshot.DepthValues[index] = static_cast<float>(WordDepth24(words[index], layout)) / kDepth24Max;
        snapshot.StencilValues[index] = WordStencil(words[index], layout);
    }
    return snapshot;
}

// Vulkan's checks before a restore, without its guide images: deko3d has
// none, so a legacy snapshot (saved at a renderer's scale, with guides)
// restores its colour, depth and stencil and leaves the guides unread.
// Empty when the set can be restored, else why not.
[[nodiscard]] inline std::string ValidateRestoreSnapshots(
    std::span<const Renderer3ds::PicaRenderTargetColorSnapshot> snapshots) {
    if (snapshots.empty()) {
        return {};
    }
    const uint16_t scale = snapshots.front().RenderScalePermille;
    const bool guestExtent = scale == Renderer3ds::kPicaGuestExtentRenderScalePermille;
    std::set<std::tuple<uint64_t, uint32_t, uint32_t, uint16_t, uint16_t, uint8_t, uint8_t>> identities;
    for (const auto& snapshot : snapshots) {
        if (snapshot.RenderScalePermille != scale) {
            return "native PICA color snapshots were saved at different render scales (" +
                   std::to_string(snapshot.RenderScalePermille) + " and " + std::to_string(scale) + "/1000)";
        }
        const uint64_t pixels = static_cast<uint64_t>(snapshot.ImageWidth) * snapshot.ImageHeight;
        const bool extentValid = !guestExtent || (snapshot.ImageWidth == snapshot.FramebufferWidth &&
                                                  snapshot.ImageHeight == snapshot.FramebufferHeight);
        if (snapshot.ColorPhysicalAddress == 0U || snapshot.FramebufferWidth == 0U ||
            snapshot.FramebufferHeight == 0U || pixels == 0U || !extentValid || snapshot.SampleCount != 1U ||
            snapshot.ColorRgba8.size() != pixels * 4U || snapshot.DepthValues.size() != pixels ||
            (!snapshot.StencilValues.empty() && snapshot.StencilValues.size() != pixels) ||
            !identities
                 .insert({snapshot.RenderTargetNamespace, snapshot.ColorPhysicalAddress,
                          snapshot.DepthPhysicalAddress, snapshot.FramebufferWidth, snapshot.FramebufferHeight,
                          snapshot.FramebufferColorFormat, snapshot.FramebufferDepthFormat})
                 .second) {
            return "native PICA color snapshot is incompatible";
        }
        for (const float depth : snapshot.DepthValues) {
            if (!std::isfinite(depth) || depth < 0.0F || depth > 1.0F) {
                return "native PICA depth restore payload is invalid";
            }
        }
    }
    return {};
}

// What a restored target receives at its extent (width x height): the colour
// resampled from the snapshot's extent and the depth/stencil words nearest.
struct TargetPixels {
    std::vector<uint8_t> Rgba8;
    std::vector<uint32_t> DepthStencil;
};

[[nodiscard]] inline TargetPixels PlanRestorePixels(const Renderer3ds::PicaRenderTargetColorSnapshot& snapshot,
                                                    uint32_t width, uint32_t height,
                                                    DepthStencilWordLayout layout) {
    TargetPixels pixels;
    pixels.Rgba8 =
        Renderer3ds::ResamplePicaRgba8(snapshot.ColorRgba8, snapshot.ImageWidth, snapshot.ImageHeight, width, height);
    std::vector<uint32_t> words(snapshot.DepthValues.size());
    for (size_t index = 0; index < words.size(); ++index) {
        const uint8_t stencil = snapshot.StencilValues.empty() ? 0U : snapshot.StencilValues[index];
        words[index] = PackDepthStencilWord(snapshot.DepthValues[index], stencil, layout);
    }
    pixels.DepthStencil =
        Renderer3ds::ResamplePicaNearest<uint32_t>(words, snapshot.ImageWidth, snapshot.ImageHeight, width, height);
    return pixels;
}

} // namespace Fast::Deko3d
