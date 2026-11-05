// Host input polling of the A32 window and the TopScreen pause transitions it drives.
#include "oot3d_native_a32_window_shared.h"
#include "fast/oot3d/gl_widescreen_menu_policy.h"
#if defined(__SWITCH__)
#include "fast/oot3d/debug_menu.h"
#include "oot3d_top_screen_hud_touch.h"

#include <switch.h>
#endif

namespace Oot3dA32Window {

#if defined(__SWITCH__)
namespace {

// The console's touch screen, read from HID shared memory (libnx). With the
// deko3d renderer SDL has no window, and its Switch driver reports no finger
// events without one, so SDL cannot be the source. The panel is 1280x720 in
// handheld; docked there are no touches. SDL's synthetic touch->mouse events
// stay disabled so a touch never reaches mouse-bound actions or mouse aim.
//
// Polling the current state alone would miss a tap that lands and lifts
// between two polls, so every sample of the HID history (17 entries) newer
// than the last poll is fed to the tracker, which latches each press until a
// guest refresh has consumed it.
struct SwitchTouchScreen {
  Oot3dNativeGame::TopScreenTouchTracker Tracker;
  std::array<HidTouchScreenState, 17> States{};
  std::uint64_t LastSample = 0;
  bool Started = false;
};

SwitchTouchScreen &SwitchTouch() {
  static SwitchTouchScreen touch;
  return touch;
}

void ObserveSwitchTouchScreen(SwitchTouchScreen &touch) {
  constexpr float kPanelWidth = 1280.0F;
  constexpr float kPanelHeight = 720.0F;
  if (!touch.Started) {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    hidInitializeTouchScreen();
  }
  auto &states = touch.States;
  const std::size_t count = std::min(
      hidGetTouchScreenStates(states.data(), states.size()), states.size());
  // The oldest sample newer than the last one fed, or null.
  const auto nextSample = [&]() -> const HidTouchScreenState * {
    const HidTouchScreenState *next = nullptr;
    for (std::size_t index = 0; index < count; ++index) {
      const auto &state = states[index];
      if (state.sampling_number > touch.LastSample &&
          (next == nullptr || state.sampling_number < next->sampling_number))
        next = &state;
    }
    return next;
  };
  if (!touch.Started) {
    // History from before launch (the tap that started the app) is not a press.
    touch.Started = true;
    while (const auto *state = nextSample())
      touch.LastSample = state->sampling_number;
    return;
  }
  std::array<Oot3dNativeGame::TopScreenTouchContact, 16> contacts{};
  while (const auto *state = nextSample()) {
    touch.LastSample = state->sampling_number;
    const std::size_t touches = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(state->count, 0)), contacts.size());
    for (std::size_t index = 0; index < touches; ++index) {
      const auto &contact = state->touches[index];
      contacts[index] = {contact.finger_id,
                         static_cast<float>(contact.x) / kPanelWidth,
                         static_cast<float>(contact.y) / kPanelHeight,
                         (contact.attributes & HidTouchAttribute_End) != 0U};
    }
    touch.Tracker.ObserveSample(std::span(contacts.data(), touches));
  }
}

} // namespace
#endif

