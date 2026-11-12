// The TopScreen pause menu at 60 FPS (oot3d_top_screen_menu_cadence.h): with a
// pause page up, the in-between frame shows the step image again, outside the
// pacing governor; everything else keeps its 60 FPS presentation.
#include "oot3d_top_screen_menu_cadence.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

using Oot3dNativeGame::InBetweenSource;
using Oot3dNativeGame::InterpolatedSampleLevel;
using Oot3dNativeGame::InterpolationPacingConfig;
using Oot3dNativeGame::InterpolationPacingGovernor;
using Oot3dNativeGame::RenderFrameKind;
using Oot3dNativeGame::TopScreenMenuCadenceInputs;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// The four page owners ReadTopScreenPauseDrawInputs reads as page gates
// (Gear, Items, Map, System), and the two other child states it reads.
constexpr std::array<std::uint32_t, 4> kPageGates{0x00504484U, 0x0050672CU, 0x00506CE8U,
                                                  0x0050A530U};
constexpr std::uint32_t kBackdropSkip = 0x005043F4U;
constexpr std::uint32_t kTouchController = 0x0050AF68U;

TopScreenMenuCadenceInputs SixtyTopScreen() {
  return {.TopScreenProfile = true, .Interpolation = true, .MenuInterpolation = false};
}

void TestHoldNeedsTopScreenSixtyAndAPage() {
  auto inputs = SixtyTopScreen();
  inputs.PausePageActive = true;
  CHECK(Oot3dNativeGame::HoldTopScreenMenuInBetween(inputs));
  auto thirty = inputs;
  thirty.Interpolation = false;
  CHECK(!Oot3dNativeGame::HoldTopScreenMenuInBetween(thirty));
  auto otherProfile = inputs;
  otherProfile.TopScreenProfile = false;
  CHECK(!Oot3dNativeGame::HoldTopScreenMenuInBetween(otherProfile));
  auto interpolatedMenu = inputs;
  interpolatedMenu.MenuInterpolation = true;
  CHECK(!Oot3dNativeGame::HoldTopScreenMenuInBetween(interpolatedMenu));
  auto gameplay = inputs;
  gameplay.PausePageActive = false;
  CHECK(!Oot3dNativeGame::HoldTopScreenMenuInBetween(gameplay));
}

void TestPageOwnersFromGuestMemory() {
  Oot3dNativeGame::NativeA32Memory memory;
  std::string error;
  if (!memory.MapRegion({"menu-cadence-test", 0x00500000U, 0x00100000U, true, false, {}},
                        &error)) {
    std::fprintf(stderr, "cannot map the fixture: %s\n", error.c_str());
    std::exit(1);
  }
  // Gameplay: no page owner, the backdrop flag and the touch controller set
  // as the game keeps them (not page gates).
  CHECK(memory.Write32(kBackdropSkip, 9U) && memory.Write32(kTouchController, 2U));
  CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(memory, SixtyTopScreen()));
  for (const auto gate : kPageGates) {
    CHECK(memory.Write32(gate, 2U));
    CHECK(Oot3dNativeGame::ResolveTopScreenMenuHold(memory, SixtyTopScreen()));
    // A caller's stale PausePageActive is not trusted: the memory decides.
    auto thirty = SixtyTopScreen();
    thirty.Interpolation = false;
    thirty.PausePageActive = true;
    CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(memory, thirty));
    auto otherProfile = SixtyTopScreen();
    otherProfile.TopScreenProfile = false;
    CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(memory, otherProfile));
    auto interpolatedMenu = SixtyTopScreen();
    interpolatedMenu.MenuInterpolation = true;
    CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(memory, interpolatedMenu));
    CHECK(memory.Write32(gate, 0U));
    auto stale = SixtyTopScreen();
    stale.PausePageActive = true;
    CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(memory, stale));
  }
  // Nothing readable (no guest memory): no hold.
  Oot3dNativeGame::NativeA32Memory empty;
  CHECK(!Oot3dNativeGame::ResolveTopScreenMenuHold(empty, SixtyTopScreen()));
}

