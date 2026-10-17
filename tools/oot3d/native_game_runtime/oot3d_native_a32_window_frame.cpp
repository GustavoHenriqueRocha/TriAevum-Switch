// Native A32 window session: one frame of the loop and the renderer-side work it schedules.
// First: on the Switch it brings in glad, which must precede any GL header.
#include "oot3d_native_gpu_frame_timer.h"
#include "oot3d_native_a32_window_session.h"
#include "oot3d_whole_aot_gcov.h"
#include <fstream>
#include "fast/renderer3ds/pica_surface_debug.h"
#include "oot3d_native_room_shadow_shell.h"
#include "fast/backends/gl_frame_profiler.h"
#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
#endif

extern "C" {
// Provided only by a Mesa built with the Horizon shader cache (mesa_switch.h).
unsigned mesa_switch_get_stats(uint64_t* out, unsigned count) __attribute__((weak));
const char* mesa_switch_shader_cache_status(void) __attribute__((weak));
#if defined(__SWITCH__)
// newlib's running totals (mallocr.c: arena = memory taken with sbrk), kept
// up to date by malloc itself: reading them takes no lock, unlike mallinfo(),
// which holds the malloc lock shared with Mesa while it walks every free list.
extern struct mallinfo __malloc_current_mallinfo __attribute__((weak));
extern size_t __malloc_max_sbrked_mem __attribute__((weak));
#endif
}

namespace Oot3dA32Window {

namespace {

uint64_t HashDiagnosticBytes(std::span<const uint8_t> bytes) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool IsNativeFrontendOpaqueTargetInitializationDraw(
    const Oot3dNativeGame::Oot3dPicaDrawSubmission &draw) {
  const auto &state = draw.State;
  const auto &blend = state.OutputMerger.Blend;
  const auto &depth = state.OutputMerger.Depth;
  const bool hasTexture =
      std::any_of(state.Textures.begin(), state.Textures.end(),
                  [](const auto &texture) { return texture.Enabled; });
  // Native UI targets include rotated/padded storage outside the logical
  // viewport, so framebuffer dimensions cannot establish whether this copy
  // quad initializes the frontend canvas. The replacement blend/depth state
  // identifies that operation; its association with CommonBackground00's
  // target AND viewport is checked by the caller, preserving the scene clear.
  return !hasTexture && state.VertexInput.Indexed &&
         state.VertexInput.VertexCount == 4U &&
         state.OutputMerger.ColorWriteMask == 0x0FU &&
         state.OutputMerger.LogicOperation ==
             Oot3dNativeGame::Oot3dPicaLogicOperation::Copy &&
         blend.Enabled &&
         blend.SourceColor == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationColor ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         blend.SourceAlpha == Oot3dNativeGame::Oot3dPicaBlendFactor::One &&
         blend.DestinationAlpha ==
             Oot3dNativeGame::Oot3dPicaBlendFactor::Zero &&
         depth.TestEnabled && depth.WriteEnabled &&
         depth.Compare == Oot3dNativeGame::Oot3dPicaCompareFunction::Always;
}

Oot3dNativeGame::NativeGuestClockDeadlineResolution AdvanceGuestClockTo(
    Oot3dNativeGame::NativeA32Process &process,
    Oot3dNativeGame::NativeA32CtrHostServices &hostServices,
    Oot3dNativeGame::NativeA32ProcessRunResult &processResult,
    uint64_t deadline, uint32_t wholeAotBlockBudget) {
  auto resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.OvershootTicks != 0U) {
    return resolution;
  }
  while (processResult.Kind !=
         Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    const auto wakeTick = hostServices.NextSleepWakeTick();
    if (!wakeTick.has_value() || *wakeTick > deadline) {
      break;
    }
    const uint64_t currentTick = hostServices.SystemTicks();
    hostServices.AdvanceSystemTicks(
        *wakeTick > currentTick ? *wakeTick - currentTick : 0U);
    processResult = RunUntilGuestWait(process, wholeAotBlockBudget);
    RequireRunnableGuest(processResult);
    resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
        hostServices.SystemTicks(), deadline);
    if (resolution.OvershootTicks != 0U) {
      return resolution;
    }
  }
  resolution = Oot3dNativeGame::ResolveNativeGuestClockDeadline(
      hostServices.SystemTicks(), deadline);
  if (resolution.AdvanceTicks != 0U) {
    hostServices.AdvanceSystemTicks(resolution.AdvanceTicks);
  }
  return resolution;
}

// GPU time of the presented frames (render thread only; one session).
Oot3dNativeGame::NativeGpuFrameTimer &GpuFrameTimer() {
  static Oot3dNativeGame::NativeGpuFrameTimer timer;
  return timer;
}

} // namespace

void NativeA32WindowSession::EnsureInterpolationPacing() {
  auto &api = *apiPointer;
  auto &realtimePacer = *realtimePacerStorage;
  const uint32_t rateHz = realtimePacer.TargetRateHz() != 0U
                              ? realtimePacer.TargetRateHz()
                              : 60U;
  if (interpolationPacingConfigured && rateHz == interpolationPacingRateHz) {
    // A backend reset (InitNativePica) forgets the namespace scale.
    if (interpolationPacing.ReducedAvailable() &&
        api.PicaNamespaceResolutionScale(kReducedVisualRenderTargetNamespace) !=
            interpolationPacing.Config().ReducedScale) {
      (void)api.SetPicaNamespaceResolutionScale(
          kReducedVisualRenderTargetNamespace,
          interpolationPacing.Config().ReducedScale);
    }
    return;
  }
  const bool firstConfiguration = !interpolationPacingConfigured;
  interpolationPacingConfigured = true;
  interpolationPacingRateHz = rateHz;
  const auto &options = interpolationPacingOptions;
  const float baseScale =
      api.PicaNamespaceResolutionScale(kVisualInterpolationRenderTargetNamespace);
  Oot3dNativeGame::InterpolationPacingConfig config;
  config.KeepGameSpeed = options.KeepGameSpeed;
  config.ReducedScale = Oot3dNativeGame::ParseInterpolationReducedScale(
      options.ReducedScaleText, baseScale);
  config.ReducedMode = options.ReducedAlways
                           ? Oot3dNativeGame::InterpolationReducedScaleMode::Always
                           : Oot3dNativeGame::InterpolationReducedScaleMode::Auto;
  const bool reducedAvailable =
      config.ReducedScale > 0.0F &&
      api.SetPicaNamespaceResolutionScale(kReducedVisualRenderTargetNamespace,
                                          config.ReducedScale);
  // Two presentation periods: one step frame and one in-between frame.
  interpolationPacing.Configure(config, 2000.0 / static_cast<double>(rateHz),
                                baseScale, reducedAvailable);
  if (firstConfiguration) {
    skipNativeReplay =
        !options.ForceNativeReplay && !api.PicaNativeNamespaceObservable();
    std::fprintf(stderr,
                 "TRIAEVUM_INTERP_PACING keep_speed=%d reduced_scale=%.2f "
                 "reduced_available=%d mode=%s base_scale=%.2f "
                 "native_replay=%d\n",
                 config.KeepGameSpeed ? 1 : 0, config.ReducedScale,
                 reducedAvailable ? 1 : 0,
                 options.ReducedAlways ? "always" : "auto", baseScale,
                 skipNativeReplay ? 0 : 1);
  }
  std::lock_guard lock(interpolationPerfMutex);
  interpolationPerf.ReducedScale = reducedAvailable ? config.ReducedScale : 0.0F;
}

