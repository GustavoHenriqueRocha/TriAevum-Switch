#pragma once

// config/skip_bottom_screen (UI profile TopScreen only): the 3DS's lower
// screen (a 240x320 PICA framebuffer) is never shown on the Switch in that
// profile (ShouldPresentNativeBottomFrontend is false; the TopScreen HUD and
// menus are drawn from guest UI primitives, not from that target), yet its
// draws cost ~2.7 ms of GPU and ~0.5 ms of render thread per step frame
// (tour2, glperf pica_bottom). This policy picks, per visual sample, the
// draws, fills and display transfers that only produce the lower screen.
//
// Conservative, all or nothing per sample: nothing is skipped when anything
// else in the sample may consume the lower target, i.e. when
// - a draw outside it samples a texture overlapping its color or depth;
// - a draw outside it renders into memory overlapping it;
// - a transfer out of it lands on memory a drawn target or texture uses, or
//   on the top screen's transfer.
// A use in a later sample (a texture sampled frames later) is not seen:
// that is the risk the config key exists for.
//
// Pure (no GL, no plan types): the scheduler adapts its sample to these
// records (oot3d_native_pica_presentation_scheduler.cpp); host tests in
// oot3d_native_bottom_screen_skip_tests.cpp.

#include <array>
#include <cstdint>
#include <vector>

namespace Oot3dNativeGame {

struct BottomSkipRange {
  uint32_t Begin = 0U;
  uint32_t End = 0U; // exclusive; Begin == End is empty

  [[nodiscard]] bool Empty() const noexcept { return End <= Begin; }
  [[nodiscard]] bool Overlaps(const BottomSkipRange &other) const noexcept {
    return !Empty() && !other.Empty() && Begin < other.End &&
           other.Begin < End;
  }
  [[nodiscard]] bool Contains(const BottomSkipRange &other) const noexcept {
    return !Empty() && !other.Empty() && Begin <= other.Begin &&
           other.End <= End;
  }
};

struct BottomSkipDraw {
  uint16_t FramebufferWidth = 0U;
  uint16_t FramebufferHeight = 0U;
  BottomSkipRange Color;
  BottomSkipRange Depth;
  std::array<BottomSkipRange, 3> Textures{};
};

struct BottomSkipTransfer {
  BottomSkipRange Input;
  BottomSkipRange Output;
  bool Top = false; // the sample's top-screen transfer
};

struct BottomSkipPlan {
  bool Active = false; // something is skipped
  std::vector<uint8_t> SkipDraw;
  std::vector<uint8_t> SkipFill;
  std::vector<uint8_t> SkipTransfer;
  size_t SkippedDraws = 0U;
};

// PICA framebuffer of the lower screen (kNativeBottomScreen physical size,
// as gl_frame_profiler.cpp classifies pica_bottom).
[[nodiscard]] inline bool IsBottomScreenFramebuffer(uint16_t width,
                                                    uint16_t height) noexcept {
  return width == 240U && height == 320U;
}

[[nodiscard]] inline uint32_t BottomSkipEnd(uint32_t address,
                                            uint64_t bytes) noexcept {
  const uint64_t end = static_cast<uint64_t>(address) + bytes;
  return end > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(end);
}

// Fills `plan` (its vectors keep their capacity across calls).
inline void PlanBottomScreenSkip(const std::vector<BottomSkipDraw> &draws,
                                 const std::vector<BottomSkipRange> &fills,
                                 const std::vector<BottomSkipTransfer> &transfers,
                                 std::vector<BottomSkipRange> &bottomScratch,
                                 BottomSkipPlan &plan) {
  plan.Active = false;
  plan.SkippedDraws = 0U;
  plan.SkipDraw.assign(draws.size(), 0U);
  plan.SkipFill.assign(fills.size(), 0U);
  plan.SkipTransfer.assign(transfers.size(), 0U);
  bottomScratch.clear();
  for (const auto &draw : draws) {
    if (!IsBottomScreenFramebuffer(draw.FramebufferWidth,
                                   draw.FramebufferHeight))
      continue;
    if (!draw.Color.Empty())
      bottomScratch.push_back(draw.Color);
    if (!draw.Depth.Empty())
      bottomScratch.push_back(draw.Depth);
  }
  if (bottomScratch.empty())
    return;
  const auto touchesBottom = [&](const BottomSkipRange &range) {
    for (const auto &bottom : bottomScratch) {
      if (bottom.Overlaps(range))
        return true;
    }
    return false;
  };
  const auto insideBottom = [&](const BottomSkipRange &range) {
    for (const auto &bottom : bottomScratch) {
      if (bottom.Contains(range))
        return true;
    }
    return false;
  };
  // Everything the rest of the sample reads or writes.
  const auto usedOutside = [&](const BottomSkipRange &range) {
    for (const auto &draw : draws) {
      if (IsBottomScreenFramebuffer(draw.FramebufferWidth,
                                    draw.FramebufferHeight))
        continue;
      if (draw.Color.Overlaps(range) || draw.Depth.Overlaps(range))
        return true;
      for (const auto &texture : draw.Textures) {
        if (texture.Overlaps(range))
          return true;
      }
    }
    return false;
  };
  for (const auto &bottom : bottomScratch) {
    if (usedOutside(bottom))
      return;
  }
  for (size_t index = 0U; index < transfers.size(); ++index) {
    const auto &transfer = transfers[index];
    if (!touchesBottom(transfer.Input))
      continue;
    // Out of the lower target: starts inside it (the extent is an upper
    // bound). One that only runs into it reads it from elsewhere.
    const BottomSkipRange start{transfer.Input.Begin,
                                BottomSkipEnd(transfer.Input.Begin, 1U)};
    if (!insideBottom(start) || transfer.Top ||
        touchesBottom(transfer.Output) ||
        usedOutside(transfer.Output))
      return;
    plan.SkipTransfer[index] = 1U;
  }
  for (size_t index = 0U; index < draws.size(); ++index) {
    if (IsBottomScreenFramebuffer(draws[index].FramebufferWidth,
                                  draws[index].FramebufferHeight)) {
      plan.SkipDraw[index] = 1U;
      ++plan.SkippedDraws;
    }
  }
  for (size_t index = 0U; index < fills.size(); ++index) {
    if (insideBottom(fills[index]))
      plan.SkipFill[index] = 1U;
  }
  plan.Active = true;
}

} // namespace Oot3dNativeGame
