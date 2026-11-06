#pragma once

// Guest fault report for logs/oot3d_native_game.log: when the guest faults
// (a memory fault, an unsupported exit, a host service that cannot serve a
// request) the one-line error says where, not why. This report adds what is
// needed to find the cause from a player's log: the faulting thread and all
// its registers, a short backtrace (lr and a scan of the stack for return
// addresses after a call instruction), 64 bytes around the fault address and
// around every register that points into mapped data, 128 bytes of the stack,
// what each address is mapped as, the area id as numbers and the last
// perf.txt windows.
//
// Built only on the fault path (NativeA32Process::Fault), logged by the window
// (RequireRunnableGuest). Host side only: it reads guest memory through the
// existing NativeA32Memory API (IsMapped/IsWritable/IsExecutable/ReadBytes,
// Read8/16/32), so nothing the whole-AOT shards compile changes. Header-only;
// tests in oot3d_native_guest_fault_report_tests.cpp.

#include "oot3d_native_area.h"
#include "recomp/a32_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

// Other guest threads at the time of the fault (one line each).
struct GuestFaultThreadSummary {
  uint32_t Id = 0U;
  std::optional<uint32_t> EntryAddress;
  uint32_t Pc = 0U;
  uint32_t Lr = 0U;
  uint32_t Sp = 0U;
  const char *Status = "";
};

// A region the process mapped itself (segments, stacks, TLS), by name.
// Mappings made by the host services (heap, shared memory) have no entry and
// are named by their CTR address area.
struct GuestFaultNamedRegion {
  std::string Name;
  uint32_t BaseAddress = 0U;
  uint32_t Size = 0U;
};

struct GuestFaultFacts {
  uint32_t ThreadId = 0U;
  std::optional<uint32_t> EntryAddress;
  oot3d::recomp::a32::GuestState State{};
  oot3d::recomp::a32::ExecutionResult Exit{};
  std::vector<GuestFaultThreadSummary> OtherThreads;
  std::vector<GuestFaultNamedRegion> NamedRegions;
};

inline constexpr uint32_t kGuestFaultDumpBytes = 64U;      // around each address
inline constexpr uint32_t kGuestFaultStackDumpBytes = 128U; // from sp up
inline constexpr uint32_t kGuestFaultStackScanBytes = 4096U;
inline constexpr size_t kGuestFaultMaxStackFrames = 12U;
inline constexpr uint32_t kGuestFaultPage = 0x1000U;
// How far a region's bounds are searched from an address, each way.
inline constexpr uint32_t kGuestFaultRegionProbePages = 4096U; // 16 MiB

[[nodiscard]] inline const char *GuestExitKindName(oot3d::recomp::a32::ExitKind kind) {
  using Kind = oot3d::recomp::a32::ExitKind;
  switch (kind) {
  case Kind::Fallthrough: return "Fallthrough";
  case Kind::Branch: return "Branch";
  case Kind::Svc: return "Svc";
  case Kind::Wait: return "Wait";
  case Kind::MemoryFault: return "MemoryFault";
  case Kind::BlockLimit: return "BlockLimit";
  case Kind::MissingBlock: return "MissingBlock";
  case Kind::Fallback: return "Fallback";
  case Kind::Unsupported: return "Unsupported";
  }
  return "?";
}

// True for an A32 call whose return address is the next word: BL, BLX
// (immediate) and BLX (register).
[[nodiscard]] inline bool IsGuestCallInstruction(uint32_t instruction) {
  const uint32_t condition = instruction >> 28U;
  if (condition != 0xFU && (instruction & 0x0F000000U) == 0x0B000000U) return true; // BL
  if ((instruction & 0xFE000000U) == 0xFA000000U) return true;                      // BLX imm
  return condition != 0xFU && (instruction & 0x0FFFFFF0U) == 0x012FFF30U;          // BLX reg
}

template <typename Memory>
[[nodiscard]] bool ReadGuestWord(const Memory &memory, uint32_t address, uint32_t *value) {
  std::array<uint8_t, 4> bytes{};
  if (!memory.ReadBytes(address, bytes)) return false;
  *value = uint32_t{bytes[0]} | (uint32_t{bytes[1]} << 8U) | (uint32_t{bytes[2]} << 16U) |
           (uint32_t{bytes[3]} << 24U);
  return true;
}

// The mapping that contains an address: its bounds (one mapped region; a
// region next to another is not merged) and permissions.
struct GuestFaultRegion {
  std::string Name;
  uint32_t Begin = 0U;
  uint64_t End = 0U;
  bool Writable = false;
  bool Executable = false;
  bool Wider = false; // the bounds search stopped before the region's ends
};

