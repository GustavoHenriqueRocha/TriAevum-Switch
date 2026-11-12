#include "oot3d_fps_counter.h"

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
    // A steady 30 FPS setting: a new image every second refresh.
    FpsCounter counter;
    bool closed = false;
    for (int refresh = 0; refresh <= 30; ++refresh)
      closed |= counter.Observe(refresh / 60.0, refresh % 2 == 0 ? 1U : 0U);
    Require(closed, "0.5 s window closes at 30 FPS");
    Require(Near(counter.Fps(), 30.0, 0.01), "30 FPS setting reads 30");
    Require(Near(counter.WorstFrameMs(), 33.33, 0.01), "each image stays 33.3 ms");
  }
  {
    // 60 with every in-between shown: a new image every refresh.
    FpsCounter counter;
    for (int refresh = 0; refresh <= 30; ++refresh) counter.Observe(refresh / 60.0, 1U);
    Require(Near(counter.Fps(), 60.0, 0.01), "full 60 reads 60");
    Require(Near(counter.WorstFrameMs(), 16.67, 0.01), "each image stays 16.7 ms");
  }
  {
    // 60 with a 50 ms hitch: the average barely changes, the worst image shows it.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now, 1U);
    for (int frame = 0; frame < 40; ++frame) {
      now += frame == 20 ? 0.050 : 1.0 / 60.0;
      counter.Observe(now, 1U);
    }
    Require(counter.Fps() > 55.0 && counter.Fps() < 60.0, "average with a hitch");
    Require(Near(counter.WorstFrameMs(), 50.0, 0.01), "50 ms hitch shows up");
  }
  {
    // The next window restarts the worst image.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now, 1U);
    now += 0.2; counter.Observe(now, 1U);
    for (int frame = 0; frame < 60; ++frame) { now += 1.0 / 60.0; counter.Observe(now, 1U); }
    Require(Near(counter.WorstFrameMs(), 16.67, 0.01), "worst image resets each window");
  }
  {
    // An image still on screen when the window closes counts its time so far.
    FpsCounter counter;
    counter.Observe(0.0, 1U);
    for (int refresh = 1; refresh <= 30; ++refresh) counter.Observe(refresh / 60.0, 0U);
    Require(Near(counter.Fps(), 0.0, 0.01), "frozen picture reads 0");
    Require(Near(counter.WorstFrameMs(), 500.0, 0.01), "frozen picture shows in max");
  }
}

// logs16b session 2, Sacred Forest Meadow (spot05) at 60 FPS, level=dropped:
// per 150 refreshes step_frames=75 interp_frames=1 interp_dropped=74 (perf.txt
// fps=60.0). Each refresh shows a step, then a dropped in-between repeats it,
// except one in-between that was drawn.
void DroppedInBetweens() {
  PerfOverlay overlay;
  overlay.ObserveFrame(0.0, 1U);
  bool closed = false;
  for (int refresh = 1; refresh <= 150; ++refresh) {
    const bool step = refresh % 2 == 0;
    const uint32_t newImages = step || refresh == 75 ? 1U : 0U;
    overlay.ObserveRefreshes(step ? 1U : 0U, step ? 1U : 0U, 0U);
    closed |= overlay.ObserveFrame(refresh / 60.0, newImages);
  }
  Require(closed, "windows closed");
  // The last 0.5 s window (refreshes 121-150): 15 steps, no in-between.
  Require(Same(overlay.FrameLine(), "30.0 FPS  max 33.3 ms  speed 100%"),
          "60 with the in-betweens dropped reads 30, held image 33.3 ms");

  FpsCounter counter(150.0 / 60.0);
  counter.Observe(0.0, 1U);
  for (int refresh = 1; refresh <= 150; ++refresh)
    counter.Observe(refresh / 60.0, refresh % 2 == 0 || refresh == 75 ? 1U : 0U);
  // 75 steps + 1 in-between in 2.5 s.
  Require(Near(counter.Fps(), 76.0 / 2.5, 0.01), "spot05 window reads 30.4, not 60");
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
              Same(SwitchClockSourceName(SwitchClockSource::Clkrst), "clkrst") &&
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
  SwitchClockReading clkrst{1224, 461, 1600, SwitchClockSource::Clkrst, 0U};
  FormatPerfOverlayClockLine(text, sizeof(text), clkrst);
  Require(Same(text, "CPU 1224  GPU 461  MEM 1600  clkrst"), "clkrst line (CPU High on)");

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
      closed |= overlay.ObserveFrame(frame / 30.0, 1U);
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
    overlay.ObserveFrame(0.0, 1U);
    for (int frame = 1; frame <= 30; ++frame) {
      overlay.ObserveRefreshes(frame <= 15 ? 1U : 2U, 2U, 0U);
      overlay.ObserveFrame(frame / 60.0, 1U);
    }
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed 75%"), "half the frames ran one of two");
    for (int frame = 31; frame <= 60; ++frame) {
      overlay.ObserveRefreshes(1U, 1U, 0U);
      overlay.ObserveFrame(frame / 60.0, 1U);
    }
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed 100%"), "count resets per window");
  }
  {
    // Frames that owed nothing (preparation screen, interpolated frames only).
    PerfOverlay overlay;
    for (int frame = 0; frame <= 30; ++frame) overlay.ObserveFrame(frame / 60.0, 1U);
    Require(Same(overlay.FrameLine(), "60.0 FPS  max 16.7 ms  speed --"), "no refresh owed");
  }
  Require(!PerfOverlayEnabled().load(), "overlay off until the Video tab loads config/fps_counter");
}

} // namespace

int main() {
  FpsCounterWindows();
  DroppedInBetweens();
  StockTable();
  Lines();
  Overlay();
  std::puts("perf overlay: PASS");
  return 0;
}
