// Guest fault report (oot3d_native_guest_fault_report.h): the lines a guest
// fault writes to logs/oot3d_native_game.log, the stack scan for return
// addresses and the perf windows it repeats. Header-only, with a small fake
// guest memory laid out like the process (code, data, heap, stack, the
// shared memory block above the stack).
#include "oot3d_native_guest_fault_report.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

// Like NativeA32Memory: a span is mapped only inside one region.
class FakeMemory {
public:
  struct Region {
    std::string Name;
    uint32_t Base = 0;
    std::vector<uint8_t> Bytes;
    bool Writable = true;
    bool Executable = false;
  };

  void Map(std::string name, uint32_t base, size_t size, bool executable = false) {
    mRegions.push_back({std::move(name), base, std::vector<uint8_t>(size), !executable, executable});
  }
  void Write32(uint32_t address, uint32_t value) {
    for (uint32_t index = 0; index < 4U; ++index) Byte(address + index) = static_cast<uint8_t>(value >> (8U * index));
  }
  void WriteBytes(uint32_t address, std::initializer_list<uint8_t> bytes) {
    for (uint8_t value : bytes) Byte(address++) = value;
  }

  bool IsMapped(uint32_t address, size_t size) const { return Find(address, size) != nullptr; }
  bool IsWritable(uint32_t address, size_t size) const {
    const Region *region = Find(address, size);
    return region != nullptr && region->Writable;
  }
  bool IsExecutable(uint32_t address, size_t size) const {
    const Region *region = Find(address, size);
    return region != nullptr && region->Executable;
  }
  bool ReadBytes(uint32_t address, std::span<uint8_t> bytes) const {
    const Region *region = Find(address, bytes.size());
    if (region == nullptr) return false;
    std::memcpy(bytes.data(), region->Bytes.data() + (address - region->Base), bytes.size());
    return true;
  }
  bool Read8(uint32_t address, uint8_t *value) { return ReadBytes(address, std::span<uint8_t>(value, 1U)); }
  bool Read16(uint32_t address, uint16_t *value) {
    std::array<uint8_t, 2> bytes{};
    if (!ReadBytes(address, bytes)) return false;
    *value = static_cast<uint16_t>(bytes[0] | (bytes[1] << 8U));
    return true;
  }
  bool Read32(uint32_t address, uint32_t *value) { return Oot3dNativeGame::ReadGuestWord(*this, address, value); }

private:
  const Region *Find(uint32_t address, size_t size) const {
    for (const auto &region : mRegions)
      if (address >= region.Base && uint64_t{address} + size <= uint64_t{region.Base} + region.Bytes.size())
        return &region;
    return nullptr;
  }
  uint8_t &Byte(uint32_t address) {
    for (auto &region : mRegions)
      if (address >= region.Base && address < region.Base + region.Bytes.size())
        return region.Bytes[address - region.Base];
    std::fprintf(stderr, "FAIL: fixture write outside the regions\n");
    std::exit(1);
  }
  std::vector<Region> mRegions;
};

constexpr uint32_t kCode = 0x00100000U;
constexpr uint32_t kData = 0x004F9000U;
constexpr uint32_t kHeap = 0x08000000U;
constexpr uint32_t kStack = 0x0FFFC000U;
constexpr uint32_t kShared = 0x10000000U;
constexpr uint32_t kSp = 0x0FFFFE00U;
constexpr uint32_t kText = 0x08000100U;
constexpr uint32_t kPlay = 0x08001000U;