// "heap", "shared memory", "linear heap" for the CTR application areas.
[[nodiscard]] inline const char *GuestCtrAreaName(uint32_t address) {
  if (address >= 0x08000000U && address < 0x10000000U) return "heap";
  if (address >= 0x10000000U && address < 0x14000000U) return "shared memory";
  if (address >= 0x14000000U && address < 0x1C000000U) return "linear heap";
  return "mapped";
}

template <typename Memory>
[[nodiscard]] std::optional<GuestFaultRegion>
DescribeGuestRegion(const Memory &memory, uint32_t address, std::span<const GuestFaultNamedRegion> names = {}) {
  if (!memory.IsMapped(address, 1U)) return std::nullopt;
  GuestFaultRegion region;
  region.Writable = memory.IsWritable(address, 1U);
  region.Executable = memory.IsExecutable(address, 1U);
  // IsMapped of a span is true only inside one region, so growing the span
  // page by page finds that region's bounds.
  uint64_t begin = address, end = uint64_t{address} + 1U;
  const uint64_t page = address & ~(kGuestFaultPage - 1U);
  if (memory.IsMapped(static_cast<uint32_t>(page), static_cast<size_t>(end - page))) begin = page;
  const uint64_t pageEnd = page + kGuestFaultPage;
  if (memory.IsMapped(static_cast<uint32_t>(begin), static_cast<size_t>(pageEnd - begin))) end = pageEnd;
  uint32_t steps = 0U;
  while (begin >= kGuestFaultPage && steps < kGuestFaultRegionProbePages &&
         memory.IsMapped(static_cast<uint32_t>(begin - kGuestFaultPage),
                         static_cast<size_t>(end - begin + kGuestFaultPage))) {
    begin -= kGuestFaultPage;
    ++steps;
  }
  region.Wider = steps == kGuestFaultRegionProbePages;
  steps = 0U;
  while (end + kGuestFaultPage <= 0x100000000ULL && steps < kGuestFaultRegionProbePages &&
         memory.IsMapped(static_cast<uint32_t>(begin), static_cast<size_t>(end - begin + kGuestFaultPage))) {
    end += kGuestFaultPage;
    ++steps;
  }
  region.Wider = region.Wider || steps == kGuestFaultRegionProbePages;
  region.Begin = static_cast<uint32_t>(begin);
  region.End = end;
  region.Name = GuestCtrAreaName(address);
  for (const auto &named : names)
    if (address >= named.BaseAddress && uint64_t{address} < uint64_t{named.BaseAddress} + named.Size) {
      region.Name = named.Name;
      break;
    }
  return region;
}

// A return address: word aligned, in executable memory, right after a call.
template <typename Memory> [[nodiscard]] bool IsGuestReturnAddress(const Memory &memory, uint32_t value) {
  uint32_t instruction = 0U;
  return (value & 3U) == 0U && value >= 4U && memory.IsExecutable(value - 4U, 4U) &&
         ReadGuestWord(memory, value - 4U, &instruction) && IsGuestCallInstruction(instruction);
}

struct GuestStackReturn {
  uint32_t Offset = 0U; // from sp
  uint32_t Address = 0U;
};

// Return addresses on the stack, nearest frame first: the words from sp up to
// the end of sp's region (at most kGuestFaultStackScanBytes) that pass
// IsGuestReturnAddress. A heuristic: stale return addresses of finished calls
// look the same, and a stack the fault overwrote has none.
template <typename Memory>
[[nodiscard]] std::vector<GuestStackReturn> ScanGuestStackReturns(const Memory &memory, uint32_t sp,
                                                                  size_t maxFrames = kGuestFaultMaxStackFrames) {
  std::vector<GuestStackReturn> frames;
  const uint32_t start = sp & ~3U;
  for (uint64_t address = start; address + 4U <= uint64_t{start} + kGuestFaultStackScanBytes &&
                                 frames.size() < maxFrames &&
                                 memory.IsMapped(start, static_cast<size_t>(address + 4U - start));
       address += 4U) {
    uint32_t value = 0U;
    if (ReadGuestWord(memory, static_cast<uint32_t>(address), &value) && IsGuestReturnAddress(memory, value))
      frames.push_back({static_cast<uint32_t>(address - start), value});
  }
  return frames;
}

