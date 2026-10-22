#pragma once

// Host model of the console's fps60 pipeline, to compare config/fps60_mode
// policies on synthetic traces (oot3d_native_cadence_governor_tests.cpp).
//
// Modelled after oot3d_native_a32_window_frame.cpp on the Switch:
// - game thread (core 0): one host frame after another. Each host frame
//   advances the guest by the refreshes the wall clock owes
//   (NativePresentationScheduler, DropExcess, at most 2); the game steps
//   once every two refreshes (the work split between them, the frame closes
//   in the second) and a host frame costs a fixed overhead on top (input,
//   UI, submission). Its end-of-frame command waits until the renderer has
//   finished the previous host frame (NativeRenderCommandStream,
//   MaxFramesInFlight = 1).
// - render thread (core 2): a host frame starts when the previous one is
//   presented and its commands are queued. A step frame draws the new guest
//   frame; a host frame without one draws the in-between picture (full),
//   or drops it (the step picture again, almost free); the GPU runs the
//   frames in order. Then the pacer (absolute 60 Hz deadlines, debt of up
//   to two periods carried) and the swap, which returns at the vblank that
//   shows the picture (vsync on, nothing queued ahead).
// - costs come from the console's perf.txt (switch-logs/integ2b): render
//   thread 8-17 ms per step frame and 3-6 ms per in-between frame, GPU
//   ~12-17 ms per replay of the scene (step and full in-between alike),
//   game thread 13-25 ms per step plus ~2.5 ms per host frame.
//
// The governors are the production ones; only the threads and the clock are
// simulated. Deterministic (fixed seed).

#include "oot3d_native_cadence_governor.h"
#include "oot3d_native_interpolation_pacing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace Oot3dNativeGame::CadenceSim {

struct Segment {
  double Seconds = 10.0;
  double GameStepMs = 18.0; // guest work of one step (both refreshes)
  double GameStepSdMs = 2.0;
  double GpuMs = 14.0; // one replay of the scene
  double GpuSdMs = 0.7;
  double RtStepMinMs = 8.0;
  double RtStepMaxMs = 15.0;
  double RtFullMinMs = 3.0;
  double RtFullMaxMs = 6.0;
  double SpikeProbability = 0.02; // a step that costs SpikeMs more
  double SpikeMs = 8.0;
};

struct Params {
  double RefreshMs = 1000.0 / 60.0;
  double HostFrameOverheadMs = 2.5; // game thread, every host frame
  double StepSubmitMs = 1.5;        // game thread, host frames with a step
  double CloseFraction = 0.5; // guest step work in the refresh that closes it
  double VblankPhaseMs = 5.0;
  double SwapLatencyMs = 0.3;
  // Locked 30 (adaptive): the game thread runs both refreshes of a step in
  // one host frame and the pacer waits two periods (PlanThirtyRefreshes).
  bool ThirtyHostLoop = false; // console integ4: worse, not used
  // At 60 (adaptive/strict60): a late host frame does not run two
  // refreshes (which starts the irregular 30 Hz loop); one refresh of debt
  // is carried instead.
  bool SixtyNoBatch = false; // slowed the game near the limit (integ4)
  // Locked 30: a late step rebases the 30 Hz deadline (no catch-up).
  bool ThirtyResyncWhenLate = true;
  // Swap: 0 = returns at the vblank that shows the picture; 1 = one picture
  // may wait queued (returns when the previous one reached the screen).
  uint32_t SwapQueue = 0U;
  // NativeRenderCommandStream MaxFramesInFlight (config/frames_in_flight):
  // the game thread's end-of-frame command waits until fewer than this many
  // host frames are pending on the renderer.
  uint32_t FramesInFlight = 1U;
  uint32_t Seed = 12345U;
  Fps60ModeConfig Mode{};
  InterpolationPacingConfig Pacing{};
};