Oot3dNativeGame::NativeA32InputFrame
PollNativeA32Input(Fast::Fast3dWindow &window,
                   bool nativeFrontendTouchEnabled,
                   bool hostGuiVisible,
                   bool topScreenUiProfile,
                   bool topScreenFreeCameraEnabled,
                   Oot3dNativeGame::NativeControlConfigRuntime &controls,
                   const Oot3dNativeGame::NativeAimProfileTransform
                       &aimTransform,
                   double samplePeriodSeconds,
                   bool guestRefreshWillConsume,
                   NativeControlPollingState &pollingState,
                   [[maybe_unused]] std::span<const oot3d::ui::UiPrimitive>
                       topScreenHudPrimitives,
                   bool widescreenMenuCorners) {
  using namespace Oot3dNativeGame;
  auto config = controls.Snapshot().Config;
  NativeControlHostInputState host;
  if (hostGuiVisible) pollingState.PendingInputSeconds = 0.0;
  host.SamplePeriodSeconds = ThreeDsRecomp::Input::ConsumeInputPeriod(
      samplePeriodSeconds, guestRefreshWillConsume || hostGuiVisible, pollingState.PendingInputSeconds);
  ThreeDsRecomp::Input::SdlControllerSelection selectedController;
  auto *context = Ship::Context::GetRawInstance();
  auto controlDeck = context != nullptr ? context->GetControlDeck() : nullptr;
  if (controlDeck != nullptr) {
    auto devices = controlDeck->GetConnectedPhysicalDeviceManager();
    if (devices != nullptr) {
      selectedController = ThreeDsRecomp::Input::ResolveSdlController(
          devices->GetConnectedSDLGamepadsForPort(0), config.PreferredControllerGuid,
          config.PreferredControllerSerial, pollingState.ControllerInstance);
    }
  }
  if (pollingState.ControllerInstance != selectedController.InstanceId) {
    pollingState.RightStickProfile = {};
    pollingState.VirtualMotion.ResetController();
    pollingState.PreviousControllerButtons = pollingState.PendingControllerButtons = 0;
  }
  pollingState.ControllerInstance = selectedController.InstanceId;
#if defined(__SWITCH__)
  // L3+R3 opens/closes the debug menu (fast/oot3d/debug_menu.h). Read from the
  // physical pad even when the controller is disabled for gameplay; while the
  // menu is open the game sees it as host UI (no buttons, sticks or touch).
  {
    SDL_GameController *pad = selectedController.Controller;
    const bool debugCombo =
        pad != nullptr &&
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0 &&
        SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    if (pollingState.DebugMenuCombo.Update(debugCombo)) Fast::Oot3d::ToggleDebugMenu();
  }
  if (Fast::Oot3d::DebugMenuOpen()) {
    hostGuiVisible = true;
    pollingState.PendingInputSeconds = 0.0;
  }
#endif
  const auto &io = ImGui::GetIO();
  const bool keyboardCaptured = hostGuiVisible || io.WantCaptureKeyboard;
  const bool mouseCaptured = hostGuiVisible;

  const auto selectedDevice = std::find_if(selectedController.Devices.begin(), selectedController.Devices.end(),
      [](const auto& device) { return device.Selected; });
  config = ControlsForDevice(config, selectedDevice != selectedController.Devices.end() ? &*selectedDevice : nullptr);
  controls.ObserveDevices(std::move(selectedController.Devices));

  const int16_t triggerThreshold = static_cast<int16_t>(
      32767 * std::clamp(config.TriggerDeadZonePercent, 0, 95) / 100);
  class LusHostButtonSource final
      : public ThreeDsRecomp::Input::HostButtonSource {
   public:
    LusHostButtonSource(Fast::Fast3dWindow &window,
                        bool keyboardCaptured,
                        bool mouseCaptured,
                        SDL_GameController *controller,
                        int16_t triggerThreshold)
        : mWindow(window), mKeyboardCaptured(keyboardCaptured),
          mMouseCaptured(mouseCaptured), mController(controller),
          mTriggerThreshold(triggerThreshold) {}

    bool IsKeyboardKeyHeld(
        NativeKeyboardKey key) const noexcept override {
      return !mKeyboardCaptured && key != NativeKeyboardKey::None && key != NativeKeyboardKey::Escape &&
             mWindow.IsKeyDown(static_cast<int32_t>(key));
    }

    bool IsMouseButtonHeld(
        NativeMouseButton button) const noexcept override {
      return !mMouseCaptured && button != NativeMouseButton::None &&
             mWindow.GetMouseState(static_cast<Ship::MouseBtn>(button));
    }

    bool IsGamepadButtonHeld(
        NativeGamepadButton binding) const noexcept override {
      return ThreeDsRecomp::Input::IsSdlControllerButtonHeld(mController, binding, mTriggerThreshold);
    }

   private:
    Fast::Fast3dWindow &mWindow;
    bool mKeyboardCaptured = false;
    bool mMouseCaptured = false;
    SDL_GameController *mController = nullptr;
    int16_t mTriggerThreshold = 0;
  };
  // Remapping observes the same physical source, before gameplay's device and
  // UI filters. It works even for a currently disabled device.
  LusHostButtonSource rawButtonSource(window, false, false,
      selectedController.Controller,
      triggerThreshold);
  if (hostGuiVisible) {
    controls.ObserveBindingCapture(rawButtonSource, window.IsKeyDown(Ship::LUS_KB_ESCAPE));
  } else {
    const auto phase = controls.BindingCaptureStatus().Phase;
    if (phase != ThreeDsRecomp::Input::BindingCapturePhase::Idle &&
        phase != ThreeDsRecomp::Input::BindingCapturePhase::Cancelled)
      controls.CancelBindingCapture();
  }
  LusHostButtonSource buttonSource(
      window, keyboardCaptured, mouseCaptured,
      !hostGuiVisible ? selectedController.Controller : nullptr,
      triggerThreshold);
  const ThreeDsRecomp::Input::HostDeviceEnablement enabledDevices{
      config.KeyboardEnabled,
      config.MouseEnabled,
      config.ControllerEnabled,
  };

  for (size_t index = 0; index < kNativeControlActionCount; ++index) {
    if (hostGuiVisible) pollingState.PendingPressedActions[index] = false;
    const auto &binding = config.Bindings[index];
    const bool physicalHeld = ThreeDsRecomp::Input::IsHostBindingHeld(
        binding, enabledDevices, buttonSource);
    if (physicalHeld && !pollingState.PhysicalActions[index] &&
        static_cast<NativeControlAction>(index) !=
            NativeControlAction::Start) {
      pollingState.PendingPressedActions[index] = true;
    }
    pollingState.PhysicalActions[index] = physicalHeld;
    host.Actions[index] = physicalHeld;
    if (guestRefreshWillConsume &&
        static_cast<NativeControlAction>(index) !=
            NativeControlAction::Start) {
      host.Actions[index] =
          physicalHeld || pollingState.PendingPressedActions[index];
      pollingState.PendingPressedActions[index] = false;
    }
  }

  ThreeDsRecomp::Input::SampleSdlController(
      config.ControllerEnabled ? selectedController.Controller : nullptr, host, !hostGuiVisible,
      config.ControllerTouchpadEnabled ? config.ControllerTouchpadIndex : -1, triggerThreshold);
  controls.ObserveMotion(host.ControllerMotion);
  pollingState.PendingControllerButtons |= host.ControllerButtons & ~pollingState.PreviousControllerButtons;
  pollingState.PreviousControllerButtons = host.ControllerButtons;
  if (guestRefreshWillConsume) {
    host.ControllerPressed = pollingState.PendingControllerButtons;
    pollingState.PendingControllerButtons = 0;
  }
  if (hostGuiVisible) {
    pollingState.PendingControllerButtons = 0;
    host.ControllerMotion = {};
    pollingState.RightStickProfile = {};
  }

  const bool sourceUsesMouse =
      config.NativeAimSource == NativeMotionSource::Mouse ||
      config.NativeAimSource == NativeMotionSource::Automatic ||
      (topScreenUiProfile && topScreenFreeCameraEnabled &&
       (config.FreeCameraSource == NativeMotionSource::Mouse ||
        config.FreeCameraSource == NativeMotionSource::Automatic));
  const bool gameplayMouseEligible = ResolveNativeGameplayMouseOwnership(
      nativeFrontendTouchEnabled, hostGuiVisible, config.MouseEnabled,
      sourceUsesMouse);
  const bool wasCaptured = window.IsMouseCaptured();
  window.SetMouseCapture(gameplayMouseEligible && config.CaptureMouseInGameplay);
  window.SetCursorVisibility(!window.IsMouseCaptured());
  const bool gameplayMouseOwned = gameplayMouseEligible && !window.IsMouseCaptureReleased();
  bool mouseCaptureChanged =
      gameplayMouseOwned != pollingState.GameplayMouseOwned || wasCaptured != window.IsMouseCaptured();
  pollingState.MouseEligiblePolls += gameplayMouseEligible ? 1U : 0U;
  pollingState.MouseReleasedPolls += window.IsMouseCaptureReleased() ? 1U : 0U;
  pollingState.MouseHostUiPolls += hostGuiVisible ? 1U : 0U;
  pollingState.MouseNativeUiPolls += nativeFrontendTouchEnabled ? 1U : 0U;
  pollingState.MouseCaptureTransitions += mouseCaptureChanged ? 1U : 0U;
  pollingState.GameplayMouseOwned = gameplayMouseOwned;
  // SDL relative motion is an accumulator. Drain it even while ImGui owns the
  // pointer so menu movement and capture warps cannot leak into gameplay.
  const auto mouseDelta = window.GetMouseDelta();
  pollingState.MouseMovementPolls += mouseDelta.x != 0 || mouseDelta.y != 0 ? 1U : 0U;
  pollingState.DevMouseDeltaX += mouseDelta.x;
  pollingState.DevMouseDeltaY += mouseDelta.y;
  pollingState.DevMouseRightHeld = window.GetMouseState(Ship::LUS_MOUSE_BTN_RIGHT);
  {
    SDL_GameController *pad = selectedController.Controller;
    const auto axis = [&](SDL_GameControllerAxis which, float sign) {
      if (pad == nullptr) return 0.0F;
      const float value = static_cast<float>(SDL_GameControllerGetAxis(pad, which)) / 32767.0F * sign;
      constexpr float kDeadZone = 0.2F;
      return std::abs(value) < kDeadZone ? 0.0F
          : std::clamp((value - std::copysign(kDeadZone, value)) / (1.0F - kDeadZone), -1.0F, 1.0F);
    };
    pollingState.DevPadLeftStick = {axis(SDL_CONTROLLER_AXIS_LEFTX, 1.0F), axis(SDL_CONTROLLER_AXIS_LEFTY, -1.0F)};
    pollingState.DevPadRightStick = {axis(SDL_CONTROLLER_AXIS_RIGHTX, 1.0F), axis(SDL_CONTROLLER_AXIS_RIGHTY, -1.0F)};
    pollingState.DevPadLeftShoulder =
        pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
    pollingState.DevPadRightShoulder =
        pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
  }
  if (gameplayMouseOwned && !mouseCaptureChanged) {
    pollingState.PendingMouseDeltaX += mouseDelta.x;
    pollingState.PendingMouseDeltaY += mouseDelta.y;
    if (guestRefreshWillConsume) {
      host.MouseDeltaX = static_cast<int32_t>(std::clamp<int64_t>(
          pollingState.PendingMouseDeltaX, INT32_MIN, INT32_MAX));
      host.MouseDeltaY = static_cast<int32_t>(std::clamp<int64_t>(
          pollingState.PendingMouseDeltaY, INT32_MIN, INT32_MAX));
      pollingState.PendingMouseDeltaX = 0;
      pollingState.PendingMouseDeltaY = 0;
    }
  } else {
    pollingState.PendingMouseDeltaX = 0;
    pollingState.PendingMouseDeltaY = 0;
  }

#if defined(__SWITCH__)
  // Sampled every poll: each finger gets its owner when it lands (the native
  // page touch, a gameplay HUD control, the aim, or nothing), so a press that
  // landed outside a touch-enabled page is never replayed when one opens.
  const bool hudTouchEligible =
      topScreenUiProfile && !nativeFrontendTouchEnabled && !hostGuiVisible;
  const auto hudTargets =
      hudTouchEligible
          ? ResolveTopScreenHudTouchTargets(topScreenHudPrimitives,
                                            window.GetWidth(),
                                            window.GetHeight())
          : TopScreenHudTouchTargets{};
  auto &touchScreen = SwitchTouch();
  ObserveSwitchTouchScreen(touchScreen);
  const auto switchTouch = ResolveTopScreenTouchPoll(
      touchScreen.Tracker,
      {nativeFrontendTouchEnabled, hostGuiVisible, &hudTargets,
       window.GetWidth(), window.GetHeight(), config.TouchAimEnabled},
      guestRefreshWillConsume);
  // A drag aims through the native aim sources, next to the right stick and
  // the gyro (aim.touch_degrees_per_pixel; nonzero only on consuming polls).
  host.TouchAimDeltaX = switchTouch.AimDeltaX;
  host.TouchAimDeltaY = switchTouch.AimDeltaY;
#endif
  auto frame = MapNativeControlInput(config, host, aimTransform,
                                     &pollingState.RightStickProfile,
                                     guestRefreshWillConsume,
                                     &pollingState.VirtualMotion);
#if defined(__SWITCH__)
  // A tapped HUD control presses the physical control it pictures.
  frame.Hid.Buttons |= switchTouch.HudButtons;
#endif
  if (nativeFrontendTouchEnabled && !hostGuiVisible) {
    if (window.IsMouseCaptured()) {
      window.SetMouseCapture(false);
    }
    window.SetCursorVisibility(true);
    auto pointer = window.GetMousePos();
    bool pointerPressed = window.GetMouseState(Ship::LUS_MOUSE_BTN_LEFT);
#if defined(__SWITCH__)
    if (switchTouch.PagePressed) {
      pointer.x = static_cast<int32_t>(
          switchTouch.PageX * static_cast<float>(window.GetWidth()));
      pointer.y = static_cast<int32_t>(
          switchTouch.PageY * static_cast<float>(window.GetHeight()));
      pointerPressed = true;
    }
#endif
    if (widescreenMenuCorners) {
      // The OpenGL widescreen draws the relocated lower menu's bottom
      // corners (the BACK tab) at the window's corners
      // (gl_widescreen_menu_policy.h); a touch there goes back to the corner
      // of the menu the game expects.
      const auto remapped = Fast::Oot3d::RemapGlWidescreenCornerPointer(
          static_cast<float>(pointer.x), static_cast<float>(pointer.y),
          static_cast<float>(window.GetWidth()),
          static_cast<float>(window.GetHeight()));
      pointer.x = static_cast<int32_t>(remapped.X);
      pointer.y = static_cast<int32_t>(remapped.Y);
    }
    const auto touch = Oot3dNativeGame::MapHostPointerToNativeA32Touch(
        pointer.x, pointer.y, window.GetWidth(), window.GetHeight(),
        pointerPressed,
        topScreenUiProfile
            ? Oot3dNativeGame::NativeA32TouchPresentation::TopScreen400x240
            : Oot3dNativeGame::NativeA32TouchPresentation::
                  NativeLowerScreen320x240);
    frame.Hid.TouchX = touch.X;
    frame.Hid.TouchY = touch.Y;
    frame.Hid.TouchPressed = touch.Pressed;
    if (!frame.Hid.TouchPressed) {
      const auto pad = ThreeDsRecomp::Input::MapNormalizedTouch(host.ControllerTouch);
      if (pad.Pressed) {
        frame.Hid.TouchX = pad.X;
        frame.Hid.TouchY = pad.Y;
        frame.Hid.TouchPressed = true;
      }
    }
  }
  ApplyNativeControlShortcutTouch(host, frame);
  return frame;
}

