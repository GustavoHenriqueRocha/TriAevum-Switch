#pragma once

// L3+R3 > Video > "GPU clock" (config/gpu_performance, present = High).
//
// High asks HOS, through the application's apm session, for the official
// handheld performance configuration 0x92220008: CPU 1020 MHz, GPU 460.8 MHz,
// memory 1331.2 MHz (the default handheld one, 0x00020003, has GPU 307.2 MHz).
// Only the handheld configuration (ApmPerformanceMode_Normal) is touched:
// docked already runs the GPU at 768 MHz. Standard puts back the handheld
// configuration the session reported before the first change. The app never
// opens clkrst/pcv (oot3d_switch_clocks.h); a running sys-clk overrides
// whatever HOS applies.
//
// The file helpers and the choice of the configuration to restore are plain
// code, tested on the host (oot3d_fps_counter_tests).
//
// Reference, no code copied: choosing 0x92220008 through apm follows the
// findings in masseffect-nx's sdk/src/ui/switch_apm.cpp and
// docs/platform-notes.md (https://github.com/nebadasSwifty/masseffect-nx;
// BSD-3-Clause as part of its SDK, the docs GPL-3.0), also used by usf4-nx
// and nfsmw-nx.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr const char *kGpuPerformanceConfigName = "gpu_performance";
// switchbrew PTM services, PerformanceConfiguration (oot3d_switch_clocks.h).
inline constexpr uint32_t kGpuHighHandheldConfiguration = 0x92220008U;
inline constexpr uint32_t kGpuStandardHandheldConfiguration = 0x00020003U;

// The handheld configuration Standard restores: what the session reported
// before the first change, unless that was nothing (0) or High itself (a
// previous run's choice that HOS kept), then the HOS handheld default.
constexpr uint32_t GpuStandardConfiguration(uint32_t reportedBeforeChange) {
  return reportedBeforeChange == 0U || reportedBeforeChange == kGpuHighHandheldConfiguration
             ? kGpuStandardHandheldConfiguration
             : reportedBeforeChange;
}

inline bool ReadGpuPerformanceHigh(const std::filesystem::path &config) {
  std::error_code error;
  return std::filesystem::exists(config / kGpuPerformanceConfigName, error);
}

// Creates (High) or removes (Standard) the file; false if that failed.
inline bool SaveGpuPerformanceHigh(const std::filesystem::path &config, bool high) {
  std::error_code error;
  if (!high) {
    std::filesystem::remove(config / kGpuPerformanceConfigName, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / kGpuPerformanceConfigName, std::ios::trunc);
  return static_cast<bool>(file);
}

struct GpuPerformanceStatus {
  bool Attempted = false;  // an apm request was made
  bool Available = false;  // apm opened and accepted the configuration
  uint32_t Result = 0;     // libnx Result of the failing step (0 = success)
  const char *FailedStep = "";  // "apmInitialize" / "apmSetPerformanceConfiguration"
  uint32_t Requested = 0;  // handheld configuration asked for
  uint32_t Effective = 0;  // appletGetCurrentPerformanceConfiguration afterwards (0 = unread)
};

#if defined(__SWITCH__)
// Applies High or Standard (oot3d_switch_clocks.cpp). Opens the apm session
// on the first call and keeps it for the whole run; logs one
// "TRIAEVUM_GPU_PERF" line with the result codes and the configuration in
// effect. Standard before any High is a no-op (no apm session opened).
GpuPerformanceStatus ApplyGpuPerformance(bool high);
#else
inline GpuPerformanceStatus ApplyGpuPerformance(bool) { return {}; }
#endif

} // namespace Oot3dNativeGame
