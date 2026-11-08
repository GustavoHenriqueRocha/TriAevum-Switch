// L3+R3 > Video > Performance mode (oot3d_switch_performance.h): the three
// files, GPU x memory -> the official handheld configuration, the rules for
// what Standard puts back, the mode derived from the three choices, and the
// CPU clock keeper against a fake clock (HOS re-applying its configuration,
// sys-clk fighting, HOME, exit, failures).
#include "oot3d_switch_performance.h"
#include "oot3d_switch_clocks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace {

using namespace Oot3dNativeGame;
using Action = CpuClockKeeper::Action;
using Step = CpuClockKeeper::Step;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Same(const std::string &a, const char *b) {
  if (a == b) return true;
  std::fprintf(stderr, "  got \"%s\", expected \"%s\"\n", a.c_str(), b);
  return false;
}

void Files() {
  const auto dir = std::filesystem::temp_directory_path() / "oot3d_switch_performance_test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  Require(ReadPerformanceChoice(dir) == PerformanceChoice{}, "no files = all Standard");
  Require(SavePerformanceChoice(dir, {true, true, true}) == nullptr, "On saved");
  Require(std::filesystem::exists(dir / "cpu_performance") && std::filesystem::exists(dir / "gpu_performance") &&
              std::filesystem::exists(dir / "memory_performance"),
          "On = the three files");
  Require(ReadPerformanceChoice(dir) == PerformanceChoice{true, true, true}, "On read back");
  Require(SavePerformanceChoice(dir, {false, true, false}) == nullptr, "GPU only saved");
  Require(ReadPerformanceChoice(dir) == PerformanceChoice{false, true, false}, "GPU only read back");
  Require(DerivePerformanceMode(ReadPerformanceChoice(dir)) == PerformanceMode::Custom, "one file = Custom");
  Require(SavePerformanceChoice(dir, {}) == nullptr && ReadPerformanceChoice(dir) == PerformanceChoice{},
          "Off removes the files");
  Require(SavePerformanceChoice(dir, {}) == nullptr, "removing absent files is fine");
  // The GPU clock file of earlier versions keeps its meaning.
  Require(SavePerformanceFlag(dir, kGpuPerformanceConfigName, true) &&
              ReadPerformanceChoice(dir) == PerformanceChoice{false, true, false},
          "config/gpu_performance alone = GPU High");
  std::filesystem::remove_all(dir, error);
}

void Mode() {
  static_assert(DerivePerformanceMode({}) == PerformanceMode::Off);
  static_assert(DerivePerformanceMode({true, true, true}) == PerformanceMode::On);
  static_assert(DerivePerformanceMode({true, false, false}) == PerformanceMode::Custom);
  static_assert(DerivePerformanceMode({false, true, true}) == PerformanceMode::Custom);
  static_assert(DerivePerformanceMode({true, true, false}) == PerformanceMode::Custom);
  Require(Same(PerformanceModeName(PerformanceMode::Off), "Off") &&
              Same(PerformanceModeName(PerformanceMode::On), "On") &&
              Same(PerformanceModeName(PerformanceMode::Custom), "Custom"),
          "mode names");
}

void Configurations() {
  constexpr uint32_t standard = kHandheldStandardConfiguration;
  static_assert(HandheldConfiguration(false, false, standard) == 0x00020003U);
  static_assert(HandheldConfiguration(false, true, standard) == 0x00020001U);
  static_assert(HandheldConfiguration(true, false, standard) == 0x92220008U);
  static_assert(HandheldConfiguration(true, true, standard) == 0x92220007U);
  // Both Standard puts back what was reported before.
  static_assert(HandheldConfiguration(false, false, 0x00020005U) == 0x00020005U);

  struct Row {
    uint32_t Id;
    int Cpu, Gpu, Mem;
  };
  const Row rows[] = {{0x00020003U, 1020, 307, 1331},
                      {0x00020001U, 1020, 307, 1600},
                      {0x92220008U, 1020, 461, 1331},
                      {0x92220007U, 1020, 461, 1600}};
  for (const Row &row : rows) {
    const SwitchClockReading reading = StockClockReading(row.Id);
    Require(reading.CpuMhz == row.Cpu && reading.GpuMhz == row.Gpu && reading.MemMhz == row.Mem,
            "the four handheld configurations' clocks");
  }
}

