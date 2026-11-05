#pragma once

// The console's own motion sensors as the controller motion of
// PollNativeA32Input (oot3d_switch_motion.cpp).
//
// devkitPro's SDL2 Switch driver has no sensor support, so on the Switch the
// SDL path never reports a gyro. This module reads libnx's six-axis sensor of
// the device behind SDL pad 0 (player 1 or handheld) and converts it to the
// 3DS frame and units that host.ControllerMotion carries (ConvertSdlMotion's
// output). Everything downstream (calibration, aim.source, sensitivity,
// inversion, the guest HID ring) then treats it like any SDL gyro.
//
// Sources, in order of preference:
//   - player 1 Pro Controller (FullKey);
//   - player 1 dual Joy-Con pair: the right Joy-Con (the aiming hand), else
//     the left one if only it is connected;
//   - Handheld (the Switch Lite, or Joy-Cons attached to the console).
// Player 1 comes first so a Pro Controller paired to a Lite wins over the
// Lite's built-in sensor. A single sideways Joy-Con is not supported: the
// frame of its processed six-axis state is undocumented.
//
// The conversion and the averaging are pure and tested on the host
// (oot3d_switch_motion_tests.cpp); only the libnx part is Switch-only.

#include "three_ds_input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

// HidSixAxisSensorState as libnx reports it, in the Switch frame: +X right,
// +Y toward the top edge, +Z out of the screen. Acceleration in G pointing
// along gravity (face up at rest reads (0, 0, -1)); angular velocity in
// revolutions per second, counter-clockwise positive (1.0 = 360 deg/s).
struct SwitchSixAxisSample {
  std::uint64_t SamplingNumber = 0;
  std::uint64_t DeltaTimeNanoseconds = 0;
  std::array<float, 3> Acceleration{};
  std::array<float, 3> AngularVelocity{};
  bool Connected = false;
};

// Switch frame -> 3DS frame (cross-checked against yuzu, Ryujinx, Akira and
// borealis; equal to Switch -> SDL -> ConvertSdlMotion):
//   accelerometer [G]     = (-a.x, +a.z, +a.y)
//   gyroscope     [deg/s] = 360 * (-w.x, +w.z, +w.y)
// Both are the same proper rotation, so a physical rotation stays coherent
// between the two sensors. Timestamps are left at zero.
[[nodiscard]] ThreeDsRecomp::Input::MotionObservation ConvertSwitchSixAxis(
    const std::array<float, 3>& accelerationG,
    const std::array<float, 3>& angularVelocityRevolutionsPerSecond) noexcept;

// Turns the six-axis LIFO (about 200 Hz, 17 entries) into one observation per
// poll. The host polls once per presented frame and the guest consumes input
// only on some polls, while its gyro ring repeats one value for every tick of
// a refresh; the latest sample alone would alias, so this reports the
// time-weighted mean of every sample since the last consumed poll.
class SwitchMotionAccumulator {
 public:
  // Sensor data older than this is reported as unavailable (a stopped sensor
  // leaves its last state in the LIFO).
  static constexpr std::uint64_t StaleMicroseconds = 200'000;
  // Used when a sample carries no delta time.
  static constexpr std::uint64_t DefaultDeltaNanoseconds = 5'000'000;

  // Forgets every sample; the next Observe only takes the newest entry.
  void Reset(std::uint64_t nowMicroseconds) noexcept;

  // `newestFirst` is the LIFO as hidGetSixAxisSensorStates returns it.
  // Entries with a sampling number above the last one seen are added, oldest
  // first. Returns how many were added.
  std::size_t Observe(std::span<const SwitchSixAxisSample> newestFirst,
                      std::uint64_t nowMicroseconds) noexcept;

  // This poll's motion: the mean since the last consumed poll, or the last
  // reported value when nothing arrived since then. The timestamp only
  // advances with new samples, so calibration counts each window once.
  // Invalid when the newest sample is stale. `consume` starts a new window.
  [[nodiscard]] ThreeDsRecomp::Input::MotionObservation Take(
      bool consume, std::uint64_t nowMicroseconds) noexcept;

  [[nodiscard]] bool Stale(std::uint64_t nowMicroseconds) const noexcept {
    return nowMicroseconds - mLastSampleMicroseconds > StaleMicroseconds;
  }

 private:
  std::array<double, 3> mAccelerationSum{};
  std::array<double, 3> mAngularVelocitySum{};
  double mSeconds = 0.0;
  std::uint64_t mLastSamplingNumber = 0;
  bool mHasSamplingNumber = false;
  std::uint64_t mLastSampleMicroseconds = 0;
  std::uint64_t mWindowMicroseconds = 0;
  ThreeDsRecomp::Input::MotionObservation mLast;
};

#if defined(__SWITCH__)
struct SwitchMotionPoll {
  ThreeDsRecomp::Input::MotionObservation Motion;
  // The sensor in use changed (or restarted): the caller resets state that
  // assumed a continuous pose (VirtualMotionState::ResetController).
  bool SourceChanged = false;
};

// True when player 1 or handheld has a supported six-axis sensor connected.
// Shared-memory reads only.
[[nodiscard]] bool SwitchMotionSensorConnected() noexcept;

// Game thread only. `wanted` false stops the sensor (no IMU streaming while
// nothing uses it). Starts and stops the sensor on device changes, restarts
// it after focus regain or resume and when its data stalls, and reads the
// LIFO every call. `consume` mirrors the guest consuming this poll's input.
[[nodiscard]] SwitchMotionPoll PollSwitchMotion(bool wanted, bool consume) noexcept;
#endif

} // namespace Oot3dNativeGame
