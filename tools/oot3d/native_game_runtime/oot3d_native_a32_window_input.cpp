// Host input polling of the A32 window and the TopScreen pause transitions it drives.
#include "oot3d_native_a32_window_shared.h"
#include "fast/oot3d/gl_widescreen_menu_policy.h"

#if defined(__SWITCH__)
#include <mutex>
#endif

namespace Oot3dA32Window {

#if defined(__SWITCH__)
namespace {

// The console's touch screen. SDL reports it as finger events normalised to
// the window (the panel is 1280x720; docked there is no touch device). The
// PC path reads the mouse as the pointer; here the first finger is the
// pointer instead, and SDL's synthetic touch->mouse events are disabled so a
// touch never reaches mouse-bound gameplay actions or mouse aim.
//
// Polling alone would miss a tap that goes down and up between two polls
// (the guest samples at most 30-60 times a second), so an event watch
// latches every press until a guest refresh has consumed it, like the
// pending button presses of PollNativeA32Input.
struct SwitchTouchLatch {
  std::mutex Mutex;
  SDL_FingerID Finger = 0;
  bool Down = false;
  bool PendingPress = false;
  float X = 0.0F;
  float Y = 0.0F;
};

SwitchTouchLatch &SwitchTouch() {
  static SwitchTouchLatch latch;
  return latch;
}

int ObserveSwitchTouchEvent(void *, SDL_Event *event) {
  if (event->type != SDL_FINGERDOWN && event->type != SDL_FINGERMOTION &&
      event->type != SDL_FINGERUP) {
    return 0;
  }
  auto &touch = SwitchTouch();
  std::lock_guard lock(touch.Mutex);
  const auto &finger = event->tfinger;
  if (event->type == SDL_FINGERDOWN) {
    if (touch.Down && finger.fingerId != touch.Finger) {
      return 0; // a second finger does not move the pointer
    }
    touch.Finger = finger.fingerId;
    touch.Down = true;
    touch.PendingPress = true;
  } else if (!touch.Down || finger.fingerId != touch.Finger) {
    return 0;
  } else if (event->type == SDL_FINGERUP) {
    touch.Down = false;
  }
  touch.X = std::clamp(finger.x, 0.0F, 1.0F);
  touch.Y = std::clamp(finger.y, 0.0F, 1.0F);
  return 0;
}

struct SwitchTouchSample {
  bool Pressed = false;
  float X = 0.0F;
  float Y = 0.0F;
};

SwitchTouchSample SampleSwitchTouch(bool guestRefreshWillConsume) {
  static std::once_flag installed;
  std::call_once(installed, [] {
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_AddEventWatch(&ObserveSwitchTouchEvent, nullptr);
  });
  auto &touch = SwitchTouch();
  std::lock_guard lock(touch.Mutex);
  SwitchTouchSample sample{touch.Down || touch.PendingPress, touch.X, touch.Y};
  if (guestRefreshWillConsume) {
    touch.PendingPress = false;
  }
  return sample;
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
                   bool widescreenMenuCorners) {
  using namespace Oot3dNativeGame;
  auto config = controls.Snapshot().Config;
  NativeControlHostInputState host;
  if (hostGuiVisible) pollingState.PendingInputSeconds = 0.0;
  host.SamplePeriodSeconds = ThreeDsRecomp::Input::ConsumeInputPeriod(
      samplePeriodSeconds, guestRefreshWillConsume || hostGuiVisible, pollingState.PendingInputSeconds);
  const auto &io = ImGui::GetIO();
  const bool keyboardCaptured = hostGuiVisible || io.WantCaptureKeyboard;
  const bool mouseCaptured = hostGuiVisible;

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

  auto frame = MapNativeControlInput(config, host, aimTransform,
                                     &pollingState.RightStickProfile,
                                     guestRefreshWillConsume,
                                     &pollingState.VirtualMotion);
#if defined(__SWITCH__)
  // Sampled every poll so a press latched outside a touch-enabled page is
  // consumed (and so never replayed when one opens).
  const auto switchTouch = SampleSwitchTouch(guestRefreshWillConsume);
#endif
  if (nativeFrontendTouchEnabled && !hostGuiVisible) {
    if (window.IsMouseCaptured()) {
      window.SetMouseCapture(false);
    }
    window.SetCursorVisibility(true);
    auto pointer = window.GetMousePos();
    bool pointerPressed = window.GetMouseState(Ship::LUS_MOUSE_BTN_LEFT);
#if defined(__SWITCH__)
    if (switchTouch.Pressed) {
      pointer.x = static_cast<int32_t>(
          switchTouch.X * static_cast<float>(window.GetWidth()));
      pointer.y = static_cast<int32_t>(
          switchTouch.Y * static_cast<float>(window.GetHeight()));
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
