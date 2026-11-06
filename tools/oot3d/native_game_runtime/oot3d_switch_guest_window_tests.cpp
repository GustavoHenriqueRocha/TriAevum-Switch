// fastmem window, phase 1 (oot3d_switch_guest_window.h, config/fastmem_window,
// SWITCH_PORT.md "fastmem window"), on the host backend (mmap reservation,
// MAP_FIXED slabs, the operator new/delete hook): every region of the owner
// image at window base + guest address, the same after copies, moves,
// restores, CreatePrimaryThread staging and TLS pages, the flat page-flag and
// card tables equal to the page entries, the fallbacks (switch absent,
// refused mapping, a range held by another image), and guest code on two host
// threads while slabs are mapped and savestates re-home the image.
//
// --stress: longer two-core runs.
#include "oot3d_native_a32_ctr_host.h"
#include "oot3d_native_a32_second_core.h"
#include "oot3d_switch_guest_window.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace Oot3dNativeGame;
namespace Window = Oot3dNativeGame::GuestWindow;
using oot3d::recomp::a32::Condition;
using oot3d::recomp::a32::EncodeMetadata;
using oot3d::recomp::a32::GuestState;
using oot3d::recomp::a32::Opcode;
using oot3d::recomp::a32::PackedOp;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_switch_guest_window_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

void Reset(Window::Mode mode, size_t failSlabsAfter = SIZE_MAX) {
    Require(Window::ResetForTesting(mode, failSlabsAfter),
            "window reset (a live placement left by the previous test?)");
}

const uint8_t* Bytes(const NativeA32Memory& memory, uint32_t address) {
    return memory.SynchronousReadView(address, 1U).data();
}

bool AtHome(const NativeA32Memory& memory, uint32_t address) {
    return Window::Base() != nullptr && Bytes(memory, address) == Window::Base() + address;
}

void RequireNoMismatch(std::string_view where) {
    const auto status = Window::CurrentStatus();
    Require(status.ShadowMismatches == 0U,
            std::string(where) + ": flat tables differ from the page entries (" +
                std::to_string(status.ShadowMismatches) + ")");
}

// The game's layout (EUR): text, read-only data, data+bss, heap, primary
// stack, GSP and HID shared memory, linear heap, VRAM, DSP RAM, config
// memory, shared page, TLS.
struct LayoutRegion {
    const char* Name;
    uint32_t Base;
    uint32_t Size;
    bool Writable;
    bool Executable;
};
constexpr std::array<LayoutRegion, 13> kLayout{{
    {"text", 0x00100000U, 0x003CA000U, false, true},
    {"rodata", 0x004CA000U, 0x0002F000U, false, false},
    {"data", 0x004F9000U, 0x000CE000U, true, false},
    {"heap", 0x08000000U, 0x00400000U, true, false},
    {"primary_stack", 0x0FFFC000U, 0x00004000U, true, false},
    {"gsp_shared", 0x10002000U, 0x00001000U, true, false},
    {"hid_shared", 0x10000000U, 0x00001000U, false, false},
    {"linear", 0x14000000U, 0x00800000U, true, false},
    {"vram", 0x1F000000U, 0x00600000U, true, false},
    {"dsp_ram", 0x1FF00000U, 0x00080000U, true, false},
    {"config_memory", 0x1FF80000U, 0x00001000U, false, false},
    {"shared_page", 0x1FF81000U, 0x00001000U, false, false},
    {"tls", 0x1FF82000U, 0x00001000U, true, false},
}};
// Distinct 2 MiB slabs of kLayout: 0-6 MiB (3), heap (2), stack (1), the
// shared memory at 0x10000000 (1), linear (4), VRAM (3), 0x1FE00000 (1).
constexpr size_t kLayoutSlabs = 15U;

void MapLayout(NativeA32Memory& memory, const std::vector<uint8_t>& text) {
    std::string error;
    for (const auto& region : kLayout) {
        NativeA32MemoryRegionConfig config{region.Name, region.Base, region.Size, region.Writable,
                                           region.Executable, {}};
        if (std::string_view(region.Name) == "text") config.InitialBytes = text;
        Require(memory.MapRegion(config, &error), error);
    }
}

