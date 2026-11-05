#pragma once

// The console's current clocks for logs/perf.txt and the performance overlay
// (oot3d_switch_clocks.cpp).
//
// Read-only: the MHz come from sys-clk's public IPC ("sys:clk",
// GetCurrentContext); without sys-clk they come from the performance
// configuration HOS has applied (AM ICommonStateGetter
// GetCurrentPerformanceConfiguration, mapped by the table below: the stock
// clocks, not a measurement). The handheld/dock mode and the performance mode
// come from the applet (values libnx already keeps, no IPC). The app never
// opens clkrst nor pcv and never changes clocks (SwitchWakerHD issue #33:
// clkrst in the app froze the console on wake from sleep; sys-clk is what
// changes clocks).
//
// Off the Switch the readings are empty; the table and the reading type are
// plain data, so host tests use them.

#include <cstdint>
#include <string>

namespace Oot3dNativeGame {

enum class SwitchClockSource : uint8_t {
  None,         // nothing read yet, or neither sys-clk nor the configuration answered
  SysClkReal,   // sys-clk realFreqs (measured, API >= 2)
  SysClkTarget, // sys-clk freqs (the clocks it asked for)
  Stock,        // the HOS performance configuration, through the table below
};

// perf.txt `clk_src=` names.
constexpr const char *SwitchClockSourceName(SwitchClockSource source) {
  switch (source) {
  case SwitchClockSource::SysClkReal: return "sysclk_real";
  case SwitchClockSource::SysClkTarget: return "sysclk";
  case SwitchClockSource::Stock: return "stock";
  case SwitchClockSource::None: break;
  }
  return "none";
}

// A MHz that couldn't be read is -1. PerformanceConfiguration is the raw ID
// for SwitchClockSource::Stock (an ID missing from the table keeps -1 MHz).
struct SwitchClockReading {
  int CpuMhz = -1;
  int GpuMhz = -1;
  int MemMhz = -1;
  SwitchClockSource Source = SwitchClockSource::None;
  uint32_t PerformanceConfiguration = 0;

  bool operator==(const SwitchClockReading &) const = default;
};

// Performance configuration -> clocks, from switchbrew's PTM services page
// (https://switchbrew.org/wiki/PTM_services#PerformanceConfiguration), in kHz
// so 307.2 and 1331.2 stay exact. 0x10001 and 0x20000 exist only docked,
// 0x10002 and 0x20002 only on SDEV units; 0x92220009-C are 7.0.0+ (also the
// minimum firmware of GetCurrentPerformanceConfiguration).
struct StockPerformanceConfiguration {
  uint32_t Id;
  uint32_t CpuKhz;
  uint32_t GpuKhz;
  uint32_t MemKhz;
};

inline constexpr StockPerformanceConfiguration kStockPerformanceConfigurations[] = {
    {0x00010000U, 1020000U, 384000U, 1600000U}, {0x00010001U, 1020000U, 768000U, 1600000U},
    {0x00010002U, 1224000U, 691200U, 1600000U}, {0x00020000U, 1020000U, 230400U, 1600000U},
    {0x00020001U, 1020000U, 307200U, 1600000U}, {0x00020002U, 1224000U, 230400U, 1600000U},
    {0x00020003U, 1020000U, 307200U, 1331200U}, {0x00020004U, 1020000U, 384000U, 1331200U},
    {0x00020005U, 1020000U, 307200U, 1065600U}, {0x00020006U, 1020000U, 384000U, 1065600U},
    {0x92220007U, 1020000U, 460800U, 1600000U}, {0x92220008U, 1020000U, 460800U, 1331200U},
    {0x92220009U, 1785000U, 76800U, 1600000U},  {0x9222000AU, 1785000U, 76800U, 1331200U},
    {0x9222000BU, 1020000U, 76800U, 1600000U},  {0x9222000CU, 1020000U, 76800U, 1331200U},
};

// A Stock reading for `id`, MHz rounded to the nearest (307.2 -> 307,
// 1065.6 -> 1066); an unknown ID keeps -1 MHz and its raw value.
constexpr SwitchClockReading StockClockReading(uint32_t id) {
  SwitchClockReading reading;
  reading.Source = SwitchClockSource::Stock;
  reading.PerformanceConfiguration = id;
  for (const auto &entry : kStockPerformanceConfigurations) {
    if (entry.Id == id) {
      reading.CpuMhz = static_cast<int>((entry.CpuKhz + 500U) / 1000U);
      reading.GpuMhz = static_cast<int>((entry.GpuKhz + 500U) / 1000U);
      reading.MemMhz = static_cast<int>((entry.MemKhz + 500U) / 1000U);
      break;
    }
  }
  return reading;
}

#if defined(__SWITCH__)
// The latest reading. Like SwitchClockPerfSummary it only copies the cache and
// requests the next read from the reader thread: the game thread never waits
// on an IPC.
SwitchClockReading SwitchClockLatest();

// " clocks: cpu_mhz=1785 gpu_mhz=768 mem_mhz=1600 clk_src=sysclk_real
// op_mode=handheld perf_mode=normal sysclk_profile=handheld sysclk_enabled=1
// sysclk_api=4 clk_ipc_us=40 clk_busy=0" for the window line (without sys-clk:
// "clk_src=stock perf_cfg=0x00020003" and the configuration's MHz). Returns the
// previous reading and requests the next one from its own thread (the IPC never runs on the
// game thread; clk_busy=1 if the previous one hasn't returned yet); the session with
// sys-clk is opened the first time and kept. A field that couldn't be read
// is -1 (clk_src=none when neither source answered).
std::string SwitchClockPerfSummary();
#else
inline SwitchClockReading SwitchClockLatest() { return {}; }
inline std::string SwitchClockPerfSummary() { return {}; }
#endif

} // namespace Oot3dNativeGame
