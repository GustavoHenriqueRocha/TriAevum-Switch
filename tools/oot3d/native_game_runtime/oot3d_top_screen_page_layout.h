#pragma once

// Where TopScreen draws the lower screen's 320x240 pages (pause Items, Gear,
// Map, System; file select, name entry) on the 400x240 top screen, and the
// inverse: the lower-screen touch X under a top-screen X. The page target
// plans, the widened Items draw and the touch mapping all read it, so what is
// drawn and what a tap selects cannot drift apart.
//
// Top-screen X is the presented column in native pixels (0..400). The
// renderer places the whole 400x240 screen on the output (centred fit,
// FitUiPresentation); this header only covers the page inside it.

#include <cstdint>

namespace Oot3dNativeGame {

inline constexpr float kTopScreenPresentedWidth = 400.0F;
// The lower screen, which is also the native touch domain.
inline constexpr float kTopScreenPageWidth = 320.0F;

// Centred page: TopScreen relocates a lower-screen page into the top
// target's viewport (0, 40, 480, 320); the rotated target's Y is the
// presented X, so page X 0..320 lands on top-screen X 40..360, one native
// pixel per page pixel (the page's own 320-wide orthographic).
inline constexpr std::uint32_t kTopScreenCentredPageX = 40U;
inline constexpr std::uint32_t kTopScreenPresentedWidthPixels = 400U;
inline constexpr std::uint32_t kTopScreenPageWidthPixels = 320U;
static_assert(kTopScreenCentredPageX * 2U + kTopScreenPageWidthPixels ==
              kTopScreenPresentedWidthPixels);

// Wide pages (2.1.1 0x005D32A4: Items, dungeon Map, System) take the whole
// viewport (0, 0, 480, 400) and the payload's projection row
// clip = kTopScreenWidePageScale * x + offset (0x005CACA0), which puts page
// X on top-screen X 200 * (1 - offset) + x.
inline constexpr float kTopScreenWidePageScale = -0.005F;      // 0xBBA3D70A
inline constexpr float kTopScreenWidePageOffset = 0.8F;        // 0x3F4CCCCD
inline constexpr float kTopScreenWideItemsOffset = 0.6F;       // 0x3F19999A
inline constexpr float kTopScreenWideItemsCursorOffset = 0.685F; // 0x3F2F5C29

[[nodiscard]] constexpr float TopScreenWidePageX(float offset) noexcept {
  return (1.0F - offset) * kTopScreenPresentedWidth * 0.5F;
}

// One page pixel per presented pixel, as on the centred page.
static_assert(kTopScreenWidePageScale * kTopScreenPresentedWidth * 0.5F > -1.0001F &&
              kTopScreenWidePageScale * kTopScreenPresentedWidth * 0.5F < -0.9999F);
// The page offset reproduces the centred page (X + 40); dungeon Map and
// System therefore keep the centred layout.
static_assert(TopScreenWidePageX(kTopScreenWidePageOffset) >
                  kTopScreenCentredPageX - 0.001F &&
              TopScreenWidePageX(kTopScreenWidePageOffset) <
                  kTopScreenCentredPageX + 0.001F);

// The widened Items page (0x005D19F8): its draw uses the offset 0.6 (X + 80),
// and on the page's model streams the quads left of X 256 move first: the
// background columns by -40 (X + 40, where the centred page had them), the
// item grid and its icons by -17 (X + 63, the grid cursor's 0.685). What
// starts at X 256 or later, the special sixth column, stays at X + 80.
inline constexpr float kTopScreenWideItemsLeftRegionEnd = 256.0F;
inline constexpr float kTopScreenWideItemsBackgroundShift = -40.0F;
inline constexpr float kTopScreenWideItemsGridShift = -17.0F;

static_assert(TopScreenWidePageX(kTopScreenWideItemsOffset) +
                      kTopScreenWideItemsGridShift >
                  TopScreenWidePageX(kTopScreenWideItemsCursorOffset) - 0.001F &&
              TopScreenWidePageX(kTopScreenWideItemsOffset) +
                      kTopScreenWideItemsGridShift <
                  TopScreenWidePageX(kTopScreenWideItemsCursorOffset) + 0.001F);

enum class TopScreenPageLayout : std::uint8_t {
  // Centred pages, the relocated frontend menus, and the wide dungeon Map and
  // System (whose offset 0.8 is the centred placement).
  Centred,
  // The widened Items page.
  WideItems,
};

// Top-screen X of the interactive content at page (touch) X.
[[nodiscard]] constexpr float TopScreenPageToPresentedX(
    float pageX, TopScreenPageLayout layout) noexcept {
  if (layout == TopScreenPageLayout::Centred)
    return static_cast<float>(kTopScreenCentredPageX) + pageX;
  const float items = TopScreenWidePageX(kTopScreenWideItemsOffset);
  return pageX < kTopScreenWideItemsLeftRegionEnd
             ? items + kTopScreenWideItemsGridShift + pageX
             : items + pageX;
}

// The inverse: the page (touch) X under a top-screen X. On the widened Items
// page a point between the grid's region and the sixth column (top-screen X
// 319..336, where neither is drawn) maps to X 256, between them as well.
[[nodiscard]] constexpr float TopScreenPresentedToPageX(
    float presentedX, TopScreenPageLayout layout) noexcept {
  if (layout == TopScreenPageLayout::Centred)
    return presentedX - static_cast<float>(kTopScreenCentredPageX);
  const float items = TopScreenWidePageX(kTopScreenWideItemsOffset);
  const float grid = presentedX - (items + kTopScreenWideItemsGridShift);
  if (grid < kTopScreenWideItemsLeftRegionEnd)
    return grid;
  const float right = presentedX - items;
  return right < kTopScreenWideItemsLeftRegionEnd
             ? kTopScreenWideItemsLeftRegionEnd
             : right;
}

} // namespace Oot3dNativeGame