std::vector<uint8_t> TextPattern() {
    std::vector<uint8_t> text(0x10000U);
    for (size_t index = 0; index < text.size(); ++index)
        text[index] = static_cast<uint8_t>(index * 7U + 3U);
    return text;
}

// --- switch absent -------------------------------------------------------------

void TestAbsent() {
    Reset(Window::Mode::Off);
    Require(!Window::Accepting() && Window::Base() == nullptr, "off: no reservation");
    NativeA32Memory memory;
    MapLayout(memory, TextPattern());
    for (const auto& region : kLayout) {
        Require(!Window::Contains(Bytes(memory, region.Base)),
                std::string("off: ") + region.Name + " is an ordinary heap buffer");
    }
    Require(Window::LivePlacements() == 0U, "off: no placement");
    NativeA32Memory copy = memory;
    NativeA32Memory moved = std::move(copy);
    Require(!Window::Contains(Bytes(moved, 0x08000000U)) && Window::LivePlacements() == 0U,
            "off: copies and moves stay on the heap");
    const std::string line = Window::FormatStatus(Window::CurrentStatus());
    Require(line == "fastmem_window: off (off for this test)", "off line: " + line);
    std::cout << "  absent: " << line << '\n';
}

// --- placement -----------------------------------------------------------------

void TestPlacement() {
    Reset(Window::Mode::Verify);
    Require(Window::Accepting(), "window on");
    Require((reinterpret_cast<uintptr_t>(Window::Base()) & (Window::kSlabSize - 1U)) == 0U,
            "base on a 2 MiB boundary");
    const auto text = TextPattern();
    NativeA32Memory memory;
    MapLayout(memory, text);
    Require(Window::IsOwner(&memory), "the first image claims the vacant window");
    for (const auto& region : kLayout) {
        Require(AtHome(memory, region.Base) && AtHome(memory, region.Base + region.Size - 1U),
                std::string(region.Name) + " at window base + address");
    }
    Require(std::memcmp(Window::Base() + 0x00100000U, text.data(), text.size()) == 0,
            "initial bytes in the window");
    // Inline fast paths read and write the window bytes.
    uint32_t word = 0;
    Require(memory.ReadFast<uint32_t>(0x00100004U, &word) &&
                std::memcmp(&word, text.data() + 4, 4) == 0,
            "fast read of the text");
    Require(memory.WriteFast<uint32_t>(0x08000010U, 0xA1B2C3D4U), "fast write of the heap");
    uint32_t stored = 0;
    std::memcpy(&stored, Window::Base() + 0x08000010U, 4);
    Require(stored == 0xA1B2C3D4U, "the store reached the window");
    Require(!memory.WriteFast<uint32_t>(0x00100000U, 1U), "text stays read-only");

    const auto status = Window::CurrentStatus();
    Require(status.On && status.Regions == kLayout.size() && status.Slabs == kLayoutSlabs,
            "status: " + Window::FormatStatus(status));
    const uint8_t* flags = Window::PageFlags();
    Require(flags[0x00100000U >> 12U] ==
                (Window::kPageMapped | Window::kPageReadOnly | Window::kPageStoreTrap),
            "text page: mapped, read-only, stores trap");
    Require(flags[0x08000000U >> 12U] == Window::kPageMapped,
            "heap page: direct (" + std::to_string(flags[0x08000000U >> 12U]) + ")");
    Require(flags[0x00000000U] == Window::kPageUnmapped, "page 0: unmapped");
    Require(flags[0x1FF81000U >> 12U] ==
                (Window::kPageMapped | Window::kPageReadOnly | Window::kPageStoreTrap),
            "shared page: read-only");
    RequireNoMismatch("placement");
    Require(status.ShadowChecks != 0U, "verify mode checked the tables");
    std::cout << "  placement: " << Window::FormatStatus(status) << '\n';
}

// --- re-homing -----------------------------------------------------------------

void RequireLayoutHome(const NativeA32Memory& memory, std::string_view where) {
    for (const auto& region : kLayout) {
        Require(AtHome(memory, region.Base),
                std::string(where) + ": " + region.Name + " back at window base + address");
    }
}

