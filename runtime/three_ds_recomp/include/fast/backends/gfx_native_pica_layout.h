#pragma once

#include "oot3d/renderer/ui_presentation_layout.h"

#include <cstdint>

namespace Fast {

using GfxNativePicaPresentationRect =
    ::Oot3d::Renderer::UiPresentationRect;

inline GfxNativePicaPresentationRect FitNativePicaPresentation(
    uint32_t targetWidth, uint32_t targetHeight, float logicalWidth,
    float logicalHeight) noexcept {
    return ::Oot3d::Renderer::FitUiPresentation(
        targetWidth, targetHeight, logicalWidth, logicalHeight);
}

// The part of a display transfer's output that a 3DS LCD shows (Azahar:
// AccelerateDisplay samples only GetSurfaceSubRect's rectangle). A memory row
// of the output is one screen column, left to right; its first Pixels are the
// column's visible pixels, bottom up. MK7 points its upper screen 16 rows into
// a 416-row copy whose rows hold 256 pixels: FirstRow 16, Rows 400, Pixels 240.
struct GfxNativePicaScreenCrop {
    uint32_t FirstRow = 0;
    uint32_t Rows = 0;
    uint32_t Pixels = 0;
};

struct GfxNativePicaScanoutPlacement {
    // Where the whole display is drawn.
    GfxNativePicaPresentationRect Viewport;
    // Where its shown part lands (the scissor).
    GfxNativePicaPresentationRect Visible;
};

// `place` maps a logical (width, height) to its rectangle on the target
// (FitNativePicaPresentation, PlaceUiPictureInPicture). The display is
// displayRows x displayPixels host texels for guestRows x guestPixels guest
// ones. Without a crop, or with one that covers the whole output or does not
// fit it, Viewport and Visible are both place(displayRows, displayPixels).
template <typename Place>
inline GfxNativePicaScanoutPlacement PlaceNativePicaScanout(
    Place&& place, float displayRows, float displayPixels,
    uint32_t guestRows, uint32_t guestPixels,
    const GfxNativePicaScreenCrop* crop) noexcept {
    if (crop == nullptr || guestRows == 0U || guestPixels == 0U ||
        crop->Rows == 0U || crop->Pixels == 0U ||
        crop->FirstRow >= guestRows || crop->Rows > guestRows - crop->FirstRow ||
        crop->Pixels > guestPixels ||
        (crop->FirstRow == 0U && crop->Rows == guestRows &&
         crop->Pixels == guestPixels)) {
        const auto whole = place(displayRows, displayPixels);
        return {whole, whole};
    }
    const auto visible = place(
        displayRows * static_cast<float>(crop->Rows) / static_cast<float>(guestRows),
        displayPixels * static_cast<float>(crop->Pixels) / static_cast<float>(guestPixels));
    if (!visible.Valid()) return {visible, visible};
    const float columnWidth = visible.Width / static_cast<float>(crop->Rows);
    const float pixelHeight = visible.Height / static_cast<float>(crop->Pixels);
    return {
        {visible.X - static_cast<float>(crop->FirstRow) * columnWidth,
         visible.Y - static_cast<float>(guestPixels - crop->Pixels) * pixelHeight,
         static_cast<float>(guestRows) * columnWidth,
         static_cast<float>(guestPixels) * pixelHeight},
        visible,
    };
}

} // namespace Fast