FakeMemory MakeProcessMemory() {
  FakeMemory memory;
  memory.Map("text", kCode, 0x1000U, true);
  memory.Map("data_bss", kData, 0xCF000U);
  memory.Map("ctr_heap_0", kHeap, 0x10000U);
  memory.Map("primary_stack", kStack, 0x4000U);
  memory.Map("shared_memory:gsp", kShared, 0x1000U);
  memory.Write32(kCode + 0x100U, 0xEB000010U); // bl
  memory.Write32(kCode + 0x200U, 0xE1A00000U); // mov r0, r0
  memory.Write32(kCode + 0x300U, 0xE12FFF33U); // blx r3
  // The stack: data, two return addresses and three look-alikes.
  memory.Write32(kSp + 0x00U, 0x12345678U);
  memory.Write32(kSp + 0x08U, kCode + 0x104U);
  memory.Write32(kSp + 0x0CU, kCode + 0x204U); // after a mov: not a return
  memory.Write32(kSp + 0x10U, kCode + 0x304U);
  memory.Write32(kSp + 0x14U, kCode + 0x102U); // misaligned
  memory.Write32(kSp + 0x18U, 0x00200000U);    // unmapped
  // Text with an escape whose opcode the parser has no entry for.
  memory.WriteBytes(kText, {'H', 'i', 0x7F, 0x0E, 0x01, 0x02, 0x03, 0x04, 0x05, 0x7F, 0x41, 'b', 'c'});
  // The PlayState: Kokiri Forest (0x55), room 2.
  memory.Write32(Oot3dNativeGame::kNativeGameStatePointer, kPlay);
  memory.WriteBytes(kPlay + Oot3dNativeGame::kNativeGameStateTypeOffset, {Oot3dNativeGame::kNativeGameStatePlay});
  memory.WriteBytes(kPlay + Oot3dNativeGame::kNativePlaySceneOffset, {0x55, 0x00});
  memory.WriteBytes(kPlay + Oot3dNativeGame::kNativePlayRoomOffset, {2});
  return memory;
}

Oot3dNativeGame::GuestFaultFacts MakeFacts() {
  Oot3dNativeGame::GuestFaultFacts facts;
  facts.ThreadId = 3U;
  facts.EntryAddress = 0x00200000U;
  auto &r = facts.State.r;
  for (uint32_t index = 0; index < 16U; ++index) r[index] = 0x00700000U + index; // unmapped
  r[0] = 0x10000FFCU;  // the store pointer, past the stack
  r[1] = kText + 3U;   // the opcode
  r[2] = kText + 9U;   // same 64-byte window as r1
  r[3] = 0x006F0072U;  // a count, unmapped
  r[13] = kSp;
  r[14] = kCode + 0x104U;
  r[15] = 0x004C0A88U;
  facts.State.cpsr = 0x60000010U;
  facts.State.fpscr = 0x03C00010U;
  facts.State.thread_pointer = 0x1FF82200U;
  facts.Exit = {oot3d::recomp::a32::ExitKind::MemoryFault, 0x004C0A8CU,
                oot3d::recomp::a32::FallbackReason::None, 0x10001000U};
  facts.OtherThreads.push_back({0U, 0x00100000U, 0x00123456U, 0x00100104U, 0x0FFFF000U, "waiting"});
  // What the process mapped itself; the heap and the shared block came from
  // the host services and have no name.
  facts.NamedRegions = {{"text", kCode, 0x1000U}, {"data_bss", kData, 0xCF000U}, {"primary_stack", kStack, 0x4000U}};
  return facts;
}

void TestRegions() {
  using Oot3dNativeGame::DescribeGuestRegion;
  FakeMemory memory = MakeProcessMemory();
  // A large unnamed mapping in the linear heap area: the bounds search stops
  // after 16 MiB each way and says so.
  memory.Map("linear", 0x14000000U, 0x1800000U);
  const std::vector<Oot3dNativeGame::GuestFaultNamedRegion> names{{"primary_stack", kStack, 0x4000U}};
  const auto stack = DescribeGuestRegion(memory, kSp + 3U, names);
  Require(stack && stack->Name == "primary_stack" && stack->Begin == kStack && stack->End == kShared &&
              stack->Writable && !stack->Executable && !stack->Wider,
          "stack bounds stop where the shared block starts");
  const auto shared = DescribeGuestRegion(memory, kShared + 0xFFFU, names);
  Require(shared && shared->Name == "shared memory" && shared->Begin == kShared && shared->End == kShared + 0x1000U,
          "unnamed shared block, one page");
  const auto code = DescribeGuestRegion(memory, kCode + 0x10U, names);
  Require(code && code->Executable && code->Name == "mapped", "code outside the named areas");
  const auto heap = DescribeGuestRegion(memory, kText, names);
  Require(heap && heap->Name == "heap" && heap->Begin == kHeap && heap->End == kHeap + 0x10000U, "heap");
  const auto linear = DescribeGuestRegion(memory, 0x15000000U, names);
  Require(linear && linear->Name == "linear heap" && linear->Wider && linear->Begin == 0x14000000U &&
              linear->End == 0x15800000U,
          "large region: bounds searched 16 MiB each way");
  Require(!DescribeGuestRegion(memory, 0x10001000U, names), "unmapped");
}

