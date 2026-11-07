#pragma once

// The render thread's pacing of one presentation rate: the in-between frame
// governor (oot3d_native_interpolation_pacing.h), dynamic resolution
// (oot3d_native_dynamic_resolution.h) and the presentation cadence
// (oot3d_native_cadence_governor.h). All three take their budget from the
// rate: the pair is two presentation periods (33.3 ms at 60, 66.7 ms at 30)
// and a cadence "refresh" one period.
//
// The Switch's L3+R3 > Video > FPS switches between 30 and 60 (interpolated)
// while the game runs (oot3d_native_launch_frame_rate.h, LiveFrameRate):
// both keep the 30 Hz simulation, so only the presentation changes. The
// session applies the new rate at a frame boundary and calls this once
// more; every governor then starts over as at a start at that rate (only
// the cumulative counters of perf.txt carry on), so no estimate, lock or
// interval measured at the old rate leaks into the new one.
//
// One exception (log16, 07/11): dynamic resolution. A start at 60 begins at
// MaxScale and steps down as the pairs come in; after a switch from 30 into
// Forest Temple room 8 (14-21 ms a step frame at 2.25, 26 at 3.00) that
// descent cost 16 and 23 dropped in-between frames (windows 32250, 34800).
// A switch to 60 now begins at the scale 60 FPS last ran at in this session
// (RatePacingResolutionMemory), or at the floor if it never ran at 60. The
// first configuration of a session still starts at MaxScale. Under
// Resolution at 60 FPS = Full (config/drs_60) 60 always runs at MaxScale,
// a switch to 60 included, and a held full scale is not remembered as the
// scale Dynamic resumes at.
//
// Pure logic: oot3d_native_cadence_governor_tests.cpp
// (TestLiveRateSwitchMatchesFreshStart, TestLiveRateSwitchKeepsSixtyScale,
// TestLiveRateSwitchFullStartsAtFull).

#include "oot3d_native_cadence_governor.h"
#include "oot3d_native_dynamic_resolution.h"
#include "oot3d_native_interpolation_pacing.h"

#include <cstdint>

namespace Oot3dNativeGame {

struct RatePacingConfig {
  InterpolationPacingConfig Interpolation{};
  float BaseScale = 0.0F;
  bool ReducedAvailable = false;
  DynamicResolutionConfig Resolution{};
  // The scale dynamic resolution begins at (0: MaxScale);
  // RatePacingResolutionMemory::StartScale.
  float ResolutionStartScale = 0.0F;
  // config/drs_60 (L3+R3 > Video > Resolution at 60 FPS).
  DynamicResolutionSixtyMode ResolutionSixty = DynamicResolutionSixtyMode::Dynamic;
  // config/fps60_mode; only means something at 60 (at 30 every picture
  // holds two refreshes by design): legacy at any other rate.
  Fps60ModeConfig Cadence{};
};

// Two presentation periods: one step frame and one in-between frame.
[[nodiscard]] inline double RatePacingPairBudgetMs(uint32_t rateHz) noexcept {
  return 2000.0 / static_cast<double>(rateHz != 0U ? rateHz : 60U);
}

// What dynamic resolution last used at 60 in this session.
struct RatePacingResolutionMemory {
  float LastScaleAtSixty = 0.0F; // 0: never ran at 60

  // Before reconfiguring: the rate being left and its controller.
  void Leave(uint32_t rateHz, const DynamicResolutionController &resolution) noexcept {
    if (rateHz == 60U && resolution.LowestLevel() > 0U)
      LastScaleAtSixty = resolution.Scale();
  }

  // The start scale of a configuration at rateHz (0: MaxScale). A switch to
  // 60 begins at the scale 60 last ran at, or the floor; anything else at
  // MaxScale.
  [[nodiscard]] float StartScale(uint32_t rateHz, bool rateSwitch,
                                 const DynamicResolutionConfig &config) const noexcept {
    if (rateHz != 60U || !rateSwitch)
      return 0.0F;
    return LastScaleAtSixty > 0.0F ? LastScaleAtSixty : config.MinScale;
  }
};

[[nodiscard]] inline Fps60ModeConfig RatePacingCadence(uint32_t rateHz,
                                                       Fps60ModeConfig config) noexcept {
  if (rateHz != 60U)
    config.Mode = Fps60Mode::Legacy;
  return config;
}

inline void ConfigureRatePacing(uint32_t rateHz, const RatePacingConfig &config,
                                InterpolationPacingGovernor &interpolation,
                                DynamicResolutionController &resolution,
                                NativeCadenceGovernor &cadence) noexcept {
  if (rateHz == 0U)
    rateHz = 60U;
  const double pairMs = RatePacingPairBudgetMs(rateHz);
  interpolation.Configure(config.Interpolation, pairMs, config.BaseScale,
                          config.ReducedAvailable);
  DynamicResolutionConfig resolutionConfig = config.Resolution;
  ApplyDynamicResolutionSixtyMode(rateHz, config.ResolutionSixty, resolutionConfig);
  resolution.Configure(resolutionConfig, pairMs, config.ResolutionStartScale);
  cadence.Configure(RatePacingCadence(rateHz, config.Cadence),
                    1000.0 / static_cast<double>(rateHz));
  // Single in-between frames are dropped only in legacy; adaptive drops
  // them as a whole (the locked 30) and strict60 never.
  interpolation.SetDropAllowed(cadence.PerFrameDropAllowed());
}

} // namespace Oot3dNativeGame