void NativeA32WindowSession::FinishRenderFrameCost() {
  auto &timer = GpuFrameTimer();
  const auto kind = renderFrameKind;
  const double cpuMs = renderFrameCpuMs;
  renderFrameKind = 0U;
  renderFrameCpuMs = 0.0;
  const auto observe = [&](uint8_t frameKind, double costMs) {
    switch (static_cast<Oot3dNativeGame::RenderFrameKind>(frameKind)) {
    case Oot3dNativeGame::RenderFrameKind::Step:
      interpolationPacing.ObserveStep(costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Full:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Full, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Reduced:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Reduced, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Dropped:
      interpolationPacing.ObserveInterpolated(
          Oot3dNativeGame::InterpolatedSampleLevel::Dropped, costMs);
      break;
    case Oot3dNativeGame::RenderFrameKind::Other:
      break;
    }
  };
  std::array<double, Oot3dNativeGame::kRenderFrameKindCount> gpuMs{};
  std::array<uint64_t, Oot3dNativeGame::kRenderFrameKindCount> gpuFrames{};
  const bool gpuAvailable = timer.Available();
  timer.EndFrame(kind, cpuMs);
  timer.Poll([&](const Oot3dNativeGame::NativeGpuFrameTime &result) {
    if (result.Kind >= gpuMs.size()) return;
    gpuMs[result.Kind] += result.Milliseconds;
    ++gpuFrames[result.Kind];
    // Both sides of a frame overlap the neighbouring frames: the pair fits
    // only if each fits.
    observe(result.Kind, std::max(result.Milliseconds, result.CpuMilliseconds));
  });
  if (!gpuAvailable) {
    // No GPU clock (yet): render-thread time only. The swap is left out:
    // it includes the vsync wait, which made every pair look over budget
    // in the first seconds and dropped in-between frames that fit.
    observe(kind, cpuMs);
  }
  std::lock_guard lock(interpolationPerfMutex);
  auto &perf = interpolationPerf;
  if (kind < perf.Frames.size()) {
    ++perf.Frames[kind];
    perf.CpuMs[kind] += cpuMs;
  }
  for (size_t index = 0U; index < gpuMs.size(); ++index) {
    perf.GpuMs[index] += gpuMs[index];
    perf.GpuFrames[index] += gpuFrames[index];
  }
  const auto &stats = interpolationPacing.Stats();
  perf.Probes = stats.Probes;
  perf.LevelChanges = stats.LevelChanges;
  perf.Level = static_cast<uint8_t>(interpolationPacing.Level());
  perf.GpuTimer = timer.Available();
  perf.GpuFactor = timer.NanosecondsPerTick();
  perf.StepCostMs = interpolationPacing.StepCostMs();
  perf.FullEstimateMs = interpolationPacing.Estimate(
      Oot3dNativeGame::InterpolatedSampleLevel::Full);
  perf.ReducedEstimateMs = interpolationPacing.Estimate(
      Oot3dNativeGame::InterpolatedSampleLevel::Reduced);
  perf.BudgetMs = interpolationPacing.PairBudgetMs();
}

bool NativeA32WindowSession::RunFrame() {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &api = *apiPointer;
  auto &preparationScreen = *preparationScreenStorage;
  auto &ocarinaText = *ocarinaTextStorage;
  auto &n64UiRenderer = *n64UiRendererStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  auto &presentationScheduler = *presentationSchedulerStorage;
  auto &realtimePacer = *realtimePacerStorage;
  const auto phaseBeforeFrame = phaseTiming.Values();
  const double nestedGuestBeforeFrame = phaseTiming.GuestInsidePicaSubmitSeconds;
  const double guestClockBeforeFrame = phaseTiming.GuestClockSeconds;
  widescreenProjection.CurrentHostFrame =
      static_cast<uint32_t>(presentationFrameCount);
  // The render thread saw the exit request: leave before simulating more.
  if (renderStream.ShutdownRequested()) {
    window.Close();
    return false;
  }
  WindowDemoFrameTiming frameTiming;
  if (!PrepareNextWindowDemoFrame(hostArgs, window, timing, frameTiming)) {
    return true;
  }
  const auto requestedPresentationPolicy =
      Fast::Oot3d::ResolvePresentationPacingPolicy(EffectiveFrameRateMode());
  if (Fast::Oot3d::PresentationPacingPolicyChanged(
          presentationPacingPolicy, requestedPresentationPolicy)) {
    const auto requestedCompositionPolicy =
        ResolveFrameCompositionPolicy(requestedPresentationPolicy);
    const bool previousInterpolationEnabled = guestInterpolationEnabled;
    const bool requestedInterpolationEnabled =
        Oot3dNativeGame::ShouldUseNativeVisualInterpolation(
            frameRateContract,
            requestedCompositionPolicy.InterpolationEnabled());
    const bool compositionModeChanged =
        guestCompositionPolicy.Interpolation !=
            requestedCompositionPolicy.Interpolation ||
        guestCompositionPolicy.FixedSampleMultiplier !=
            requestedCompositionPolicy.FixedSampleMultiplier;
    presentationPacingPolicy = requestedPresentationPolicy;
    guestCompositionPolicy = requestedCompositionPolicy;
    guestInterpolationEnabled = requestedInterpolationEnabled;
    renderStream.Submit([&, requestedPresentationPolicy,
                         requestedCompositionPolicy,
                         requestedInterpolationEnabled,
                         previousInterpolationEnabled,
                         compositionModeChanged] {
    frameCompositionPolicy = requestedCompositionPolicy;
    realtimePacer.Configure(requestedPresentationPolicy.Enabled &&
                                !hostArgs.ThroughputBenchmark,
                            requestedPresentationPolicy.TargetRateHz);
    visualSampleCadence.Configure(
        frameCompositionPolicy.FixedMultiplier()
            ? frameCompositionPolicy.FixedSampleMultiplier
            : 0U);
    visualInterpolationEnabled = requestedInterpolationEnabled;
    if (compositionModeChanged) {
      ++visualContinuityEpoch;
    }
    if (previousInterpolationEnabled != visualInterpolationEnabled) {
      preparedVisualTransition.Reset();
      visualContinuityTracker = {};
      previousVisualFrame.reset();
      latestVisualFrame.reset();
      latestVisualTransitionContinuous = false;
      visualSampleCadence.Reset();
    }
    });
  }
  const auto presentationTime = std::chrono::steady_clock::now();
  renderStream.Submit([&, presentationTime] {
    realtimePacer.ObservePresentation(presentationTime);
  });
  const double presentationElapsedSeconds =
      hostArgs.FixedDeltaSeconds > 0.0
          ? hostArgs.FixedDeltaSeconds
          : std::chrono::duration<double>(presentationTime -
                                          lastPresentationTime)
                .count();
  lastPresentationTime = presentationTime;
  const auto presentationStateBeforeFrame = presentationScheduler.CaptureState();
  const auto presentationStep =
      presentationScheduler.Advance(presentationElapsedSeconds);
  const uint32_t guestRefreshesDue = presentationStep.GuestRefreshesDue;
  const float visualInterpolationAlpha = static_cast<float>(
      std::clamp(presentationStep.InterpolationAlpha, 0.0, 1.0));
  ApplyTopScreenConfigSnapshot();
  uiLifecycleBridge.BeginHostFrame(
      static_cast<uint32_t>(presentationFrameCount));
  if (topScreenTextureOverrideRuntime.has_value()) {
    topScreenTextureOverrideRuntime->Observe(uiLifecycleBridge);
  }

  const uint32_t width = std::max<uint32_t>(1, window.GetWidth());
  const uint32_t height = std::max<uint32_t>(1, window.GetHeight());
  hdFonts.SetOutputHeight(height);
  UpdateScenePresentation();
  phaseStart = std::chrono::steady_clock::now();
  Fast::Renderer3ds::SlowStepTrace hostStartSteps("host_frame_start");
  window.GetMouseStateManager()->StartFrame();
  hostStartSteps.Mark("mouse");
  gui->StartDraw();
  hostStartSteps.Mark("gui_start_draw");
  renderStream.Submit([&] {
    window.StartFrame();
    // The window's events are pumped here, on the render thread: an exit
    // request (Quit, HOME > Close) seen now ends rendering before the rest of
    // this frame and any frame queued behind it runs.
    // (A run that closes itself completes its frames: see Finish.)
    if (!window.IsRunning() && !scriptedExit.load())
      renderStream.BeginShutdown();
  });
  hostStartSteps.Mark("render_start_frame");
  phaseTiming.HostFrameStartSeconds += SecondsSince(phaseStart);
  const auto inputPhaseStart = std::chrono::steady_clock::now();
  // F5 saves the game moment and F8 loads the last state; with Shift, the
  // settings are saved beside it / applied from it.
  const bool quickSaveKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F5);
  const bool quickLoadKeyDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_F8);
  const bool shiftDown = window.IsKeyDown(Ship::KbScancode::LUS_KB_SHIFT) ||
                         window.IsKeyDown(Ship::KbScancode::LUS_KB_RSHIFT);
  if (quickLoadKeyDown && !quickLoadKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Load, shiftDown, {}});
  } else if (quickSaveKeyDown && !quickSaveKeyWasDown) {
    savestateLibrary->Request(
        {Oot3dNativeGame::SavestateRequestKind::Save, shiftDown, {}});
  }
  quickSaveKeyWasDown = quickSaveKeyDown;
  quickLoadKeyWasDown = quickLoadKeyDown;
  bool nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
      process.Memory(), uiLifecycleBridge,
      nativeCandidateDispatch.TopScreenUiProfile);
  uint32_t topScreenNativeGameMode = UINT32_MAX;
  const bool topScreenGameplayInputMode =
      process.Memory().Read32(kNativeGameMode, &topScreenNativeGameMode) &&
      Oot3dNativeGame::IsTopScreenGameplayInputMode(topScreenNativeGameMode);
  picaCompositionDomain =
      topScreenGameplayInputMode
          ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
          : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  bool topScreenStartRoutingEligible =
      uiLifecycleBridge.NativeGameplayPresentationActive() &&
      topScreenGameplayInputMode &&
      Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
  Oot3dNativeGame::NativeAimProfileTransform aimProfileTransform;
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const auto topScreenAim =
        Oot3dNativeGame::ResolveTopScreenCStickAimPolicy(
            activeTopScreenConfig);
    aimProfileTransform.RightStickScale = topScreenAim.SpeedMultiplier;
    aimProfileTransform.RightStickSmoothingCoefficient =
        Oot3dNativeGame::ResolveTopScreenCStickSmoothingCoefficient(
            activeTopScreenConfig.FreeCameraSmoothing);
    aimProfileTransform.RightStickInvertX = topScreenAim.InvertX;
    aimProfileTransform.RightStickInvertY = topScreenAim.InvertY;
  }
  const bool hostGuiVisible =
      gui->GetMenuOrMenubarVisible() || gui->GetAnyGuiWindowVisible();
  auto physicalInputFrame =
      PollNativeA32Input(window, nativeFrontendTouchEnabled,
                         hostGuiVisible,
                         nativeCandidateDispatch.TopScreenUiProfile,
                         activeTopScreenConfig.FreeCameraEnabled,
                         *controlConfigRuntime, aimProfileTransform,
                         presentationElapsedSeconds,
                         guestRefreshesDue != 0U,
                         nativeControlPollingState);
  if (physicalInputFrame.Exit || devLabQuit) {
    if (devLabQuit) scriptedExit = true;
    window.Close();
    return false;
  }
  // Developer qualification: scheduled settings take the F1 commit path
  // (applied, not persisted) before this frame is rendered.
  for (const auto *step : graphicsSettingsSchedule.TakeDue(runFrameCount)) {
    auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
    const auto current = graphicsSettings.Snapshot();
    const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
        Oot3dNativeGame::MergeScheduledGraphics(
            Fast::Oot3d::SerializeGraphicsSettings(current), step->Graphics),
        current);
    const auto applied = graphicsSettings.Apply(decoded.Value, false);
    std::fprintf(stderr,
                 "TRIAEVUM_GRAPHICS_SETTINGS_SCHEDULE frame=%u accepted=%d "
                 "decode_issues=%zu apply_issues=%zu\n",
                 step->Frame, applied.Accepted() ? 1 : 0,
                 decoded.Issues.size(), applied.Issues.size());
    if (decoded.UnsupportedFutureVersion || !applied.Accepted())
      throw std::runtime_error(
          "graphics settings schedule step at frame " +
          std::to_string(step->Frame) + " was rejected");
  }
  if (!inputTimeline.Enabled()) {
    const uint32_t start = Oot3dNativeGame::NativeA32HidButtonMask(
        Oot3dNativeGame::NativeA32HidButton::Start);
    const bool physicalStartHeld =
        (physicalInputFrame.Hid.Buttons & start) != 0U;
    Oot3dNativeGame::ObserveNativeA32PolledButton(
        physicalStartHeld, nativeCandidateDispatch.StartButtonLatch);
    Oot3dNativeGame::ReleaseTopScreenStartRoutingLatch(
        physicalStartHeld, nativeCandidateDispatch.TopScreenStartRouting);
  }
  Oot3dNativeGame::NativeA32InputFrame inputFrame;
  phaseTiming.InputPollSeconds += SecondsSince(inputPhaseStart);
  const auto rendererFrameStart = std::chrono::steady_clock::now();
  CpuProbe::Scope cpuRendererStart(CpuProbe::Phase::RendererStart);
  const auto beginRendererFrame = [&, width, height,
                                   presentationSerial =
                                       presentationFrameCount] {
    api.UpdateFramebufferParameters(0, width, height, 1, false, true, true,
                                    true);
    if (titleRenderBackend != nullptr) {
      // Display settings are window operations: applied only by a frame
      // begun on the window's thread.
      titleRenderBackend->SetPresentationSettingsHeld(
          !renderStream.InSynchronousCommand());
    }
    api.StartFrame();
    rendererFrameSkipped = !api.HasActiveFrame();
    if (rendererFrameSkipped) {
      rendererFrameWasInactive.store(true);
      return;
    }
    picaPresentationScheduler.BeginPresentation(presentationSerial);
    api.StartDrawToFramebuffer(0, 1.0F);
    api.SetClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    api.ClearFramebuffer(true, true);
    api.SetViewport(0, 0, static_cast<int>(width), static_cast<int>(height));
    api.SetScissor(0, 0, static_cast<int>(width), static_cast<int>(height));
  };
  // A frame begin may recreate targets (and so read back GPU copies into
  // guest memory) after a resize or a graphics settings change, may apply
  // display settings (window operations), and an inactive frame must not
  // advance the guest: those begin synchronously, on this thread.
  // Otherwise the guest goes on while the renderer finishes the previous
  // frame.
  const auto renderSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance()
                                  .SnapshotWithRevision()
                                  .Revision;
  const bool renderNativePresentation =
      Fast::Oot3d::GraphicsSettingsRuntime::Instance()
          .NativePresentationOverrideActive();
  const bool presentationTransactionActive =
      Fast::Oot3d::GraphicsSettingsRuntime::Instance()
          .PresentationStatus()
          .Phase != Fast::Oot3d::PresentationTransactionPhase::Idle;
  const bool previousRendererFrameInactive =
      rendererFrameWasInactive.exchange(false);
  const bool synchronousFrameBegin =
      renderStream.CurrentMode() ==
          Oot3dNativeGame::NativeRenderCommandStream::Mode::Inline ||
      presentationTransactionActive || previousRendererFrameInactive ||
      width != guestFrameBeginWidth ||
      height != guestFrameBeginHeight ||
      renderSettings != guestFrameBeginSettings ||
      renderNativePresentation != guestFrameBeginNativePresentation;
  guestFrameBeginWidth = width;
  guestFrameBeginHeight = height;
  guestFrameBeginSettings = renderSettings;
  guestFrameBeginNativePresentation = renderNativePresentation;
  bool rendererFrameActive = true;
  if (synchronousFrameBegin) {
    renderStream.Run(beginRendererFrame);
    // The renderer is idle after Run: its frame state is readable.
    rendererFrameActive = !rendererFrameSkipped;
  } else {
    renderStream.Submit(beginRendererFrame);
  }
  if (!rendererFrameActive) {
    // A minimized or changing surface is not a guest/PICA failure. Keep events
    // and UI balanced, but do not consume guest time without an acquired frame.
    if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
      throw std::runtime_error("cannot restore suspended presentation clock: " + error);
    }
    gui->EndDraw();
    renderStream.Submit([&] { window.EndFrame(); }, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
    lastPresentationTime = std::chrono::steady_clock::now();
    return true;
  }
  const auto preparationStatus = programPreparation != nullptr
      ? programPreparation->PreparationStatus()
      : Fast::Renderer3ds::PicaPreparationStatus{};
  if (preparationScreen.HoldsGuest(preparationStatus,
                                   std::chrono::steady_clock::now())) {
    // The guest waits for the initial preparation: its clock does not run
    // and the frame shows the progress instead of a game image.
    if (!presentationScheduler.Restore(presentationStateBeforeFrame, &error)) {
      throw std::runtime_error("cannot restore the presentation clock during preparation: " + error);
    }
    preparationScreen.Draw(preparationStatus);
    gui->EndDraw();
    renderStream.Submit([&] { window.EndFrame(); }, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    renderStream.Submit([&] { realtimePacer.ResetDeadline(); });
    lastPresentationTime = std::chrono::steady_clock::now();
    lastCompletedFrameTime = lastPresentationTime;
    return true;
  }
  phaseTiming.RendererFrameStartSeconds += SecondsSince(rendererFrameStart);
  cpuRendererStart.Stop();
  phaseTiming.FrameStartSeconds += SecondsSince(phaseStart);
  const uint32_t guestRefreshIterations =
      std::max<uint32_t>(1U, guestRefreshesDue);
  for (uint32_t guestRefreshIndex = 0;
       guestRefreshIndex < guestRefreshIterations; ++guestRefreshIndex) {
    const bool advanceGuest = guestRefreshIndex < guestRefreshesDue;
    const bool presentHostFrame =
        guestRefreshIndex + 1U == guestRefreshIterations;
    if (advanceGuest) {
      if (!SampleGuestRefreshInput(
              guestRefreshIndex,
              nativeFrontendTouchEnabled,
              topScreenNativeGameMode,
              topScreenStartRoutingEligible,
              physicalInputFrame,
              inputFrame)) {
        break;
      }
      if (!UpdateGuestServicesForRefresh(topScreenStartRoutingEligible, inputFrame)) {
        break;
      }
      RunGuestRefresh();
    }
    if (presentHostFrame) {
      PresentVisualFrame(presentationElapsedSeconds, visualInterpolationAlpha);
    }
    frameCount += advanceGuest ? 1U : 0U;
  }

  if (launch.ExtendedDiagnostics) {
    inputConsumerDiagnostics.Observe(process.Memory(), frameCount);
  }

  for (std::size_t subsystemIndex = 0;
       subsystemIndex < oot3d::ui::kUiSubsystemCount; ++subsystemIndex) {
    const auto subsystem =
        static_cast<oot3d::ui::UiSubsystem>(subsystemIndex);
    const bool topScreenProfile =
        launch.UiProfile == Oot3dNativeGame::Oot3dUiProfile::TopScreen;
    if (topScreenProfile &&
        subsystem != oot3d::ui::UiSubsystem::GameplayHud &&
        subsystem != oot3d::ui::UiSubsystem::TouchControls) {
      continue;
    }
    const bool topScreenPresentation = topScreenProfile;
    if (!topScreenPresentation && !uiLifecycleBridge.Runtime()
                                       .PlanFrame(subsystem)
                                       .run_host_presentation) {
      continue;
    }
    auto primitives =
        topScreenPresentation
            ? uiLifecycleBridge.BuildTopScreenPresentation(subsystem)
            : uiLifecycleBridge.BuildShadowPresentation(subsystem);
    if (topScreenPresentation &&
        subsystem == oot3d::ui::UiSubsystem::GameplayHud) {
      Oot3dNativeGame::AppendTopScreenFreeCameraOptionPresentation(
          nativeCandidateDispatch.TopScreenCamera.Camera, primitives);
    }
    if (topScreenPresentation && subsystem == oot3d::ui::UiSubsystem::TouchControls) {
      ocarinaText.Append(primitives);
    }
    if (topScreenPresentation) {
      lastTopScreenPrimitivesBySubsystem[subsystemIndex] = primitives;
    }
    const auto canvasMode =
        topScreenPresentation
            ? oot3d::ui::N64UiCanvasMode::NativeTopScreen400x240
            : oot3d::ui::N64UiCanvasMode::Widescreen16x9;
    // Texture payloads are read from guest memory here; the renderer
    // only uploads them.
    auto preparedTextures = n64UiRenderer.PrepareTextures(primitives);
    renderStream.Submit([&, primitives = std::move(primitives), width,
                         height, canvasMode,
                         prepared = std::move(preparedTextures)]() mutable {
      if (rendererFrameSkipped) {
        n64UiRenderer.Stage(std::move(prepared));
        return;
      }
      struct UiCost {
        double &Total;
        std::chrono::steady_clock::time_point Start =
            std::chrono::steady_clock::now();
        ~UiCost() { Total += SecondsSince(Start) * 1000.0; }
      } uiCost{renderFrameCpuMs};
      if (!n64UiRenderer.Render(primitives, width, height, canvasMode,
                                std::move(prepared), &renderError)) {
        throw std::runtime_error("integrated N64 UI presentation failed: " +
                                 renderError);
      }
    });
  }

  phaseStart = std::chrono::steady_clock::now();
  CpuProbe::Scope cpuPresent(CpuProbe::Phase::Present);
  Fast::Renderer3ds::SlowStepTrace presentSteps("present");
  // The first frames render under the preparation screen.
  preparationScreen.Draw(preparationStatus);
  gui->EndDraw();
  presentSteps.Mark("gui_end_draw");
  renderStream.Submit([&, width, height,
                       presentationSerial = presentationFrameCount,
                       forcedScreenshot = std::exchange(pendingForcedScreenshot, {})] {
    if (!forcedScreenshot.empty()) screenshotState.ForcedPath = forcedScreenshot;
    if (rendererFrameSkipped) {
      capturedTemporalSample.reset();
      window.EndFrame();
      realtimePacer.ResetDeadline();
      return;
    }
    MaybeWriteFramebufferScreenshot(
        hostArgs, api, width, height,
        static_cast<uint32_t>(presentationSerial), screenshotState,
        capturedTemporalSample ? &*capturedTemporalSample : nullptr);
    capturedTemporalSample.reset();
    FinishRenderFrameCost();
    const auto waitStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuPacing(CpuProbe::Phase::Pacing);
    Fast::GlPerf::Mark(Fast::GlPerf::Stage::Pacing);
    realtimePacer.WaitForNextRefresh();
    cpuPacing.Stop();
    rendererTiming->PacingWaitSeconds += SecondsSince(waitStart);
    const auto swapStart = std::chrono::steady_clock::now();
    window.EndFrame();
    lastSwapMs = SecondsSince(swapStart) * 1000.0;
  }, true);
  presentSteps.Mark("render_end_frame");
  picaSemanticTrace.RecordFrameBoundary(
      presentationFrameCount, frameCount, guestRefreshesDue != 0U);
  phaseTiming.PresentSeconds += SecondsSince(phaseStart);
  cpuPresent.Stop();
  ++presentationFrameCount;
  ++runFrameCount;
  if (guestProfiler.Active()) {
    // Perfil guest por amostragem de quadros: nos quadros medidos o perfil
    // recebe todos os blocos; nos outros volta o callback original com o
    // filtro de ganchos, e o jogo roda na velocidade normal.
    const bool sample = guestProfiler.OnHostFrame(runFrameCount);
    if (sample != guestProfilerInstalled && processStorage) {
      guestProfilerInstalled = sample;
      const bool forAll =
          launch.ProfileA32Blocks || widescreenProjection.TraceA32Blocks;
      if (sample) {
        processStorage->SetBlockEntryCallback(
            &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry, &guestProfiler,
            std::vector<uint32_t>{});
        nativeCandidateDispatch.BlockEntry =
            &Oot3dNativeGame::GuestBlockProfiler::OnBlockEntry;
        nativeCandidateDispatch.BlockEntryUser = &guestProfiler;
        nativeBlockEntryPcs = {};
      } else {
        processStorage->SetBlockEntryCallback(
            &ApplyNativeWidescreenProjectionPolicy, &widescreenProjection,
            forAll ? std::vector<uint32_t>{} : blockEntryHooks);
        nativeCandidateDispatch.BlockEntry = &ApplyNativeWidescreenProjectionPolicy;
        nativeCandidateDispatch.BlockEntryUser = &widescreenProjection;
        nativeBlockEntryPcs = forAll ? std::span<const uint32_t>{}
                                     : std::span<const uint32_t>{blockEntryHooks};
      }
      nativeCandidateDispatch.BlockEntryPcs = nativeBlockEntryPcs;
      nativeCandidateDispatch.MassAotBlockEntryFilter =
          Oot3dNativeGame::BuildOot3dMassAotPcFilter(nativeBlockEntryPcs,
                                                     nativeBlockEntryPcs.empty());
      guestProfiler.Interrupt();
    }
  }
#if defined(__SWITCH__)
  gSwitchRuntimeHostFrames = runFrameCount;
  gSwitchRuntimeGuestFrames = frameCount;
  Oot3dNativeGame::Oot3dWholeAotGcovFrame(runFrameCount);
  // Physical-console profile: every 150 host frames, append the rate and the
  // per-frame milliseconds of each phase over that window to logs/perf.txt.
  {
    static auto windowStart = std::chrono::steady_clock::now();
    static std::array<double, 15> windowValues{};
    static uint64_t windowHostFrames = 0U;
    static uint64_t windowGuestFrames = 0U;
    if (runFrameCount - windowHostFrames >= 150U) {
      const auto now = std::chrono::steady_clock::now();
      const double seconds = std::chrono::duration<double>(now - windowStart).count();
      const auto values = phaseTiming.Values();
      const uint64_t hostFrames = runFrameCount - windowHostFrames;
      static constexpr std::array<const char*, 15> kNames{
          "guest", "frame_start", "host_frame_start", "input", "renderer_frame_start",
          "dsp_mix", "audio_out", "pica_submit", "pica_plan", "pica_backend",
          "pica_diag", "visual_present", "visual_replay", "present", "pacing_wait"};
      std::ofstream perf("sdmc:/switch/oot3dre/logs/perf.txt", std::ios::app);
      perf << "host_frames=" << runFrameCount << " fps=" << (hostFrames / seconds)
           << " guest_fps=" << ((frameCount - windowGuestFrames) / seconds) << " ms/frame:";
      for (size_t index = 0; index < values.size(); ++index) {
        perf << ' ' << kNames[index] << '='
             << ((values[index] - windowValues[index]) * 1000.0 / hostFrames);
      }
      perf << " guest_in_pica=" << phaseTiming.GuestInsidePicaSubmitSeconds
           << " total_ms=" << (seconds * 1000.0 / hostFrames);
      if (hostAudioStream != nullptr) {
        perf << ' ' << hostAudioStream->PerfSummary();
      }
      // Plan builds off the guest thread (deferred planning): ms per host
      // frame on the plan worker and on the render thread / dependency
      // flushes ("late": the worker had not reached them yet).
      {
        static Oot3dNativeGame::Oot3dPicaDeferredPlanStats windowPlanStats{};
        const auto planStats = picaPlanBuilder.Stats();
        const auto perFrameMs = [&](uint64_t now, uint64_t before) {
          return static_cast<double>(now - before) / 1.0e6 / hostFrames;
        };
        perf << " pica_plan_deferred=" << (picaPlanBuilder.Deferred() ? 1 : 0)
             << " plan_worker=" << (picaPlanBuilder.WorkerEnabled() ? 1 : 0)
             << " plan_worker_ms="
             << perFrameMs(planStats.WorkerBuildNanoseconds,
                           windowPlanStats.WorkerBuildNanoseconds)
             << " plan_late_ms="
             << perFrameMs(planStats.ConsumerBuildNanoseconds,
                           windowPlanStats.ConsumerBuildNanoseconds)
             << " plan_late_draws="
             << (planStats.ConsumerBuilds - windowPlanStats.ConsumerBuilds)
             << " plan_serial_ms="
             << perFrameMs(planStats.SerialBuildNanoseconds,
                           windowPlanStats.SerialBuildNanoseconds)
             // Must stay small and constant (recycled batches).
             << " plan_batches_live=" << planStats.LiveBatches
             << " plan_batches_queued=" << planStats.QueuedBatches
             << " plan_batches_allocated=" << planStats.AllocatedBatches;
        windowPlanStats = planStats;
      }
      // Paced interpolation (config/fps60) and frame costs: frames of each
      // kind in the window, their GPU ms (GL_TIMESTAMP; gpu_timer=0 until
      // the clock is calibrated) and render-thread ms, the governor's level
      // and estimates, and the native replays skipped.
      {
        static NativeA32WindowSession::InterpolationPerfCounters previous{};
        NativeA32WindowSession::InterpolationPerfCounters current;
        {
          std::lock_guard lock(interpolationPerfMutex);
          current = interpolationPerf;
        }
        const auto frames = [&](size_t kind) {
          return current.Frames[kind] - previous.Frames[kind];
        };
        perf << " interp: pacing=" << (interpolationPacingOptions.KeepGameSpeed ? 1 : 0)
             << " interp_scale=" << current.ReducedScale
             << " level=" << Oot3dNativeGame::InterpolatedSampleLevelName(
                    static_cast<Oot3dNativeGame::InterpolatedSampleLevel>(current.Level))
             << " step_frames=" << frames(1)
             << " interp_frames=" << frames(2)
             << " interp_reduced=" << frames(3)
             << " interp_dropped=" << frames(4)
             << " other_frames=" << frames(0)
             << " reduced_refused=" << (current.ReducedRefused - previous.ReducedRefused)
             << " repeat_fallbacks=" << (current.RepeatFallbacks - previous.RepeatFallbacks)
             << " native_replay_skipped=" << (current.NativeReplaysSkipped - previous.NativeReplaysSkipped)
             << " probes=" << (current.Probes - previous.Probes)
             << " level_changes=" << (current.LevelChanges - previous.LevelChanges)
             << " gpu_timer=" << (current.GpuTimer ? 1 : 0)
             << " gpu_ns_per_tick=" << current.GpuFactor;
        for (size_t kind = 0U; kind < Oot3dNativeGame::kRenderFrameKindCount; ++kind) {
          const uint64_t gpuFrames = current.GpuFrames[kind] - previous.GpuFrames[kind];
          if (gpuFrames != 0U) {
            perf << " gpu_ms_" << Oot3dNativeGame::kRenderFrameKindNames[kind] << '='
                 << ((current.GpuMs[kind] - previous.GpuMs[kind]) / gpuFrames);
          }
          if (frames(kind) != 0U) {
            perf << " rt_ms_" << Oot3dNativeGame::kRenderFrameKindNames[kind] << '='
                 << ((current.CpuMs[kind] - previous.CpuMs[kind]) / frames(kind));
          }
        }
        perf << " est_step_ms=" << current.StepCostMs
             << " est_full_ms=" << current.FullEstimateMs
             << " est_reduced_ms=" << current.ReducedEstimateMs
             << " pair_budget_ms=" << current.BudgetMs;
        previous = current;
      }
      // GPU residency and memory: every live count and byte total must stay
      // bounded over a long session (a texture cache that never released
      // ran the console out of memory after ~8 minutes).
      {
        constexpr double kMb = 1024.0 * 1024.0;
        const auto gpu = api.GetNativePicaBackendStats();
        perf << " gl_textures_live=" << gpu.TexturesLive
             << " gl_tex_mb=" << (gpu.TextureBytes / kMb)
             << " gl_tex_budget_mb=" << (gpu.TextureBudgetBytes / kMb)
             << " gl_tex_created=" << gpu.TexturesCreated
             << " gl_tex_evicted=" << gpu.TexturesEvicted
             << " gl_luts_live=" << gpu.LutsLive
             << " gl_lut_mb=" << (gpu.LutBytes / kMb)
             << " gl_programs=" << gpu.ProgramsLive
             << " gl_targets=" << gpu.TargetsLive
             << " gl_displays=" << gpu.DisplaysLive
             << " gl_buffers_live=" << gpu.GeometryBuffersLive
             << " gl_buffer_mb=" << (gpu.GeometryBufferBytes / kMb)
             << " gl_geometry_entries=" << gpu.GeometryRegistryEntries
             << " gl_ring_orphans=" << gpu.StreamRingOrphans
             << " gl_oom=" << gpu.OutOfMemoryEvents
             << " gl_draws_skipped=" << gpu.SkippedDraws;
        // Lock-free arena every window. The bytes in use need mallinfo(),
        // which holds the malloc lock shared with Mesa and walks the heap
        // (a corrupted heap crashes there): opt-in only, with
        // config/heap_stats or config/gpu_timing, every 20th window.
        static uint32_t heapWindows = 0U;
        static const bool heapStats =
            std::ifstream("sdmc:/switch/oot3dre/config/heap_stats").good() ||
            std::ifstream("sdmc:/switch/oot3dre/config/gpu_timing").good();
        if (&__malloc_current_mallinfo != nullptr) {
          const volatile size_t& arena = __malloc_current_mallinfo.arena;
          perf << " heap_arena_mb=" << (static_cast<double>(arena) / kMb);
        }
        if (&__malloc_max_sbrked_mem != nullptr) {
          const volatile size_t& peak = __malloc_max_sbrked_mem;
          perf << " heap_arena_peak_mb=" << (static_cast<double>(peak) / kMb);
        }
        // config/heap_check_windows (1-1000): walk the heap (mallinfo) every
        // N windows; a corruption is then found within ~N*5 s of when it
        // happened. Also enabled every 20 windows by config/heap_stats or
        // config/gpu_timing. Absent: never (mallinfo holds the malloc lock and
        // is what trips over a damaged heap).
        static const uint32_t heapCheckWindows = [] {
          std::ifstream input("sdmc:/switch/oot3dre/config/heap_check_windows");
          long long value = 0;
          return input && (input >> value) && value >= 1 && value <= 1000
                     ? static_cast<uint32_t>(value)
                     : 0U;
        }();
        const uint32_t heapEvery = heapCheckWindows != 0U ? heapCheckWindows : 20U;
        if ((heapStats || heapCheckWindows != 0U) && heapWindows++ % heapEvery == 0U) {
          const struct mallinfo heap = mallinfo();
          perf << " heap_used_mb=" << (static_cast<double>(heap.uordblks) / kMb);
        }
        uint64_t usedMemory = 0U;
        uint64_t totalMemory = 0U;
        if (R_SUCCEEDED(svcGetInfo(&usedMemory, InfoType_UsedMemorySize,
                                   CUR_PROCESS_HANDLE, 0)) &&
            R_SUCCEEDED(svcGetInfo(&totalMemory, InfoType_TotalMemorySize,
                                   CUR_PROCESS_HANDLE, 0))) {
          perf << " proc_used_mb=" << (usedMemory / kMb)
               << " proc_total_mb=" << (totalMemory / kMb);
        }
      }
      // OpenGL PICA draw path (config/gl_stats), per host frame: draws,
      // fixed-function GL calls issued and dropped by the state cache
      // (config/gl_state_cache), binds, uniform uploads and render-thread
      // time inside SubmitPicaDraw.
      {
        const auto gl = api.GetNativePicaBackendStats();
        if (gl.GlStatsEnabled) {
          static decltype(api.GetNativePicaBackendStats()) windowGl{};
          if (gl.PicaDraws < windowGl.PicaDraws ||
              gl.PicaPresents < windowGl.PicaPresents) {
            windowGl = {}; // backend state was recreated
          }
          const auto perFrame = [&](uint64_t now, uint64_t before) {
            return static_cast<double>(now - before) / hostFrames;
          };
          const uint64_t draws = gl.PicaDraws - windowGl.PicaDraws;
          const uint64_t issued =
              gl.GlStateCallsIssued - windowGl.GlStateCallsIssued;
          const uint64_t skipped =
              gl.GlStateCallsSkipped - windowGl.GlStateCallsSkipped;
          const uint64_t nanoseconds =
              gl.PicaDrawNanoseconds - windowGl.PicaDrawNanoseconds;
          perf << " gl_cache=" << (gl.GlStateCacheEnabled ? 1 : 0)
               << " gl_draws=" << perFrame(gl.PicaDraws, windowGl.PicaDraws)
               << " gl_presents="
               << perFrame(gl.PicaPresents, windowGl.PicaPresents)
               << " gl_state_calls=" << static_cast<double>(issued) / hostFrames
               << " gl_state_skipped="
               << static_cast<double>(skipped) / hostFrames
               << " gl_state_calls_per_draw="
               << (draws != 0U ? static_cast<double>(issued) / draws : 0.0)
               << " gl_program_binds="
               << perFrame(gl.GlProgramBinds, windowGl.GlProgramBinds)
               << " gl_texture_binds="
               << perFrame(gl.GlTextureBinds, windowGl.GlTextureBinds)
               << " gl_vao_binds="
               << perFrame(gl.GlVertexArrayBinds, windowGl.GlVertexArrayBinds)
               << " gl_fb_binds="
               << perFrame(gl.GlFramebufferBinds, windowGl.GlFramebufferBinds)
               << " gl_ubo_uploads="
               << perFrame(gl.PublishedUniformUploads,
                           windowGl.PublishedUniformUploads)
               << " gl_draw_ms="
               << static_cast<double>(nanoseconds) / 1.0e6 / hostFrames
               << " gl_us_per_draw="
               << (draws != 0U ? static_cast<double>(nanoseconds) / 1.0e3 / draws
                               : 0.0);
          windowGl = gl;
        }
      }
      // Totals of a Mesa built with the Horizon shader cache (absent from
      // the stock devkitPro library, hence weak).
      if (mesa_switch_get_stats != nullptr) {
        static constexpr std::array<const char*, 25> kMesaNames{
            "glsl_compiles", "glsl_compile_ms", "glsl_compiles_skipped", "links", "link_ms",
            "link_glsl_ms", "link_st_ms", "links_from_cache", "nvc0_translates", "nvc0_translate_ms",
            "nvc0_cache_hits", "binary_loads", "binary_load_fails", "binary_load_ms", "binary_saves",
            "binary_save_ms", "cache_gets", "cache_get_hits", "cache_get_ms", "cache_corrupt",
            "cache_puts", "cache_put_bytes", "cache_put_ms", "cache_puts_dropped", "cache_has_key_hits"};
        std::array<uint64_t, 32> mesa{};
        const unsigned count = std::min<unsigned>(
            mesa_switch_get_stats(mesa.data(), static_cast<unsigned>(mesa.size())), kMesaNames.size());
        perf << " mesa:";
        for (unsigned index = 0; index < count; ++index) {
          const bool nanoseconds = std::string_view(kMesaNames[index]).ends_with("_ms");
          perf << ' ' << kMesaNames[index] << '='
               << (nanoseconds ? mesa[index] / 1000000U : mesa[index]);
        }
        if (mesa_switch_shader_cache_status != nullptr) {
          perf << " cache_status=\"" << mesa_switch_shader_cache_status() << '"';
        }
      }
      perf << '\n';
      // config/gpu_timing: GPU/CPU per render-thread stage and GL call
      // counts over the same window (empty when off).
      if (Fast::GlPerf::Enabled()) {
        perf << Fast::GlPerf::TakeReport(runFrameCount) << '\n';
      }
      windowStart = now;
      windowValues = values;
      windowHostFrames = runFrameCount;
      windowGuestFrames = frameCount;
    }
  }
  // Write once at the first frame and at most once more when a persistent
  // geometry is first observed. Periodic receipt writes would put
  // synchronous SD filesystem work directly in the presentation loop.
  if (runFrameCount == 1U || !gSwitchPicaGeometryCacheObserved) {
    const auto wholeAotStats =
        Oot3dNativeGame::GetOot3dCompiledFunctionStats();
    gSwitchWholeAotBlockLimitExits =
        wholeAotStats.WholeAotBlockLimitExits;
    const auto picaBackend = api.GetNativePicaBackendStats();
    gSwitchPicaGeometryCacheEnabled =
        picaBackend.GeometryCacheEnabled;
    gSwitchPicaGeometryPersistentDraws =
        picaBackend.GeometryPersistentDraws;
    gSwitchPicaGeometryPersistentUploads =
        picaBackend.GeometryPersistentUploads;
    gSwitchPicaGeometryRegistryEntries =
        picaBackend.GeometryRegistryEntries;
    gSwitchPicaGeometryCacheObserved =
        gSwitchPicaGeometryPersistentDraws != 0U &&
        gSwitchPicaGeometryPersistentUploads != 0U &&
        gSwitchPicaGeometryRegistryEntries != 0U;
    if (runFrameCount == 1U || gSwitchPicaGeometryCacheObserved) {
      WriteSwitchBootStage("runtime_running");
    }
  }
#endif
  const auto completedFrameTime = std::chrono::steady_clock::now();
  if (!preparationScreen.Done()) {
    preparationScreen.ObserveFrame(
        std::chrono::duration<double, std::milli>(completedFrameTime - lastCompletedFrameTime).count(),
        completedFrameTime);
    if (preparationScreen.Done()) {
      std::fprintf(stderr,
                   "TRIAEVUM_INITIAL_PREPARATION parts=%zu startup_seconds=%.3f wait_seconds=%.3f "
                   "first_frames=%u first_frames_seconds=%.3f\n",
                   preparationScreen.Parts(), preparationScreen.StartupSeconds(),
                   preparationScreen.WaitSeconds(),
                   preparationScreen.FirstFrames(), preparationScreen.FirstFramesSeconds());
    }
  }
  lastCompletedFrameTime = completedFrameTime;
  if (!benchmarkMeasurementStarted && preparationScreen.Done() &&
      runFrameCount >= hostArgs.BenchmarkWarmupFrames) {
    benchmark.MeasurementStart = completedFrameTime;
    benchmark.MeasurementEnd = completedFrameTime;
    benchmarkMeasurementStarted = true;
    if (cpuProbeRequested) CpuProbe::Thread.StartAt(CpuProbe::Read());
  } else if (benchmarkMeasurementStarted &&
             runFrameCount > hostArgs.BenchmarkWarmupFrames) {
    const auto currentPhases = phaseTiming.Values();
    std::array<double, 15> delta{};
    for (size_t i=0; i<delta.size(); ++i) {
      delta[i]=currentPhases[i]-phaseBeforeFrame[i];
      benchmark.PhaseSeconds[i]+=delta[i];
    }
    const double nestedGuest=phaseTiming.GuestInsidePicaSubmitSeconds-nestedGuestBeforeFrame;
    benchmark.NestedGuestSeconds+=nestedGuest;
    benchmark.GuestClockSeconds+=phaseTiming.GuestClockSeconds-guestClockBeforeFrame;
    const Oot3dNativeGame::PreBackendBudget budget{
      delta[0],delta[7],nestedGuest,delta[8],delta[2],delta[3],delta[5],delta[6]};
    if(budget.Valid()) benchmark.PreBackendTimes.RecordMilliseconds(budget.Total()*1000.0);
    else ++benchmark.InvalidPreBackendIntervals;
    // Present includes pacing. Visual presentation includes backend/replay.
    // Remove each inclusive scope once; keep unclassified host/UI work visible.
    const double envelope=std::chrono::duration<double>(completedFrameTime-benchmark.MeasurementEnd).count()
        -delta[4]-delta[11]-delta[13];
    benchmark.PreBackendEnvelopeTimes.RecordMilliseconds(envelope*1000.0);
    benchmark.UnattributedSeconds+=envelope-budget.Total();
    if (benchmarkFrameTimes) {
      auto phases = phaseTiming.Values();
      for (size_t i = 0; i < phases.size(); ++i)
        phases[i] = (phases[i] - phaseBeforeFrame[i]) * 1000.0;
      benchmark.SlowFrames.Record(runFrameCount, std::chrono::duration<double, std::milli>(
          completedFrameTime - benchmark.MeasurementEnd).count(), phases);
      benchmarkFrameTimes->RecordMilliseconds(
          std::chrono::duration<double, std::milli>(
              completedFrameTime - benchmark.MeasurementEnd).count());
    }
    benchmark.MeasurementEnd = completedFrameTime;
    ++benchmark.MeasuredFrames;
    if (cpuProbeRequested) benchmarkCpuPhases = CpuProbe::Thread.Snapshot();
  }
  for (const auto &action : launch.SavestateActions) {
    if (runFrameCount == static_cast<uint64_t>(action.RunFrame) + 1U) {
      savestateLibrary->Request(
          {action.Load ? Oot3dNativeGame::SavestateRequestKind::Load
                       : Oot3dNativeGame::SavestateRequestKind::Save,
           action.WithSettings, action.Path});
    }
  }
  ServiceSavestateRequests();
  {
    if (Oot3dNativeGame::ShouldCaptureNativeA32AutomatedState(
            launch.SaveStateFrameAvailable,
            automatedStateSaved || automatedStateSaveFailed, runFrameCount,
            launch.SaveStateFrame)) {
      const auto outcome =
          SaveState(launch.SaveStatePath, launch.SaveStateWithSettings);
      if (outcome == SaveStateOutcome::Saved) {
        automatedStateSaved = true;
        automatedStateSavedAtRunFrame = runFrameCount - 1U;
        automatedStateSavedAtGuestFrame = frameCount;
      } else if (outcome == SaveStateOutcome::Failed) {
        automatedStateSaveFailed = true;
      }
    }
  }
  if (hostArgs.FrameLimit > 0 && runFrameCount >= hostArgs.FrameLimit)
    scriptedExit = true;  // before the close the render thread may observe
  ApplyWindowDemoFrameLimit(hostArgs, window, runFrameCount);
  return true;
}

