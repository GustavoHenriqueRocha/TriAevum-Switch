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
// The sample's top transfer counts as one of its transfers even when it is
// not among them (the scheduler submits it apart): reading the lower target
// keeps everything.
//
// Across samples, BottomSkipTracker remembers which lower targets hold no
// GPU image because their draws were skipped. A later transfer out of one
// (the GL backend threw "PICA display transfer source is missing" ~5 s
// after boot, console tour3) is left out instead of submitted, and the
// target is never skipped again; a texture sampled frames later is still
// not seen: that is the risk the config key exists for.
//
// Pure (no GL, no plan types): the scheduler adapts its sample to these
// records (oot3d_native_pica_presentation_scheduler.cpp); host tests in
// oot3d_native_bottom_screen_skip_tests.cpp.

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
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

// Why a sample with lower-screen draws kept them (perf.txt bottom_skip:).
enum class BottomSkipReason : uint8_t {
  Skipped,      // lower screen left out
  NoBottom,     // no lower-screen draw in the sample
  Unsafe,       // a target read earlier without its image (tracker)
  ColorShared,  // another draw writes into the lower color target
  DepthShared,  // another draw writes colour into the lower depth
  TextureRead,  // another draw samples the lower color or depth
  TransferInto, // a transfer only runs into the lower target
  TransferTop,  // the top-screen transfer reads it
  TransferUsed, // a transfer out of it lands where something reads
  Count,
};

[[nodiscard]] inline const char *BottomSkipReasonName(
    BottomSkipReason reason) noexcept {
  switch (reason) {
  case BottomSkipReason::Skipped: return "skipped";
  case BottomSkipReason::NoBottom: return "no_bottom";
  case BottomSkipReason::Unsafe: return "unsafe";
  case BottomSkipReason::ColorShared: return "color_shared";
  case BottomSkipReason::DepthShared: return "depth_shared";
  case BottomSkipReason::TextureRead: return "texture_read";
  case BottomSkipReason::TransferInto: return "transfer_into";
  case BottomSkipReason::TransferTop: return "transfer_top";
  case BottomSkipReason::TransferUsed: return "transfer_used";
  case BottomSkipReason::Count: break;
  }
  return "?";
}

