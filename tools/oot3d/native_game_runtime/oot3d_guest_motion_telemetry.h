#pragma once

// The guest's side of gyro aiming, for the game log's "motion:" line
// (SwitchMotionLog, oot3d_switch_motion.h): whether the game has the gyro
// enabled (hid:USER EnableGyroscopeLow minus DisableGyroscopeLow) and how many
// samples the CTR host wrote to its HID shared-memory gyro ring, and how many
// of them were not all zero. Written by NativeA32CtrHostServices
// (oot3d_native_a32_ctr_host_hid.cpp), read by the input poll
// (oot3d_native_a32_window_input.cpp). Relaxed atomics: counters for a log.

#include <atomic>
#include <cstdint>

namespace Oot3dNativeGame {

struct GuestMotionTelemetry {
  std::atomic<bool> GyroscopeEnabled{false};
  std::atomic<std::uint64_t> GyroscopeSamples{0};
  std::atomic<std::uint64_t> NonzeroGyroscopeSamples{0};
};

// The process's telemetry (one instance; never destroyed).
inline GuestMotionTelemetry &GuestMotion() noexcept {
  static GuestMotionTelemetry *telemetry = new GuestMotionTelemetry();
  return *telemetry;
}

} // namespace Oot3dNativeGame