// The guest input of one refresh: touch and TopScreen routing state, the input timeline or the polled
// frame. False when the input asked to close the window.
bool NativeA32WindowSession::SampleGuestRefreshInput(
    const uint32_t &guestRefreshIndex,
    bool &nativeFrontendTouchEnabled,
    uint32_t &topScreenNativeGameMode,
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &physicalInputFrame,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  if (guestRefreshIndex != 0U) {
    nativeFrontendTouchEnabled = ResolveNativeA32TouchInputEnabled(
        process.Memory(), uiLifecycleBridge,
        nativeCandidateDispatch.TopScreenUiProfile);
    topScreenNativeGameMode = UINT32_MAX;
    const bool refreshedTopScreenGameplayInputMode =
        process.Memory().Read32(kNativeGameMode,
                                &topScreenNativeGameMode) &&
        Oot3dNativeGame::IsTopScreenGameplayInputMode(
            topScreenNativeGameMode);
    topScreenStartRoutingEligible =
        uiLifecycleBridge.NativeGameplayPresentationActive() &&
        refreshedTopScreenGameplayInputMode &&
        Oot3dNativeGame::IsTopScreenStartRoutingMode(topScreenNativeGameMode);
    picaCompositionDomain =
        refreshedTopScreenGameplayInputMode
            ? Oot3dNativeGame::Oot3dPicaCompositionDomain::Scene
            : Oot3dNativeGame::Oot3dPicaCompositionDomain::Unknown;
  }
  // Re-arm after any state restore: RestoreState replaces the memory
  // image, and the watch is diagnostic state that must not disappear with
  // it. Cheap when the trapped page set is unchanged.
  if (!rawCopyWatchRanges.empty() &&
      !process.Memory().RangeWatchActive()) {
    process.Memory().SetRangeWatch(rawCopyWatchRanges);
  }
  inputFrame = ResolveNativeA32GuestInput(
      physicalInputFrame, inputTimeline, frameCount, runFrameCount,
      nativeFrontendTouchEnabled, topScreenStartRoutingEligible,
      nativeCandidateDispatch.TopScreenUiProfile,
      nativeCandidateDispatch.StartButtonLatch,
      nativeCandidateDispatch.TopScreenStartRouting);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ApplyTopScreenVersionChoiceReveal(
          inputFrame, uiLifecycleBridge.NativeVersionChoiceAwaitingReveal(),
          uiLifecycleBridge.NativeVersionChoicePrompting(),
          nativeCandidateDispatch.TopScreenVersionChoiceReveal)) {
    uiLifecycleBridge.RevealNativeVersionChoice();
  }
  inputDiagnostics.Observe(inputFrame);
  if (inputFrame.Exit) {
    window.Close();
    return false;
  }
  return true;
}

