// The console's own motion sensors as controller motion: see
// oot3d_switch_motion.h.
#include "oot3d_switch_motion.h"

#include <algorithm>
#include <cmath>

#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#endif

namespace Oot3dNativeGame {

ThreeDsRecomp::Input::MotionObservation ConvertSwitchSixAxis(
    const std::array<float, 3>& a, const std::array<float, 3>& w) noexcept {
  constexpr float kDegreesPerRevolution = 360.0F;
  const auto finite = [](const std::array<float, 3>& value) {
    return std::all_of(value.begin(), value.end(), [](float v) { return std::isfinite(v); });
  };
  ThreeDsRecomp::Input::MotionObservation result;
  if (finite(a)) {
    result.Accelerometer = {-a[0], a[2], a[1]};
    result.AccelerometerValid = true;
  }
  if (finite(w)) {
    result.GyroscopeDegreesPerSecond = {-w[0] * kDegreesPerRevolution, w[2] * kDegreesPerRevolution,
                                        w[1] * kDegreesPerRevolution};
    result.GyroscopeValid = true;
  }
  return result;
}

void SwitchMotionAccumulator::Reset(std::uint64_t nowMicroseconds) noexcept {
  *this = {};
  mLastSampleMicroseconds = nowMicroseconds;
}

std::size_t SwitchMotionAccumulator::Observe(std::span<const SwitchSixAxisSample> newestFirst,
                                             std::uint64_t nowMicroseconds) noexcept {
  if (newestFirst.empty()) return 0;
  const std::uint64_t newest = newestFirst.front().SamplingNumber;
  // First read after a (re)start: the LIFO may still hold the state the
  // sensor left before it stopped, so only the position is taken. A sampling
  // number that went backwards is a restart of the sensor's counter.
  if (!mHasSamplingNumber || newest < mLastSamplingNumber) {
    mLastSamplingNumber = newest;
    mHasSamplingNumber = true;
    return 0;
  }
  constexpr std::uint64_t kMaximumDeltaNanoseconds = 50'000'000;
  std::size_t added = 0;
  for (auto entry = newestFirst.rbegin(); entry != newestFirst.rend(); ++entry) {
    if (entry->SamplingNumber <= mLastSamplingNumber || !entry->Connected) continue;
    const std::uint64_t deltaNanoseconds = std::min(
        entry->DeltaTimeNanoseconds != 0 ? entry->DeltaTimeNanoseconds : DefaultDeltaNanoseconds,
        kMaximumDeltaNanoseconds);
    const double seconds = static_cast<double>(deltaNanoseconds) * 1e-9;
    for (std::size_t axis = 0; axis < 3; ++axis) {
      mAccelerationSum[axis] += static_cast<double>(entry->Acceleration[axis]) * seconds;
      mAngularVelocitySum[axis] += static_cast<double>(entry->AngularVelocity[axis]) * seconds;
    }
    mSeconds += seconds;
    ++added;
  }
  mLastSamplingNumber = newest;
  if (added != 0) {
    mLastSampleMicroseconds = nowMicroseconds;
    mWindowMicroseconds = nowMicroseconds;
  }
  return added;
}

ThreeDsRecomp::Input::MotionObservation SwitchMotionAccumulator::Take(
    bool consume, std::uint64_t nowMicroseconds) noexcept {
  if (Stale(nowMicroseconds)) {
    mLast = {};
  } else if (mSeconds > 0.0) {
    std::array<float, 3> acceleration{};
    std::array<float, 3> angularVelocity{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      acceleration[axis] = static_cast<float>(mAccelerationSum[axis] / mSeconds);
      angularVelocity[axis] = static_cast<float>(mAngularVelocitySum[axis] / mSeconds);
    }
    mLast = ConvertSwitchSixAxis(acceleration, angularVelocity);
    mLast.AccelerometerTimestampMicroseconds = mWindowMicroseconds;
    mLast.GyroscopeTimestampMicroseconds = mWindowMicroseconds;
  }
  if (consume) {
    mAccelerationSum = {};
    mAngularVelocitySum = {};
    mSeconds = 0.0;
  }
  return mLast;
}

#if defined(__SWITCH__)
namespace {

enum class MotionSource : std::uint8_t { None, FullKey, JoyDualLeft, JoyDualRight, Handheld };

// Restart a sensor whose data stalled at most this often.
constexpr std::uint64_t kRestartIntervalMicroseconds = 1'000'000;

struct SwitchMotionProvider {
  bool HandlesRequested = false;
  bool HandheldReady = false;
  bool FullKeyReady = false;
  bool JoyDualReady = false;
  // Never used unless its Ready flag is set: libnx aborts the app on a
  // handle with an invalid style index.
  HidSixAxisSensorHandle Handheld{};
  HidSixAxisSensorHandle FullKey{};
  HidSixAxisSensorHandle JoyDual[2]{};
  MotionSource Started = MotionSource::None;
  // The current sensor has reported valid data since it last started.
  bool HadData = false;
  std::uint64_t StartedMicroseconds = 0;
  AppletHookCookie Hook{};
  // Set by the applet hook (focus change, resume); read by the poll. Both run
  // on the game thread (SDL's pump calls appletMainLoop), atomic regardless.
  std::atomic<bool> RestartRequested{false};
  SwitchMotionAccumulator Accumulator;
};

SwitchMotionProvider& Provider() {
  static SwitchMotionProvider provider;
  return provider;
}

std::uint64_t NowMicroseconds() {
  return armTicksToNs(armGetSystemTick()) / 1000;
}

void OnAppletHook(AppletHookType hook, void* param) {
  if (hook == AppletHookType_OnFocusState || hook == AppletHookType_OnResume) {
    static_cast<SwitchMotionProvider*>(param)->RestartRequested.store(true, std::memory_order_relaxed);
  }
}

void RequestHandles(SwitchMotionProvider& provider) {
  if (provider.HandlesRequested) return;
  provider.HandlesRequested = true;
  // Computed by libnx without IPC; fails only for unsupported styles.
  provider.HandheldReady = R_SUCCEEDED(hidGetSixAxisSensorHandles(
      &provider.Handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld));
  provider.FullKeyReady = R_SUCCEEDED(hidGetSixAxisSensorHandles(
      &provider.FullKey, 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey));
  provider.JoyDualReady = R_SUCCEEDED(hidGetSixAxisSensorHandles(
      provider.JoyDual, 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual));
  appletHook(&provider.Hook, &OnAppletHook, &provider);
}

MotionSource SelectSource(const SwitchMotionProvider& provider) {
  HidNpadCommonState state{};
  const u32 player1 = hidGetNpadStyleSet(HidNpadIdType_No1);
  if (provider.FullKeyReady && (player1 & HidNpadStyleTag_NpadFullKey) != 0 &&
      hidGetNpadStatesFullKey(HidNpadIdType_No1, &state, 1) != 0 &&
      (state.attributes & HidNpadAttribute_IsConnected) != 0) {
    return MotionSource::FullKey;
  }
  if (provider.JoyDualReady && (player1 & HidNpadStyleTag_NpadJoyDual) != 0 &&
      hidGetNpadStatesJoyDual(HidNpadIdType_No1, &state, 1) != 0) {
    if ((state.attributes & HidNpadAttribute_IsRightConnected) != 0) return MotionSource::JoyDualRight;
    if ((state.attributes & HidNpadAttribute_IsLeftConnected) != 0) return MotionSource::JoyDualLeft;
  }
  if (provider.HandheldReady &&
      (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) != 0 &&
      hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &state, 1) != 0 &&
      (state.attributes & HidNpadAttribute_IsConnected) != 0) {
    return MotionSource::Handheld;
  }
  return MotionSource::None;
}

HidSixAxisSensorHandle HandleFor(const SwitchMotionProvider& provider, MotionSource source) {
  switch (source) {
  case MotionSource::FullKey: return provider.FullKey;
  case MotionSource::JoyDualLeft: return provider.JoyDual[0];
  case MotionSource::JoyDualRight: return provider.JoyDual[1];
  case MotionSource::Handheld: return provider.Handheld;
  case MotionSource::None: break;
  }
  return {};
}

void Stop(SwitchMotionProvider& provider) {
  if (provider.Started == MotionSource::None) return;
  hidStopSixAxisSensor(HandleFor(provider, provider.Started));
  provider.Started = MotionSource::None;
}

// A failed start is retried by the stall check of PollSwitchMotion.
void Start(SwitchMotionProvider& provider, MotionSource source, std::uint64_t now) {
  if (source == MotionSource::None) return;
  hidStartSixAxisSensor(HandleFor(provider, source));
  provider.Started = source;
  provider.StartedMicroseconds = now;
  provider.Accumulator.Reset(now);
}

} // namespace