void TestRehome() {
    Reset(Window::Mode::Verify);
    NativeA32Memory memory;
    MapLayout(memory, TextPattern());
    Require(memory.Write32(0x08000100U, 0x11111111U), "write");
    // Copy construction: an independent image on the heap.
    NativeA32Memory snapshot = memory;
    Require(!Window::Contains(Bytes(snapshot, 0x08000000U)), "a copy is not in the window");
    Require(memory.Write32(0x08000100U, 0x22222222U), "write");
    // Copy assignment into the owner (a savestate load from memory).
    memory = snapshot;
    RequireLayoutHome(memory, "copy assignment");
    uint32_t value = 0;
    Require(memory.Read32(0x08000100U, &value) && value == 0x11111111U, "copied bytes");
    Require(std::memcmp(Window::Base() + 0x08000100U, &value, 4) == 0, "copied into the window");
    Require(Window::LivePlacements() == kLayout.size(), "the old buffers were released");

    // Move assignment from a staged image (CreatePrimaryThread, RestoreState).
    NativeA32Memory staged = memory;
    std::string error;
    Require(staged.MapRegion({"staged", 0x0FF00000U, 0x4000U, true, false, {}}, &error), error);
    Require(staged.Write32(0x0FF00010U, 0x33333333U), "staged write");
    Require(!Window::Contains(Bytes(staged, 0x0FF00000U)), "staged image on the heap");
    memory = std::move(staged);
    RequireLayoutHome(memory, "move assignment");
    Require(AtHome(memory, 0x0FF00000U), "the staged region at home");
    Require(memory.Read32(0x0FF00010U, &value) && value == 0x33333333U, "moved bytes");
    Require(memory.WriteFast<uint32_t>(0x0FF00010U, 0x44444444U), "fast write after the move");
    std::memcpy(&value, Window::Base() + 0x0FF00010U, 4);
    Require(value == 0x44444444U, "page table points at the window after the move");
    Require(Window::LivePlacements() == kLayout.size() + 1U, "one placement per region");

    // RestoreState (decoded into a staged image, then moved).
    const auto state = memory.CaptureState();
    Require(memory.Write32(0x14000000U, 0x55555555U), "write");
    Require(memory.RestoreState(state, &error), error);
    RequireLayoutHome(memory, "RestoreState");
    Require(memory.Read32(0x14000000U, &value) && value == 0U, "restored bytes");
    Require(Window::LivePlacements() == kLayout.size() + 1U, "restore released the old buffers");
    RequireNoMismatch("re-homing");
    std::cout << "  re-homing: copy, move, restore: ok\n";
}

// --- another image holding window buffers ----------------------------------------

void TestConflicts() {
    Reset(Window::Mode::On);
    NativeA32Memory owner;
    std::string error;
    Require(owner.MapRegion({"data", 0x00200000U, 0x1000U, true, false, {}}, &error), error);
    Require(AtHome(owner, 0x00200000U), "owner placed");
    // A second image while the owner's buffers live: the heap.
    NativeA32Memory other;
    Require(other.MapRegion({"data", 0x00200000U, 0x1000U, true, false, {}}, &error), error);
    Require(!Window::IsOwner(&other) && !Window::Contains(Bytes(other, 0x00200000U)),
            "a second image stays on the heap");
    // Moved out of the owner: the buffer stays valid where it is, and the
    // owner cannot place over it.
    NativeA32Memory holder = std::move(owner);
    Require(AtHome(holder, 0x00200000U), "the moved-out buffer is still the window's");
    Require(owner.MapRegion({"data", 0x00200000U, 0x1000U, true, false, {}}, &error), error);
    Require(!Window::Contains(Bytes(owner, 0x00200000U)), "no overlapping placement");
    Require(Window::CurrentStatus().Conflicts == 1U, "the refusal is counted");
    holder = NativeA32Memory();
    Require(Window::LivePlacements() == 0U, "released with its image");
    std::cout << "  conflicts: ok\n";
}

// --- a refused mapping ---------------------------------------------------------------