void RestoreRules() {
  static_assert(HandheldStandardConfiguration(0U) == kHandheldStandardConfiguration);
  static_assert(HandheldStandardConfiguration(0x92220008U) == kHandheldStandardConfiguration);
  static_assert(HandheldStandardConfiguration(0x92220007U) == kHandheldStandardConfiguration);
  static_assert(HandheldStandardConfiguration(0x00020001U) == kHandheldStandardConfiguration);
  static_assert(HandheldStandardConfiguration(0x00020003U) == 0x00020003U);
  static_assert(HandheldStandardConfiguration(0x00020005U) == 0x00020005U);
  static_assert(CpuRestoreRate(0U) == kCpuStandardHz);
  static_assert(CpuRestoreRate(kCpuHighHz) == kCpuStandardHz);
  static_assert(CpuRestoreRate(1785000000U) == 1785000000U);
  static_assert(CpuRestoreRate(kCpuStandardHz) == kCpuStandardHz);
  static_assert(kCpuHighHz == 1224000000U && kCpuStandardHz == 1020000000U);
}

// The console's CPU clock: the keeper's steps set it, HOS and sys-clk change
// it behind its back.
struct FakeClock {
  uint32_t Hz = kCpuStandardHz;
  unsigned Sets = 0;

  void Carry(Step step) {
    if (step.Do == Action::SetHigh) {
      Require(step.Hz == kCpuHighHz, "SetHigh asks for 1224 MHz only");
      Hz = step.Hz;
      ++Sets;
    } else if (step.Do == Action::Restore) {
      Hz = step.Hz;
      ++Sets;
    }
  }
};

constexpr uint64_t kOutsideGrace = CpuClockKeeper::kGraceMs + 1U;

void KeeperBasics() {
  FakeClock clock;
  CpuClockKeeper keeper;
  uint64_t t = 1000U;
  Require(keeper.Check(clock.Hz, t) == Step{}, "nothing before CPU High is chosen");
  Require(keeper.Exit() == Step{}, "nothing to restore before CPU High");

  Step step = keeper.Choose(true, clock.Hz, t);
  Require(step == Step{Action::SetHigh, kCpuHighHz} && keeper.PriorHz() == kCpuStandardHz,
          "High: 1224 MHz, the earlier 1020 kept");
  clock.Carry(step);
  Require(keeper.Check(clock.Hz, t + 250U) == Step{} && keeper.Reapplies() == 0U, "1224 held: nothing");

  // HOS applies a configuration (our apm change, wake, dock): CPU 1020 again.
  for (int round = 0; round < 10; ++round) {
    t += 60000U;
    keeper.HosEvent(t);
    clock.Hz = kCpuStandardHz;
    step = keeper.Check(clock.Hz, t + 250U);
    Require(step.Do == Action::SetHigh, "HOS's 1020 after an event: set again");
    clock.Carry(step);
    Require(keeper.Check(clock.Hz, t + 1000U) == Step{}, "then held");
  }
  Require(keeper.Reapplies() == 10U && !keeper.Overridden(), "HOS's own resets never count as an override");

  // HOME / sleep: the earlier rate first; nothing while out of focus.
  t += 60000U;
  step = keeper.Focus(false, 0U, t);
  Require(step == Step{Action::Restore, kCpuStandardHz}, "focus lost: 1020 back");
  clock.Carry(step);
  Require(keeper.Check(clock.Hz, t + 100U) == Step{}, "out of focus: no read-back action");
  Require(keeper.Exit() == Step{}, "already restored: exit does nothing more");
  step = keeper.Focus(true, clock.Hz, t + 30000U);
  Require(step.Do == Action::SetHigh, "back in focus: 1224 again");
  clock.Carry(step);
  Require(keeper.Reapplies() == 11U, "the focus return counts as a re-apply");

  // Exit restores once.
  step = keeper.Exit();
  Require(step == Step{Action::Restore, kCpuStandardHz}, "exit: 1020 back");
  clock.Carry(step);
  Require(keeper.Exit() == Step{}, "exit twice (ExitRequest, then atexit): once");
}

