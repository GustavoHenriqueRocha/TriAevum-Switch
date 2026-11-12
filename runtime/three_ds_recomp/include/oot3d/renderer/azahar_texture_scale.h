#pragma once

// Size cap and mipmaps for texture pack images, run on the pack worker
// (azahar_texture_pack.cpp) before an image reaches a renderer. Pure RGBA8
// functions, checked on the host (oot3d_texture_pack_scale_tests).

#include <cstdint>
#include <span>
#include <vector>

namespace Oot3d::Renderer {

struct TextureSize {
    uint32_t Width = 0;
    uint32_t Height = 0;

    bool operator==(const TextureSize&) const = default;
};

// The size a pack image of `replacement` is reduced to for a native texture
// of `native`: each axis at most native x maxScale, never enlarged.
// maxScale 0 (no cap) or an unknown native size keep the image as it is.
[[nodiscard]] TextureSize CappedReplacementSize(TextureSize native, TextureSize replacement,
                                                uint32_t maxScale) noexcept;

// Whether a pack image of file size `source` for a native texture of
// `native` is reduced to another size at cap `toCap` than at `fromCap` (0 =
// no cap): a change of only "HD texture size" keeps the images it does not
// change (test.15: every size change decoded and uploaded them all again).
[[nodiscard]] bool TexturePackCapChangesImage(TextureSize native, TextureSize source, uint32_t fromCap,
                                              uint32_t toCap) noexcept;

// HUD textures (texture pack replacements and the TopScreen HD UI atlas):
// the HUD is drawn at 3x its 3DS size on a 720p screen (720 = 3 x 240), so
// 3x native is one texel per pixel in handheld mode.
inline constexpr uint32_t kUiTextureMaxScale = 3U;

// RGBA8 `source` of `from` resampled to `to` (each axis no larger than in
// `from`, any ratio). Every output texel is the area-weighted average of the
// source texels it covers. Colour is weighted by alpha, so transparent
// texels do not darken the edges of cut-out art; a texel with no coverage at
// all keeps the plain colour average. Values are averaged as stored (UNORM,
// like the GPU's own filtering of these textures). Empty on invalid input.
[[nodiscard]] std::vector<uint8_t> DownscaleRgba8(std::span<const uint8_t> source, TextureSize from,
                                                  TextureSize to);

// Levels 1 to levels - 1 of a mip chain below `level0`: each level is
// max(1, previous / 2) on both axes (the layout PlanPicaRgba8Mips expects),
// filtered as DownscaleRgba8 from the previous level. levels counts level 0
// and is clamped to the full chain.
[[nodiscard]] std::vector<std::vector<uint8_t>> BuildRgba8MipChain(std::span<const uint8_t> level0, TextureSize size,
                                                                   uint32_t levels);

// Levels of a full chain down to 1x1.
[[nodiscard]] uint32_t FullMipLevelCount(TextureSize size) noexcept;

} // namespace Oot3d::Renderer