// Before the guest runs a refresh: TopScreen input routing, the structural scenario, the guest clock to the
// next vblank, HID and DSP audio. False when the scenario closed the window.
bool NativeA32WindowSession::UpdateGuestServicesForRefresh(
    bool &topScreenStartRoutingEligible,
    Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &window = *windowPointer;
  auto &dspHle = *dspHleStorage;
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  UpdateDevFreeCamera(inputFrame);
#endif
  const uint32_t topScreenButtons = inputFrame.Hid.Buttons;
  const uint32_t topScreenPressed =
      topScreenButtons &
      ~nativeCandidateDispatch.PreviousTopScreenButtons;
  const auto topScreenButton =
      [](Oot3dNativeGame::NativeA32HidButton value) {
        return Oot3dNativeGame::NativeA32HidButtonMask(value);
      };
  Oot3dNativeGame::TopScreenDpadActionState topScreenDpadActions;
  Oot3dNativeGame::TopScreenOcarinaState ocarinaOwner;
  const bool ocarinaOwnsDpad = nativeCandidateDispatch.TopScreenUiProfile &&
      Oot3dNativeGame::ReadTopScreenOcarinaState(process.Memory(), &ocarinaOwner) &&
      ocarinaOwner.Active;
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      topScreenStartRoutingEligible && !ocarinaOwnsDpad) {
    bool childLink = false;
    std::string childLinkError;
    if (!Oot3dNativeGame::ReadTopScreenChildLink(
            process.Memory(), &childLink, &childLinkError)) {
      throw std::runtime_error("TopScreen Link age decode failed: " +
                               childLinkError);
    }
    Oot3dNativeGame::TopScreenDpadPhysicalState dpadPhysical;
    dpadPhysical.ChildLink = childLink;
    constexpr std::array<Oot3dNativeGame::NativeA32HidButton, 4>
        kDpadButtons{Oot3dNativeGame::NativeA32HidButton::DpadUp,
                     Oot3dNativeGame::NativeA32HidButton::DpadDown,
                     Oot3dNativeGame::NativeA32HidButton::DpadLeft,
                     Oot3dNativeGame::NativeA32HidButton::DpadRight};
    for (std::size_t direction = 0U;
         direction < kDpadButtons.size(); ++direction) {
      const auto mask = topScreenButton(kDpadButtons[direction]);
      dpadPhysical.Pressed[direction] =
          (topScreenPressed & mask) != 0U;
      dpadPhysical.Held[direction] =
          (topScreenButtons & mask) != 0U;
    }
    topScreenDpadActions = Oot3dNativeGame::ResolveTopScreenDpadActions(
        activeTopScreenConfig, dpadPhysical);
  }
  const bool zrHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zr);
  const bool zlHeld = ThreeDsRecomp::Input::IsButtonHeld(
      inputFrame, ThreeDsRecomp::Input::Button::Zl);
  Oot3dNativeGame::TopScreenExtendedInputFrame sampledTopScreenInput = {
      .ZrHeld =
          zrHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZr),
      .ZlHeld =
          zlHeld ||
          topScreenDpadActions.IsHeld(
              Oot3dNativeGame::TopScreenDpadAction::ItemZl),
      .DpadLeftHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadLeft)) != 0U,
      .DpadRightHeld =
          (inputFrame.Hid.Buttons &
           Oot3dNativeGame::NativeA32HidButtonMask(
               Oot3dNativeGame::NativeA32HidButton::DpadRight)) != 0U};
  sampledTopScreenInput.DpadUpHeld =
      (inputFrame.Hid.Buttons & topScreenButton(
          Oot3dNativeGame::NativeA32HidButton::DpadUp)) != 0U;
  if (ocarinaOwnsDpad) {
    inputFrame.Hid.Buttons &= ~(topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadLeft) |
                               (ocarinaOwner.Page == 12 ? topScreenButton(
                                   Oot3dNativeGame::NativeA32HidButton::DpadUp) : 0U) |
                               topScreenButton(Oot3dNativeGame::NativeA32HidButton::DpadRight));
  }
  sampledTopScreenInput.XHeld =
      (topScreenButtons &
       topScreenButton(Oot3dNativeGame::NativeA32HidButton::X)) != 0U;
  sampledTopScreenInput.YHeld =
      (topScreenButtons &
       topScreenButton(Oot3dNativeGame::NativeA32HidButton::Y)) != 0U;
  sampledTopScreenInput.GameplayDpadActionsOwned = true;
  sampledTopScreenInput.RestorationLayout =
      activeTopScreenConfig.HudLayout ==
      Oot3dNativeGame::TopScreenHudLayout::Restoration;
  nativeCandidateDispatch.TopScreenInputClock.ObserveGuest(sampledTopScreenInput);
  if (nativeCandidateDispatch.TopScreenUiProfile &&
      activeTopScreenConfig.FreeCameraEnabled &&
      nativeControlPollingState.GameplayMouseOwned) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else if (inputFrame.CStick.Kind ==
             Oot3dNativeGame::NativeFreeCameraInputKind::Absolute) {
    nativeCandidateDispatch.TopScreenCameraInput.Observe(
        inputFrame.CStick);
  } else {
    nativeCandidateDispatch.TopScreenCameraInput.Reset();
  }
  if (nativeCandidateDispatch.TopScreenUiProfile) {
    const uint32_t buttons = inputFrame.Hid.Buttons;
    const uint32_t pressed =
        buttons & ~nativeCandidateDispatch.PreviousTopScreenButtons;
    const auto button = [](Oot3dNativeGame::NativeA32HidButton value) {
      return Oot3dNativeGame::NativeA32HidButtonMask(value);
    };
    const bool gameplayViewPressed = topScreenDpadActions.WasPressed(
        Oot3dNativeGame::TopScreenDpadAction::View);
    const bool gameplayOcarinaPressed =
        topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::Ocarina);
    const bool leftShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::L)) != 0U;
    const bool rightShoulderHeld =
        (buttons &
         button(Oot3dNativeGame::NativeA32HidButton::R)) != 0U;
    bool selectPressed =
        (pressed &
         button(Oot3dNativeGame::NativeA32HidButton::Select)) != 0U;
    // R6 (2.1.1 0x005D5730..5BC0): on an open Items/Gear page SELECT toggles
    // the item description; it reaches the gameplay routes (minimap, Save)
    // only while the pause controller is in its gameplay state.
    if (Oot3dNativeGame::TopScreenItemDescriptionInputs description;
        Oot3dNativeGame::ReadTopScreenItemDescriptionInputs(process.Memory(), &description)) {
      auto &descriptionRuntime = *nativeCandidateDispatch.ItemDescription;
      if (Oot3dNativeGame::UpdateTopScreenItemDescription(
              description, selectPressed, descriptionRuntime.VisibleState())) {
        descriptionRuntime.CountToggle();
        std::string soundError;
        if (!Oot3dNativeGame::PlayTopScreenUiSound(process, 0x01000498U,
                                                   kTopScreenDirectCallReturn, &soundError))
          throw std::runtime_error("TopScreen description sound failed: " + soundError);
      }
      if (selectPressed && Oot3dNativeGame::TopScreenItemDescriptionOwnsSelect(description)) {
        selectPressed = false;
        inputFrame.Hid.Buttons &= ~button(Oot3dNativeGame::NativeA32HidButton::Select);
      }
    }
    if (topScreenDpadActions.WasPressed(
            Oot3dNativeGame::TopScreenDpadAction::MinimapToggle) ||
        (selectPressed &&
         activeTopScreenConfig.SelectAction ==
             Oot3dNativeGame::TopScreenSelectAction::MinimapToggle)) {
      SetTopScreenSessionMinimapVisible(
          !activeTopScreenConfig.MinimapVisible);
    }
    if (selectPressed &&
        activeTopScreenConfig.SelectAction ==
            Oot3dNativeGame::TopScreenSelectAction::MinimapToggle) {
      inputFrame.Hid.Buttons &=
          ~button(Oot3dNativeGame::NativeA32HidButton::Select);
    }

    if (gameplayViewPressed || gameplayOcarinaPressed) {
      ++nativeCandidateDispatch.TopScreenGameplayDpadAttempts;
      Oot3dNativeGame::TopScreenGameplayDpadResult dpadResult;
      std::string dpadError;
      if (!Oot3dNativeGame::PrepareTopScreenGameplayDpadGuest(
              process.Memory(),
              {.ViewPressed = gameplayViewPressed,
               .OcarinaPressed = gameplayOcarinaPressed,
               .LeftShoulderHeld = leftShoulderHeld,
               .RightShoulderHeld = rightShoulderHeld},
              &dpadResult, &dpadError)) {
        throw std::runtime_error(
            "TopScreen gameplay D-pad prepare failed: " + dpadError);
      }
      nativeCandidateDispatch.TopScreenNaviViewActivations +=
          dpadResult.NaviViewActivated ? 1U : 0U;
      if (dpadResult.OcarinaQueryRequired) {
        ++nativeCandidateDispatch.TopScreenOcarinaQueries;
        uint32_t nativeEligibility = 0U;
        constexpr std::array<std::uint32_t, 0> kNoArguments{};
        if (!process.InvokeFunctionWithResult(
                kTopScreenOcarinaEligibility, kNoArguments,
                kTopScreenDirectCallReturn, &nativeEligibility,
                &dpadError) ||
            !Oot3dNativeGame::CommitTopScreenGameplayOcarinaGuest(
                process.Memory(), nativeEligibility, &dpadResult,
                &dpadError)) {
          throw std::runtime_error(
              "TopScreen ocarina D-pad route failed: " + dpadError);
        }
        nativeCandidateDispatch.TopScreenOcarinaActivations +=
            dpadResult.OcarinaActivated ? 1U : 0U;
      }
    }
    std::string gameplayActionError;
    if (const auto sound = uiLifecycleBridge.TakeOcarinaGuideSound(); sound != 0U) {
      if (!Oot3dNativeGame::PlayTopScreenUiSound(
              process, sound, kTopScreenDirectCallReturn, &gameplayActionError)) {
        throw std::runtime_error("TopScreen ocarina guide sound failed: " + gameplayActionError);
      }
    }
    if (!Oot3dNativeGame::ConsumeTopScreenGameplayActions(
            process, topScreenDpadActions,
            nativeCandidateDispatch.TopScreenInput.ZrPressed ||
                nativeCandidateDispatch.TopScreenInput.ZlPressed,
            kTopScreenDirectCallReturn,
            &nativeCandidateDispatch.TopScreenGameplayActionRuntime,
            &nativeCandidateDispatch.TopScreenGameplayActions,
            &gameplayActionError)) {
      throw std::runtime_error(
          "TopScreen gameplay action failed: " +
          gameplayActionError);
    }
    nativeCandidateDispatch.PreviousTopScreenButtons = buttons;
    nativeCandidateDispatch.TopScreenItems.DirectItemId =
        nativeCandidateDispatch.TopScreenGameplayActionRuntime.DirectItem.ActiveItemId;
    nativeCandidateDispatch.PauseTransaction->AgeUndelivered();
    Oot3dNativeGame::TopScreenPauseDrawInputs pauseInputs;
    if (!Oot3dNativeGame::ReadTopScreenPauseDrawInputs(process.Memory(),
                                                       &pauseInputs)) {
      nativeCandidateDispatch.TopScreenPauseAotBarrier = true;
    } else {
      // ChildStates: [0] Gear 0x00504484, [1] Items 0x0050672C,
      // [2] Dungeon/World Map 0x00506CE8.
      Oot3dNativeGame::ObserveTopScreenStartRoutingPage(
          pauseInputs.ChildStates[1] != 0U, pauseInputs.ChildStates[0] != 0U,
          pauseInputs.ChildStates[2] != 0U,
          nativeCandidateDispatch.TopScreenStartRouting);
      const auto closePlan =
          Oot3dNativeGame::ResolveTopScreenPauseStartClose(
              pauseInputs,
              (pressed &
               button(Oot3dNativeGame::NativeA32HidButton::Start)) != 0U);
      // The transactions run inside this refresh's native UI update
      // (oot3d_top_screen_pause_transaction.h), not here: the guest may be
      // suspended inside a routine whose tail would undo them.
      if (closePlan.Page !=
          Oot3dNativeGame::TopScreenPauseClosePage::None) {
        nativeCandidateDispatch.PauseTransaction->RequestStartClose();
        Oot3dNativeGame::MarkTopScreenStartCloseGestureConsumed(
            nativeCandidateDispatch.TopScreenStartRouting);
        inputFrame.Hid.Buttons &=
            ~button(Oot3dNativeGame::NativeA32HidButton::Start);
        // START wins: a pending Save/Quit of the same page is dropped.
        nativeCandidateDispatch.TopScreenPauseSystemOpen = {};
      } else if (nativeCandidateDispatch.TopScreenStartRouting.OpenRequested) {
        nativeCandidateDispatch.PauseTransaction->RequestOpenLastPage(
            nativeCandidateDispatch.TopScreenStartRouting.LastPage);
      } else if (activeTopScreenConfig.ExitItemsToSaveScreen) {
        const auto systemOpenPlan =
            Oot3dNativeGame::ResolveTopScreenPauseBSystemOpen(
                pauseInputs,
                (pressed &
                 button(Oot3dNativeGame::NativeA32HidButton::B)) != 0U,
                nativeCandidateDispatch.TopScreenPauseSystemOpen);
        if (systemOpenPlan.Page !=
            Oot3dNativeGame::TopScreenPauseClosePage::None) {
          nativeCandidateDispatch.PauseTransaction->RequestSystemOpen(
              systemOpenPlan);
          // 0x005D5478..5558: B-to-Save clears the description.
          nativeCandidateDispatch.ItemDescription->Hide();
        }
      }
      const bool sceneRuntimeRoute = pauseInputs.HasScene &&
                                     pauseInputs.SceneMode == 3U &&
                                     (pauseInputs.RuntimeMode == 1U ||
                                      pauseInputs.RuntimeMode == 2U);
      nativeCandidateDispatch.TopScreenPauseAotBarrier =
          sceneRuntimeRoute ||
          Oot3dNativeGame::ResolveTopScreenPauseRouteActive(
              pauseInputs,
              &nativeCandidateDispatch.TopScreenAotPauseRoute);
    }
  }
  nativeCandidateDispatch.TopScreenStartRouting.OpenRequested = false;
  if (scenarioBootstrap.has_value() && !scenarioBootstrap->Finished()) {
    std::string scenarioError;
    if (!scenarioBootstrap->Advance(
            process, sceneViewProbe.Stats().LastPlayStateAddress,
            frameCount, &scenarioError)) {
      std::cerr << "oot3d_native_game: structural scenario failed: "
                << scenarioError << '\n';
      if (launch.ScenarioStrict) {
        scriptedExit = true;
        window.Close();
        return false;
      }
    }
    if (launch.ScenarioAutoExit && scenarioBootstrap->Complete()) {
      scriptedExit = true;
      window.Close();
      return false;
    }
  }
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  // F1 > Dev Tools warps run at the same guest-frame boundary as the
  // structural scenarios, through the same native transition request.
  if (devWarp != nullptr) {
    if (devWarpGuest == nullptr) devWarpGuest = Oot3dNativeGame::MakeDevWarpProcessGuest(process);
    devWarp->Advance(*devWarpGuest, sceneViewProbe.Stats().LastPlayStateAddress, frameCount);
    UpdateDevFreeCameraTour(frameCount);
    if (devLab) UpdateDevLab(frameCount);
  }
#endif
  refreshTickRemainder += kCtrArm11TicksPerSecond;
  nextVblankTick += refreshTickRemainder / kGuestDisplayRefreshRate;
  refreshTickRemainder %= kGuestDisplayRefreshRate;
  const auto clockStart = std::chrono::steady_clock::now();
  const auto clockResolution = AdvanceGuestClockTo(
      process, hostServices, processResult, nextVblankTick,
      launch.WholeAotBlockBudget);
  const double clockSeconds = SecondsSince(clockStart);
  phaseTiming.GuestClockSeconds += clockSeconds;
  phaseTiming.GuestSeconds += clockSeconds;
  if (clockResolution.OvershootTicks != 0U) {
    ++lateVblankDeadlineCount;
    maximumVblankDeadlineOvershootTicks =
        std::max(maximumVblankDeadlineOvershootTicks,
                 clockResolution.OvershootTicks);
  }
  const auto hidUpdate = hostServices.AdvanceHidToCurrentTick(
      process.Memory(), inputFrame.Hid);
  if (hidUpdate.Status ==
      Oot3dNativeGame::NativeA32CtrHidUpdateStatus::Failed) {
    throw std::runtime_error(
        "native HID sample could not be written to shared memory");
  }
  if (hidUpdate.EventsSignaled &&
      processResult.Kind !=
          Oot3dNativeGame::NativeA32ProcessRunKind::Terminated) {
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  const uint32_t dspAudioFrames =
      hostServices.TakePendingDspAudioFrames();
  std::vector<int16_t> audioSamples;
  audioSamples.reserve(static_cast<size_t>(dspAudioFrames) * 320U);
  for (uint32_t audioFrame = 0; audioFrame < dspAudioFrames;
       ++audioFrame) {
    const size_t frameSampleOffset = audioSamples.size();
    const auto dspMixStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuDsp(CpuProbe::Phase::Dsp);
    if (!dspHle.ProcessFrame(process.Memory(), audioSamples, &error)) {
      throw std::runtime_error("native DSP HLE mix failed: " + error);
    }
    phaseTiming.DspMixSeconds += SecondsSince(dspMixStart);
    cpuDsp.Stop();
    pcmContinuityDiagnostics.ObserveFrame(
        dspStats.FramesMixed, std::span<const int16_t>(audioSamples)
                            .subspan(frameSampleOffset));
    ++dspStats.FramesMixed;
    dspStats.FramesSuppressed += launch.DisableAudio ? 1U : 0U;
    if (!hostServices.SignalDspAudioFrame()) {
      break;
    }
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    phaseTiming.GuestSeconds += SecondsSince(phaseStart);
    RequireRunnableGuest(processResult);
  }
  for (const int16_t sample : audioSamples) {
    const int32_t magnitude = std::abs(static_cast<int32_t>(sample));
    dspStats.PeakMagnitude = std::max(dspStats.PeakMagnitude, magnitude);
    dspStats.NonzeroSamples += sample != 0 ? 1U : 0U;
    const uint16_t bits = static_cast<uint16_t>(sample);
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
    dspStats.PcmFnv1a64 ^= static_cast<uint8_t>(bits >> 8U);
    dspStats.PcmFnv1a64 *= 1099511628211ULL;
  }
  dspStats.SamplesMixed += audioSamples.size() / 2U;
  if (!launch.AudioPcmDumpPath.empty()) {
    pcmDumpSamples.insert(pcmDumpSamples.end(), audioSamples.begin(),
                          audioSamples.end());
  }
  if (hostAudioStream != nullptr && !audioSamples.empty()) {
    // Stretch, resampling and the device queue run on the audio worker.
    const auto audioOutputStart = std::chrono::steady_clock::now();
    hostAudioStream->Submit(audioSamples);
    phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
  } else if (hostAudioStream == nullptr && !launch.DisableAudio &&
             audioPlayer != nullptr && audioPlayer->IsInitialized() &&
             !audioSamples.empty()) {
    const auto audioOutputStart = std::chrono::steady_clock::now();
    CpuProbe::Scope cpuAudio(CpuProbe::Phase::AudioOutput);
    const int32_t bufferedBefore = audioPlayer->Buffered();
    const std::vector<int16_t> *outputSamples = &audioSamples;
    if (hostAudioResampler.has_value()) {
      // Keep the host queue at its target by nudging the conversion ratio
      // (at most 0.5%, a few cents) instead of letting rate mismatch end in
      // underruns or dropped chunks.
      const double target =
          std::max<double>(audioPlayer->GetDesiredBuffered(), 1.0);
      hostAudioQueueEstimate =
          hostAudioQueueEstimate < 0.0
              ? static_cast<double>(bufferedBefore)
              : hostAudioQueueEstimate +
                    0.05 * (static_cast<double>(bufferedBefore) -
                            hostAudioQueueEstimate);
      hostAudioResampler->SetRateAdjustment(std::clamp(
          (hostAudioQueueEstimate - target) / target * 0.01, -0.005, 0.005));
      hostAudioResampled.clear();
      hostAudioResampler->Process(audioSamples, hostAudioResampled);
      outputSamples = &hostAudioResampled;
    }
    if (!outputSamples->empty()) {
      audioPlayer->Play(
          reinterpret_cast<const uint8_t *>(outputSamples->data()),
          outputSamples->size() * sizeof(int16_t));
    }
    audioOutputDiagnostics.Observe(
        frameCount, bufferedBefore, audioPlayer->Buffered(),
        outputSamples->size() / 2U, audioPlayer->GetDesiredBuffered());
    phaseTiming.AudioOutputSeconds += SecondsSince(audioOutputStart);
  }
  if (!hostServices.SignalVBlank(process.Memory())) {
    throw std::runtime_error(
        "native VBlank could not enter the GSP relay queue");
  }
  ++vblankCount;
  return true;
}