void KeeperStandard() {
  FakeClock clock;
  CpuClockKeeper keeper;
  clock.Carry(keeper.Choose(true, clock.Hz, 0U));
  const Step step = keeper.Choose(false, 0U, 10U);
  Require(step == Step{Action::Restore, kCpuStandardHz}, "Standard: the earlier rate at once");
  clock.Carry(step);
  Require(clock.Hz == kCpuStandardHz, "fake back at 1020");
  clock.Hz = 1785000000U;
  Require(keeper.Check(clock.Hz, kOutsideGrace * 3U) == Step{} && keeper.Focus(true, clock.Hz, 99999U) == Step{},
          "Standard: the keeper leaves the clock alone");
  Require(keeper.Choose(false, 0U, 100000U) == Step{}, "Standard twice: nothing more");
}

void KeeperPrior() {
  CpuClockKeeper fromSysClk;
  Require(fromSysClk.Choose(true, 1785000000U, 0U).Do == Action::SetHigh, "High from 1785");
  Require(fromSysClk.Exit() == Step{Action::Restore, 1785000000U}, "exit puts sys-clk's 1785 back");
  CpuClockKeeper leftOver;
  Require(leftOver.Choose(true, kCpuHighHz, 0U) == Step{}, "already 1224 (a previous run): no request");
  Require(leftOver.Exit() == Step{Action::Restore, kCpuStandardHz}, "a left-over 1224 restores to 1020");
  CpuClockKeeper unread;
  unread.Choose(true, 0U, 0U);
  Require(unread.PriorHz() == kCpuStandardHz, "unread earlier rate: 1020");
  // The earlier rate is the one before the first change, not a later read.
  Require(unread.Choose(false, 0U, 1U).Do == Action::Restore, "Standard");
  unread.Choose(true, kCpuHighHz, 2U);
  Require(unread.PriorHz() == kCpuStandardHz, "prior kept across Standard/High");
}

void KeeperOverride() {
  // sys-clk with a CPU target for this title: it sets 1785 back within its
  // polling interval after every request of ours.
  FakeClock clock;
  CpuClockKeeper keeper;
  uint64_t t = 0U;
  clock.Carry(keeper.Choose(true, clock.Hz, t));
  for (uint32_t window = 1; window <= CpuClockKeeper::kOverrideStreak; ++window) {
    clock.Hz = 1785000000U;  // sys-clk
    t += kOutsideGrace;      // one perf.txt window later, no HOS event
    const Step step = keeper.Check(clock.Hz, t);
    if (window < CpuClockKeeper::kOverrideStreak) {
      Require(step.Do == Action::SetHigh && !keeper.Overridden(), "a foreign rate: set again, counted");
      clock.Carry(step);
    } else {
      Require(step == Step{Action::GiveUp, 1785000000U} && keeper.Overridden() && keeper.ForeignHz() == 1785000000U,
              "three windows in a row: overridden, logged once");
    }
  }
  Require(keeper.Reapplies() == CpuClockKeeper::kOverrideStreak - 1U, "two re-applies before giving up");
  Require(keeper.Check(1785000000U, t + kOutsideGrace) == Step{}, "overridden: no more requests");
  keeper.HosEvent(t + 2U * kOutsideGrace);
  Require(keeper.Check(kCpuStandardHz, t + 2U * kOutsideGrace + 1U) == Step{}, "nor after HOS events");
  Require(keeper.Focus(false, 0U, t + 3U * kOutsideGrace) == Step{} &&
              keeper.Focus(true, 1785000000U, t + 4U * kOutsideGrace) == Step{},
          "overridden: HOME neither restores nor sets");
  Require(keeper.Exit() == Step{}, "overridden: exit leaves the other tool's rate");
  // Choosing High again (Standard, then High) starts over.
  Require(keeper.Choose(false, 0U, t) == Step{}, "Standard while overridden: nothing to put back");
  Require(keeper.Choose(true, 1785000000U, t).Do == Action::SetHigh && !keeper.Overridden(),
          "High again: the keeper tries again");

  // Even right after a HOS event, a rate other than HOS's 1020 is foreign.
  CpuClockKeeper fast;
  FakeClock fastClock;
  fastClock.Carry(fast.Choose(true, fastClock.Hz, 0U));
  for (uint32_t check = 1; check <= CpuClockKeeper::kOverrideStreak; ++check) {
    fast.HosEvent(check * 10U);
    const Step step = fast.Check(1785000000U, check * 10U + 1U);
    if (check < CpuClockKeeper::kOverrideStreak) Require(step.Do == Action::SetHigh, "foreign during grace: counted");
    else Require(step.Do == Action::GiveUp, "foreign during grace: overridden after the streak");
  }
}

