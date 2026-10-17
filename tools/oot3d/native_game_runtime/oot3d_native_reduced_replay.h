#pragma once

// Whether an in-between sample may be replayed at the reduced scale
// (config/interp_scale): adapts a visual sample to SampleClearsEveryTarget
// (oot3d_native_interpolation_pacing.h). Render thread only; the scratch
// vectors belong to the caller (the session), so nothing here is shared.

#include "oot3d_native_interpolation_pacing.h"
#include "oot3d_native_pica_visual_frame.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Oot3dNativeGame {

struct ReducedReplayScratch {
  std::vector<ReducedReplayDraw> Draws;
  std::vector<ReducedReplayFill> Fills;
};

[[nodiscard]] inline uint32_t ReducedReplayColorBytes(uint8_t format) noexcept {
  return format == 0U ? 4U : (format == 1U ? 3U : 2U);
}
[[nodiscard]] inline uint32_t ReducedReplayDepthBytes(uint8_t format) noexcept {
  return format == 0U ? 2U : (format == 2U ? 3U : 4U);
}

[[nodiscard]] inline bool
SampleSuitsReducedReplay(const Oot3dPicaVisualFrameViewSample &sample,
                         ReducedReplayScratch &scratch) {
  scratch.Draws.clear();
  scratch.Fills.clear();
  scratch.Draws.reserve(sample.Draws.size());
  scratch.Fills.reserve(sample.MemoryFills.size());
  const auto end = [](uint32_t address, uint64_t bytes) {
    return static_cast<uint32_t>(
        std::min<uint64_t>(UINT32_MAX, static_cast<uint64_t>(address) + bytes));
  };
  for (const auto &view : sample.Draws) {
    if (view.BasePlan == nullptr)
      return false;
    const auto &plan = *view.BasePlan;
    const auto &framebuffer = plan.State.Framebuffer;
    const auto &merger = plan.State.OutputMerger;
    const uint64_t pixels =
        static_cast<uint64_t>(framebuffer.Width) * framebuffer.Height;
    ReducedReplayDraw draw;
    draw.SubmissionId = plan.SubmissionId;
    draw.ColorAddress = framebuffer.ColorPhysicalAddress;
    draw.ColorEnd = end(framebuffer.ColorPhysicalAddress,
                        pixels * ReducedReplayColorBytes(framebuffer.ColorFormat));
    draw.DepthAddress = framebuffer.DepthPhysicalAddress;
    draw.DepthEnd = end(framebuffer.DepthPhysicalAddress,
                        pixels * ReducedReplayDepthBytes(framebuffer.DepthFormat));
    draw.UsesDepthStencil =
        framebuffer.DepthPhysicalAddress != 0U &&
        (framebuffer.DepthStencilWriteEnabled || merger.Depth.TestEnabled ||
         merger.Depth.WriteEnabled || merger.Stencil.Enabled);
    scratch.Draws.push_back(draw);
  }
  for (const auto &fill : sample.MemoryFills) {
    scratch.Fills.push_back({fill.BeforeDrawSubmissionId,
                             fill.StartPhysicalAddress,
                             fill.EndPhysicalAddress});
  }
  return SampleClearsEveryTarget(scratch.Draws, scratch.Fills);
}

} // namespace Oot3dNativeGame