// Runs the guest until it waits and drains the PICA work it submitted during the refresh.
void NativeA32WindowSession::RunGuestRefresh() {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  const auto &picaMemoryView = *picaMemoryViewStorage;
  auto &submissionQueue = *submissionQueueStorage;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  // PPF unblocks the current guest refresh, not the next presentation.
  // The dependency capture keeps its plans for the complete visual frame.
  (void)FlushNativeDependencies();
  phaseStart = std::chrono::steady_clock::now();
  processResult =
      RunUntilGuestWait(process, launch.WholeAotBlockBudget);
  phaseTiming.GuestSeconds += SecondsSince(phaseStart);
  RequireRunnableGuest(processResult);

  phaseStart = std::chrono::steady_clock::now();
  const auto picaSubmitStart = phaseStart;
  CpuProbe::Scope cpuQueue(CpuProbe::Phase::PicaQueue);
  size_t drainPassesThisRefresh = 0;
  bool refreshHadNativeDraws = false;
  while (drainPassesThisRefresh < kMaximumPicaDrainPassesPerRefresh) {
    PublishNativeSceneView();
    submissionQueue.TakePendingDraws(drainedDraws);
    auto &draws = drainedDraws;
    auto completions = submissionQueue.TakePendingCompletions();
    auto displayTransfers = submissionQueue.TakePendingDisplayTransfers();
    auto memoryFills = submissionQueue.TakePendingMemoryFills();
    if (draws.empty() && completions.empty() &&
        displayTransfers.empty() && memoryFills.empty()) {
      break;
    }
    ++drainPassesThisRefresh;
    ++picaStats.PicaDrainPassCount;
    refreshHadNativeDraws = refreshHadNativeDraws || !draws.empty();
    const bool suppressTopScreenFrontendBackdrop =
        Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    // The native menu backdrops TopScreen removes: the shared
    // CommonBackground00 of File Select and Name Entry, and the version
    // choice's own background.
    std::array<std::optional<uint32_t>, 2> frontendBackdropPhysicalAddresses{};
    if (suppressTopScreenFrontendBackdrop) {
      const std::array<std::optional<uint32_t>, 2> frontendBackdropGuestAddresses{
          uiLifecycleBridge.NativePauseSharedTextureGuestAddress(
              oot3d::ui::UiPauseSharedTextureSlot::CommonBackground00),
          uiLifecycleBridge.NativeVersionChoiceBackdropGuestAddress()};
      for (size_t index = 0; index < frontendBackdropGuestAddresses.size();
           ++index) {
        if (frontendBackdropGuestAddresses[index].has_value()) {
          frontendBackdropPhysicalAddresses[index] =
              picaMemoryView.TranslateGuest(
                  *frontendBackdropGuestAddresses[index], 1U);
        }
      }
    }
    const auto usesFrontendBackdrop =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          return std::any_of(
              state.Textures.begin(), state.Textures.end(),
              [&](const auto &texture) {
                return texture.Enabled &&
                       std::any_of(frontendBackdropPhysicalAddresses.begin(),
                                   frontendBackdropPhysicalAddresses.end(),
                                   [&](const auto &address) {
                                     return address.has_value() &&
                                            texture.PhysicalAddress == *address;
                                   });
              });
        };
    std::vector<Oot3dNativeGame::TopScreenFrontendCanvas>
        frontendBackdropCanvases;
    const auto rememberFrontendBackdropCanvas =
        [&](const Oot3dNativeGame::Oot3dPicaDecodedDrawState& state) {
          if (state.Framebuffer.ColorPhysicalAddress != 0U &&
              std::none_of(frontendBackdropCanvases.begin(),
                           frontendBackdropCanvases.end(),
                           [&](const auto& canvas) {
                             return canvas.Matches(state);
                           })) {
            frontendBackdropCanvases.push_back(
                {state.Framebuffer.ColorPhysicalAddress, state.Viewport});
          }
        };
    if (suppressTopScreenFrontendBackdrop) {
      for (const auto &draw : draws) {
        if (usesFrontendBackdrop(draw.State)) {
          rememberFrontendBackdropCanvas(draw.State);
        }
      }
    }
    bool signaledPicaCompletion = false;
    size_t completionIndex = 0;
    size_t memoryFillIndex = 0;
    size_t displayTransferIndex = 0;
    const auto submitMemoryFill =
        [&](const Oot3dNativeGame::Oot3dPicaMemoryFillSubmission &fill) {
          picaSemanticTrace.RecordMemoryFill(
              presentationFrameCount, frameCount, fill);
          picaPresentationScheduler.Capture(fill);
          if (fill.CompletionId != 0U) {
            if (!fill.Interrupt.has_value() ||
                !hostServices.SignalPicaInterrupt(process.Memory(),
                                                  *fill.Interrupt)) {
              throw std::runtime_error(
                  "native PICA memory fill completion "
                  "could not enter the GSP relay queue");
            }
            if (*fill.Interrupt ==
                Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
              ++picaStats.CompletedP3dCount;
            } else if (*fill.Interrupt ==
                       Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
              ++picaStats.CompletedPpfCount;
            }
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
        };
    const auto submitDisplayTransfer =
        [&](const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
                &transfer) {
          const auto topFramebuffer = hostServices.TopFramebuffer();
          const bool selectedForTop = topFramebuffer.has_value() &&
                                      (transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressLeft ||
                                       transfer.Transfer.OutputAddress ==
                                           topFramebuffer->AddressRight);
          picaSemanticTrace.RecordDisplayTransfer(
              presentationFrameCount, frameCount, transfer,
              selectedForTop, suppressTopScreenFrontendBackdrop);
          if (launch.ExtendedDiagnostics) {
            if (displayTransferEvents.size() == 64U) {
              displayTransferEvents.erase(displayTransferEvents.begin());
            }
            displayTransferEvents.push_back({
                {"input_address", transfer.Transfer.InputAddress},
                {"output_address", transfer.Transfer.OutputAddress},
                {"input_physical_address", transfer.InputPhysicalAddress},
                {"output_physical_address",
                 transfer.OutputPhysicalAddress},
                {"input_size", transfer.Transfer.InputSize},
                {"output_size", transfer.Transfer.OutputSize},
                {"flags", transfer.Transfer.Flags},
                {"texture_copy_bytes", transfer.Transfer.TextureCopyBytes},
                {"after_draw_submission_id",
                 transfer.AfterDrawSubmissionId},
                {"signals_guest_interrupt", transfer.SignalInterrupt},
                {"selected_for_top_at_submission", selectedForTop},
                {"topscreen_frontend_backdrop_suppression_active",
                 suppressTopScreenFrontendBackdrop},
            });
            if (transfer.Transfer.TextureCopyBytes != 0U) {
              // Same span the backend writes: byte count plus the per-row
              // gap, so every byte the copy materializes is inside the
              // watch. For a raw copy OutputSize carries the native
              // width/height pair in 16-byte units (see the bridge).
              const uint32_t copySize =
                  transfer.Transfer.TextureCopyBytes & ~15U;
              const uint32_t copyGap =
                  (transfer.Transfer.OutputSize >> 16U) * 16U;
              const uint32_t copyWidth =
                  copyGap == 0U
                      ? copySize
                      : (transfer.Transfer.OutputSize & 0xFFFFU) * 16U;
              if (copySize != 0U && copyWidth != 0U) {
                const uint64_t copyEnd =
                    uint64_t{transfer.OutputPhysicalAddress} + copySize +
                    uint64_t{(copySize - 1U) / copyWidth} * copyGap;
                const auto copySpan = static_cast<size_t>(
                    std::min<uint64_t>(copyEnd, uint64_t{1} << 32) -
                    transfer.OutputPhysicalAddress);
                // The memory object is indexed by guest addresses: the
                // transfer names a physical destination, so it has to be
                // translated before anything can be trapped. A span that
                // does not translate is skipped rather than watched at the
                // wrong address.
                const auto watchGuest = picaMemoryView.Translate(
                    transfer.OutputPhysicalAddress, copySpan);
                if (watchGuest.has_value()) {
                  const std::pair<uint32_t, uint32_t> watched{
                      *watchGuest,
                      *watchGuest + static_cast<uint32_t>(copySpan)};
                  if (std::find(rawCopyWatchRanges.begin(),
                                rawCopyWatchRanges.end(),
                                watched) == rawCopyWatchRanges.end()) {
                    rawCopyWatchRanges.push_back(watched);
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  } else if (!process.Memory().RangeWatchActive()) {
                    process.Memory().SetRangeWatch(rawCopyWatchRanges);
                  }
                }
              }
            }
          }
          picaPresentationScheduler.Capture(transfer);
          if (transfer.SignalInterrupt && transfer.Transfer.TextureCopyBytes == 0U) {
            if (!hostServices.SignalPicaInterrupt(
                    process.Memory(),
                    Oot3dNativeGame::Oot3dPicaInterruptId::Ppf)) {
              throw std::runtime_error(
                  "native PICA display transfer completion could not "
                  "enter the GSP relay queue");
            }
            ++picaStats.CompletedPpfCount;
            ++picaStats.RecordedCompletionCount;
            signaledPicaCompletion = true;
          }
          displayTransfersByOutput[transfer.Transfer.OutputAddress] =
              transfer;
          ++picaStats.DisplayTransferCount;
        };
    while (displayTransferIndex < displayTransfers.size() &&
           displayTransfers[displayTransferIndex].AfterDrawSubmissionId <=
               lastSubmittedDrawId) {
      submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
    }
    // Deferred planning: each draw enters the accumulator as a placeholder
    // (identity and decoded state only) and its plan is built off this
    // thread from the same immutable submission snapshot. Diagnostics that
    // read the plan here (semantic trace, extended diagnostics) keep the
    // serial build.
    std::shared_ptr<Oot3dNativeGame::Oot3dPicaDeferredPlanBatch> deferredPlans;
    if (picaPlanBuilder.Deferred() && !draws.empty() &&
        !launch.ExtendedDiagnostics && !picaSemanticTrace.Enabled()) {
      // Swaps vectors with a recycled batch: `draws` comes back empty with
      // capacity for the submission queue to fill next; nothing is copied
      // or allocated per pass.
      deferredPlans = picaPlanBuilder.BeginBatch(draws);
    }
    auto &drawList =
        deferredPlans != nullptr ? deferredPlans->Submissions : draws;
    for (size_t drawIndex = 0; drawIndex < drawList.size(); ++drawIndex) {
      auto &draw = drawList[drawIndex];
      while (memoryFillIndex < memoryFills.size() &&
             memoryFills[memoryFillIndex].BeforeDrawSubmissionId <=
                 draw.Id) {
        submitMemoryFill(memoryFills[memoryFillIndex++]);
      }
      const bool usesCommonBackground =
          suppressTopScreenFrontendBackdrop && usesFrontendBackdrop(draw.State);
      const bool targetsFrontendBackdrop =
          std::any_of(frontendBackdropCanvases.begin(),
                      frontendBackdropCanvases.end(),
                      [&](const auto& canvas) {
                        return canvas.Matches(draw.State);
                      });
      const bool isOpaqueTargetInitialization =
          IsNativeFrontendOpaqueTargetInitializationDraw(draw);
      const bool suppressNativeFrontendBackdropDraw =
          suppressTopScreenFrontendBackdrop &&
          targetsFrontendBackdrop &&
          (usesCommonBackground || isOpaqueTargetInitialization);
      Oot3dNativeGame::Oot3dPicaVulkanDrawPlan plan;
      auto drawPhaseStart = std::chrono::steady_clock::now();
      if (deferredPlans != nullptr) {
        if (!suppressNativeFrontendBackdropDraw) {
          plan = picaPlanBuilder.MakeDeferredPlan(deferredPlans, drawIndex);
        }
      } else if (!picaPlanBuilder.Build(draw, plan, &error)) {
        throw std::runtime_error("native PICA Vulkan plan failed: " +
                                 error);
      }
      phaseTiming.PicaPlanSeconds += SecondsSince(drawPhaseStart);
      if (deferredPlans == nullptr) {
        picaSemanticTrace.RecordDraw(
            presentationFrameCount, frameCount, draw, plan,
            suppressNativeFrontendBackdropDraw, usesCommonBackground,
            isOpaqueTargetInitialization);
      }
      if (suppressNativeFrontendBackdropDraw) {
        picaStats.NativeMenuBackdropDrawOmissionCount +=
            usesCommonBackground ? 1U : 0U;
        picaStats.NativeMenuClearDrawOmissionCount +=
            isOpaqueTargetInitialization ? 1U : 0U;
      }
      if (launch.ExtendedDiagnostics) {
        drawPhaseStart = std::chrono::steady_clock::now();
        if (drawEvents.size() == 128U) {
          drawEvents.erase(drawEvents.begin());
        }
        nlohmann::json textures = nlohmann::json::array();
        for (size_t slot = 0; slot < draw.State.Textures.size();
             ++slot) {
          const auto &texture = draw.State.Textures[slot];
          if (!texture.Enabled) {
            continue;
          }
          const auto planTexture = std::find_if(
              plan.Textures.begin(), plan.Textures.end(),
              [slot](const auto &candidate) {
                return candidate.Slot == slot;
              });
          const std::span<const uint8_t> nativeBytes =
              planTexture != plan.Textures.end()
                  ? std::span<const uint8_t>(planTexture->NativeBytes)
                  : std::span<const uint8_t>();
          textures.push_back({
              {"slot", slot},
              {"width", texture.Width},
              {"height", texture.Height},
              {"physical_address", texture.PhysicalAddress},
              {"format", texture.Format},
              {"type", texture.Type},
              {"native_byte_count", nativeBytes.size()},
              {"native_byte_hash", HashDiagnosticBytes(nativeBytes)},
              {"native_nonzero_bytes",
               std::count_if(nativeBytes.begin(), nativeBytes.end(),
                             [](const uint8_t byte) {
                               return byte != 0U;
                             })},
          });
        }
        nlohmann::json vertexBindings = nlohmann::json::array();
        for (const auto &binding : plan.VertexBindings) {
          constexpr size_t kDiagnosticByteLimit = 256U;
          const auto bindingBytes = binding.ResolvedBytes();
          const size_t byteCount =
              std::min(bindingBytes.size(), kDiagnosticByteLimit);
          vertexBindings.push_back({
              {"binding", binding.Binding},
              {"byte_stride", binding.ByteStride},
              {"per_instance",
               binding.InputRate ==
                   Oot3dNativeGame::Oot3dPicaVertexInputRate::
                       PerInstance},
              {"byte_count", bindingBytes.size()},
              {"prefix_bytes",
               std::vector<uint8_t>(bindingBytes.begin(),
                                    bindingBytes.begin() + byteCount)},
          });
        }
        nlohmann::json defaultAttributes = nlohmann::json::array();
        for (size_t attributeIndex = 0;
             attributeIndex < draw.State.VertexInput.AttributeCount;
             ++attributeIndex) {
          if (!draw.State.VertexInput.Attributes[attributeIndex]
                   .Default) {
            continue;
          }
          defaultAttributes.push_back({
              {"attribute", attributeIndex},
              {"input_register",
               draw.State.ShaderInterface
                   .InputRegisterByAttribute[attributeIndex]},
              {"value", draw.Packet.DefaultAttributes[attributeIndex]},
          });
        }
        constexpr std::array<uint16_t, 6> kTevStageRegisters{
            0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
        nlohmann::json tevStages = nlohmann::json::array();
        for (const uint16_t base : kTevStageRegisters) {
          tevStages.push_back({
              {"base_register", base},
              {"sources", draw.Packet.Registers[base]},
              {"modifiers", draw.Packet.Registers[base + 1U]},
              {"operations", draw.Packet.Registers[base + 2U]},
              {"constant", draw.Packet.Registers[base + 3U]},
              {"scales", draw.Packet.Registers[base + 4U]},
          });
        }
        drawEvents.push_back({
            {"frame", frameCount},
            {"submission_id", draw.Id},
            {"command_list_address", draw.Packet.CommandListAddress},
            {"command_list_offset_words",
             draw.Packet.CommandListOffsetWords},
            {"topscreen_frontend_backdrop_suppressed",
             suppressNativeFrontendBackdropDraw},
            {"native_frontend_background_texture", usesCommonBackground},
            {"native_frontend_target_initialization",
             isOpaqueTargetInitialization},
            {"vertex_count", draw.State.VertexInput.VertexCount},
            {"vertex_offset", draw.State.VertexInput.VertexOffset},
            {"minimum_vertex_index", draw.MinimumVertexIndex},
            {"maximum_vertex_index", draw.MaximumVertexIndex},
            {"indexed", draw.State.VertexInput.Indexed},
            {"indices_are_16_bit",
             draw.State.VertexInput.IndicesAre16Bit},
            {"attribute_count", draw.State.VertexInput.AttributeCount},
            {"vulkan_base_vertex", plan.BaseVertex},
            {"vulkan_vertex_bindings", plan.VertexBindings.size()},
            {"vertex_shader_key", plan.VertexShader.StateKey},
            {"fragment_shader_key", plan.FragmentShader.StateKey},
            {"vertex_shader_source",
             std::string(plan.ResolvedVertexShaderSource())},
            {"fragment_shader_source",
             std::string(plan.ResolvedFragmentShaderSource())},
            {"vertex_uniform_floats", plan.VertexShader.Uniforms.Floats},
            {"shader_output_mask", draw.State.ShaderInterface.OutputMask},
            {"shader_output_total",
             draw.Packet.Registers[0x04FU] & 7U},
            {"geometry_shader_enabled",
             draw.State.ShaderInterface.GeometryShaderEnabled},
            {"geometry_shader_mode",
             draw.Packet.Registers[0x229U] & 3U},
            {"shader_output_maps",
             {draw.Packet.Registers[0x050U],
              draw.Packet.Registers[0x051U],
              draw.Packet.Registers[0x052U],
              draw.Packet.Registers[0x053U],
              draw.Packet.Registers[0x054U],
              draw.Packet.Registers[0x055U],
              draw.Packet.Registers[0x056U]}},
            {"framebuffer",
             {{"color_address",
               draw.State.Framebuffer.ColorPhysicalAddress},
              {"depth_address",
               draw.State.Framebuffer.DepthPhysicalAddress},
              {"width", draw.State.Framebuffer.Width},
              {"height", draw.State.Framebuffer.Height},
              {"color_format", draw.State.Framebuffer.ColorFormat},
              {"depth_format", draw.State.Framebuffer.DepthFormat},
              {"flipped", draw.State.Framebuffer.Flipped}}},
            {"viewport",
             {{"half_width", draw.State.Viewport.HalfWidth},
              {"half_height", draw.State.Viewport.HalfHeight},
              {"depth_range", draw.State.Viewport.DepthRange},
              {"near_plane", draw.State.Viewport.NearPlane},
              {"z_buffering", draw.State.Viewport.ZBuffering},
              {"corner_x", draw.State.Viewport.CornerX},
              {"corner_y", draw.State.Viewport.CornerY}}},
            {"scissor",
             {{"mode", static_cast<uint32_t>(draw.State.Scissor.Mode)},
              {"x1", draw.State.Scissor.X1},
              {"y1", draw.State.Scissor.Y1},
              {"x2", draw.State.Scissor.X2},
              {"y2", draw.State.Scissor.Y2}}},
            {"topology", static_cast<uint32_t>(draw.State.Topology)},
            {"cull_mode", static_cast<uint32_t>(draw.State.CullMode)},
            {"output_merger",
             {{"fragment_operation_mode",
               plan.State.OutputMerger.FragmentOperationMode},
              {"color_write_mask",
               plan.State.OutputMerger.ColorWriteMask},
              {"logic_operation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.LogicOperation)},
              {"blend_enabled", plan.State.OutputMerger.Blend.Enabled},
              {"blend_color_equation",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.ColorEquation)},
              {"blend_source_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.SourceColor)},
              {"blend_destination_color",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Blend.DestinationColor)},
              {"depth_test_enabled",
               plan.State.OutputMerger.Depth.TestEnabled},
              {"depth_write_enabled",
               plan.State.OutputMerger.Depth.WriteEnabled},
              {"depth_compare",
               static_cast<uint32_t>(
                   plan.State.OutputMerger.Depth.Compare)}}},
            {"textures", std::move(textures)},
            {"vertex_bindings", std::move(vertexBindings)},
            {"default_attributes", std::move(defaultAttributes)},
            {"tev_stages", std::move(tevStages)},
            {"combiner_buffer_color", draw.Packet.Registers[0x0FDU]},
        });
        phaseTiming.PicaDiagnosticsSeconds +=
            SecondsSince(drawPhaseStart);
      }
      if (!suppressNativeFrontendBackdropDraw) {
        picaPresentationScheduler.Capture(std::move(plan));
      }
      while (completionIndex < completions.size() &&
             completions[completionIndex].AfterDrawSubmissionId ==
                 draw.Id) {
        const auto &completion = completions[completionIndex++];
        if (!hostServices.SignalPicaInterrupt(process.Memory(),
                                              completion.Interrupt)) {
          throw std::runtime_error(
              "native PICA draw completion could not "
              "enter the GSP relay queue");
        }
        if (completion.Interrupt ==
            Oot3dNativeGame::Oot3dPicaInterruptId::P3d) {
          ++picaStats.CompletedP3dCount;
        } else if (completion.Interrupt ==
                   Oot3dNativeGame::Oot3dPicaInterruptId::Ppf) {
          ++picaStats.CompletedPpfCount;
        }
        ++picaStats.RecordedCompletionCount;
        signaledPicaCompletion = true;
      }
      lastSubmittedDrawId = draw.Id;
      while (
          displayTransferIndex < displayTransfers.size() &&
          displayTransfers[displayTransferIndex].AfterDrawSubmissionId ==
              draw.Id) {
        submitDisplayTransfer(displayTransfers[displayTransferIndex++]);
      }
      ++picaStats.DrawCount;
    }
    // From here on the submissions belong to the plan builder.
    picaPlanBuilder.Publish(std::move(deferredPlans));
    if (completionIndex != completions.size()) {
      throw std::runtime_error(
          "native PICA completion did not match a submitted draw");
    }
    while (memoryFillIndex < memoryFills.size()) {
      submitMemoryFill(memoryFills[memoryFillIndex++]);
    }
    if (displayTransferIndex != displayTransfers.size()) {
      throw std::runtime_error("native PICA display transfer did not "
                               "match a submitted draw");
    }
    signaledPicaCompletion =
        FlushNativeDependencies() || signaledPicaCompletion;
    if (!signaledPicaCompletion) {
      break;
    }
    phaseStart = std::chrono::steady_clock::now();
    processResult =
        RunUntilGuestWait(process, launch.WholeAotBlockBudget);
    const double resumedGuestSeconds = SecondsSince(phaseStart);
    phaseTiming.GuestSeconds += resumedGuestSeconds;
    phaseTiming.GuestInsidePicaSubmitSeconds += resumedGuestSeconds;
    RequireRunnableGuest(processResult);
  }
  if (drainPassesThisRefresh == kMaximumPicaDrainPassesPerRefresh &&
      (!submissionQueue.PendingDraws().empty() ||
       !submissionQueue.PendingCompletions().empty() ||
       !submissionQueue.PendingDisplayTransfers().empty() ||
       !submissionQueue.PendingMemoryFills().empty())) {
    throw std::runtime_error("native PICA completion chain exceeded "
                             "the per-refresh drain limit");
  }
  picaStats.MaximumPicaDrainPassesPerRefresh = std::max<uint64_t>(
      picaStats.MaximumPicaDrainPassesPerRefresh, drainPassesThisRefresh);
  picaStats.RefreshesWithNativeDraws += refreshHadNativeDraws ? 1U : 0U;
  phaseTiming.PicaSubmitSeconds += SecondsSince(picaSubmitStart);
}

// Presents the host frame: selects the top-screen transfer and replays the visual frame or its samples.
void NativeA32WindowSession::PresentVisualFrame(
    const double &presentationElapsedSeconds,
    const float &visualInterpolationAlpha) {
  auto &hostServices = *hostServicesStorage;
  auto &process = *processStorage;
  auto &uiLifecycleBridge = *uiLifecycleBridgeStorage;
  auto &api = *apiPointer;
  auto &picaSemanticTrace = *picaSemanticTraceStorage;
  CpuProbe::Scope cpuVisual(CpuProbe::Phase::VisualPresentation);
  const auto visualPresentationStart = std::chrono::steady_clock::now();
  constexpr auto presentationRenderTargetNamespace =
      kVisualInterpolationRenderTargetNamespace;
  const bool lcdForceBlack = hostServices.LcdForceBlack();
  const auto topFramebuffer = hostServices.TopFramebuffer();
  const Oot3dNativeGame::Oot3dPicaDisplayTransferSubmission
      *traceSelectedTopTransfer = nullptr;
  if (topFramebuffer.has_value()) {
    auto traceSelected =
        displayTransfersByOutput.find(topFramebuffer->AddressLeft);
    if (traceSelected == displayTransfersByOutput.end() &&
        topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
      traceSelected =
          displayTransfersByOutput.find(topFramebuffer->AddressRight);
    }
    if (traceSelected != displayTransfersByOutput.end()) {
      traceSelectedTopTransfer = &traceSelected->second;
    }
  }
  picaSemanticTrace.RecordPresentationSelection(
      presentationFrameCount, frameCount, lcdForceBlack,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressLeft)
          : std::nullopt,
      topFramebuffer.has_value()
          ? std::optional<uint32_t>(topFramebuffer->AddressRight)
          : std::nullopt,
      traceSelectedTopTransfer);
  if (!lcdForceBlack) {
    if (topFramebuffer.has_value()) {
      auto selected =
          displayTransfersByOutput.find(topFramebuffer->AddressLeft);
      if (selected == displayTransfersByOutput.end() &&
          topFramebuffer->AddressRight != topFramebuffer->AddressLeft) {
        selected =
            displayTransfersByOutput.find(topFramebuffer->AddressRight);
      }
      if (selected != displayTransfersByOutput.end()) {
        // Guest side: transfer selection, frame completion and the
        // guest-memory reads the renderer needs. The rest runs as one
        // render command, in renderer order.
        const auto topSelection = selected->second;
        std::shared_ptr<Oot3dNativeGame::Oot3dPicaVisualFrame>
            completedFrame;
        // Guest frames completed before the selected one while the host
        // fell behind: canonical work and interpolation history only.
        std::shared_ptr<std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>
            supersededFrames;
        std::vector<Fast::Oot3d::GrassInteractor> frameActors;
        bool suppressTopScreenBackdrop = false;
        if (topSelection.CompletionId !=
            lastSelectedTopTransferCompletionId) {
          lastSelectedTopTransferCompletionId =
              topSelection.CompletionId;
          ++picaStats.SelectedTopTransferCount;
          // TopScreen draws the relocated lower menu onto the upper target
          // after the upper snapshot: that work completes the same frame.
          const bool relocatedFrontend =
              Oot3dNativeGame::ShouldSuppressTopScreenFrontendBackdrop(
                  launch.UiProfile,
                  uiLifecycleBridge.NativeFrontendPresentationActive());
          const auto frontendMenuCanvases =
              Oot3dNativeGame::TopScreenFrontendMenuCanvases();
          auto finishedFrames = picaPresentationScheduler.FinishFrames(
              topSelection, topDisplayOutputs,
              relocatedFrontend
                  ? std::span<const Oot3dNativeGame::Oot3dPicaViewportState>(
                        frontendMenuCanvases)
                  : std::span<const Oot3dNativeGame::Oot3dPicaViewportState>());
          if (std::find(topDisplayOutputs.begin(), topDisplayOutputs.end(),
                        topSelection.OutputPhysicalAddress) == topDisplayOutputs.end()) {
            if (topDisplayOutputs.size() == 4U)
              topDisplayOutputs.erase(topDisplayOutputs.begin());
            topDisplayOutputs.push_back(topSelection.OutputPhysicalAddress);
          }
          std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame> finished;
          if (!finishedFrames.empty()) {
            finished = std::move(finishedFrames.back());
            finishedFrames.pop_back();
            if (!finishedFrames.empty())
              supersededFrames = std::make_shared<
                  std::vector<Oot3dNativeGame::Oot3dPicaVisualFrame>>(
                  std::move(finishedFrames));
          }
          if (finished.has_value()) {
            frameActors = Oot3dNativeGame::CollectNativeActorInteractions(
                process.Memory(),
                sceneViewProbe.Stats().LastPlayStateAddress,
                finished->Sequence);
            ++visualStats.SnapshotCount;
            suppressTopScreenBackdrop = relocatedFrontend;
            completedFrame =
                std::make_shared<Oot3dNativeGame::Oot3dPicaVisualFrame>(
                    std::move(*finished));
          }
        }
        renderStream.Submit([&, topSelection, completedFrame, supersededFrames,
                             frameActors, suppressTopScreenBackdrop,
                             presentationElapsedSeconds,
                             visualInterpolationAlpha, frameCount = frameCount,
                             guestReferences =
                                 CollectGuestCopyReferences()] {
        if (rendererFrameSkipped) return;
        struct RenderCost {
          double &Total;
          std::chrono::steady_clock::time_point Start =
              std::chrono::steady_clock::now();
          ~RenderCost() { Total += SecondsSince(Start) * 1000.0; }
        } renderCost{renderFrameCpuMs};
        GpuFrameTimer().BeginFrame();
        EnsureInterpolationPacing();
        // Deferred plans of the closed frames are built (or taken from the
        // plan worker) here, on the render thread, before any use.
        if (completedFrame != nullptr) {
          if (supersededFrames != nullptr) {
            for (auto &superseded : *supersededFrames) {
              if (!Oot3dNativeGame::ResolveOot3dPicaDeferredVisualFrame(
                      superseded, &renderError))
                throw std::runtime_error(renderError);
            }
          }
          if (!Oot3dNativeGame::ResolveOot3dPicaDeferredVisualFrame(
                  *completedFrame, &renderError))
            throw std::runtime_error(renderError);
        }
        guestCopyReferences = guestReferences;
        uint64_t nativeReplaysSkippedThisFrame = 0U;
        uint64_t reducedRefusedThisFrame = 0U;
        uint64_t repeatFallbacksThisFrame = 0U;
        bool presentedVisualSample = false;
        bool selectedNewVisualFrame = false;
        {
          if (completedFrame != nullptr && supersededFrames != nullptr) {
            for (auto &superseded : *supersededFrames) {
              auto &sample = visualFrameSampleScratch;
              (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                  superseded, sample, true);
              const auto nativeTemporalSample =
                  Fast::Oot3d::BuildNativeFrameTemporalSample(
                      frameCompositionPolicy,
                      Fast::Oot3d::NativeFrameTemporalSampleKind::
                          Authoritative,
                      superseded.Sequence, superseded.Sequence,
                      visualContinuityEpoch, 1.0F,
                      static_cast<float>(presentationElapsedSeconds));
              if (skipNativeReplay) {
                ++nativeReplaysSkippedThisFrame;
              } else {
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                        NativeFrame,
                    false, nativeTemporalSample,
                    kNativeDependencyRenderTargetNamespace);
              }
              if (visualInterpolationEnabled) {
                Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                    superseded, suppressTopScreenBackdrop);
                preparedVisualTransition.Reset();
                if (latestVisualFrame.has_value())
                  previousVisualFrame = std::move(latestVisualFrame);
                latestVisualFrame = std::move(superseded);
              }
            }
          }
          if (completedFrame != nullptr) {
            std::optional<Oot3dNativeGame::Oot3dPicaVisualFrame>
                currentVisualFrame(std::move(*completedFrame));
            Fast::Oot3d::GrassInteractionBridge::Instance().PublishFrame(
                currentVisualFrame->Sequence, frameActors);
            selectedNewVisualFrame = true;
            renderFrameKind =
                static_cast<uint8_t>(Oot3dNativeGame::RenderFrameKind::Step);
            auto &sample = visualFrameSampleScratch;
            // Keep the canonical target complete independently of the
            // delayed visual target. Raw-copy prefixes have already
            // executed; only the native suffix remains here.
            (void)Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                *currentVisualFrame, sample, true);
            const auto nativeTemporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Authoritative,
                    currentVisualFrame->Sequence,
                    currentVisualFrame->Sequence, visualContinuityEpoch,
                    1.0F, static_cast<float>(presentationElapsedSeconds));
            // The native namespace exists for read-backs into guest memory;
            // a backend that never reads it (GL) skips the replay, a whole
            // scene drawn for nothing (config/native_replay=1 keeps it).
            if (skipNativeReplay) {
              ++nativeReplaysSkippedThisFrame;
            } else {
              ExecuteVisualSample(
                  sample,
                  Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::
                      NativeFrame,
                  false, nativeTemporalSample,
                  kNativeDependencyRenderTargetNamespace);
            }
            Oot3dNativeGame::ComposeTopScreenFrontendFrame(
                *currentVisualFrame, suppressTopScreenBackdrop);
            if (!visualInterpolationEnabled) {
              if (Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Authoritative,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.DirectFramesPresented;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              // Direct gameplay modes retain no cross-frame matching
              // history. The scheduler keeps only the executed scanout
              // snapshot needed if presentation temporarily outruns
              // simulation.
              preparedVisualTransition.Reset();
              previousVisualFrame.reset();
              latestVisualFrame.reset();
              latestVisualTransitionContinuous = false;
            } else {
              bool resynchronizeVisualNamespace =
                  !latestVisualFrame.has_value();
              if (latestVisualFrame.has_value()) {
                const bool transitionPrepared =
                    preparedVisualTransition.Prepare(
                        *latestVisualFrame, *currentVisualFrame,
                        &visualInterpolationTiming);
                const auto &transition =
                    preparedVisualTransition.Stats();
                const float sampleAlpha =
                    visualSampleCadence.Resolve(
                        true, visualInterpolationAlpha);
                const bool visualSampleReady =
                    transitionPrepared &&
                    preparedVisualTransition.Sample(
                        *latestVisualFrame, *currentVisualFrame,
                        sampleAlpha, sample,
                        &visualInterpolationTiming);
                ++visualStats.TransitionCount;
                visualStats.MatchedDrawCount += transition.MatchedDraws;
                visualStats.StrictUniqueMatchedDrawCount +=
                    transition.StrictUniqueMatchedDraws;
                visualStats.StrictOrdinalMatchedDrawCount +=
                    transition.StrictOrdinalMatchedDraws;
                visualStats.StructuralOrdinalMatchedDrawCount +=
                    transition.StructuralOrdinalMatchedDraws;
                visualStats.PipelineOrdinalMatchedDrawCount +=
                    transition.PipelineOrdinalMatchedDraws;
                visualStats.ChangedContinuousDrawCount +=
                    transition.ChangedContinuousDraws;
                visualStats.ChangedPerInstanceVertexDrawCount +=
                    transition.ChangedPerInstanceVertexDraws;
                visualStats.ChangedPerVertexDrawCount +=
                    transition.ChangedPerVertexDraws;
                visualStats.AmbiguousPreviousDrawCount +=
                    transition.AmbiguousPreviousDraws;
                visualStats.AmbiguousCurrentDrawCount +=
                    transition.AmbiguousCurrentDraws;
                visualStats.UnmatchedPreviousDrawCount +=
                    transition.UnmatchedPreviousDraws;
                visualStats.UnmatchedCurrentDrawCount +=
                    transition.UnmatchedCurrentDraws;
                const double continuityThreshold =
                    visualContinuityTracker.CurrentThreshold();
                const bool continuousTransition =
                    transitionPrepared &&
                    visualContinuityTracker.Accept(transition);
                latestVisualTransitionContinuous = continuousTransition;
                visualStats.DiscontinuityCount +=
                    continuousTransition ? 0U : 1U;
                if (launch.ExtendedDiagnostics) {
                  if (visualTransitionEvents.size() == 1024U) {
                    visualTransitionEvents.erase(
                        visualTransitionEvents.begin());
                  }
                  visualTransitionEvents.push_back({
                      {"host_frame", frameCount},
                      {"previous_sequence",
                       latestVisualFrame->Sequence},
                      {"current_sequence",
                       currentVisualFrame->Sequence},
                      {"matched_draws", transition.MatchedDraws},
                      {"strict_unique_matched_draws",
                       transition.StrictUniqueMatchedDraws},
                      {"strict_ordinal_matched_draws",
                       transition.StrictOrdinalMatchedDraws},
                      {"structural_ordinal_matched_draws",
                       transition.StructuralOrdinalMatchedDraws},
                      {"pipeline_ordinal_matched_draws",
                       transition.PipelineOrdinalMatchedDraws},
                      {"changed_per_instance_vertex_draws",
                       transition.ChangedPerInstanceVertexDraws},
                      {"changed_per_vertex_draws",
                       transition.ChangedPerVertexDraws},
                      {"unmatched_previous_draws",
                       transition.UnmatchedPreviousDraws},
                      {"unmatched_current_draws",
                       transition.UnmatchedCurrentDraws},
                      {"top_target_matched_draws",
                       transition.TopTargetMatchedDraws},
                      {"matched_draw_coverage",
                       transition.MatchedDrawCoverage},
                      {"median_normalized_continuous_delta",
                       transition.MedianNormalizedContinuousDelta},
                      {"p90_normalized_continuous_delta",
                       transition.P90NormalizedContinuousDelta},
                      {"continuity_threshold", continuityThreshold},
                      {"continuous", continuousTransition},
                  });
                }
                if (continuousTransition && visualSampleReady) {
                  const auto temporalSample =
                      Fast::Oot3d::BuildNativeFrameTemporalSample(
                          frameCompositionPolicy,
                          Fast::Oot3d::NativeFrameTemporalSampleKind::
                              Transition,
                          latestVisualFrame->Sequence,
                          currentVisualFrame->Sequence,
                          visualContinuityEpoch, sampleAlpha,
                          static_cast<float>(
                              presentationElapsedSeconds));
                  ExecuteVisualSample(
                      sample,
                      Oot3dNativeGame::
                          Oot3dPicaPresentationExecutionKind::
                              InterpolatedFrame,
                      true, temporalSample);
                  ++visualStats.SamplesPresented;
                  visualStats.SampleDrawCount += sample.Draws.size();
                  visualStats.InterpolatedDrawCount +=
                      sample.InterpolatedDraws;
                  visualStats.InterpolatedPerInstanceVertexDrawCount +=
                      sample.InterpolatedPerInstanceVertexDraws;
                  visualStats.InterpolatedPerVertexDrawCount +=
                      sample.InterpolatedPerVertexDraws;
                  presentedVisualSample = true;
                } else {
                  resynchronizeVisualNamespace = true;
                }
              }
              if (resynchronizeVisualNamespace &&
                  Oot3dNativeGame::ViewOot3dPicaVisualFrame(
                      *currentVisualFrame, sample)) {
                preparedVisualTransition.Reset();
                ++visualContinuityEpoch;
                const auto temporalSample =
                    Fast::Oot3d::BuildNativeFrameTemporalSample(
                        frameCompositionPolicy,
                        Fast::Oot3d::NativeFrameTemporalSampleKind::
                            Resynchronized,
                        currentVisualFrame->Sequence,
                        currentVisualFrame->Sequence,
                        visualContinuityEpoch, 1.0F,
                        static_cast<float>(
                            presentationElapsedSeconds));
                ExecuteVisualSample(
                    sample,
                    Oot3dNativeGame::
                        Oot3dPicaPresentationExecutionKind::CurrentFrame,
                    true, temporalSample);
                ++visualStats.NamespaceResyncCount;
                ++visualStats.SamplesPresented;
                visualStats.SampleDrawCount += sample.Draws.size();
                presentedVisualSample = true;
              }
              if (latestVisualFrame.has_value()) {
                previousVisualFrame = std::move(latestVisualFrame);
              } else {
                latestVisualTransitionContinuous = false;
              }
              latestVisualFrame = std::move(currentVisualFrame);
            }
          }
        }
        if (!selectedNewVisualFrame && !presentedVisualSample &&
            visualInterpolationEnabled &&
            latestVisualTransitionContinuous &&
            previousVisualFrame.has_value() &&
            latestVisualFrame.has_value() &&
            preparedVisualTransition.Ready()) {
          auto &sample = visualFrameSampleScratch;
          const float sampleAlpha =
              visualSampleCadence.Resolve(
                  false, visualInterpolationAlpha);
          // Paced: the in-between frame at full or reduced resolution, or
          // dropped (the step image again) when the pair would not fit.
          auto level = interpolationPacing.Decide();
          if (level == Oot3dNativeGame::InterpolatedSampleLevel::Dropped) {
            if (lastPresentedVisual.has_value() &&
                picaPresentationScheduler.HasSnapshot(
                    lastPresentedVisual->first,
                    lastPresentedVisual->second)) {
              const auto started = std::chrono::steady_clock::now();
              if (!picaPresentationScheduler.PresentExisting(
                      api, lastPresentedVisual->first,
                      lastPresentedVisual->second, &renderError)) {
                throw std::runtime_error(
                    "native top-screen dropped in-between frame failed: " +
                    renderError);
              }
              rendererTiming->VisualReplaySeconds += SecondsSince(started);
              renderFrameKind = static_cast<uint8_t>(
                  Oot3dNativeGame::RenderFrameKind::Dropped);
              presentedVisualSample = true;
            } else {
              level = Oot3dNativeGame::InterpolatedSampleLevel::Full;
            }
          }
          if (!presentedVisualSample &&
              preparedVisualTransition.Sample(
                  *previousVisualFrame, *latestVisualFrame, sampleAlpha,
                  sample, &visualInterpolationTiming)) {
            uint64_t sampleNamespace = kVisualInterpolationRenderTargetNamespace;
            if (level == Oot3dNativeGame::InterpolatedSampleLevel::Reduced) {
              if (Oot3dNativeGame::SampleSuitsReducedReplay(
                      sample, reducedReplayScratch)) {
                sampleNamespace = kReducedVisualRenderTargetNamespace;
              } else {
                level = Oot3dNativeGame::InterpolatedSampleLevel::Full;
                ++reducedRefusedThisFrame;
              }
            }
            const auto temporalSample =
                Fast::Oot3d::BuildNativeFrameTemporalSample(
                    frameCompositionPolicy,
                    Fast::Oot3d::NativeFrameTemporalSampleKind::
                        Transition,
                    previousVisualFrame->Sequence,
                    latestVisualFrame->Sequence,
                    visualContinuityEpoch, sampleAlpha,
                    static_cast<float>(presentationElapsedSeconds));
            ExecuteVisualSample(
                sample,
                Oot3dNativeGame::
                    Oot3dPicaPresentationExecutionKind::RepeatedFrame,
                true, temporalSample, sampleNamespace);
            renderFrameKind = static_cast<uint8_t>(
                Oot3dNativeGame::RenderFrameKindOf(level));
            ++visualStats.SamplesPresented;
            visualStats.SampleDrawCount += sample.Draws.size();
            visualStats.InterpolatedDrawCount += sample.InterpolatedDraws;
            visualStats.InterpolatedPerInstanceVertexDrawCount +=
                sample.InterpolatedPerInstanceVertexDraws;
            visualStats.InterpolatedPerVertexDrawCount +=
                sample.InterpolatedPerVertexDraws;
            presentedVisualSample = true;
          }
        }
        if (!presentedVisualSample &&
            picaPresentationScheduler.HasSnapshot(
                topSelection,
                presentationRenderTargetNamespace)) {
          const auto started = std::chrono::steady_clock::now();
          if (!picaPresentationScheduler.PresentExisting(
                  api, topSelection,
                  presentationRenderTargetNamespace, &renderError)) {
            throw std::runtime_error(
                "native top-screen scheduled scanout failed: " +
                renderError);
          }
          rendererTiming->VisualReplaySeconds += SecondsSince(started);
          ++visualStats.ReusedSnapshotPresentations;
        } else if (!presentedVisualSample && visualInterpolationEnabled &&
                   lastPresentedVisual.has_value() &&
                   picaPresentationScheduler.HasSnapshot(
                       lastPresentedVisual->first,
                       lastPresentedVisual->second)) {
          // The delayed visual namespace holds the previous guest frame,
          // not the one just selected: show the last presented sample
          // again rather than nothing.
          const auto started = std::chrono::steady_clock::now();
          if (!picaPresentationScheduler.PresentExisting(
                  api, lastPresentedVisual->first,
                  lastPresentedVisual->second, &renderError)) {
            throw std::runtime_error(
                "native top-screen repeated scanout failed: " + renderError);
          }
          rendererTiming->VisualReplaySeconds += SecondsSince(started);
          ++visualStats.ReusedSnapshotPresentations;
          ++repeatFallbacksThisFrame;
        }
        if (nativeReplaysSkippedThisFrame != 0U ||
            reducedRefusedThisFrame != 0U || repeatFallbacksThisFrame != 0U) {
          std::lock_guard lock(interpolationPerfMutex);
          interpolationPerf.NativeReplaysSkipped += nativeReplaysSkippedThisFrame;
          interpolationPerf.ReducedRefused += reducedRefusedThisFrame;
          interpolationPerf.RepeatFallbacks += repeatFallbacksThisFrame;
        }
        });
      }
    }

    const bool presentNativeBottomFrontend =
        Oot3dNativeGame::ShouldPresentNativeBottomFrontend(
            launch.UiProfile,
            uiLifecycleBridge.NativeFrontendPresentationActive());
    const auto bottomFramebuffer = hostServices.BottomFramebuffer();
    if (presentNativeBottomFrontend &&
        bottomFramebuffer.has_value()) {
      ++picaStats.NativeFrontendPresentationActiveCount;
      auto selectedBottom =
          displayTransfersByOutput.find(bottomFramebuffer->AddressLeft);
      if (selectedBottom == displayTransfersByOutput.end() &&
          bottomFramebuffer->AddressRight != 0U &&
          bottomFramebuffer->AddressRight !=
              bottomFramebuffer->AddressLeft) {
        selectedBottom = displayTransfersByOutput.find(
            bottomFramebuffer->AddressRight);
      }
      picaStats.NativeFrontendBottomTransferHitCount +=
          selectedBottom != displayTransfersByOutput.end() ? 1U : 0U;
      if (selectedBottom != displayTransfersByOutput.end()) {
        renderStream.Submit(
            [&, bottomSelection = selectedBottom->second] {
              if (rendererFrameSkipped ||
                  !picaPresentationScheduler.HasSnapshot(
                      bottomSelection,
                      presentationRenderTargetNamespace)) {
                return;
              }
              if (!picaPresentationScheduler.PresentExisting(
                      api, bottomSelection,
                      presentationRenderTargetNamespace, &renderError)) {
                throw std::runtime_error(
                    "native bottom-screen frontend presentation "
                    "failed: " + renderError);
              }
              ++picaStats.NativeBottomFrontendPresentationCount;
            });
      }
    }
  }
  phaseTiming.VisualPresentationSeconds +=
      SecondsSince(visualPresentationStart);
}

