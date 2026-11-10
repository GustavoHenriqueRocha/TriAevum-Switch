// The part of a display transfer's output a 3DS LCD shows
// (PlaceNativePicaScanout, gfx_native_pica_layout.h).

#include "fast/backends/gfx_native_pica_layout.h"

#include <cmath>
#include <cstdio>

namespace {

int gFailures = 0;

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__,    \
                         #condition);                                         \
            ++gFailures;                                                      \
        }                                                                     \
    } while (false)

using namespace Fast;

bool Near(float a, float b) { return std::fabs(a - b) < 0.01F; }

bool Same(const GfxNativePicaPresentationRect& a,
          const GfxNativePicaPresentationRect& b) {
    return a.X == b.X && a.Y == b.Y && a.Width == b.Width &&
           a.Height == b.Height;
}

// The MK7 window: 1352x728, the upper screen 16 rows into a 416x256 copy at
// a host scale of 2 (832x512 texels).
void TestUpperScreenInsideACopy() {
    const auto fit = [](float w, float h) {
        return FitNativePicaPresentation(1352U, 728U, w, h);
    };
    const GfxNativePicaScreenCrop crop{16U, 400U, 240U};
    const auto placed = PlaceNativePicaScanout(fit, 832.0F, 512.0F, 416U, 256U, &crop);
    // The shown part is 400x240, fitted like a 400x240 screen.
    const auto screen = fit(400.0F, 240.0F);
    CHECK(Near(placed.Visible.X, screen.X) && Near(placed.Visible.Y, screen.Y));
    CHECK(Near(placed.Visible.Width, screen.Width) &&
          Near(placed.Visible.Height, screen.Height));
    CHECK(Near(placed.Visible.Width / placed.Visible.Height, 400.0F / 240.0F));
    // The whole copy at the same scale: 16 columns more on the left, 16
    // pixels more on top (a column's pixels go bottom up).
    const float scale = screen.Width / 400.0F;
    CHECK(Near(placed.Viewport.Width, 416.0F * scale));
    CHECK(Near(placed.Viewport.Height, 256.0F * scale));
    CHECK(Near(placed.Viewport.X, screen.X - 16.0F * scale));
    CHECK(Near(placed.Viewport.Y, screen.Y - 16.0F * scale));
    // The shown part ends where the copy does on the right and the bottom.
    CHECK(Near(placed.Viewport.X + placed.Viewport.Width,
               placed.Visible.X + placed.Visible.Width));
    CHECK(Near(placed.Viewport.Y + placed.Viewport.Height,
               placed.Visible.Y + placed.Visible.Height));
}

// The lower screen in a corner: a 320x256 copy shows 320x240.
void TestLowerScreenPictureInPicture() {
    const auto pip = [](float w, float h) {
        return ::Oot3d::Renderer::PlaceUiPictureInPicture(1352U, 728U, w, h);
    };
    const GfxNativePicaScreenCrop crop{0U, 320U, 240U};
    const auto placed = PlaceNativePicaScanout(pip, 320.0F, 256.0F, 320U, 256U, &crop);
    CHECK(Same(placed.Visible, pip(320.0F, 240.0F)));
    const float scale = placed.Visible.Width / 320.0F;
    CHECK(Near(placed.Viewport.X, placed.Visible.X));
    CHECK(Near(placed.Viewport.Y, placed.Visible.Y - 16.0F * scale));
    CHECK(Near(placed.Viewport.Height, 256.0F * scale));
}

// OoT3D (no crop) and any crop that is the whole output or does not fit it:
// exactly the placement of before.
void TestNoCropKeepsThePlacement() {
    const auto fit = [](float w, float h) {
        return FitNativePicaPresentation(1280U, 720U, w, h);
    };
    const auto before = fit(800.0F, 480.0F);
    const GfxNativePicaScreenCrop whole{0U, 400U, 240U};
    const GfxNativePicaScreenCrop pastTheRows{20U, 400U, 240U};
    const GfxNativePicaScreenCrop widerThanARow{0U, 400U, 256U};
    const GfxNativePicaScreenCrop empty{0U, 0U, 240U};
    for (const GfxNativePicaScreenCrop* crop :
         {static_cast<const GfxNativePicaScreenCrop*>(nullptr), &whole,
          &pastTheRows, &widerThanARow, &empty}) {
        const auto placed = PlaceNativePicaScanout(fit, 800.0F, 480.0F, 400U, 240U, crop);
        CHECK(Same(placed.Visible, before));
        CHECK(Same(placed.Viewport, before));
    }
}

} // namespace

int main() {
    TestUpperScreenInsideACopy();
    TestLowerScreenPictureInPicture();
    TestNoCropKeepsThePlacement();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::puts("native PICA screen crop tests passed");
    return 0;
}
