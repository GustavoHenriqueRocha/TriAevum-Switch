#pragma once

// Which display transfer the window presents for a 3DS screen. The screen
// (GSP framebuffer) names an address; the transfer that wrote it is the one
// whose output starts there (OoT3D: always) or, failing that, the one whose
// output contains it a whole number of memory rows in (MK7 points its upper
// screen 0x2000 bytes = 16 rows of 0x200 into the 416-row copy at 0x1f090000;
// Azahar reads the containing surface's sub-rect, AccelerateDisplay ->
// GetSurfaceSubRect). An exact output always wins, left eye before right, so
// a title whose screens start their transfers is unchanged.
//
// A memory row of a 3DS framebuffer is one screen column (the LCDs are
// rotated): LineOffset counts columns of the transfer before the screen's
// first one.
//
// Pure, over any submission with a .Transfer (InputSize/OutputSize/Flags/
// TextureCopyBytes); host tests in
// oot3d_native_display_transfer_selection_tests.cpp.

#include <cstdint>
#include <map>

namespace Oot3dNativeGame {

// What a 3DS LCD shows from its address: columns (memory rows) of 240 pixels.
inline constexpr uint32_t kCtrTopScreenColumns = 400U;
inline constexpr uint32_t kCtrBottomScreenColumns = 320U;
inline constexpr uint32_t kCtrScreenColumnPixels = 240U;

template <typename Submission>
struct PresentedDisplayTransfer {
  const Submission* Selected = nullptr;
  uint32_t LineOffset = 0;
};

// Bytes per pixel of a display transfer's output format (flags 12..14).
inline uint32_t DisplayTransferOutputBytesPerPixel(uint32_t flags) {
  switch ((flags >> 12U) & 7U) {
    case 0U: return 4U;  // RGBA8
    case 1U: return 3U;  // RGB8
    default: return 2U;  // RGB565, RGB5A1, RGBA4
  }
}

template <typename Submission>
PresentedDisplayTransfer<Submission> SelectPresentedDisplayTransfer(
    const std::map<uint32_t, Submission>& byOutput, uint32_t addressLeft,
    uint32_t addressRight, uint32_t stride) {
  for (const uint32_t address : {addressLeft, addressRight}) {
    const auto exact = byOutput.find(address);
    if (exact != byOutput.end()) return {&exact->second, 0U};
  }
  for (const uint32_t address : {addressLeft, addressRight}) {
    auto containing = byOutput.upper_bound(address);
    if (containing == byOutput.begin()) continue;
    --containing;
    const auto& transfer = containing->second.Transfer;
    // A raw TextureCopy has no rows; the screen must read the copy's rows.
    if (transfer.TextureCopyBytes != 0U) continue;
    const uint32_t scaling = (transfer.Flags >> 24U) & 3U;
    const uint32_t width =
        (transfer.OutputSize & 0xFFFFU) >> (scaling != 0U ? 1U : 0U);
    const uint32_t rows =
        (transfer.OutputSize >> 16U) >> (scaling == 2U ? 1U : 0U);
    const uint32_t rowBytes =
        width * DisplayTransferOutputBytesPerPixel(transfer.Flags);
    if (rowBytes == 0U || rowBytes != stride) continue;
    const uint32_t delta = address - containing->first;
    if (delta % rowBytes != 0U || delta / rowBytes >= rows) continue;
    return {&containing->second, delta / rowBytes};
  }
  return {};
}

} // namespace Oot3dNativeGame