namespace GuestFaultDetail {

inline void Append(std::string &out, const char *format, auto... values) {
  char text[160];
  const int length = std::snprintf(text, sizeof(text), format, values...);
  if (length > 0) out.append(text, std::min<size_t>(static_cast<size_t>(length), sizeof(text) - 1U));
}

inline void AppendRegion(std::string &out, const std::optional<GuestFaultRegion> &region) {
  if (!region.has_value()) {
    out += "unmapped";
    return;
  }
  out += region->Name;
  Append(out, " %s [%08X-%08llX)%s", region->Executable ? "rx" : region->Writable ? "rw" : "r", region->Begin,
         static_cast<unsigned long long>(region->End), region->Wider ? " (wider)" : "");
}

// 16-byte rows from `begin`, hex and ASCII; "--" for unmapped bytes.
template <typename Memory> void AppendDump(std::string &out, const Memory &memory, uint32_t begin, uint32_t bytes) {
  for (uint64_t row = begin; row < uint64_t{begin} + bytes && row <= 0xFFFFFFF0U; row += 16U) {
    std::array<uint8_t, 16> data{};
    std::array<bool, 16> mapped{};
    if (memory.ReadBytes(static_cast<uint32_t>(row), data)) {
      mapped.fill(true);
    } else {
      for (size_t index = 0; index < data.size(); ++index)
        mapped[index] = memory.ReadBytes(static_cast<uint32_t>(row + index), std::span<uint8_t>(&data[index], 1U));
    }
    Append(out, "    %08X:", static_cast<uint32_t>(row));
    std::string ascii;
    for (size_t index = 0; index < data.size(); ++index) {
      if (mapped[index]) {
        Append(out, " %02X", data[index]);
        ascii += data[index] >= 0x20U && data[index] < 0x7FU ? static_cast<char>(data[index]) : '.';
      } else {
        out += " --";
        ascii += ' ';
      }
    }
    out += "  |" + ascii + "|\n";
  }
}

inline uint32_t DumpStart(uint32_t address) {
  const uint32_t row = address & ~15U;
  return row >= kGuestFaultDumpBytes / 2U ? row - kGuestFaultDumpBytes / 2U : 0U;
}

} // namespace GuestFaultDetail