bool SwitchMotionSensorConnected() noexcept {
  auto& provider = Provider();
  RequestHandles(provider);
  return SelectSource(provider) != MotionSource::None;
}

SwitchMotionPoll PollSwitchMotion(bool wanted, bool consume) noexcept {
  auto& provider = Provider();
  RequestHandles(provider);
  SwitchMotionPoll result;
  const std::uint64_t now = NowMicroseconds();
  const bool inFocus = appletGetFocusState() == AppletFocusState_InFocus;
  // Out of focus (HOME menu, sleep, the controller applet) the sensor is
  // stopped; it starts again on the first poll back in focus.
  const MotionSource target = wanted && inFocus ? SelectSource(provider) : MotionSource::None;
  const bool restart = inFocus && provider.RestartRequested.exchange(false, std::memory_order_relaxed);
  if (target != provider.Started || (restart && target != MotionSource::None)) {
    Stop(provider);
    Start(provider, target, now);
    result.SourceChanged = true;
    provider.HadData = false;
  }
  if (provider.Started == MotionSource::None) return result;

  HidSixAxisSensorState states[17];
  const size_t count = hidGetSixAxisSensorStates(HandleFor(provider, provider.Started), states,
                                                 sizeof(states) / sizeof(states[0]));
  std::array<SwitchSixAxisSample, 17> samples{};
  for (size_t index = 0; index < count && index < samples.size(); ++index) {
    const auto& state = states[index];
    samples[index] = {state.sampling_number,
                      state.delta_time,
                      {state.acceleration.x, state.acceleration.y, state.acceleration.z},
                      {state.angular_velocity.x, state.angular_velocity.y, state.angular_velocity.z},
                      (state.attributes & HidSixAxisSensorAttribute_IsConnected) != 0};
  }
  provider.Accumulator.Observe(std::span(samples.data(), std::min(count, samples.size())), now);
  result.Motion = provider.Accumulator.Take(consume, now);
  if (result.Motion.GyroscopeValid) provider.HadData = true;
  // Data that stopped arriving (a sensor that did not survive a mode change
  // or a resume without a focus message): start it again. Only the first
  // restart after real data resets the caller's pose; retries of a sensor
  // that never answers do not.
  if (provider.Accumulator.Stale(now) && now - provider.StartedMicroseconds > kRestartIntervalMicroseconds) {
    const MotionSource source = provider.Started;
    Stop(provider);
    Start(provider, source, now);
    result.SourceChanged = provider.HadData;
    provider.HadData = false;
  }
  return result;
}
#endif

} // namespace Oot3dNativeGame
