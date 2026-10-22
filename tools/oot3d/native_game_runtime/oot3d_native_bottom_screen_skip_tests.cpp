// config/skip_bottom_screen: which draws, fills and transfers of a sample
// only produce the 3DS's lower screen (oot3d_native_bottom_screen_skip.h).

#include "oot3d_native_bottom_screen_skip.h"

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

constexpr uint32_t kTopColor = 0x18000000U;
constexpr uint32_t kTopDepth = 0x18100000U;
constexpr uint32_t kBottomColor = 0x18200000U;
constexpr uint32_t kBottomDepth = 0x18300000U;
constexpr uint32_t kTexture = 0x20000000U;
constexpr uint32_t kTopDisplay = 0x1F000000U;
constexpr uint32_t kBottomDisplay = 0x1F100000U;

BottomSkipRange Range(uint32_t address, uint32_t bytes) {
  return {address, BottomSkipEnd(address, bytes)};
}

BottomSkipDraw TopDraw() {
  BottomSkipDraw draw;
  draw.FramebufferWidth = 480U;
  draw.FramebufferHeight = 400U;
  draw.Color = Range(kTopColor, 480U * 400U * 4U);
  draw.Depth = Range(kTopDepth, 480U * 400U * 4U);
  draw.Textures[0] = Range(kTexture, 64U * 64U * 4U);
  return draw;
}

BottomSkipDraw BottomDraw() {
  BottomSkipDraw draw;
  draw.FramebufferWidth = 240U;
  draw.FramebufferHeight = 320U;
  draw.Color = Range(kBottomColor, 240U * 320U * 4U);
  draw.Depth = Range(kBottomDepth, 240U * 320U * 4U);
  draw.Textures[0] = Range(kTexture + 0x10000U, 32U * 32U * 4U);
  return draw;
}

struct Sample {
  std::vector<BottomSkipDraw> Draws;
  std::vector<BottomSkipRange> Fills;
  std::vector<BottomSkipTransfer> Transfers;
};

Sample Usual() {
  Sample sample;
  sample.Draws = {TopDraw(), BottomDraw(), TopDraw(), BottomDraw()};
  sample.Fills = {Range(kTopColor, 480U * 400U * 4U),
                  Range(kBottomColor, 240U * 320U * 4U),
                  Range(kBottomDepth, 240U * 320U * 4U)};
  sample.Transfers = {
      {Range(kTopColor, 480U * 400U * 4U), Range(kTopDisplay, 400U * 240U * 4U),
       true},
      {Range(kBottomColor, 240U * 320U * 4U),
       Range(kBottomDisplay, 320U * 240U * 4U), false}};
  return sample;
}

BottomSkipPlan Plan(const Sample &sample) {
  std::vector<BottomSkipRange> scratch;
  BottomSkipPlan plan;
  PlanBottomScreenSkip(sample.Draws, sample.Fills, sample.Transfers, scratch,
                       plan);
  return plan;
}

void TestSkipsOnlyTheLowerScreen() {
  const auto plan = Plan(Usual());
  CHECK(plan.Active);
  CHECK(plan.SkippedDraws == 2U);
  CHECK(plan.SkipDraw[0] == 0U && plan.SkipDraw[1] == 1U &&
        plan.SkipDraw[2] == 0U && plan.SkipDraw[3] == 1U);
  CHECK(plan.SkipFill[0] == 0U && plan.SkipFill[1] == 1U &&
        plan.SkipFill[2] == 1U);
  CHECK(plan.SkipTransfer[0] == 0U && plan.SkipTransfer[1] == 1U);
}

void TestNothingWithoutLowerScreen() {
  Sample sample;
  sample.Draws = {TopDraw()};
  const auto plan = Plan(sample);
  CHECK(!plan.Active);
  CHECK(plan.SkippedDraws == 0U);
}

void TestKeptWhenTheTopSamplesIt() {
  auto sample = Usual();
  sample.Draws[2].Textures[1] = Range(kBottomColor + 0x1000U, 0x1000U);
  CHECK(!Plan(sample).Active);
  sample = Usual();
  sample.Draws[0].Textures[2] = Range(kBottomDepth, 0x100U);
  CHECK(!Plan(sample).Active);
}

void TestKeptWhenTheTopWritesIntoIt() {
  auto sample = Usual();
  sample.Draws[0].Color = Range(kBottomColor - 0x100U, 0x200U);
  CHECK(!Plan(sample).Active);
}

void TestKeptWhenCopiedWhereTheTopReads() {
  // Lower screen copied into a texture the top screen samples.
  auto sample = Usual();
  sample.Transfers[1].Output = Range(kTexture, 0x100U);
  CHECK(!Plan(sample).Active);
  // ... or onto the top target, or as the top transfer itself.
  sample = Usual();
  sample.Transfers[1].Output = Range(kTopColor, 0x100U);
  CHECK(!Plan(sample).Active);
  sample = Usual();
  sample.Transfers[1].Top = true;
  CHECK(!Plan(sample).Active);
}