// The multi-line report (each line ends in '\n'). Scene and room are
// numbers only, never names.
template <typename Memory>
[[nodiscard]] std::string FormatGuestFaultReport(Memory &memory, const GuestFaultFacts &facts) {
  using namespace GuestFaultDetail;
  const auto &r = facts.State.r;
  const auto &exit = facts.Exit;
  const std::span<const GuestFaultNamedRegion> names = facts.NamedRegions;
  std::string out = "guest fault report:";
  Append(out, " thread=%u", facts.ThreadId);
  if (facts.EntryAddress.has_value()) Append(out, " entry=0x%08X", *facts.EntryAddress);
  Append(out, " exit=%s pc=0x%08X detail=0x%08X", GuestExitKindName(exit.kind), exit.pc, exit.detail);
  if (exit.fallback != oot3d::recomp::a32::FallbackReason::None)
    Append(out, " fallback=%u", static_cast<unsigned>(exit.fallback));
  const bool memoryFault = exit.kind == oot3d::recomp::a32::ExitKind::MemoryFault;
  if (memoryFault) {
    Append(out, " fault_address=0x%08X (", exit.detail);
    AppendRegion(out, DescribeGuestRegion(memory, exit.detail, names));
    out += ')';
  }
  out += '\n';

  static constexpr std::array<const char *, 16> kNames{"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                                        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "r15"};
  for (size_t row = 0; row < 4U; ++row) {
    out += ' ';
    for (size_t column = 0; column < 4U; ++column) {
      const size_t index = row * 4U + column;
      Append(out, " %s=%08X", kNames[index], r[index]);
    }
    out += '\n';
  }
  const uint32_t cpsr = facts.State.cpsr;
  Append(out, "  cpsr=%08X (%c%c%c%c mode=%02X) fpscr=%08X tls=%08X\n", cpsr, (cpsr >> 31U) & 1U ? 'N' : 'n',
         (cpsr >> 30U) & 1U ? 'Z' : 'z', (cpsr >> 29U) & 1U ? 'C' : 'c', (cpsr >> 28U) & 1U ? 'V' : 'v',
         cpsr & 0x1FU, facts.State.fpscr, facts.State.thread_pointer);

  // An empty stack's sp is the end of its region.
  out += "  stack: sp in ";
  if (!memory.IsMapped(r[13], 1U) && r[13] >= 4U && memory.IsMapped(r[13] - 4U, 4U)) {
    AppendRegion(out, DescribeGuestRegion(memory, r[13] - 4U, names));
    out += " at its end";
  } else {
    AppendRegion(out, DescribeGuestRegion(memory, r[13], names));
  }
  out += '\n';
  Append(out, "  backtrace: pc=%08X", exit.pc);
  Append(out, " lr=%08X%s", r[14], IsGuestReturnAddress(memory, r[14]) ? "" : "(not after a call)");
  for (const auto &frame : ScanGuestStackReturns(memory, r[13]))
    Append(out, " sp+%X=%08X", frame.Offset, frame.Address);
  out += '\n';

  const NativeArea area = ReadNativeArea(memory);
  if (area.Scene < 0) out += "  area: scene=- room=-\n";
  else if (area.Room < 0) Append(out, "  area: scene=0x%02X room=-\n", static_cast<unsigned>(area.Scene));
  else Append(out, "  area: scene=0x%02X room=%d\n", static_cast<unsigned>(area.Scene), static_cast<int>(area.Room));

  for (const auto &thread : facts.OtherThreads) {
    Append(out, "  other thread=%u", thread.Id);
    if (thread.EntryAddress.has_value()) Append(out, " entry=0x%08X", *thread.EntryAddress);
    Append(out, " %s pc=%08X lr=%08X sp=%08X\n", thread.Status, thread.Pc, thread.Lr, thread.Sp);
  }

  // 64 bytes around the fault address and each register into mapped data
  // (code is left out); registers sharing a window share its dump.
  struct Window {
    uint32_t Start = 0U;
    std::string Labels;
    uint32_t Address = 0U;
  };
  std::vector<Window> windows;
  const auto add = [&](const std::string &label, uint32_t address) {
    const uint32_t start = DumpStart(address);
    for (auto &window : windows)
      if (window.Start == start) {
        window.Labels += ',' + label;
        return;
      }
    windows.push_back({start, label, address});
  };
  if (memoryFault) add("fault", exit.detail);
  for (size_t index = 0; index < 14U; ++index)
    if (memory.IsMapped(r[index], 1U) && !memory.IsExecutable(r[index], 1U)) add(kNames[index], r[index]);
  for (const auto &window : windows) {
    Append(out, "  memory %s=%08X ", window.Labels.c_str(), window.Address);
    AppendRegion(out, DescribeGuestRegion(memory, window.Address, names));
    out += '\n';
    AppendDump(out, memory, window.Start, kGuestFaultDumpBytes);
  }
  if (memory.IsMapped(r[13], 1U)) {
    Append(out, "  stack from sp=%08X\n", r[13]);
    AppendDump(out, memory, r[13] & ~15U, kGuestFaultStackDumpBytes);
  }
  return out;
}

// One logs/perf.txt window (150 host frames), as the fault report repeats it.
struct GuestFaultPerfWindow {
  uint64_t HostFrames = 0U;
  double Fps = 0.0;
  double GuestMs = 0.0;     // per host frame
  double PresentMs = 0.0;   // per host frame
  double TotalMs = 0.0;     // per host frame
  double GpuBusyMs = -1.0;  // deko3d busy_ms; -1 without the dkperf line
  double GpuFenceMs = -1.0; // deko3d fence_cpu_ms; -1 without the dkperf line
};

// The value of `key` in a "key=value key=value" perf line; -1 when absent.
[[nodiscard]] inline double PerfLineValue(std::string_view line, std::string_view key) {
  for (size_t position = line.find(key); position != std::string_view::npos;
       position = line.find(key, position + 1U)) {
    const size_t equals = position + key.size();
    if ((position == 0U || line[position - 1U] == ' ') && equals < line.size() && line[equals] == '=') {
      const std::string value(line.substr(equals + 1U, line.find(' ', equals) - equals - 1U));
      char *end = nullptr;
      const double parsed = std::strtod(value.c_str(), &end);
      return end != value.c_str() ? parsed : -1.0;
    }
  }
  return -1.0;
}

// The last kWindows perf windows. Written by the perf window and read by the
// fault report, both on the game thread; no allocation when recording.
class GuestFaultPerfTrail {
public:
  static constexpr size_t kWindows = 3U;

  void Record(const GuestFaultPerfWindow &window) {
    mWindows[mRecorded % kWindows] = window;
    ++mRecorded;
  }

  // "  perf: (none)\n" or one line per window, oldest first.
  [[nodiscard]] std::string Format() const {
    std::string out;
    const uint64_t count = std::min<uint64_t>(mRecorded, kWindows);
    if (count == 0U) return "  perf: (none)\n";
    for (uint64_t index = mRecorded - count; index < mRecorded; ++index) {
      const auto &window = mWindows[index % kWindows];
      GuestFaultDetail::Append(out,
                               "  perf: host_frames=%llu fps=%.1f guest_ms=%.1f present_ms=%.1f total_ms=%.1f "
                               "gpu_busy_ms=%.1f gpu_fence_ms=%.1f\n",
                               static_cast<unsigned long long>(window.HostFrames), window.Fps, window.GuestMs,
                               window.PresentMs, window.TotalMs, window.GpuBusyMs, window.GpuFenceMs);
    }
    return out;
  }

private:
  std::array<GuestFaultPerfWindow, kWindows> mWindows{};
  uint64_t mRecorded = 0U;
};

// The process-wide trail (game thread).
inline GuestFaultPerfTrail &GuestFaultPerf() {
  static GuestFaultPerfTrail trail;
  return trail;
}

} // namespace Oot3dNativeGame