struct Result {
  CadenceStats Cadence{};
  InterpolationPacingStats Pacing{};
  double Seconds = 0.0;
  uint64_t HostFrames = 0;
  uint64_t Presents = 0;
  uint64_t Pictures = 0; // distinct pictures shown
  uint64_t Steps = 0;    // guest steps presented
  uint64_t InBetweenDrawn = 0;
  uint64_t InBetweenDropped = 0;
  uint64_t DroppedRefreshes = 0; // guest refreshes lost (game slowed)
  // Between consecutive distinct pictures: display interval (mean, std)
  // and judder = RMS of (display interval - content interval), ms.
  double PictureIntervalMeanMs = 0.0;
  double PictureIntervalStdMs = 0.0;
  double JudderRmsMs = 0.0;
  double GuestRefreshesPerSecond = 0.0;
  std::vector<double> PictureIntervalsMs; // for tests
};

[[nodiscard]] inline Result Run(const std::vector<Segment> &segments,
                                const Params &params) {
  Result result;
  const double P = params.RefreshMs;
  std::mt19937 rng(params.Seed);
  std::normal_distribution<double> normal(0.0, 1.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);

  InterpolationPacingGovernor pacing;
  pacing.Configure(params.Pacing, 2.0 * P, 2.0F, false);
  NativeCadenceGovernor cadence;
  cadence.Configure(params.Mode, P);
  const auto mode = params.Mode.Mode;

  double total = 0.0;
  for (const auto &segment : segments)
    total += segment.Seconds;
  const auto segmentAt = [&](double ms) -> const Segment & {
    double end = 0.0;
    for (const auto &segment : segments) {
      end += segment.Seconds * 1000.0;
      if (ms < end)
        return segment;
    }
    return segments.back();
  };

  // Game thread state.
  double gameStart = 0.0;
  double previousGameStart = 0.0;
  double phase = 0.0;
  bool started = false;
  uint64_t refreshCounter = 0U;
  uint64_t stepIndex = 0U;
  double currentStepGuestMs = 0.0;
  double currentStepGpuMs = 0.0;
  const auto sampleStep = [&](double now) {
    const auto &segment = segmentAt(now);
    double guest =
        segment.GameStepMs + segment.GameStepSdMs * normal(rng);
    if (unit(rng) < segment.SpikeProbability)
      guest += segment.SpikeMs;
    currentStepGuestMs = std::max(4.0, guest);
    currentStepGpuMs =
        std::max(2.0, segment.GpuMs + segment.GpuSdMs * normal(rng));
  };
  sampleStep(0.0);

  // Render thread / GPU state.
  double renderEnd = 0.0; // previous host frame presented (swap returned)
  // renderEnd of the last FramesInFlight host frames, oldest first.
  std::vector<double> renderEnds;
  double lastDisplay = -1.0e9;
  double gpuFree = 0.0;
  bool pacerStarted = false;
  double deadline = 0.0;
  bool hasStepPicture = false;
  bool inBetweenShownForStep = false;
  uint64_t lastStep = 0U;
  double stepGpuForInBetween = 14.0;

  // Distinct pictures: display time and content time.
  bool hasPicture = false;
  double lastPictureDisplay = 0.0;
  double lastPictureContent = 0.0;
  double sumInterval = 0.0, sumInterval2 = 0.0, sumJudder2 = 0.0;
  uint64_t intervals = 0U;
  uint64_t guestRefreshes = 0U;

  const auto nextVblank = [&](double t) {
    const double k = std::ceil((t - params.VblankPhaseMs) / P - 1.0e-9);
    return params.VblankPhaseMs + k * P;
  };

  while (gameStart < total * 1000.0) {
    // --- game thread: one host frame ---
    const double elapsed = started ? gameStart - previousGameStart : 0.0;
    uint32_t due = 1U;
    const bool adaptiveThirty = mode == Fps60Mode::Adaptive &&
                                cadence.DropsInBetween() &&
                                params.ThirtyHostLoop;
    const bool noBatch = mode == Fps60Mode::Adaptive && params.SixtyNoBatch &&
                         !cadence.DropsInBetween();
    if (started) {
      NativeCadenceRefreshPlan plan;
      plan.Elapsed = elapsed / P;
      plan.Phase = phase + plan.Elapsed;
      const auto decision = adaptiveThirty ? PlanThirtyRefreshes(plan)
                            : noBatch      ? PlanSixtyRefreshes(plan)
                                           : PlanLegacyRefreshes(plan);
      due = decision.Due;
      result.DroppedRefreshes += decision.Dropped;
      phase = decision.Phase;
    }
    started = true;
    previousGameStart = gameStart;
    const auto &segment = segmentAt(gameStart);
    double busy = params.HostFrameOverheadMs;
    double guestMs = 0.0;
    bool completed = false;
    uint64_t completedStep = 0U;
    double completedGpu = 0.0;
    for (uint32_t refresh = 0U; refresh < due; ++refresh) {
      const bool closes = (refreshCounter % 2U) == 1U;
      ++refreshCounter;
      ++guestRefreshes;
      if (closes) {
        busy += params.CloseFraction * currentStepGuestMs;
        guestMs += params.CloseFraction * currentStepGuestMs;
        completed = true;
        completedStep = stepIndex;
        completedGpu = currentStepGpuMs;
        ++stepIndex;
        sampleStep(gameStart + busy);
      } else {
        busy += (1.0 - params.CloseFraction) * currentStepGuestMs;
        guestMs += (1.0 - params.CloseFraction) * currentStepGuestMs;
      }
    }
    if (completed)
      busy += params.StepSubmitMs;
    const double commandsReady = gameStart + busy;
    cadence.ObserveGameFrame(busy, guestMs, due, completed);

    // --- render thread: the same host frame ---
    const double renderStart = std::max(renderEnd, commandsReady);
    double rtMs = 0.3;
    double gpuMs = 0.3;
    PresentedPicture picture = PresentedPicture::Repeat;
    double content = 0.0;
    RenderFrameKind kind = RenderFrameKind::Other;
    if (completed) {
      kind = RenderFrameKind::Step;
      rtMs = segment.RtStepMinMs +
             (segment.RtStepMaxMs - segment.RtStepMinMs) * unit(rng);
      gpuMs = completedGpu;
      stepGpuForInBetween = completedGpu;
      picture = PresentedPicture::Step;
      content = static_cast<double>(completedStep) * 2.0 * P;
      hasStepPicture = true;
      inBetweenShownForStep = false;
      lastStep = completedStep;
      ++result.Steps;
    } else if (hasStepPicture) {
      // In-between host frame.
      bool drop = false;
      if (mode == Fps60Mode::Adaptive && cadence.DropsInBetween()) {
        drop = true;
      } else {
        pacing.SetDropAllowed(cadence.PerFrameDropAllowed());
        drop = pacing.Decide() == InterpolatedSampleLevel::Dropped;
      }
      if (drop) {
        kind = RenderFrameKind::Dropped;
        rtMs = 0.15;
        gpuMs = 0.4;
        picture = PresentedPicture::Repeat;
        ++result.InBetweenDropped;
      } else {
        kind = RenderFrameKind::Full;
        rtMs = segment.RtFullMinMs +
               (segment.RtFullMaxMs - segment.RtFullMinMs) * unit(rng);
        gpuMs = stepGpuForInBetween * (0.97 + 0.06 * unit(rng));
        // A second in-between frame of the same step repeats its picture.
        picture = inBetweenShownForStep ? PresentedPicture::Repeat
                                        : PresentedPicture::InBetween;
        content = static_cast<double>(lastStep) * 2.0 * P + P;
        inBetweenShownForStep = true;
        ++result.InBetweenDrawn;
      }
    }
    const double gpuStart = std::max(renderStart + 0.2, gpuFree);
    const double gpuEnd = gpuStart + gpuMs;
    gpuFree = gpuEnd;
    const double rtEnd = renderStart + rtMs;
    // Cost the governor learns: max of GPU and render thread.
    const double cost = std::max(gpuMs, rtMs);
    switch (kind) {
    case RenderFrameKind::Step:
      pacing.ObserveStep(cost);
      break;
    case RenderFrameKind::Full:
      pacing.ObserveInterpolated(InterpolatedSampleLevel::Full, cost);
      break;
    case RenderFrameKind::Dropped:
      pacing.ObserveInterpolated(InterpolatedSampleLevel::Dropped, cost);
      break;
    default:
      break;
    }
    // Pacer (NativeRealtimeRefreshPacer::WaitForNextRefresh): one period,
    // two for a step of the locked 30 host loop.
    const bool thirtyStep = adaptiveThirty && completed && due == 2U;
    const double period = thirtyStep ? 2.0 * P : P;
    double afterPacer = rtEnd;
    if (!pacerStarted) {
      pacerStarted = true;
      deadline = rtEnd;
    } else {
      deadline += period;
      if (rtEnd >= deadline) {
        if (rtEnd - deadline > 2.0 * period ||
            (thirtyStep && params.ThirtyResyncWhenLate))
          deadline = rtEnd;
      } else {
        afterPacer = deadline;
      }
    }
    // Swap: shown at the first vblank after the GPU finished.
    double display =
        nextVblank(std::max(afterPacer, gpuEnd) + params.SwapLatencyMs);
    if (display <= lastDisplay + 0.5 * P)
      display = lastDisplay + P;
    renderEnd = params.SwapQueue == 0U
                    ? display + 0.1
                    : std::max(afterPacer, lastDisplay) + 0.1;
    lastDisplay = display;
    ++result.Presents;
    cadence.ObservePresent(
        picture, renderEnd / 1000.0,
        pacing.StepCostMs() > 0.0
            ? pacing.StepCostMs() +
                  pacing.Estimate(InterpolatedSampleLevel::Full)
            : 0.0);
    if (picture != PresentedPicture::Repeat) {
      ++result.Pictures;
      if (hasPicture) {
        const double shown = display - lastPictureDisplay;
        const double meant = content - lastPictureContent;
        sumInterval += shown;
        sumInterval2 += shown * shown;
        sumJudder2 += (shown - meant) * (shown - meant);
        ++intervals;
        result.PictureIntervalsMs.push_back(shown);
      }
      hasPicture = true;
      lastPictureDisplay = display;
      lastPictureContent = content;
    }
    ++result.HostFrames;
    // Next host frame of the game thread: its end-of-frame command waited
    // until fewer than FramesInFlight host frames were pending, i.e. host
    // frame k - FramesInFlight presented (1: the previous one, which is this
    // one's render start).
    const size_t inFlight = std::max<size_t>(1U, params.FramesInFlight);
    double submitted = commandsReady;
    if (renderEnds.size() >= inFlight)
      submitted = std::max(submitted, renderEnds[renderEnds.size() - inFlight]);
    renderEnds.push_back(renderEnd);
    if (renderEnds.size() > inFlight)
      renderEnds.erase(renderEnds.begin());
    gameStart = std::max(commandsReady, submitted) + 0.05;
  }
  result.Seconds = gameStart / 1000.0;
  result.Cadence = cadence.Stats();
  result.Pacing = pacing.Stats();
  if (intervals != 0U) {
    result.PictureIntervalMeanMs = sumInterval / static_cast<double>(intervals);
    const double variance =
        sumInterval2 / static_cast<double>(intervals) -
        result.PictureIntervalMeanMs * result.PictureIntervalMeanMs;
    result.PictureIntervalStdMs = std::sqrt(std::max(0.0, variance));
    result.JudderRmsMs = std::sqrt(sumJudder2 / static_cast<double>(intervals));
  }
  result.GuestRefreshesPerSecond =
      static_cast<double>(guestRefreshes) / std::max(1.0e-9, result.Seconds);
  return result;
}

} // namespace Oot3dNativeGame::CadenceSim