#if defined(TRIAEVUM_DEVELOPER_TOOLS)
void NativeA32WindowSession::UpdateDevFreeCameraTour(uint64_t guestFrame) {
  if (devTourPoses.empty() || devTourIndex >= static_cast<int>(devTourPoses.size()) || devWarp == nullptr) return;
  constexpr uint32_t kStartFrames = 90U;  // the scene's title card fades
  // Held for 45 frames in which the game took the free view (an intro or
  // other cutscene camera does not count), at most 900 frames.
  constexpr uint32_t kHoldFrames = 45U;
  constexpr uint32_t kGiveUpFrames = 900U;
  constexpr float kRadians = 3.14159265F / 180.0F;
  const auto status = devWarp->Status();
  const auto pose = [&](int index) {
    const auto &p = devTourPoses[static_cast<size_t>(index)];
    const float yaw = devTourAnchorYaw * kRadians;
    const std::array<float, 3> forward{std::sin(yaw), 0.0F, std::cos(yaw)};
    const std::array<float, 3> right{std::cos(yaw), 0.0F, -std::sin(yaw)};
    std::array<float, 3> eye{p.Forward, p.Right, p.Up};
    if (!p.Absolute)
      for (size_t axis = 0; axis < 3; ++axis)
        eye[axis] = devTourAnchor[axis] + forward[axis] * p.Forward + right[axis] * p.Right + (axis == 1 ? p.Up : 0.0F);
    const float viewYaw = p.Absolute ? p.Yaw : devTourAnchorYaw + p.Yaw;
    nativeCandidateDispatch.DevFreeCamera->SetPose(eye, viewYaw, p.Pitch);
    devTourAppliedAtPose = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR pose=%d eye=%.1f,%.1f,%.1f yaw=%.1f pitch=%.1f frame=%llu\n", index,
                 eye[0], eye[1], eye[2], viewYaw, p.Pitch, static_cast<unsigned long long>(guestFrame));
  };
  if (devTourIndex < 0) {
    // A boot warp point is requested once gameplay starts: wait for it too.
    if (status.Phase != Oot3dNativeGame::DevWarpPhase::Idle) devTourAfterWarp = true;
    const bool ready = devTourAfterWarp ? status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived
                                        : status.Observation.Gameplay && status.Observation.Player;
    if (!ready) {
      devTourFrames = 0;
      return;
    }
    if (++devTourFrames < kStartFrames) return;
    devTourAnchor = {status.Observation.Position[0], status.Observation.Position[1], status.Observation.Position[2]};
    devTourAnchorYaw = static_cast<float>(status.Observation.RotY) * (180.0F / 32768.0F);
    devTourIndex = 0;
    devTourFrames = 0;
    pose(0);
    return;
  }
  ++devTourFrames;
  // Only frames drawn from the main camera count: PlayState +0xA64 is the
  // active camera's index (cameras at +0xA54), 0 the main camera.
  int16_t activeCamera = -1;
  const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
  if (playState != 0U) (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
  const uint64_t appliedNow = nativeCandidateDispatch.DevFreeCamera->AppliedCount();
  if (activeCamera != 0) devTourAppliedAtPose = appliedNow;
  const uint64_t applied = appliedNow - devTourAppliedAtPose;
  if (applied < kHoldFrames && devTourFrames < kGiveUpFrames) return;
  char name[32];
  std::snprintf(name, sizeof(name), "tour-%02d.bmp", devTourIndex);
  pendingForcedScreenshot = devTourDirectory / name;
  std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR capture=%s applied=%llu%s\n",
               pendingForcedScreenshot.string().c_str(), static_cast<unsigned long long>(applied),
               applied < kHoldFrames ? " not-applied" : "");
  devTourFrames = 0;
  if (++devTourIndex < static_cast<int>(devTourPoses.size()))
    pose(devTourIndex);
  else
    std::fprintf(stderr, "TRIAEVUM_DEV_FREECAM_TOUR done\n");
}