void KeeperStreakResets() {
  // A stray change outside an event (unexplained 1020) is set again; it is
  // not an override while 1224 holds in between.
  FakeClock clock;
  CpuClockKeeper keeper;
  uint64_t t = 0U;
  clock.Carry(keeper.Choose(true, clock.Hz, t));
  for (int round = 0; round < 6; ++round) {
    t += kOutsideGrace;
    clock.Hz = kCpuStandardHz;
    Step step = keeper.Check(clock.Hz, t);
    Require(step.Do == Action::SetHigh, "unexplained 1020: set again");
    clock.Carry(step);
    t += kOutsideGrace;
    Require(keeper.Check(clock.Hz, t) == Step{}, "held in the next window");
  }
  Require(!keeper.Overridden(), "isolated changes never add up to an override");
  for (uint32_t window = 1; window <= CpuClockKeeper::kOverrideStreak; ++window) {
    t += kOutsideGrace;
    const Step step = keeper.Check(kCpuStandardHz, t);
    if (window == CpuClockKeeper::kOverrideStreak) Require(step.Do == Action::GiveUp, "1020 window after window");
  }
  Require(keeper.Overridden(), "a tool holding 1020 every window is an override too");
}

void KeeperFailures() {
  CpuClockKeeper keeper;
  Require(keeper.Choose(true, kCpuStandardHz, 0U).Do == Action::SetHigh, "High");
  keeper.Report(false);
  keeper.Report(true);
  keeper.Report(false);
  keeper.Report(false);
  Require(!keeper.Unavailable(), "failures not in a row: keep trying");
  keeper.Report(false);
  Require(keeper.Unavailable(), "three failed requests in a row: unavailable");
  Require(keeper.Check(kCpuStandardHz, 1U) == Step{} && keeper.Exit() == Step{},
          "unavailable: no more requests, nothing to restore");
  Require(keeper.Choose(false, 0U, 2U) == Step{} && keeper.Choose(true, kCpuStandardHz, 3U).Do == Action::SetHigh &&
              !keeper.Unavailable(),
          "choosing High again retries");
  CpuClockKeeper closed;
  closed.Choose(true, kCpuStandardHz, 0U);
  closed.MarkUnavailable();
  Require(closed.Unavailable() && closed.Exit() == Step{}, "clkrst refused: nothing to restore");
}

void KeeperChosenOutOfFocus() {
  CpuClockKeeper keeper;
  Require(keeper.Focus(false, 0U, 0U) == Step{}, "focus lost before any choice: nothing");
  Require(keeper.Choose(true, kCpuStandardHz, 1U) == Step{}, "chosen out of focus: wait");
  Require(keeper.Focus(true, kCpuStandardHz, 2U).Do == Action::SetHigh, "in focus: 1224");
}

void PerfFields() {
  PerformanceStatus status;
  Require(Same(FormatPerformancePerfFields(status),
               " req_cpu=0 req_gpu=0 req_mem=0 cpu_reapply=0 cpu_override=0 cpu_err=-"),
          "default fields");
  status.Requested = {true, true, true};
  status.CpuReapplies = 12U;
  status.CpuOverridden = true;
  status.CpuFailedStep = "clkrstSetClockRate";
  status.CpuResult = 0x1A3U;
  Require(Same(FormatPerformancePerfFields(status),
               " req_cpu=1 req_gpu=1 req_mem=1 cpu_reapply=12 cpu_override=1 cpu_err=clkrstSetClockRate:0x1A3"),
          "fields with an error");
  const PerformanceStatus host = ApplyPerformance({true, false, true});
  Require(host.Requested == PerformanceChoice{true, false, true} && !host.ApmAttempted && !host.CpuUnavailable,
          "off the Switch: no apm, no clkrst");
}

} // namespace

int main() {
  Files();
  Mode();
  Configurations();
  RestoreRules();
  KeeperBasics();
  KeeperStandard();
  KeeperPrior();
  KeeperOverride();
  KeeperStreakResets();
  KeeperFailures();
  KeeperChosenOutOfFocus();
  PerfFields();
  std::puts("switch performance: PASS");
  return 0;
}