void TestKeptWhenATransferRunsIntoIt() {
  // Starts outside the lower target but its extent reaches it.
  auto sample = Usual();
  sample.Transfers.push_back(
      {Range(kBottomColor - 0x100U, 0x1000U), Range(0x30000000U, 0x1000U),
       false});
  CHECK(!Plan(sample).Active);
}

void TestPartialFillKept() {
  auto sample = Usual();
  sample.Fills.push_back(Range(kBottomColor - 0x10U, 0x100U));
  const auto plan = Plan(sample);
  CHECK(plan.Active);
  CHECK(plan.SkipFill[3] == 0U);
}

void TestScratchReusedAcrossSamples() {
  std::vector<BottomSkipRange> scratch;
  BottomSkipPlan plan;
  const auto usual = Usual();
  PlanBottomScreenSkip(usual.Draws, usual.Fills, usual.Transfers, scratch,
                       plan);
  CHECK(plan.Active);
  Sample top;
  top.Draws = {TopDraw()};
  PlanBottomScreenSkip(top.Draws, top.Fills, top.Transfers, scratch, plan);
  CHECK(!plan.Active && plan.SkipDraw.size() == 1U &&
        plan.SkipDraw[0] == 0U && plan.SkippedDraws == 0U);
}

// Console tour3 (fps60fix.nro): "PICA display transfer source is missing"
// ~5 s after boot. The top transfer, submitted apart from the sample's
// transfers, read the skipped lower target; and a transfer of a later sample
// can read one skipped earlier.
void TestTopTransferReadingTheLowerTargetKeepsIt() {
  auto sample = Usual();
  // The scheduler appends the top transfer when it is not among them.
  sample.Transfers.push_back({Range(kBottomColor, 240U * 320U * 4U),
                              Range(kTopDisplay, 400U * 240U * 4U), true});
  CHECK(!Plan(sample).Active);
}

// A backend that throws (here: counts) like GfxRenderingAPIOGL when a
// transfer's source target was never drawn.
struct FakeBackend {
  std::vector<uint32_t> Targets;
  int MissingSources = 0;
  void Draw(uint32_t color) {
    for (auto target : Targets)
      if (target == color)
        return;
    Targets.push_back(color);
  }
  void Transfer(uint32_t input) {
    for (auto target : Targets)
      if (target == input)
        return;
    ++MissingSources;
  }
};

void TestLaterTransferOfASkippedTargetIsLeftOut() {
  constexpr uint64_t kNamespace = 7U;
  BottomSkipTracker tracker;
  FakeBackend backend;
  // Sample 1: lower screen skipped (its own transfer too).
  const auto plan = Plan(Usual());
  CHECK(plan.Active);
  backend.Draw(kTopColor);
  tracker.Skipped(kNamespace, kBottomColor);
  backend.Transfer(kTopColor);
  // Sample 2: no lower-screen draw, a transfer out of it.
  if (tracker.ForTransfer(kNamespace, kBottomColor) ==
      BottomSkipTracker::TransferAction::Submit)
    backend.Transfer(kBottomColor);
  CHECK(backend.MissingSources == 0);
  CHECK(tracker.SkippedTransfers() == 1U);
  // From now on that target is drawn.
  CHECK(tracker.Unsafe(kNamespace, kBottomColor));
  CHECK(!tracker.Unsafe(kNamespace + 1U, kBottomColor));
  // A target drawn again has its image: its transfers go through.
  tracker.Drawn(kNamespace, kBottomColor);
  CHECK(tracker.ForTransfer(kNamespace, kBottomColor) ==
        BottomSkipTracker::TransferAction::Submit);
  CHECK(tracker.ForTransfer(kNamespace, kTopColor) ==
        BottomSkipTracker::TransferAction::Submit);
  tracker.Reset();
  CHECK(!tracker.Unsafe(kNamespace, kBottomColor));
}

} // namespace

int main() {
  TestSkipsOnlyTheLowerScreen();
  TestNothingWithoutLowerScreen();
  TestKeptWhenTheTopSamplesIt();
  TestKeptWhenTheTopWritesIntoIt();
  TestKeptWhenCopiedWhereTheTopReads();
  TestKeptWhenATransferRunsIntoIt();
  TestPartialFillKept();
  TestScratchReusedAcrossSamples();
  TestTopTransferReadingTheLowerTargetKeepsIt();
  TestLaterTransferOfASkippedTargetIsLeftOut();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_native_bottom_screen_skip_tests: ok\n");
  return 0;
}
