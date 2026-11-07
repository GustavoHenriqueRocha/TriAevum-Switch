#include "oot3d_fps_counter.h"
#include "oot3d_switch_gpu_performance.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <cstring>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

bool Same(const char *a, const char *b) {
  if (std::strcmp(a, b) == 0) return true;
  std::fprintf(stderr, "  got \"%s\", expected \"%s\"\n", a, b);
  return false;
}

using namespace Oot3dNativeGame;

void FpsCounterWindows() {
  {
    FpsCounter counter;
    bool closed = false;
    for (int frame = 0; frame <= 30; ++frame) closed |= counter.Observe(frame / 30.0);
    Require(closed, "0.5 s window closes at 30 FPS");
    Require(Near(counter.Fps(), 30.0, 0.01), "exactly 30 FPS");
    Require(Near(counter.WorstFrameMs(), 33.33, 0.01), "worst frame 33.3 ms");
  }
  {
    // 60 FPS with a 50 ms hitch: the average barely changes, the worst frame shows it.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now);
    for (int frame = 0; frame < 40; ++frame) {
      now += frame == 20 ? 0.050 : 1.0 / 60.0;
      counter.Observe(now);
    }
    Require(counter.Fps() > 55.0 && counter.Fps() < 60.0, "average with a hitch");
    Require(Near(counter.WorstFrameMs(), 50.0, 0.01), "50 ms hitch shows up");
  }
  {
    // The next window restarts the worst frame.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now);
    now += 0.2; counter.Observe(now);
    for (int frame = 0; frame < 60; ++frame) { now += 1.0 / 60.0; counter.Observe(now); }
    Require(Near(counter.WorstFrameMs(), 16.67, 0.01), "worst frame resets each window");
  }
}

void StockTable() {
  // Every row of switchbrew's PTM_services#PerformanceConfiguration table.
  struct Row {
    uint32_t Id;
    int Cpu, Gpu, Mem;
  };
  constexpr Row kRows[] = {
      {0x00010000U, 1020, 384, 1600}, {0x00010001U, 1020, 768, 1600},
      {0x00010002U, 1224, 691, 1600}, {0x00020000U, 1020, 230, 1600},
      {0x00020001U, 1020, 307, 1600}, {0x00020002U, 1224, 230, 1600},
      {0x00020003U, 1020, 307, 1331}, {0x00020004U, 1020, 384, 1331},
      {0x00020005U, 1020, 307, 1066}, {0x00020006U, 1020, 384, 1066},
      {0x92220007U, 1020, 461, 1600}, {0x92220008U, 1020, 461, 1331},
      {0x92220009U, 1785, 77, 1600},  {0x9222000AU, 1785, 77, 1331},
      {0x9222000BU, 1020, 77, 1600},  {0x9222000CU, 1020, 77, 1331},
  };
  Require(std::size(kStockPerformanceConfigurations) == std::size(kRows), "16 configurations");
  for (const Row &row : kRows) {
    const SwitchClockReading reading = StockClockReading(row.Id);
    char message[64];
    std::snprintf(message, sizeof(message), "configuration 0x%08X", static_cast<unsigned>(row.Id));
    Require(reading.Source == SwitchClockSource::Stock && reading.PerformanceConfiguration == row.Id &&
                reading.CpuMhz == row.Cpu && reading.GpuMhz == row.Gpu && reading.MemMhz == row.Mem,
            message);
  }
  for (size_t a = 0; a < std::size(kStockPerformanceConfigurations); ++a)
    for (size_t b = a + 1; b < std::size(kStockPerformanceConfigurations); ++b)
      Require(kStockPerformanceConfigurations[a].Id != kStockPerformanceConfigurations[b].Id,
              "configuration IDs are unique");
  // Exact kHz behind the rounded MHz.
  Require(kStockPerformanceConfigurations[6].GpuKhz == 307200U &&
              kStockPerformanceConfigurations[6].MemKhz == 1331200U,
          "0x20003 is 307.2 / 1331.2");
  // The handheld default (switchbrew: "default value is 0x00020003").
  static_assert(StockClockReading(0x00020003U).GpuMhz == 307);

  const SwitchClockReading unknown = StockClockReading(0x92220010U);
  Require(unknown.Source == SwitchClockSource::Stock && unknown.CpuMhz == -1 &&
              unknown.GpuMhz == -1 && unknown.MemMhz == -1 &&
              unknown.PerformanceConfiguration == 0x92220010U,
          "unknown configuration keeps its ID and -1 MHz");
  Require(StockClockReading(0U).CpuMhz == -1, "0 (Invalid) is not a configuration");

  Require(Same(SwitchClockSourceName(SwitchClockSource::SysClkReal), "sysclk_real") &&
              Same(SwitchClockSourceName(SwitchClockSource::SysClkTarget), "sysclk") &&
              Same(SwitchClockSourceName(SwitchClockSource::Stock), "stock") &&
              Same(SwitchClockSourceName(SwitchClockSource::None), "none"),
          "perf.txt clk_src names");
}

