#pragma once

// L3+R3 > Video > Performance mode, CPU clock, GPU clock, Memory clock
// (config/cpu_performance, config/gpu_performance, config/memory_performance;
// each file present = High, all absent by default). The mode is derived from
// the three: On = all High, Off = all Standard, otherwise Custom.
//
// GPU and memory: one official handheld performance configuration through
// the application's apm session (switchbrew PTM services,
// PerformanceConfiguration): 0x00020003 1020/307.2/1331.2 (the handheld
// default), 0x00020001 1020/307.2/1600, 0x92220008 1020/460.8/1331.2,
// 0x92220007 1020/460.8/1600 (CPU/GPU/memory MHz). Only the handheld
// configuration (ApmPerformanceMode_Normal) is touched: docked already runs
// 768 / 1600 MHz.
//
// CPU: no official configuration has CPU 1224 MHz outside SDEV units
// (0x00010002 and 0x00020002), so High is a direct clock request: clkrst on
// 8.0.0+ (PcvModuleId_CpuBus), pcv before. HOS sets the CPU again whenever
// it applies a configuration (focus, sleep and wake, dock, apm changes, ours
// too): CpuClockKeeper below decides when to set it again, when to put the
// earlier rate back (Standard, HOME, sleep, exit) and when another tool
// (sys-clk) keeps changing it, so the game stops fighting it.
//
// Everything here is plain code, tested on the host
// (oot3d_switch_performance_tests); the console side is
// oot3d_switch_performance.cpp.
//
// Reference, no code copied: choosing 0x92220008 through apm follows the
// findings in masseffect-nx's sdk/src/ui/switch_apm.cpp and
// docs/platform-notes.md (https://github.com/nebadasSwifty/masseffect-nx;
// BSD-3-Clause as part of its SDK, the docs GPL-3.0), also used by usf4-nx
// and nfsmw-nx. The CPU request follows what sys-clk (retronx-team/sys-clk,
// board.cpp) and RetroArch (frontend/drivers/platform_switch.c) do, findings
// only; SwitchWakerHD's issue #33 and its commit 8d9a643e for what to avoid.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace Oot3dNativeGame {

inline constexpr const char *kCpuPerformanceConfigName = "cpu_performance";
inline constexpr const char *kGpuPerformanceConfigName = "gpu_performance";
inline constexpr const char *kMemoryPerformanceConfigName = "memory_performance";

// Official handheld configurations (oot3d_switch_clocks.h has the table).
inline constexpr uint32_t kHandheldStandardConfiguration = 0x00020003U;  // 1020 / 307.2 / 1331.2
inline constexpr uint32_t kHandheldMemoryHighConfiguration = 0x00020001U;  // 1020 / 307.2 / 1600
inline constexpr uint32_t kHandheldGpuHighConfiguration = 0x92220008U;  // 1020 / 460.8 / 1331.2
inline constexpr uint32_t kHandheldGpuMemoryHighConfiguration = 0x92220007U;  // 1020 / 460.8 / 1600

// The only CPU rates the game asks for.
inline constexpr uint32_t kCpuStandardHz = 1020000000U;
inline constexpr uint32_t kCpuHighHz = 1224000000U;

struct PerformanceChoice {
  bool Cpu = false;
  bool Gpu = false;
  bool Memory = false;

  bool operator==(const PerformanceChoice &) const = default;
};

enum class PerformanceMode : uint8_t { Off, On, Custom };

constexpr PerformanceMode DerivePerformanceMode(PerformanceChoice choice) {
  if (choice.Cpu && choice.Gpu && choice.Memory) return PerformanceMode::On;
  if (!choice.Cpu && !choice.Gpu && !choice.Memory) return PerformanceMode::Off;
  return PerformanceMode::Custom;
}

constexpr const char *PerformanceModeName(PerformanceMode mode) {
  switch (mode) {
  case PerformanceMode::On: return "On";
  case PerformanceMode::Custom: return "Custom";
  case PerformanceMode::Off: break;
  }
  return "Off";
}