void TestCallInstructions() {
  using Oot3dNativeGame::IsGuestCallInstruction;
  Require(IsGuestCallInstruction(0xEB000010U), "bl");
  Require(IsGuestCallInstruction(0x0B000010U), "bleq");
  Require(IsGuestCallInstruction(0xFA000010U) && IsGuestCallInstruction(0xFB000010U), "blx immediate");
  Require(IsGuestCallInstruction(0xE12FFF33U), "blx register");
  Require(!IsGuestCallInstruction(0xE12FFF1EU), "bx lr is not a call");
  Require(!IsGuestCallInstruction(0xEA000010U), "b is not a call");
  Require(!IsGuestCallInstruction(0xE1A00000U), "mov is not a call");
}

void TestStackScan() {
  FakeMemory memory = MakeProcessMemory();
  const auto frames = Oot3dNativeGame::ScanGuestStackReturns(memory, kSp);
  Require(frames.size() == 2U, "two return addresses on the stack");
  Require(frames[0].Offset == 0x08U && frames[0].Address == kCode + 0x104U, "bl return, nearest first");
  Require(frames[1].Offset == 0x10U && frames[1].Address == kCode + 0x304U, "blx register return");
  Require(Oot3dNativeGame::ScanGuestStackReturns(memory, kSp, 1U).size() == 1U, "frame limit");
  // The scan stops at the end of sp's region (the shared block above is not
  // stack), and an unmapped sp has no frames.
  memory.Write32(kShared, kCode + 0x104U);
  Require(Oot3dNativeGame::ScanGuestStackReturns(memory, kShared - 4U).empty(), "scan stays in the stack");
  Require(Oot3dNativeGame::ScanGuestStackReturns(memory, 0x00200000U).empty(), "unmapped sp");
}