struct BottomSkipPlan {
  bool Active = false; // something is skipped
  BottomSkipReason Reason = BottomSkipReason::NoBottom;
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
  plan.Reason = BottomSkipReason::NoBottom;
  plan.SkippedDraws = 0U;
  plan.SkipDraw.assign(draws.size(), 0U);
  plan.SkipFill.assign(fills.size(), 0U);
  plan.SkipTransfer.assign(transfers.size(), 0U);
  // Color ranges first, then depth ranges (colorCount splits them).
  bottomScratch.clear();
  for (const auto &draw : draws) {
    if (IsBottomScreenFramebuffer(draw.FramebufferWidth,
                                  draw.FramebufferHeight) &&
        !draw.Color.Empty())
      bottomScratch.push_back(draw.Color);
  }
  const size_t colorCount = bottomScratch.size();
  for (const auto &draw : draws) {
    if (IsBottomScreenFramebuffer(draw.FramebufferWidth,
                                  draw.FramebufferHeight) &&
        !draw.Depth.Empty())
      bottomScratch.push_back(draw.Depth);
  }
  if (bottomScratch.empty())
    return;
  const auto isBottom = [](const BottomSkipDraw &draw) {
    return IsBottomScreenFramebuffer(draw.FramebufferWidth,
                                     draw.FramebufferHeight);
  };
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
  // Anything another draw reads or writes.
  const auto usedOutside = [&](const BottomSkipRange &range) {
    for (const auto &draw : draws) {
      if (isBottom(draw))
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
  const auto refuse = [&](BottomSkipReason reason) { plan.Reason = reason; };
  for (size_t index = 0U; index < bottomScratch.size(); ++index) {
    const auto &bottom = bottomScratch[index];
    const bool color = index < colorCount;
    for (const auto &draw : draws) {
      if (isBottom(draw))
        continue;
      for (const auto &texture : draw.Textures) {
        if (texture.Overlaps(bottom))
          return refuse(BottomSkipReason::TextureRead);
      }
      // A depth buffer shared by both screens (one PICA depth target for
      // the upper and the lower framebuffer) is fine: the lower screen's
      // depth values are only ever tested by lower-screen draws, and a
      // clear that also covers the upper use is kept (fills below).
      if (color && (draw.Color.Overlaps(bottom) || draw.Depth.Overlaps(bottom)))
        return refuse(BottomSkipReason::ColorShared);
      if (!color && draw.Color.Overlaps(bottom))
        return refuse(BottomSkipReason::DepthShared);
    }
  }
  for (size_t index = 0U; index < transfers.size(); ++index) {
    const auto &transfer = transfers[index];
    if (!touchesBottom(transfer.Input))
      continue;
    // Out of the lower target: starts inside it (the extent is an upper
    // bound). One that only runs into it reads it from elsewhere.
    const BottomSkipRange start{transfer.Input.Begin,
                                BottomSkipEnd(transfer.Input.Begin, 1U)};
    if (!insideBottom(start))
      return refuse(BottomSkipReason::TransferInto);
    if (transfer.Top)
      return refuse(BottomSkipReason::TransferTop);
    if (touchesBottom(transfer.Output) || usedOutside(transfer.Output))
      return refuse(BottomSkipReason::TransferUsed);
    plan.SkipTransfer[index] = 1U;
  }
  for (size_t index = 0U; index < draws.size(); ++index) {
    if (isBottom(draws[index])) {
      plan.SkipDraw[index] = 1U;
      ++plan.SkippedDraws;
    }
  }
  // Only fills nothing else touches (a clear of a shared depth stays).
  for (size_t index = 0U; index < fills.size(); ++index) {
    if (insideBottom(fills[index]) && !usedOutside(fills[index]))
      plan.SkipFill[index] = 1U;
  }
  plan.Active = true;
  plan.Reason = BottomSkipReason::Skipped;
}

// Lower targets whose latest content was skipped, per render-target
// namespace (render thread only; a handful of entries).
class BottomSkipTracker final {
public:
  enum class TransferAction : uint8_t {
    Submit,
    // Its source was never drawn: leave the transfer (and, for the top
    // one, the presentation) out.
    Skip,
  };

  // Skipping a target someone once read without its image: never again.
  [[nodiscard]] bool Unsafe(uint64_t targetNamespace,
                            uint32_t colorAddress) const noexcept {
    return Has(mUnsafe, {targetNamespace, colorAddress});
  }
  void Skipped(uint64_t targetNamespace, uint32_t colorAddress) {
    const Key key{targetNamespace, colorAddress};
    if (!Has(mSkipped, key))
      mSkipped.push_back(key);
  }
  void Drawn(uint64_t targetNamespace, uint32_t colorAddress) {
    Erase(mSkipped, {targetNamespace, colorAddress});
  }
  [[nodiscard]] TransferAction ForTransfer(uint64_t targetNamespace,
                                           uint32_t inputAddress) {
    const Key key{targetNamespace, inputAddress};
    if (!Has(mSkipped, key))
      return TransferAction::Submit;
    if (!Has(mUnsafe, key))
      mUnsafe.push_back(key);
    ++mSkippedTransfers;
    return TransferAction::Skip;
  }
  [[nodiscard]] uint64_t SkippedTransfers() const noexcept {
    return mSkippedTransfers;
  }
  void Reset() {
    mSkipped.clear();
    mUnsafe.clear();
    mSkippedTransfers = 0U;
  }

private:
  using Key = std::pair<uint64_t, uint32_t>;
  static bool Has(const std::vector<Key> &keys, const Key &key) noexcept {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
  }
  static void Erase(std::vector<Key> &keys, const Key &key) {
    keys.erase(std::remove(keys.begin(), keys.end(), key), keys.end());
  }
  std::vector<Key> mSkipped;
  std::vector<Key> mUnsafe;
  uint64_t mSkippedTransfers = 0U;
};

} // namespace Oot3dNativeGame