// The configurations the game itself may request besides the default.
constexpr bool IsOwnHandheldConfiguration(uint32_t id) {
  return id == kHandheldMemoryHighConfiguration || id == kHandheldGpuHighConfiguration ||
         id == kHandheldGpuMemoryHighConfiguration;
}

// The handheld configuration GPU and memory Standard restore: what the
// session reported before the first change, unless that was nothing (0) or
// one of the game's own (a previous run's choice that HOS kept), then the
// HOS handheld default.
constexpr uint32_t HandheldStandardConfiguration(uint32_t reportedBeforeChange) {
  return reportedBeforeChange == 0U || IsOwnHandheldConfiguration(reportedBeforeChange)
             ? kHandheldStandardConfiguration
             : reportedBeforeChange;
}

// GPU x memory -> the handheld configuration to request; `standard` is
// HandheldStandardConfiguration of what was reported before.
constexpr uint32_t HandheldConfiguration(bool gpuHigh, bool memoryHigh, uint32_t standard) {
  if (gpuHigh) return memoryHigh ? kHandheldGpuMemoryHighConfiguration : kHandheldGpuHighConfiguration;
  return memoryHigh ? kHandheldMemoryHighConfiguration : standard;
}

// The CPU rate Standard, HOME, sleep and exit put back: the one read before
// the first change, unless that read failed (0) or found High itself (a
// previous run that ended without restoring), then the stock 1020 MHz of
// every official handheld and docked configuration.
constexpr uint32_t CpuRestoreRate(uint32_t readBeforeChange) {
  return readBeforeChange == 0U || readBeforeChange == kCpuHighHz ? kCpuStandardHz : readBeforeChange;
}

inline bool ReadPerformanceFlag(const std::filesystem::path &config, const char *name) {
  std::error_code error;
  return std::filesystem::exists(config / name, error);
}

// Creates (High) or removes (Standard) the file; false if that failed.
inline bool SavePerformanceFlag(const std::filesystem::path &config, const char *name, bool high) {
  std::error_code error;
  if (!high) {
    std::filesystem::remove(config / name, error);
    return !error;
  }
  std::filesystem::create_directories(config, error);
  std::ofstream file(config / name, std::ios::trunc);
  return static_cast<bool>(file);
}

inline PerformanceChoice ReadPerformanceChoice(const std::filesystem::path &config) {
  return {ReadPerformanceFlag(config, kCpuPerformanceConfigName),
          ReadPerformanceFlag(config, kGpuPerformanceConfigName),
          ReadPerformanceFlag(config, kMemoryPerformanceConfigName)};
}

// Writes the three files; the name of the first one that failed, or nullptr.
inline const char *SavePerformanceChoice(const std::filesystem::path &config, PerformanceChoice choice) {
  const char *failed = nullptr;
  if (!SavePerformanceFlag(config, kCpuPerformanceConfigName, choice.Cpu)) failed = kCpuPerformanceConfigName;
  if (!SavePerformanceFlag(config, kGpuPerformanceConfigName, choice.Gpu) && failed == nullptr)
    failed = kGpuPerformanceConfigName;
  if (!SavePerformanceFlag(config, kMemoryPerformanceConfigName, choice.Memory) && failed == nullptr)
    failed = kMemoryPerformanceConfigName;
  return failed;
}

// When to set CPU High again and when to put the earlier rate back. The
// caller reads the rate (0 = unread), passes it in and carries out the
// returned step; times are milliseconds of any monotonic clock.
//
// - HOS sets the CPU of the applied configuration (1020 MHz) on focus,
//   sleep and wake, dock and apm changes. After such an event (HosEvent)
//   the caller reads the rate back at kFollowUpMs; finding 1020 then is
//   expected and High is set again.
// - The caller also reads it once per perf.txt window (Check): a rate other
//   than High there, or any rate other than 1020 after an event, is another
//   tool's (sys-clk). High is set again, but after kOverrideStreak such
//   checks in a row the keeper stops (Overridden) until the player chooses
//   CPU High again.
// - Standard, focus loss (HOME, sleep) and exit put back the rate read
//   before the first change (CpuRestoreRate), only if the keeper set High
//   and was not overridden.
// - After kFailureLimit failed requests in a row it stops (Unavailable).
class CpuClockKeeper {
public:
  static constexpr uint64_t kGraceMs = 5000U;
  static constexpr uint32_t kOverrideStreak = 3U;
  static constexpr uint32_t kFailureLimit = 3U;
  static constexpr uint32_t kFollowUpMs[] = {0U, 250U, 1000U, 2000U, 4000U};

