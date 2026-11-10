// Which display transfer the window presents for a screen
// (oot3d_native_display_transfer_selection.h).

#include "oot3d_native_display_transfer_selection.h"

#include <cstdio>

namespace {

int gFailures = 0;

#define CHECK(condition)                                                      \
  do {                                                                        \
    if (!(condition)) {                                                       \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__,          \
                   #condition);                                               \
      ++gFailures;                                                            \
    }                                                                         \
  } while (false)

using namespace Oot3dNativeGame;

struct FakeTransfer {
  uint32_t InputSize = 0;
  uint32_t OutputSize = 0;
  uint32_t Flags = 0;
  uint32_t TextureCopyBytes = 0;
};

struct FakeSubmission {
  int Id = 0;
  FakeTransfer Transfer;
};

// RGB565 in and out (flags 0x2200), as MK7's screen copies.
constexpr uint32_t kRgb565 = 0x2200U;
constexpr uint32_t kStride = 0x200U;  // 256 px x 2 bytes

FakeSubmission Copy(int id, uint32_t rows, uint32_t flags = kRgb565) {
  FakeSubmission submission;
  submission.Id = id;
  submission.Transfer.InputSize = (rows << 16U) | 0x100U;
  submission.Transfer.OutputSize = (rows << 16U) | 0x100U;
  submission.Transfer.Flags = flags;
  return submission;
}

int SelectedId(const PresentedDisplayTransfer<FakeSubmission>& selected) {
  return selected.Selected != nullptr ? selected.Selected->Id : 0;
}

void TestExactOutputWinsLeftBeforeRight() {
  std::map<uint32_t, FakeSubmission> byOutput{
      {0x1F000000U, Copy(1, 400U)}, {0x1F034000U, Copy(2, 400U)}};
  auto selected =
      SelectPresentedDisplayTransfer(byOutput, 0x1F000000U, 0x1F034000U, kStride);
  CHECK(SelectedId(selected) == 1);
  CHECK(selected.LineOffset == 0U);
  selected =
      SelectPresentedDisplayTransfer(byOutput, 0x1F090000U, 0x1F034000U, kStride);
  CHECK(SelectedId(selected) == 2);
  CHECK(selected.LineOffset == 0U);
}

// MK7: upper screen at 0x1f092000, 16 rows into the 416-row copy.
void TestScreenInsideACopy() {
  std::map<uint32_t, FakeSubmission> byOutput{
      {0x1F000000U, Copy(1, 416U)},
      {0x1F090000U, Copy(2, 416U)},
      {0x1F300000U, Copy(3, 320U)}};
  const auto selected =
      SelectPresentedDisplayTransfer(byOutput, 0x1F092000U, 0x1F0C6000U, kStride);
  CHECK(SelectedId(selected) == 2);
  CHECK(selected.LineOffset == 16U);
}

// An exact right eye beats a left eye inside another copy.
void TestExactRightBeatsContainingLeft() {
  std::map<uint32_t, FakeSubmission> byOutput{
      {0x1F090000U, Copy(1, 416U)}, {0x1F0C6000U, Copy(2, 400U)}};
  const auto selected =
      SelectPresentedDisplayTransfer(byOutput, 0x1F092000U, 0x1F0C6000U, kStride);
  CHECK(SelectedId(selected) == 2);
  CHECK(selected.LineOffset == 0U);
}

void TestNotInsideAnyCopy() {
  std::map<uint32_t, FakeSubmission> byOutput{{0x1F090000U, Copy(1, 416U)}};
  // Past the copy's last row (416 rows of 0x200 end at 0x1f0c4000).
  CHECK(SelectPresentedDisplayTransfer(byOutput, 0x1F0C4000U, 0x1F0C4000U,
                                       kStride).Selected == nullptr);
  // Before every copy (a CPU framebuffer in FCRAM).
  CHECK(SelectPresentedDisplayTransfer(byOutput, 0x1780C360U, 0x1780BBE0U,
                                       kStride).Selected == nullptr);
  // Not on a row boundary.
  CHECK(SelectPresentedDisplayTransfer(byOutput, 0x1F090100U, 0x1F090100U,
                                       kStride).Selected == nullptr);
  // The screen's rows are not the copy's rows.
  CHECK(SelectPresentedDisplayTransfer(byOutput, 0x1F092000U, 0x1F092000U,
                                       0x1E0U).Selected == nullptr);
}

void TestCopyRowsFollowFormatScalingAndKind() {
  // RGBA8 output: 256 px = 0x400 bytes per row.
  std::map<uint32_t, FakeSubmission> rgba{
      {0x1F000000U, Copy(1, 400U, 0x0000U)}};
  auto selected =
      SelectPresentedDisplayTransfer(rgba, 0x1F004000U, 0x1F004000U, 0x400U);
  CHECK(SelectedId(selected) == 1);
  CHECK(selected.LineOffset == 16U);
  // Downscaled in both axes (flags 24..25 = 2): 832x512 in, 416x256 out.
  FakeSubmission scaled = Copy(2, 832U, kRgb565 | (2U << 24U));
  scaled.Transfer.OutputSize = (832U << 16U) | 0x200U;
  std::map<uint32_t, FakeSubmission> downscaled{{0x1F090000U, scaled}};
  selected = SelectPresentedDisplayTransfer(downscaled, 0x1F092000U,
                                            0x1F092000U, kStride);
  CHECK(SelectedId(selected) == 2);
  CHECK(selected.LineOffset == 16U);
  CHECK(SelectPresentedDisplayTransfer(downscaled, 0x1F0C4000U, 0x1F0C4000U,
                                       kStride).Selected == nullptr);
  // A raw TextureCopy has no rows to contain the screen.
  FakeSubmission raw = Copy(3, 416U);
  raw.Transfer.TextureCopyBytes = 0x34000U;
  std::map<uint32_t, FakeSubmission> textureCopy{{0x1F090000U, raw}};
  CHECK(SelectPresentedDisplayTransfer(textureCopy, 0x1F092000U, 0x1F092000U,
                                       kStride).Selected == nullptr);
}

} // namespace

int main() {
  TestExactOutputWinsLeftBeforeRight();
  TestScreenInsideACopy();
  TestExactRightBeatsContainingLeft();
  TestNotInsideAnyCopy();
  TestCopyRowsFollowFormatScalingAndKind();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::puts("display transfer selection tests passed");
  return 0;
}