void NativeA32WindowSession::UpdateDevLab(uint64_t guestFrame) {
  using Json = nlohmann::json;
  auto &camera = *nativeCandidateDispatch.DevFreeCamera;
  const auto status = devWarp->Status();
  const auto &seen = status.Observation;
  const auto place = [&] {
    return Json{{"frame", guestFrame},
                {"scene", seen.Scene},
                {"room", seen.Room},
                {"position", {seen.Position[0], seen.Position[1], seen.Position[2]}},
                {"yaw", static_cast<float>(seen.RotY) * (180.0F / 32768.0F)},
                {"gameplay", seen.Gameplay}};
  };
  const auto finish = [&](std::string_view result, Json detail) {
    devLab->Answer(*devLabCommand, result, detail);
    devLabCommand.reset();
  };
  const auto vector3 = [](const Json &value) {
    return std::array<float, 3>{value.at(0).get<float>(), value.at(1).get<float>(), value.at(2).get<float>()};
  };
  if (!devLabCommand) {
    devLabCommand = devLab->Next();
    if (!devLabCommand) return;
    devLabFrames = 0;
    devLabSawArrival = false;
    const auto &command = *devLabCommand;
    const std::string op = command["op"].get<std::string>();
    try {
      if (op == "warp" && command.contains("better")) {
        // An entrance of the Debug Warp Screen's Better list ([scene, entrance]
        // indices), with age ("child"/"adult") and time ("day"/"night").
        const auto scenes = Oot3dNativeGame::DevWarpBetterScenes();
        const auto scene = command["better"].at(0).get<size_t>();
        const auto entrance = command["better"].at(1).get<size_t>();
        if (scene >= scenes.size() || entrance >= scenes[scene].Count)
          throw std::out_of_range("no such Better scene/entrance");
        devWarp->WarpTo(Oot3dNativeGame::DevWarpBetterEntrances()[scenes[scene].First + entrance],
                        command.value("age", std::string("child")) == "adult" ? Oot3dNativeGame::DevWarpAge::Adult
                                                                               : Oot3dNativeGame::DevWarpAge::Child,
                        command.value("time", std::string("day")) == "night");
      } else if (op == "warp") {
        // Link respawns at the position in the room (SoH's warp point), facing yaw.
        Oot3dNativeGame::DevWarpPoint point;
        point.EntranceId = command.value("entrance", 0);
        point.RoomNum = static_cast<int8_t>(command.value("room", 0));
        const auto position = vector3(command.at("position"));
        std::copy(position.begin(), position.end(), point.Position);
        point.RotY = static_cast<int16_t>(std::lround(command.value("yaw", 0.0) * 32768.0 / 180.0));
        devWarp->WarpToPlace("lab " + command.value("id", Json()).dump(), point);
      } else if (op == "camera") {
        camera.SetPose(vector3(command.at("eye")), command.value("yaw", 0.0F), command.value("pitch", 0.0F));
        devLabAppliedAt = camera.AppliedCount();
      } else if (op == "camera_off") {
        camera.SetActive(false);
        finish("ok", place());
      } else if (op == "capture") {
        // Written by the render stream with this frame; the file appears shortly after.
        pendingForcedScreenshot = command.at("path").get<std::string>();
        finish("ok", {{"frame", guestFrame}, {"path", pendingForcedScreenshot.string()}});
      } else if (op == "status") {
        const auto environment = sceneEnvironment->Snapshot();
        auto detail = place();
        detail["playState"] = sceneViewProbe.Stats().LastPlayStateAddress;
        detail["warp"] = status.Message;
        detail["profile"] = {{"file", environment.FileExists ? environment.Path.string() : std::string()},
                             {"enabled", environment.Profile.Enabled},
                             {"revision", environment.Revision},
                             {"error", environment.Error}};
        finish("ok", detail);
      } else if (op == "graphics") {
        // A partial "Graphics" block, applied like an F1 commit (not saved),
        // as the qualification schedule does.
        auto &graphicsSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance();
        const auto current = graphicsSettings.Snapshot();
        const auto decoded = Fast::Oot3d::DeserializeGraphicsSettings(
            Oot3dNativeGame::MergeScheduledGraphics(Fast::Oot3d::SerializeGraphicsSettings(current),
                                                    command.at("graphics")),
            current);
        const auto applied = graphicsSettings.Apply(decoded.Value, false);
        const auto issueDetails = [](const auto& issues, const char* phase) {
          auto details = Json::array();
          for (const auto& issue : issues) {
            details.push_back({{"field", issue.Field}, {"message", issue.Message}, {"corrected", issue.Corrected}});
            std::fprintf(stderr, "TRIAEVUM_DEV_LAB_GRAPHICS_ISSUE phase=%s field=\"%s\" corrected=%d message=\"%s\"\n",
                         phase, issue.Field.c_str(), issue.Corrected, issue.Message.c_str());
          }
          return details;
        };
        const auto effective = graphicsSettings.SnapshotForRendering();
        const auto effectiveJson = Fast::Oot3d::SerializeGraphicsSettings(effective.Value);
        finish(applied.Accepted() && !decoded.UnsupportedFutureVersion ? "ok" : "failed",
               {{"frame", guestFrame}, {"decodeIssues", decoded.Issues.size()},
                {"applyIssues", applied.Issues.size()},
                {"decodeIssueDetails", issueDetails(decoded.Issues, "decode")},
                {"applyIssueDetails", issueDetails(applied.Issues, "apply")},
                {"effectiveSurfaceLighting", effectiveJson["Effects"]["SurfaceLighting"]},
                {"effectiveDirectionalShadows", effectiveJson["Effects"]["DirectionalShadows"]["Mode"]},
                {"effectiveCasterFaces", static_cast<unsigned>(effective.Environment.CasterFaces)}});
      } else if (op == "surface_trace") {
        // The surface-lighting candidates of the next frames, one JSON line each.
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().RequestTrace(command.at("path").get<std::string>(),
                                                                     command.value("frames", 1U),
                                                                     command.value("census", false));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "surface_exempt") {
        // Candidates first..last keep their native appearance ({} clears).
        Fast::Renderer3ds::PicaSurfaceDebug::Instance().SetExemptRange(command.value("first", 1U),
                                                                       command.value("last", 0U));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "peek") {
        // Guest memory as hex (at most 4 KiB): finding actor fields.
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint32_t size = std::min(command.value("size", 256U), 4096U);
        std::string hex;
        hex.reserve(size * 2U);
        static constexpr char kDigits[] = "0123456789abcdef";
        bool readable = true;
        for (uint32_t i = 0; i < size && readable; ++i) {
          uint8_t byte = 0;
          readable = processStorage->Memory().Read8(address + i, &byte);
          hex += kDigits[byte >> 4U];
          hex += kDigits[byte & 15U];
        }
        finish(readable ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}, {"hex", hex}});
      } else if (op == "poke16") {
        // One halfword written into guest memory (developer experiments only).
        const uint32_t address = command.at("address").get<uint32_t>();
        const uint16_t value = static_cast<uint16_t>(command.at("value").get<int>());
        const bool written = processStorage->Memory().Write16(address, value);
        finish(written ? "ok" : "failed", {{"frame", guestFrame}, {"address", address}});
      } else if (op == "water") {
        // The scene's water boxes as the renderer receives them.
        const auto water = Oot3dNativeGame::CaptureNativeWaterBodies(sceneViewProbe.Stats().LastPlayStateAddress, processStorage->Memory());
        Json bodies = Json::array();
        for (uint32_t i = 0; i < water.Count; ++i) {
          const auto &body = water.Bodies[i];
          bodies.push_back({{"min", {body.MinimumXZ[0], body.MinimumXZ[1]}},
                            {"max", {body.MaximumXZ[0], body.MaximumXZ[1]}},
                            {"y", body.SurfaceY},
                            {"properties", body.Properties}});
        }
        auto detail = place();
        detail["water"] = std::move(bodies);
        finish("ok", detail);
      } else if (op == "actors") {
        // The live actors (optionally only some ids): id, params, instance,
        // room, world position and scale.
        Json actors = Json::array();
        std::vector<uint16_t> only;
        if (command.contains("ids"))
          for (const auto &id : command["ids"]) only.push_back(id.get<uint16_t>());
        for (const auto &a : Oot3dNativeGame::CaptureNativeActors(sceneViewProbe.Stats().LastPlayStateAddress,
                                                                  processStorage->Memory())) {
          if (!only.empty() && std::find(only.begin(), only.end(), a.Id) == only.end()) continue;
          actors.push_back({{"id", a.Id},
                            {"params", a.Params},
                            {"category", a.Category},
                            {"room", a.Room},
                            {"address", a.Address},
                            {"position", {a.Position[0], a.Position[1], a.Position[2]}},
                            {"scale", {a.Scale[0], a.Scale[1], a.Scale[2]}}});
        }
        auto detail = place();
        detail["actors"] = std::move(actors);
        finish("ok", detail);
      } else if (op == "emitters") {
        // The native lights linked now (LightContext), with their owners.
        Json lights = Json::array();
        const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
        for (const auto &e : Oot3dNativeGame::CaptureNativeEmitters(playState, processStorage->Memory())) {
          Json light = {{"type", e.Type},
                        {"position", {e.Position[0], e.Position[1], e.Position[2]}},
                        {"direction", {e.Direction[0], e.Direction[1], e.Direction[2]}},
                        {"color", {e.Color[0], e.Color[1], e.Color[2]}},
                        {"radius", e.Radius}};
          if (e.Owner && e.Owner->Kind == "actor")
            light["owner"] = {{"kind", "actor"},
                              {"actorId", e.Owner->ActorId},
                              {"actor", e.Owner->ActorAddress},
                              {"params", e.Owner->Params},
                              {"offset", e.Owner->Offset}};
          else if (e.Owner)
            light["owner"] = {{"kind", e.Owner->Kind}, {"offset", e.Owner->Offset}};
          lights.push_back(std::move(light));
        }
        auto detail = place();
        detail["lights"] = std::move(lights);
        finish("ok", detail);
      } else if (op == "gizmos") {
        sceneEnvironment->ShowLightGizmos(command.value("shown", true));
        finish("ok", {{"frame", guestFrame}});
      } else if (op == "quit") {
        finish("ok", place());
        devLabQuit = true;
      } else if (op != "wait") {
        finish("rejected", {{"reason", "unknown op"}});
      }
    } catch (const std::exception &error) {
      finish("rejected", {{"reason", error.what()}});
    }
    return;
  }
  ++devLabFrames;
  const auto &command = *devLabCommand;
  const std::string op = command["op"].get<std::string>();
  if (op == "warp") {
    // Settled: the given frames after arriving (title cards and fades).
    if (status.Phase == Oot3dNativeGame::DevWarpPhase::Failed) {
      finish("failed", {{"reason", status.Message}});
    } else if (status.Phase == Oot3dNativeGame::DevWarpPhase::Arrived) {
      if (!devLabSawArrival) {
        devLabSawArrival = true;
        devLabFrames = 0;
      } else if (devLabFrames >= command.value("settleFrames", 60U)) {
        finish("ok", place());
      }
    } else if (devLabFrames >= 3600U) {
      finish("timeout", {{"reason", status.Message}});
    }
  } else if (op == "camera") {
    // Held until the game took the free view for the given frames from the
    // main camera (PlayState +0xA64, 0): a cutscene camera does not count.
    int16_t activeCamera = -1;
    const uint32_t playState = sceneViewProbe.Stats().LastPlayStateAddress;
    if (playState != 0U)
      (void)processStorage->Memory().Read16(playState + 0xA64U, reinterpret_cast<uint16_t *>(&activeCamera));
    if (activeCamera != 0) devLabAppliedAt = camera.AppliedCount();
    const uint64_t applied = camera.AppliedCount() - devLabAppliedAt;
    if (applied >= command.value("holdFrames", 45U))
      finish("ok", {{"frame", guestFrame}, {"applied", applied}});
    else if (devLabFrames >= command.value("timeoutFrames", 900U))
      finish("timeout", {{"frame", guestFrame}, {"applied", applied}, {"activeCamera", activeCamera}});
  } else if (op == "wait") {
    if (devLabFrames >= command.value("frames", 30U)) finish("ok", place());
  }
}