  enum class Action : uint8_t {
    None,
    SetHigh,  // request kCpuHighHz
    Restore,  // request Step::Hz
    GiveUp,   // just found overridden: log it, request nothing
  };
  struct Step {
    Action Do = Action::None;
    uint32_t Hz = 0U;

    bool operator==(const Step &) const = default;
  };

  // The player's choice (menu, or the file at start).
  Step Choose(bool high, uint32_t currentHz, uint64_t nowMs) {
    if (high) {
      mWanted = true;
      mOverridden = false;
      mUnavailable = false;
      mFailures = 0U;
      mStreak = 0U;
      if (mPriorHz == 0U) mPriorHz = CpuRestoreRate(currentHz);
      mGraceUntilMs = nowMs + kGraceMs;
      if (!mInFocus) return {};
      mEngaged = true;
      if (currentHz == kCpuHighHz) return {};
      return {Action::SetHigh, kCpuHighHz};
    }
    mWanted = false;
    return Release();
  }

  // HOME or sleep (false) and back in focus (true).
  Step Focus(bool inFocus, uint32_t currentHz, uint64_t nowMs) {
    mInFocus = inFocus;
    if (!inFocus) return Release();
    mGraceUntilMs = nowMs + kGraceMs;
    if (!Active()) return {};
    mEngaged = true;
    if (currentHz == kCpuHighHz) return {};
    ++mReapplies;
    return {Action::SetHigh, kCpuHighHz};
  }

  // HOS applied (or will apply) a configuration: wake, dock change, an apm
  // change of ours or HOS's. The caller then reads back at kFollowUpMs.
  void HosEvent(uint64_t nowMs) { mGraceUntilMs = nowMs + kGraceMs; }

  // A read-back (after an event, or once per perf.txt window).
  Step Check(uint32_t currentHz, uint64_t nowMs) {
    if (!Active() || currentHz == 0U) return {};
    if (currentHz == kCpuHighHz) {
      mStreak = 0U;
      mEngaged = true;
      return {};
    }
    const bool hosRate = currentHz == kCpuStandardHz && nowMs < mGraceUntilMs;
    if (!hosRate) {
      mForeignHz = currentHz;
      if (++mStreak >= kOverrideStreak) {
        mOverridden = true;
        mEngaged = false;
        return {Action::GiveUp, currentHz};
      }
    }
    mEngaged = true;
    ++mReapplies;
    return {Action::SetHigh, kCpuHighHz};
  }

  // Exit (atexit, or HOS asked the game to exit).
  Step Exit() { return Release(); }

  // The result of a SetHigh or Restore the caller carried out.
  void Report(bool succeeded) {
    if (succeeded) {
      mFailures = 0U;
      return;
    }
    if (++mFailures >= kFailureLimit) {
      mUnavailable = true;
      mEngaged = false;
    }
  }
  // clkrst/pcv could not even be opened: no request will work this run.
  void MarkUnavailable() {
    mUnavailable = true;
    mEngaged = false;
  }

  bool Wanted() const { return mWanted; }
  bool InFocus() const { return mInFocus; }
  bool Engaged() const { return mEngaged; }
  bool Overridden() const { return mOverridden; }
  bool Unavailable() const { return mUnavailable; }
  uint32_t Reapplies() const { return mReapplies; }
  uint32_t ForeignHz() const { return mForeignHz; }
  uint32_t PriorHz() const { return mPriorHz; }

private:
  bool Active() const { return mWanted && mInFocus && !mOverridden && !mUnavailable; }

  Step Release() {
    if (!mEngaged) return {};
    mEngaged = false;
    return {Action::Restore, mPriorHz};
  }