void TestRefusedMapping() {
    Reset(Window::Mode::On, 1U);
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"low", 0x00200000U, 0x1000U, true, false, {}}, &error), error);
    Require(AtHome(memory, 0x00200000U), "the first slab maps");
    Require(memory.MapRegion({"high", 0x08000000U, 0x1000U, true, false, {}}, &error), error);
    Require(!Window::Contains(Bytes(memory, 0x08000000U)), "refused slab: today's allocation");
    Require(memory.Write32(0x08000000U, 7U) && memory.Write32(0x00200000U, 8U),
            "both regions work");
    const auto status = Window::CurrentStatus();
    const std::string line = Window::FormatStatus(status);
    Require(!status.On && line.find("refused (test)") != std::string::npos &&
                line.find("regions_in_window=1") != std::string::npos,
            "refusal line: " + line);
    // Nothing new is placed afterwards.
    Require(memory.MapRegion({"later", 0x00201000U, 0x1000U, true, false, {}}, &error), error);
    Require(!Window::Contains(Bytes(memory, 0x00201000U)), "no placement after a refusal");
    std::cout << "  refused mapping: " << line << '\n';
}

// --- flat tables -----------------------------------------------------------------

void TestShadowTables() {
    Reset(Window::Mode::Verify);
    NativeA32Memory memory;
    std::string error;
    Require(memory.MapRegion({"text", 0x00100000U, 0x4000U, false, true, {}}, &error) &&
                memory.MapRegion({"data", 0x00200000U, 0x8000U, true, false, {}}, &error) &&
                memory.MapRegion({"odd", 0x00300800U, 0x1000U, true, false, {}}, &error),
            error);
    const uint8_t* flags = Window::PageFlags();
    const uint8_t* cards = Window::Cards();
    constexpr size_t kData = 0x00200000U >> 12U;
    Require(flags[0x00300000U >> 12U] == Window::kPageUnmapped &&
                flags[0x00301000U >> 12U] == Window::kPageUnmapped,
            "pages a region covers partly have no direct entry");
    // Observed writes: stores trap.
    memory.SetWriteObserver([](uint32_t, size_t) {});
    Require(memory.ObserveWrites(0x00201000U, 0x10U), "observe");
    Require(flags[kData + 1U] == (Window::kPageMapped | Window::kPageStoreTrap) &&
                flags[kData] == Window::kPageMapped,
            "observed page: stores trap");
    // Deferred device writes: the page leaves the table until read back.
    const std::array ranges{std::pair<uint32_t, size_t>{0x00203000U, 0x100U}};
    Require(memory.DeferWrites(ranges,
                               [](uint32_t, std::span<uint8_t> out) {
                                   std::fill(out.begin(), out.end(), 0x5AU);
                                   return true;
                               }),
            "defer");
    Require(flags[kData + 3U] == Window::kPageUnmapped, "deferred page: unmapped");
    Require(memory.MaterializeDeferredWrites() && flags[kData + 3U] == Window::kPageMapped,
            "read back: direct again");
    Require(Window::Base()[0x00203000U] == 0x5AU, "device bytes in the window");
    // Range watch: rebuilt table.
    const std::array watch{std::pair<uint32_t, uint32_t>{0x00204000U, 0x00205000U}};
    memory.SetRangeWatch(watch);
    Require(flags[kData + 4U] == Window::kPageUnmapped, "watched page traps");
    memory.ClearRangeWatch();
    Require(flags[kData + 4U] == Window::kPageMapped, "watch cleared");
    // Write tracing: every typed access traps.
    memory.EnableWriteTraceFingerprint(true);
    Require(flags[kData] == (Window::kPageMapped | Window::kPageLoadTrap | Window::kPageStoreTrap),
            "traced page: loads and stores trap");
    memory.EnableWriteTraceFingerprint(false);
    Require(flags[kData] == Window::kPageMapped, "trace off");
    // Cards: set by the checked write paths, taken by RangeWriteGeneration.
    Require(memory.RangeWriteGeneration(0x00200000U, 0x8000U).has_value(), "certificate");
    Require(cards[kData + 2U] == Window::kCardClean, "clean after a certificate");
    const std::array<uint8_t, 4> bytes{1, 2, 3, 4};
    Require(memory.WriteBytes(0x00202000U, bytes), "checked write");
    Require(cards[kData + 2U] == Window::kCardDirty, "a checked write marks the flat card");
    Require(memory.Fill(0x00205000U, 0x10U, 9U) && cards[kData + 5U] == Window::kCardDirty,
            "a fill marks it");
    Require(memory.RangeWriteGeneration(0x00202000U, 0x4000U).has_value() &&
                cards[kData + 2U] == Window::kCardClean && cards[kData + 5U] == Window::kCardClean,
            "a certificate takes it");
    // An inline store reaches only the group card (phase 1); the invariant
    // (flat dirty implies group dirty) holds.
    Require(memory.WriteFast<uint32_t>(0x00206000U, 1U) && cards[kData + 6U] == Window::kCardClean,
            "inline stores leave the flat card alone in phase 1");
    memory.CaptureState();  // a checkpoint
    RequireNoMismatch("flat tables");
    const auto status = Window::CurrentStatus();
    Require(status.ShadowChecks >= 8U, "checked after each change");
    std::cout << "  flat tables: " << status.ShadowChecks << " checks, 0 mismatches\n";
}