Oot3dNativeGame::NativeA32InputFrame ResolveNativeA32GuestInput(
    const Oot3dNativeGame::NativeA32InputFrame &physical,
    const Oot3dNativeGame::NativeA32InputTimeline &timeline,
    uint32_t guestFrameIndex, uint32_t runFrameIndex,
    bool nativeFrontendTouchEnabled,
    bool topScreenStartRoutingEligible, bool topScreenUiProfile,
    Oot3dNativeGame::NativeA32PolledButtonLatch &startButtonLatch,
    Oot3dNativeGame::TopScreenStartRoutingState &startRoutingState) {
  auto resolved = physical;
  if (timeline.Enabled()) {
    resolved = timeline.Sample(guestFrameIndex, runFrameIndex);
    resolved.Exit = physical.Exit;
  } else {
    const uint32_t start = Oot3dNativeGame::NativeA32HidButtonMask(
        Oot3dNativeGame::NativeA32HidButton::Start);
    const bool physicalStartHeld = (resolved.Hid.Buttons & start) != 0U;
    if (Oot3dNativeGame::ResolveNativeA32PolledButton(
            physicalStartHeld, startButtonLatch)) {
      resolved.Hid.Buttons |= start;
    } else {
      resolved.Hid.Buttons &= ~start;
    }
  }
  Oot3dNativeGame::ApplyTopScreenStartRouting(
      resolved, topScreenUiProfile, topScreenStartRoutingEligible,
      nativeFrontendTouchEnabled, true, startRoutingState);
  return resolved;
}

bool ResolveNativeA32TouchInputEnabled(
    Oot3dNativeGame::NativeA32Memory &memory,
    const Oot3dNativeGame::Oot3dNativeUiLifecycleBridge &uiLifecycleBridge,
    bool topScreenUiProfile) {
  bool nativePausePageActive = false;
  if (topScreenUiProfile) {
    Oot3dNativeGame::TopScreenPauseDrawInputs pauseInputs;
    nativePausePageActive =
        Oot3dNativeGame::ReadTopScreenPauseDrawInputs(memory, &pauseInputs) &&
        pauseInputs.AnyPageGateActive;
  }
  return Oot3dNativeGame::ResolveNativeA32TouchInputEnabled(
      topScreenUiProfile,
      uiLifecycleBridge.NativeTouchPresentationActive(),
      // A touch on the title also reveals the withheld version choice.
      uiLifecycleBridge.NativeFrontendPresentationActive() ||
          uiLifecycleBridge.NativeVersionChoiceAwaitingReveal(),
      nativePausePageActive);
}

} // namespace Oot3dA32Window