void TestSourceAndFrameKind() {
  CHECK(Oot3dNativeGame::ResolveInBetweenSource(false, false) == InBetweenSource::Governor);
  CHECK(Oot3dNativeGame::ResolveInBetweenSource(true, false) == InBetweenSource::StepImage);
  CHECK(Oot3dNativeGame::ResolveInBetweenSource(false, true) ==
        InBetweenSource::MenuStepImage);
  CHECK(Oot3dNativeGame::ResolveInBetweenSource(true, true) == InBetweenSource::MenuStepImage);
  // The governor's and the locked cadence's drops stay drops; the menu's
  // step image is Other (perf other_frames/menu_held, no governor cost).
  CHECK(Oot3dNativeGame::StepImageFrameKind(InBetweenSource::Governor) ==
        RenderFrameKind::Dropped);
  CHECK(Oot3dNativeGame::StepImageFrameKind(InBetweenSource::StepImage) ==
        RenderFrameKind::Dropped);
  CHECK(Oot3dNativeGame::StepImageFrameKind(InBetweenSource::MenuStepImage) ==
        RenderFrameKind::Other);
}

// What the render thread does with one step pair (oot3d_native_a32_window_frame.cpp
// PresentVisualFrame and FinishRenderFrameCost): the step frame's cost, then
// the in-between frame from its source; only a Governor in-between frame is
// decided and costed.
struct PairResult {
  bool InBetweenNew = false; // a new picture (full or reduced in-between)
  RenderFrameKind Kind = RenderFrameKind::Other;
};

PairResult Pair(InterpolationPacingGovernor &governor, bool menuHold, double stepMs,
                double inBetweenMs) {
  governor.ObserveStep(stepMs);
  const auto source = Oot3dNativeGame::ResolveInBetweenSource(false, menuHold);
  if (source != InBetweenSource::Governor)
    return {false, Oot3dNativeGame::StepImageFrameKind(source)};
  const auto level = governor.Decide();
  governor.ObserveInterpolated(level, level == InterpolatedSampleLevel::Dropped ? 1.0 : inBetweenMs);
  return {level != InterpolatedSampleLevel::Dropped,
          level == InterpolatedSampleLevel::Dropped ? RenderFrameKind::Dropped
                                                    : RenderFrameKind::Full};
}

// logs27/28 at a pause opening (perf windows 4650, 1050, 18750): the first
// menu steps cost more (the lower screen is drawn for 60 samples after the
// first one with only lower-screen draws, oot3d_native_bottom_screen_skip.h),
// the pair goes over and the governor drops; the menu then switched between
// in-between frames drawn and dropped. With the hold the menu is one picture
// per step throughout, the governor neither decides nor learns its frames,
// and gameplay after it keeps its 60.
void TestPauseOpeningCadence(bool menuHold) {
  InterpolationPacingConfig config;
  InterpolationPacingGovernor governor;
  governor.Configure(config, 2000.0 / 60.0, 2.0F, false);
  for (int pair = 0; pair < 300; ++pair)
    CHECK(Pair(governor, false, 9.0, 9.0).InBetweenNew);
  const auto before = governor.Stats();
  int menuNew = 0, menuHeld = 0, menuDropped = 0;
  for (int pair = 0; pair < 150; ++pair) {
    const double step = pair < 30 ? 16.0 : 4.0;
    const double inBetween = pair < 30 ? 19.0 : 4.0;
    const auto result = Pair(governor, menuHold, step, inBetween);
    menuNew += result.InBetweenNew ? 1 : 0;
    menuHeld += result.Kind == RenderFrameKind::Other ? 1 : 0;
    menuDropped += result.Kind == RenderFrameKind::Dropped ? 1 : 0;
  }
  const auto menu = governor.Stats();
  if (menuHold) {
    CHECK(menuHeld == 150 && menuNew == 0 && menuDropped == 0);
    CHECK(menu.FullFrames == before.FullFrames && menu.DroppedFrames == before.DroppedFrames &&
          menu.Probes == before.Probes && menu.LevelChanges == before.LevelChanges);
  } else {
    // Without the hold the opening drops and the rest draws: two cadences.
    CHECK(menuDropped > 0 && menuNew > 0 && menuHeld == 0);
  }
  int afterNew = 0;
  for (int pair = 0; pair < 60; ++pair)
    afterNew += Pair(governor, false, 9.0, 9.0).InBetweenNew ? 1 : 0;
  CHECK(afterNew == 60);
  CHECK(governor.Level() == InterpolatedSampleLevel::Full);
}

} // namespace

int main() {
  TestHoldNeedsTopScreenSixtyAndAPage();
  TestPageOwnersFromGuestMemory();
  TestSourceAndFrameKind();
  TestPauseOpeningCadence(true);
  TestPauseOpeningCadence(false);
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("TopScreen menu cadence tests passed\n");
  return 0;
}