// --- the process: staging, TLS pages, savestates, two host threads ----------------

constexpr uint32_t Ldr(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE5900000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t Ldrex(uint32_t rt, uint32_t rn) {
    return 0xE1900F9FU | rn << 16U | rt << 12U;
}
constexpr uint32_t Strex(uint32_t rd, uint32_t rt, uint32_t rn) {
    return 0xE1800F90U | rn << 16U | rd << 12U | rt;
}
constexpr uint32_t AddImm(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE2800000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t SubsImm(uint32_t rd, uint32_t rn, uint32_t imm) {
    return 0xE2500000U | rn << 16U | rd << 12U | imm;
}
constexpr uint32_t CmpImm(uint32_t rn, uint32_t imm) {
    return 0xE3500000U | rn << 16U | imm;
}

class Program {
  public:
    explicit Program(uint32_t base) : mBase(base) {}
    uint32_t Here() const { return mBase + static_cast<uint32_t>(mOps.size()) * 4U; }
    void Op(uint32_t raw) { mOps.push_back({raw, EncodeMetadata(Opcode::Core, Condition::Al)}); }
    void Branch(Condition condition, uint32_t target) {
        const uint32_t offset = ((target - Here() - 8U) >> 2U) & 0x00FFFFFFU;
        mOps.push_back({(static_cast<uint32_t>(condition) << 28U) | 0x0A000000U | offset,
                        EncodeMetadata(Opcode::Branch, condition)});
    }
    void Svc(uint32_t immediate) {
        mOps.push_back({0xEF000000U | immediate, EncodeMetadata(Opcode::Svc, Condition::Al)});
    }
    oot3d::recomp::a32::Registry Seal() {
        if (mBlocks.empty()) {
            for (size_t index = 0; index < mOps.size(); ++index)
                mBlocks.push_back({mBase + static_cast<uint32_t>(index) * 4U, &mOps[index], 1U});
            mShard = {mBase, Here() - 4U, mBlocks.data(), static_cast<uint32_t>(mBlocks.size())};
        }
        return {&mShard, 1U, nullptr, 0U};
    }

  private:
    uint32_t mBase;
    std::vector<PackedOp> mOps;
    std::vector<oot3d::recomp::a32::Block> mBlocks;
    oot3d::recomp::a32::BlockShard mShard{};
};

std::deque<Program>& Programs() {
    static std::deque<Program> programs;
    return programs;
}

constexpr uint32_t kText = 0x00100000U;
constexpr uint32_t kData = 0x00200000U;
constexpr uint32_t kSecondStack = 0x00200F00U;
constexpr uint32_t kPrimaryStack = 0x00300000U;
constexpr uint32_t kTls = 0x00400000U;
constexpr uint32_t kBudget = 64U;
constexpr uint32_t kSvcExitThread = 0x09U;

NativeA32CtrHostConfig MakeConfig() {
    NativeA32CtrHostConfig config;
    config.ResourceLimitValues = {0x18, 0x04000000, 0x20, 0x20, 0x20, 0x08, 0x08, 0x10, 0x02, 0};
    config.ResourceCurrentValues[1] = 0x1000;
    config.ResourceCurrentValues[2] = 1;
    return config;
}

struct Guest {
    explicit Guest(oot3d::recomp::a32::Registry programRegistry)
        : registry(programRegistry), process(registry, host) {}

    void Boot(uint32_t primaryEntry) {
        static const std::array<uint8_t, 0x1000> text{};
        std::string error;
        Require(process.MapRegion({"text", kText, text.size(), false, true, text}, &error) &&
                    process.MapRegion({"data", kData, 0x1000, true, false, {}}, &error) &&
                    process.CreatePrimaryThread({primaryEntry, kPrimaryStack, 0x1000, kTls,
                                                 0x1000, 0, kData, 0x10U, 0x03C00000U, 0x30U},
                                                &error),
                error);
    }

    uint32_t CreateThread(uint32_t entry, int32_t processor, uint32_t argument) {
        GuestState state{};
        state.r[0] = 0x18U;
        state.r[1] = entry;
        state.r[2] = argument;
        state.r[3] = kSecondStack;
        state.r[4] = static_cast<uint32_t>(processor);
        NativeA32HostContext context{process, 0};
        const auto result = host.HandleSvc(0x08U, state, process.Memory(), context);
        Require(result.Action == NativeA32HostAction::Resume && state.r[0] == 0U,
                "CreateThread failed");
        return static_cast<uint32_t>(process.ThreadCount() - 1U);
    }

    uint32_t Read32(uint32_t address) {
        uint32_t value = 0;
        Require(process.Memory().Read32(address, &value), "guest read failed");
        return value;
    }

    oot3d::recomp::a32::Registry registry;
    NativeA32CtrHostServices host{MakeConfig()};
    NativeA32Process process;
};

// r0 = counter address; [r0 + 4] iterations; LDREX/STREX increments, exit.
uint32_t EmitCounter(Program& program) {
    const uint32_t entry = program.Here();
    program.Op(Ldr(3, 0, 4));
    const uint32_t loop = program.Here();
    program.Op(Ldrex(1, 0));
    program.Op(AddImm(1, 1, 1));
    program.Op(Strex(2, 1, 0));
    program.Op(CmpImm(2, 0));
    program.Branch(Condition::Ne, loop);
    program.Op(SubsImm(3, 3, 1));
    program.Branch(Condition::Ne, loop);
    program.Svc(kSvcExitThread);
    return entry;
}

void TestProcess() {
    Reset(Window::Mode::Verify);
    Program& program = Programs().emplace_back(kText);
    const uint32_t counter = EmitCounter(program);
    const uint32_t exit = program.Here();
    program.Svc(kSvcExitThread);
    {
        Guest guest(program.Seal());
        Require(Window::IsOwner(&guest.process.Memory()), "the process claims its image");
        guest.Boot(counter);
        // CreatePrimaryThread staged a copy with the stack and TLS and moved it in.
        for (const uint32_t address : {kText, kData, kPrimaryStack, kTls}) {
            Require(AtHome(guest.process.Memory(), address),
                    "process region at window base + address after staging");
        }
        Require(guest.process.Memory().Write32(kData + 4U, 100U), "iterations");
        // Eight more threads: the ninth TLS slot maps another page.
        for (int thread = 0; thread < 8; ++thread) guest.CreateThread(exit, 0, kData);
        Require(AtHome(guest.process.Memory(), kTls + 0x1000U), "the extra TLS page at home");
        for (int run = 0; run < 400; ++run) guest.process.Run(kBudget);
        Require(guest.Read32(kData) == 100U, "guest counter on window memory");
        uint32_t direct = 0;
        std::memcpy(&direct, Window::Base() + kData, 4);
        Require(direct == 100U, "the guest's stores are in the window");
        // Savestate restore: staged decode, moved in, re-homed.
        const auto state = guest.process.CaptureState();
        Require(guest.process.Memory().Write32(kData, 5U), "write");
        std::string error;
        Require(guest.process.RestoreState(state, &error), error);
        Require(AtHome(guest.process.Memory(), kData) &&
                    AtHome(guest.process.Memory(), kTls + 0x1000U) && guest.Read32(kData) == 100U,
                "restored image at home");
        RequireNoMismatch("process");
    }
    Require(Window::LivePlacements() == 0U, "a destroyed process releases its buffers");
    std::cout << "  process: staging, TLS page, savestate: ok\n";
}

// The game thread's side of a two-core run (as oot3d_native_a32_second_core_tests).
class TwoCores {
  public:
    explicit TwoCores(NativeA32Process& process) : mProcess(process) {
        mProcess.EnableSecondCore();
        SecondCore::LockKernel();
        mRunner = std::thread([this] { mProcess.RunSecondCore(kBudget); });
    }
    ~TwoCores() { Stop(); }
    void Stop() {
        if (!mRunner.joinable()) return;
        mProcess.RequestSecondCoreStop();
        {
            const SecondCore::KernelRelease release;
            mRunner.join();
        }
        mProcess.DisableSecondCore();
        SecondCore::UnlockKernel();
    }

  private:
    NativeA32Process& mProcess;
    std::thread mRunner;
};

bool Terminated(const NativeA32Process& process, uint32_t id) {
    return process.ThreadStatus(id) == NativeA32ThreadStatus::Terminated;
}

// Two host threads count one word with LDREX/STREX while the game thread maps
// regions in new slabs and saves and restores the image (each under the
// second core's world stop, as the window session does): no update lost
// across a restore, every buffer at home afterwards.
void TestTwoCores(uint32_t increments) {
    Reset(Window::Mode::On);
    Program& program = Programs().emplace_back(kText);
    const uint32_t counter = EmitCounter(program);
    {
        Guest guest(program.Seal());
        guest.Boot(counter);
        Require(guest.process.Memory().Write32(kData + 4U, increments), "iterations");
        TwoCores cores(guest.process);
        const uint32_t id = guest.CreateThread(counter, 1, kData);
        uint32_t nextSlab = 0x02000000U;
        uint32_t restores = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(20);
        while (!(Terminated(guest.process, 0) && Terminated(guest.process, id))) {
            const auto result = guest.process.Run(kBudget);
            Require(result.Kind != NativeA32ProcessRunKind::Faulted, result.Error);
            if (result.Kind != NativeA32ProcessRunKind::Yielded) {
                SecondCore::WaitKernelFor(std::chrono::milliseconds(1), [] { return false; });
            }
            if ((restores & 1U) == 0U && nextSlab < 0x04000000U) {
                std::string error;
                Require(guest.process.Memory().MapRegion(
                            {"commit", nextSlab, 0x1000U, true, false, {}}, &error),
                        error);
                nextSlab += Window::kSlabSize;
            }
            {
                const SecondCore::WorldStop stop;
                const auto state = guest.process.CaptureState();
                std::string error;
                Require(guest.process.RestoreState(state, &error), error);
            }
            ++restores;
            Require(std::chrono::steady_clock::now() < deadline, "two-core run did not finish");
        }
        cores.Stop();
        Require(guest.Read32(kData) == 2U * increments,
                "an update was lost across the window's re-homing");
        Require(AtHome(guest.process.Memory(), kData) && AtHome(guest.process.Memory(), 0x02000000U),
                "every buffer at home after the run");
        std::cout << "  two cores: " << 2U * increments << " increments, " << restores
                  << " restores, " << Window::CurrentStatus().Slabs << " slabs\n";
    }
    Require(Window::LivePlacements() == 0U, "released");
}

void TestStatusLine() {
    Window::Status status;
    status.On = true;
    status.Base = 0x8000000000ULL;
    status.Regions = 17;
    status.Slabs = 38;
    status.RegionBytes = 70U << 20U;
    status.Route = "direct";
    Require(Window::FormatStatus(status) ==
                "fastmem_window: on base=0x8000000000 regions=17 mapped_kb=77824 slabs=38 "
                "region_kb=71680 route=direct",
            "on line: " + Window::FormatStatus(status));
    status.On = false;
    status.Regions = 0;
    status.Reason = "no process handle (emulator or no loader)";
    Require(Window::FormatStatus(status) ==
                "fastmem_window: off (no process handle (emulator or no loader))",
            "off line");
    Require(Window::ParseMode("") == Window::Mode::On && Window::ParseMode(" verify\n") ==
                Window::Mode::Verify && Window::ParseMode("off") == Window::Mode::Off &&
                Window::ParseMode("0\n") == Window::Mode::Off,
            "config text");
}

} // namespace

int main(int argc, char** argv) {
    if (!Window::ResetForTesting(Window::Mode::Off)) {
        std::cout << "oot3d_switch_guest_window_tests: no host window here\n";
        return 77;
    }
    const bool stress = argc == 2 && std::string_view(argv[1]) == "--stress";
    TestStatusLine();
    TestAbsent();
    TestPlacement();
    TestRehome();
    TestConflicts();
    TestRefusedMapping();
    TestShadowTables();
    TestProcess();
    TestTwoCores(stress ? 200'000U : 5'000U);
    Reset(Window::Mode::Off);
    std::cout << "oot3d_switch_guest_window_tests: ok\n";
    return 0;
}