void NativeA32WindowSession::UpdateDevFreeCamera(Oot3dNativeGame::NativeA32InputFrame &inputFrame) {
  auto &freeCamera = *nativeCandidateDispatch.DevFreeCamera;
  const Uint8 *keys = SDL_GetKeyboardState(nullptr);
  const auto &io = ImGui::GetIO();
  const bool keyboard = keys != nullptr && !io.WantCaptureKeyboard;
  const bool toggle = keyboard && keys[SDL_SCANCODE_F6] != 0U;
  if (toggle && !devFreeCameraToggleHeld) freeCamera.SetActive(!freeCamera.Active());
  devFreeCameraToggleHeld = toggle;
  const auto now = std::chrono::steady_clock::now();
  const float seconds = devFreeCameraLastRefresh.time_since_epoch().count() == 0
      ? 0.0F
      : std::min(std::chrono::duration<float>(now - devFreeCameraLastRefresh).count(), 0.1F);
  devFreeCameraLastRefresh = now;
  // Physical sources, not the game's bindings (its R is the right mouse
  // button): the mouse motion since the last refresh, the pad as it is.
  auto &sources = nativeControlPollingState;
  const float mouseX = static_cast<float>(sources.DevMouseDeltaX);
  const float mouseY = static_cast<float>(sources.DevMouseDeltaY);
  sources.DevMouseDeltaX = sources.DevMouseDeltaY = 0;
  if (!freeCamera.Active()) return;
  // Pad: left stick moves, right stick looks (120 degrees per second), L/R
  // shoulders down/up.
  float forward = sources.DevPadLeftStick[1];
  float right = sources.DevPadLeftStick[0];
  float up = (sources.DevPadRightShoulder ? 1.0F : 0.0F) - (sources.DevPadLeftShoulder ? 1.0F : 0.0F);
  float yaw = -sources.DevPadRightStick[0] * 120.0F * seconds;
  float pitch = sources.DevPadRightStick[1] * 120.0F * seconds;
  float multiplier = 1.0F;
  if (keyboard) {
    // WASD moves, Q/E down/up, Shift faster, Ctrl slower.
    forward += (keys[SDL_SCANCODE_W] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_S] ? 1.0F : 0.0F);
    right += (keys[SDL_SCANCODE_D] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_A] ? 1.0F : 0.0F);
    up += (keys[SDL_SCANCODE_E] ? 1.0F : 0.0F) - (keys[SDL_SCANCODE_Q] ? 1.0F : 0.0F);
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) multiplier = 4.0F;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) multiplier = 0.25F;
  }
  // Right mouse button held: look (0.15 degrees per pixel).
  if (sources.DevMouseRightHeld && !io.WantCaptureMouse) {
    yaw -= mouseX * 0.15F;
    pitch -= mouseY * 0.15F;
  }
  const auto clampAxis = [](float value) { return std::clamp(value, -1.0F, 1.0F); };
  freeCamera.Move(clampAxis(forward), clampAxis(right), clampAxis(up), yaw, pitch, seconds, multiplier);
  // Link stays still: the game sees no pad, sticks or touch.
  inputFrame.Hid.Buttons = 0U;
  inputFrame.Hid.CirclePadX = inputFrame.Hid.CirclePadY = 0;
  inputFrame.Hid.TouchPressed = false;
  inputFrame.CStick = {};
}
#endif

void NativeA32WindowSession::PublishNativeSceneView() {
  auto &process = *processStorage;
  // Guest memory is read here; the scene environment, the duplicate check
  // and the publish belong to the renderer and run in its order.
  auto snapshot = sceneViewProbe.Capture(process.Memory());
  auto environmentTime = snapshot
      ? Oot3dNativeGame::CaptureSceneEnvironmentTime(snapshot->PlayStateAddress, process.Memory())
      : std::nullopt;
  if (environmentTime)
    environmentTime->Group = Oot3dNativeGame::CaptureNativeEnvironmentGroup(
        snapshot->PlayStateAddress, process.Memory(), &environmentTime->AuthoredScene);
  // Where Link is (world position +0x28, shape yaw +0xBE: 0 faces +Z,
  // 0x4000 +X), for lights placed from F1.
  std::optional<Oot3dNativeGame::ScenePlayerPlacement> player;
  if (const uint32_t actor = playerTimingProbe.Stats().LastPlayerAddress; actor != 0U) {
    Oot3dNativeGame::ScenePlayerPlacement placement;
    uint32_t bits[3]{};
    uint16_t yaw = 0;
    if (process.Memory().Read32(actor + 0x28U, &bits[0]) &&
        process.Memory().Read32(actor + 0x2CU, &bits[1]) &&
        process.Memory().Read32(actor + 0x30U, &bits[2]) &&
        process.Memory().Read16(actor + 0xBEU, &yaw)) {
      for (size_t axis = 0; axis < 3; ++axis)
        placement.Position[axis] = std::bit_cast<float>(bits[axis]);
      placement.Yaw = static_cast<float>(static_cast<int16_t>(yaw)) * (180.0F / 32768.0F);
      if (std::isfinite(placement.Position[0]) && std::isfinite(placement.Position[1]) &&
          std::isfinite(placement.Position[2]))
        player = placement;
    }
  }
  // Light gizmos also show while the developer free camera looks around.
  bool freeCamera = false;
#if defined(TRIAEVUM_DEVELOPER_TOOLS)
  freeCamera = nativeCandidateDispatch.DevFreeCamera->Active();
  if (devEmitterLog && snapshot)
    devEmitterLog->Observe(++devEmitterFrames, snapshot->PlayStateAddress, snapshot->SceneId, process.Memory());
#endif
  renderStream.Submit([&, snapshot, environmentTime, player, freeCamera] {
  sceneEnvironment->ObserveScene(snapshot ? snapshot->SceneId : std::nullopt,
      environmentTime.value_or(Oot3dNativeGame::SceneEnvironmentTime{}), player);
  sceneEnvironment->PollProfileFiles();
  const auto shadowSettings = Fast::Oot3d::GraphicsSettingsRuntime::Instance().SnapshotForRendering();
  picaCompositionTracker.SetRoomShadowShellsEnabled(
      shadowSettings.Environment.CasterFaces == Fast::Renderer3ds::ShadowCasterFaces::InteriorShells &&
      shadowSettings.Value.Effects.DirectionalShadows.Mode == Fast::Oot3d::DirectionalShadowMode::VirtualShadowMaps);
  sceneEnvironment->PublishLightGizmo(
      snapshot && snapshot->CameraAvailable ? std::optional(snapshot->Eye) : std::nullopt, freeCamera);
  if (!snapshot.has_value() ||
      snapshot->Serial == lastPublishedSceneViewSerial) {
    return;
  }
  const Fast::Oot3d::TitleSceneViewSubmission view{
      snapshot->GuestFunction,
      snapshot->GuestReturnAddress,
      snapshot->Left,
      snapshot->Right,
      snapshot->Bottom,
      snapshot->Top,
      snapshot->NearPlane,
      snapshot->FarPlane,
      snapshot->Eye,
      snapshot->At,
      snapshot->CameraAvailable,
      // Rooms share their scene's coordinates; another scene is another world.
      snapshot->SceneId.has_value() ? uint64_t{*snapshot->SceneId} + 1U : 0U,
  };
  if (titleRenderBackend != nullptr &&
      titleRenderBackend->PublishSceneView(view)) {
    lastPublishedSceneViewSerial = snapshot->Serial;
    ++publishedSceneViewCount;
  }
  });
}

// Guest memory and guest interrupts belong to the guest. The renderer
// reaches them only from a command the guest waits for (inline or Run);
// anything else would race the running guest, so it fails instead.
void NativeA32WindowSession::RequireStoppedGuest() {
  if (renderStream.InCommand() && !renderStream.InSynchronousCommand()) {
    throw std::runtime_error(
        "renderer reached guest state while the guest was running");
  }
}

std::vector<Fast::Renderer3ds::PicaTextureCopyReference> NativeA32WindowSession::CollectGuestCopyReferences() {
  auto &submissionQueue = *submissionQueueStorage;
  std::vector<Fast::Renderer3ds::PicaTextureCopyReference> references;
  submissionQueue.CollectLiveCopyReferences(references);
  for (const auto& draw : submissionQueue.PendingDraws()) {
    for (const auto& resource : draw.Resources) {
      references.insert(references.end(), resource.CopyReferences.begin(),
                        resource.CopyReferences.end());
    }
  }
  for (const auto& plan : picaPresentationScheduler.PendingDraws()) {
    Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(plan, references);
  }
  return references;
}

void NativeA32WindowSession::PinGpuCopyResources(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample& sample) {
  auto &api = *apiPointer;
  pinnedCopyReferences = guestCopyReferences;
  if (previousVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *previousVisualFrame, pinnedCopyReferences);
  }
  if (latestVisualFrame) {
    Oot3dNativeGame::CollectOot3dPicaFrameCopyReferences(
        *latestVisualFrame, pinnedCopyReferences);
  }
  // The active sample may already have left the accumulator, or may be
  // the new frame before it replaces latestVisualFrame.
  for (const auto& draw : sample.Draws) {
    if (draw.BasePlan != nullptr) {
      Oot3dNativeGame::CollectOot3dPicaPlanCopyReferences(
          *draw.BasePlan, pinnedCopyReferences);
    }
  }
  if (!api.SetPinnedPicaTextureCopies(pinnedCopyReferences, &renderError)) {
    throw std::runtime_error("native PICA copy retention failed: " + renderError);
  }
}

bool NativeA32WindowSession::FlushNativeDependencies() {
  auto &api = *apiPointer;
  // The accumulator is guest-owned, so whether there is dependency work is
  // known here; only real work waits for the renderer.
  if (!picaPresentationScheduler.ViewDependencyWork(dependencySampleScratch))
    return false;
  // Synchronous: raw copies read and write guest memory and signal PPF
  // while the guest waits.
  bool flushed = false;
  renderStream.Run([&] {
    // Without an acquired image the flush waits; PPF arrives later, as it
    // may on hardware.
    if (rendererFrameSkipped) return;
    // Deferred placeholders this flush executes are built now (the guest
    // waits here anyway); the render worker is idle inside Run.
    uint64_t lastDependencyDraw = 0U;
    for (const auto& draw : dependencySampleScratch.Draws) {
      if (draw.BasePlan != nullptr)
        lastDependencyDraw = std::max(lastDependencyDraw, draw.BasePlan->SubmissionId);
    }
    if (!picaPresentationScheduler.ResolveDeferredDraws(
            lastDependencyDraw,
            [&](Oot3dNativeGame::Oot3dPicaVulkanDrawPlan& plan) {
              return Oot3dNativeGame::ResolveOot3dPicaDeferredDrawPlan(plan, &renderError);
            })) {
      throw std::runtime_error(renderError);
    }
    guestCopyReferences = CollectGuestCopyReferences();
    PinGpuCopyResources(dependencySampleScratch);
    if (!picaPresentationScheduler.Execute(
            api, dependencySampleScratch, kNativeDependencyRenderTargetNamespace,
            Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::DependencyFlush,
            false, &renderError)) {
      throw std::runtime_error("native PICA dependency flush failed: " + renderError);
    }
    flushed = true;
  });
  return flushed;
}

void NativeA32WindowSession::ExecuteVisualSample(const Oot3dNativeGame::Oot3dPicaVisualFrameViewSample &sample, Oot3dNativeGame::Oot3dPicaPresentationExecutionKind kind, bool present, const Fast::Oot3d::NativeFrameTemporalSample& temporalSample, uint64_t renderTargetNamespace) {
  auto &api = *apiPointer;
  // Leaving the game: a command already running (a new frame followed by its
  // interpolated sample, a repeated sample of the previous frame) draws
  // nothing more once the exit has begun.
  if (renderStream.ShutdownRequested()) return;
  const auto started = std::chrono::steady_clock::now();
  (void)api.PublishPicaFrameTemporalSample(temporalSample);
  // Only displayed samples advance the grass colliders: the authoritative
  // replay of a new frame is not shown, and taking it as the previous position
  // gave the next displayed sample a backward sweep.
  if (present) Fast::Oot3d::GrassInteractionBridge::Instance().SelectSample(temporalSample);
  PinGpuCopyResources(sample);
  // config/gpu_timing: step/interp frame and canonical (hidden) draws.
  // A new guest frame is every kind but the repeated (in-between) sample:
  // with interpolation the step frame presents an InterpolatedFrame (the
  // delayed sample at alpha 0), and the native replay may be skipped, so
  // counting only NativeFrame/CurrentFrame classified every step as interp.
  Fast::GlPerf::SampleScope perfSample(
      kind != Oot3dNativeGame::Oot3dPicaPresentationExecutionKind::RepeatedFrame,
      present);
  if (!picaPresentationScheduler.Execute(
          api, sample, renderTargetNamespace, kind,
          present, &renderError)) {
    throw std::runtime_error(
        "native PICA scheduled presentation failed: " + renderError);
  }
  const double elapsed = SecondsSince(started);
  if (present) {
    capturedTemporalSample = temporalSample;
    lastPresentedVisual.emplace(sample.TopTransfer, renderTargetNamespace);
  }
  rendererTiming->PicaBackendSeconds += elapsed;
  rendererTiming->VisualReplaySeconds += elapsed;
}

} // namespace Oot3dA32Window