void TestReport() {
  FakeMemory memory = MakeProcessMemory();
  const std::string report = Oot3dNativeGame::FormatGuestFaultReport(memory, MakeFacts());
  Require(Contains(report, "guest fault report: thread=3 entry=0x00200000 exit=MemoryFault pc=0x004C0A8C "
                           "detail=0x10001000 fault_address=0x10001000 (unmapped)\n"),
          "header: thread, exit, fault address and its mapping");
  Require(Contains(report, "  r0=10000FFC r1=08000103 r2=08000109 r3=006F0072\n"), "registers r0-r3");
  Require(Contains(report, "  r12=0070000C sp=0FFFFE00 lr=00100104 r15=004C0A88\n"), "registers r12-r15");
  Require(Contains(report, "  cpsr=60000010 (nZCv mode=10) fpscr=03C00010 tls=1FF82200\n"), "cpsr and fpscr");
  Require(Contains(report, "  stack: sp in primary_stack rw [0FFFC000-10000000)\n"), "which stack");
  Require(Contains(report, "  backtrace: pc=004C0A8C lr=00100104 sp+8=00100104 sp+10=00100304\n"), "backtrace");
  Require(Contains(report, "  area: scene=0x55 room=2\n") && !Contains(report, "spot04"),
          "area as numbers, never the scene's name");
  Require(Contains(report, "  other thread=0 entry=0x00100000 waiting pc=00123456 lr=00100104 sp=0FFFF000\n"),
          "other threads");
  // The fault address has its own window: the stack's last bytes are not
  // there, the unmapped page shows as --.
  Require(Contains(report, "  memory fault=10001000 unmapped\n"
                           "    10000FE0: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  |................|\n"),
          "fault window");
  Require(Contains(report, "    10001000: -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --  |                |\n"),
          "unmapped bytes");
  Require(Contains(report, "  memory r0=10000FFC shared memory rw [10000000-10001000)\n    10000FD0:"),
          "what the store pointer points into");
  // r1 and r2 share one window: the text with the bad escape, hex and ASCII.
  Require(Contains(report, "  memory r1,r2=08000103 heap rw [08000000-08010000)\n"), "shared window");
  Require(Contains(report, "    08000100: 48 69 7F 0E 01 02 03 04 05 7F 41 62 63 00 00 00  |Hi........Abc...|\n"),
          "text bytes");
  Require(Contains(report, "  memory sp=0FFFFE00 primary_stack"), "sp window");
  Require(Contains(report, "  stack from sp=0FFFFE00\n    0FFFFE00: 78 56 34 12"), "stack dump from sp");
  // Unmapped values and code (lr) get no dump.
  Require(!Contains(report, "memory r3") && !Contains(report, "memory lr"), "no dump for unmapped values or code");
  Require(!report.empty() && report.back() == '\n', "lines end in a newline");

  // Another exit: no fault address; outside gameplay the area is "-".
  FakeMemory quiet = MakeProcessMemory();
  quiet.Write32(Oot3dNativeGame::kNativeGameStatePointer, 0U);
  auto facts = MakeFacts();
  facts.Exit = {oot3d::recomp::a32::ExitKind::Unsupported, 0x00100200U,
                oot3d::recomp::a32::FallbackReason::Core, 0U};
  facts.State.r[14] = kCode + 0x204U;
  const std::string other = Oot3dNativeGame::FormatGuestFaultReport(quiet, facts);
  Require(Contains(other, "exit=Unsupported pc=0x00100200 detail=0x00000000 fallback=1\n"), "unsupported exit");
  Require(!Contains(other, "fault_address") && !Contains(other, "memory fault="), "no fault address");
  Require(Contains(other, "lr=00100204(not after a call)"), "lr that is not a return address");
  Require(Contains(other, "  area: scene=- room=-\n"), "outside gameplay");
}

void TestPerfTrail() {
  using Oot3dNativeGame::PerfLineValue;
  const std::string dkperf = "dkperf host_frames=4950 frames=150 gpu_ms=89.3528 busy_ms=89.3405 "
                             "busy_max_ms=133.172 fence_cpu_ms=80.3612 fence_cpu_max_ms=126.2";
  Require(PerfLineValue(dkperf, "busy_ms") == 89.3405, "busy_ms");
  Require(PerfLineValue(dkperf, "fence_cpu_ms") == 80.3612, "fence_cpu_ms, not fence_cpu_max_ms");
  Require(PerfLineValue(dkperf, "fence_cpu_max_ms") == 126.2, "last key");
  Require(PerfLineValue(dkperf, "ms") == -1.0 && PerfLineValue(dkperf, "max_ms") == -1.0, "whole keys only");
  Require(PerfLineValue("", "busy_ms") == -1.0, "no dkperf line (GL)");

  Oot3dNativeGame::GuestFaultPerfTrail trail;
  Require(trail.Format() == "  perf: (none)\n", "nothing recorded");
  for (uint64_t window = 1U; window <= 4U; ++window)
    trail.Record({window * 150U, 30.0 / window, 20.0 + window, 7.0, 33.3 * window, 10.0 * window, -1.0});
  const std::string text = trail.Format();
  Require(!Contains(text, "host_frames=150 "), "only the last three windows");
  Require(Contains(text, "  perf: host_frames=300 fps=15.0 guest_ms=22.0 present_ms=7.0 total_ms=66.6 "
                         "gpu_busy_ms=20.0 gpu_fence_ms=-1.0\n  perf: host_frames=450 "),
          "oldest first");
  Require(Contains(text, "  perf: host_frames=600 fps=7.5 guest_ms=24.0 present_ms=7.0 total_ms=133.2 "
                         "gpu_busy_ms=40.0 gpu_fence_ms=-1.0\n"),
          "newest last");
}

} // namespace

int main() {
  TestRegions();
  TestCallInstructions();
  TestStackScan();
  TestReport();
  TestPerfTrail();
  std::puts("guest fault report: PASS");
  return 0;
}