  bool mWanted = false;
  bool mInFocus = true;
  bool mEngaged = false;  // High was requested and the earlier rate is owed
  bool mOverridden = false;
  bool mUnavailable = false;
  uint32_t mPriorHz = 0U;
  uint32_t mReapplies = 0U;
  uint32_t mStreak = 0U;
  uint32_t mFailures = 0U;
  uint32_t mForeignHz = 0U;
  uint64_t mGraceUntilMs = 0U;
};

// What the last ApplyPerformance did (the menu's error text) and where the
// CPU request stands now.
struct PerformanceStatus {
  PerformanceChoice Requested;
  // apm (GPU and memory)
  bool ApmAttempted = false;  // an apm request was made
  bool ApmAvailable = false;  // apm opened and accepted the configuration
  uint32_t ApmResult = 0;     // libnx Result of the failing step (0 = success)
  const char *ApmFailedStep = "";  // "apmInitialize" / "apmSetPerformanceConfiguration"
  uint32_t ApmRequested = 0;  // handheld configuration asked for
  uint32_t ApmEffective = 0;  // appletGetCurrentPerformanceConfiguration afterwards (0 = unread)
  // CPU (clkrst / pcv)
  bool CpuUnavailable = false;  // clkrst/pcv refused: CPU High does nothing this run
  bool CpuOverridden = false;   // another tool keeps changing the CPU clock
  uint32_t CpuResult = 0;       // libnx Result of the last failed CPU step (0 = none)
  const char *CpuFailedStep = "";
  uint32_t CpuForeignHz = 0;    // the other tool's rate (CpuOverridden)
  uint32_t CpuReapplies = 0;
};

// The perf.txt fields after `clocks:` (oot3d_switch_clocks.cpp): the settings
// asked for, the CPU re-apply count, whether another tool overrides the CPU
// clock and the last failed CPU step with its libnx Result:
// " req_cpu=1 req_gpu=1 req_mem=1 cpu_reapply=2 cpu_override=0 cpu_err=-".
inline std::string FormatPerformancePerfFields(const PerformanceStatus &status) {
  char text[160];
  char error[64] = "-";
  if (status.CpuFailedStep[0] != '\0')
    std::snprintf(error, sizeof(error), "%s:0x%X", status.CpuFailedStep, static_cast<unsigned>(status.CpuResult));
  std::snprintf(text, sizeof(text), " req_cpu=%d req_gpu=%d req_mem=%d cpu_reapply=%u cpu_override=%d cpu_err=%s",
                status.Requested.Cpu ? 1 : 0, status.Requested.Gpu ? 1 : 0, status.Requested.Memory ? 1 : 0,
                static_cast<unsigned>(status.CpuReapplies), status.CpuOverridden ? 1 : 0, error);
  return text;
}

// The three rates through clkrst (oot3d_switch_performance.cpp), for the
// clock reader thread.
struct PerformanceClockSample {
  bool Read = false;
  uint32_t CpuHz = 0;
  uint32_t GpuHz = 0;
  uint32_t MemHz = 0;
};

#if defined(__SWITCH__)
// Applies the choice at once (oot3d_switch_performance.cpp): the handheld
// configuration through apm when GPU or memory changed (the apm session is
// opened on the first High and kept), then CPU High or the earlier rate.
// Logs TRIAEVUM_PERF_APM / TRIAEVUM_PERF_CPU lines. All Standard before any
// High is a no-op: no apm session, no clkrst, the default focus handling.
PerformanceStatus ApplyPerformance(PerformanceChoice choice);
// The same status with the CPU fields as they are now (the keeper may have
// found an override or failed since).
PerformanceStatus CurrentPerformanceStatus();
// While CPU High is in use: the three rates, each through its own short
// clkrst session, and with `perfWindow` the keeper's read-back (CPU High set
// again if HOS or another tool changed it). Read = false otherwise: the
// reader then uses sys-clk or the configuration, as without CPU High.
// Called on the clock reader thread only, never the game thread.
PerformanceClockSample SamplePerformanceClocks(bool perfWindow);
#else
inline PerformanceStatus ApplyPerformance(PerformanceChoice choice) {
  PerformanceStatus status;
  status.Requested = choice;
  return status;
}
inline PerformanceStatus CurrentPerformanceStatus() { return {}; }
#endif

} // namespace Oot3dNativeGame