void Lines() {
  char text[64];
  FormatPerfOverlayFrameLine(text, sizeof(text), 59.94, 18.24, 1.0);
  Require(Same(text, "59.9 FPS  max 18.2 ms  speed 100%"), "frame line");
  FormatPerfOverlayFrameLine(text, sizeof(text), 30.0, 33.3, 0.874);
  Require(Same(text, "30.0 FPS  max 33.3 ms  speed 87%"), "frame line, slowed game");
  FormatPerfOverlayFrameLine(text, sizeof(text), 0.0, 0.0, -1.0);
  Require(Same(text, "0.0 FPS  max 0.0 ms  speed --"), "frame line, no refresh owed");

  SwitchClockReading sysClk;
  sysClk.CpuMhz = 1785;
  sysClk.GpuMhz = 768;
  sysClk.MemMhz = 1600;
  sysClk.Source = SwitchClockSource::SysClkReal;
  FormatPerfOverlayClockLine(text, sizeof(text), sysClk);
  Require(Same(text, "CPU 1785  GPU 768  MEM 1600  sys-clk"), "sys-clk line");
  sysClk.Source = SwitchClockSource::SysClkTarget;
  sysClk.GpuMhz = -1;
  FormatPerfOverlayClockLine(text, sizeof(text), sysClk);
  Require(Same(text, "CPU 1785  GPU --  MEM 1600  sys-clk"), "sys-clk target, one clock unread");

  FormatPerfOverlayClockLine(text, sizeof(text), StockClockReading(0x00020003U));
  Require(Same(text, "CPU 1020  GPU 307  MEM 1331  stock"), "stock line");
  FormatPerfOverlayClockLine(text, sizeof(text), StockClockReading(0x92220010U));
  Require(Same(text, "CPU --  GPU --  MEM --  stock 0x92220010"), "unknown configuration line");
  FormatPerfOverlayClockLine(text, sizeof(text), SwitchClockReading{});
  Require(Same(text, "CPU --  GPU --  MEM --"), "nothing read");

  // The longest lines fit their buffers (no truncation).
  sysClk = SwitchClockReading{2397, 1267, 2131, SwitchClockSource::SysClkReal, 0U};
  FormatPerfOverlayClockLine(text, sizeof(text), sysClk);
  Require(Same(text, "CPU 2397  GPU 1267  MEM 2131  sys-clk"), "widest clock line");
}

void Overlay() {
  {
    PerfOverlay overlay;
    Require(Same(overlay.FrameLine(), "-- FPS"), "frame line before the 1st window");
    Require(Same(overlay.ClockLine(), "CPU --  GPU --  MEM --"), "clock line before a reading");
    // 30 FPS host frames, each owing 2 refreshes; one frame drops one.
    bool closed = false;
    for (int frame = 0; frame <= 15; ++frame) {
      if (frame > 0) overlay.ObserveRefreshes(2U, 2U, frame == 10 ? 1U : 0U);
      closed |= overlay.ObserveFrame(frame / 30.0);
    }
    Require(closed, "window closed");
    // 30 run of 31 owed = 96.8%.
    Require(Same(overlay.FrameLine(), "30.0 FPS  max 33.3 ms  speed 97%"), "speed from drops");
    overlay.SetClocks(StockClockReading(0x92220008U));
    Require(Same(overlay.ClockLine(), "CPU 1020  GPU 461  MEM 1331  stock"), "clocks set");
  }
  {
    // A refresh that couldn't run (the loop stopped early) counts as lost;
    // the next window starts its count over.
    PerfOverlay overlay;
    overlay.ObserveFrame(0.0);
    for (int frame = 1; frame <= 30; ++frame) {
      overlay.ObserveRefreshes(frame <= 15 ? 1U : 2U, 2U, 0U);
      overlay.ObserveFrame(frame / 60.0);
    }
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed 75%"), "half the frames ran one of two");
    for (int frame = 31; frame <= 60; ++frame) {
      overlay.ObserveRefreshes(1U, 1U, 0U);
      overlay.ObserveFrame(frame / 60.0);
    }
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed 100%"), "count resets per window");
  }
  {
    // Frames that owed nothing (preparation screen, interpolated frames only).
    PerfOverlay overlay;
    for (int frame = 0; frame <= 30; ++frame) overlay.ObserveFrame(frame / 60.0);
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed --"), "no refresh owed");
  }
  Require(!PerfOverlayEnabled().load(), "overlay off until the Video tab loads config/fps_counter");
}

// L3+R3 > Video > GPU clock: the configurations are the table's 460.8 and
// 307.2 MHz handheld ones, Standard restores what the session reported, and
// the file round-trips.
void GpuPerformance() {
  const SwitchClockReading high = StockClockReading(kGpuHighHandheldConfiguration);
  Require(high.GpuMhz == 461 && high.CpuMhz == 1020 && high.MemMhz == 1331, "High = 1020/460.8/1331.2");
  const SwitchClockReading standard = StockClockReading(kGpuStandardHandheldConfiguration);
  Require(standard.GpuMhz == 307 && standard.CpuMhz == 1020 && standard.MemMhz == 1331,
          "Standard = 1020/307.2/1331.2");
  static_assert(GpuStandardConfiguration(0U) == kGpuStandardHandheldConfiguration);
  static_assert(GpuStandardConfiguration(kGpuHighHandheldConfiguration) == kGpuStandardHandheldConfiguration);
  static_assert(GpuStandardConfiguration(0x00020005U) == 0x00020005U);
  Require(!ApplyGpuPerformance(true).Attempted, "no apm off the Switch");

  const auto dir = std::filesystem::temp_directory_path() / "oot3d_gpu_performance_test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  Require(!ReadGpuPerformanceHigh(dir), "absent = Standard");
  Require(SaveGpuPerformanceHigh(dir, true) && ReadGpuPerformanceHigh(dir), "High saved");
  Require(SaveGpuPerformanceHigh(dir, false) && !ReadGpuPerformanceHigh(dir), "Standard removes the file");
  Require(SaveGpuPerformanceHigh(dir, false), "removing an absent file is fine");
  std::filesystem::remove_all(dir, error);
}

} // namespace

int main() {
  FpsCounterWindows();
  StockTable();
  Lines();
  Overlay();
  GpuPerformance();
  std::puts("perf overlay: PASS");
  return 0;
}
