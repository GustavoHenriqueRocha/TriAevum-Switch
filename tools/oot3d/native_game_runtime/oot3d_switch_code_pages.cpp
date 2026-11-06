// The NRO's code and read-only data on 2 MB blocks (Switch).
//
// Ported from sa2-nx (sdk/src/ui/switch_code_blocks.cpp, GPL-3.0, from
// kameo-nx v1.0.1; the idea and the measurements come from the Torchlight
// port): an instruction fetch that misses the TLB costs +115 ns on 4 KB pages
// and +7 ns on 2 MB blocks, a data read +81 ns and +5 ns; combat ran 2-4 %
// faster there. Our .text is 133.8 MB of whole-AOT code (3.4x sa2's), so the
// game thread misses the instruction TLB all the time.
//
// hbloader reads the NRO to the start of its heap and maps it with
// svcMapProcessCodeMemory at a random address, so virtual and physical
// addresses do not line up modulo 2 MB and the kernel maps 4 KB pages. Here
// the same bytes are copied to heap that does line up and mapped at the same
// virtual address: no pointer moves and the code does not know.
//
// What makes it safe:
//  - The code cannot unmap itself, so the swap runs from a page of its own
//    (kStub) and only uses registers and the stack.
//  - Nothing else runs the code meanwhile: priority 101 is the first
//    constructor priority a program may use (only libstdc++'s iostream set-up,
//    90, comes earlier and starts no thread: checked in the linked
//    .init_array; the PGO build registers its counters in a section, not with
//    libgcov constructors), libnx's start-up (__appInit)
//    starts no thread, and our other constructors (oot3d_switch_threads.cpp,
//    oot3d_switch_guard_alloc.cpp) run at 102, after this one.
//  - Text and read-only data have no relocations left to apply (crt0 applied
//    them to .data/.got before the constructors), so the copy equals the
//    original.
//  - svcUnmapProcessCodeMemory changes nothing unless it is given the pages
//    mapped there; a wrong guess of the loader's copy costs nothing.
//  - Text and read-only data are swapped one call each: a refused read-only
//    swap keeps the text on blocks and the data where it was.
//  - hbloader aborts if the NRO returns to it after a swap (its own unmap of
//    the original pages fails: hbl error 24/25). So a successful swap sets
//    __nx_applet_exit_mode = 1 for the whole process: every return from main
//    and every exit() ends the application through AM (as "Exit game" and the
//    fatal error dialog already did, oot3d_native_game.cpp).
//
// The cost: the loader's copy cannot be given back (it is at the start of the
// heap with .data after it), so text + read-only data take their size again
// (~138 MB of the ~3 GB an application has).
//
// An emulator runs the NRO without hbloader (no process handle): the code
// stays where it is and the swap is tested on two pages of its own instead.
//
// Kill switch: sdmc:/switch/oot3dre/config/no_code_hugepages. libnx's
// __appInit mounts sdmc (fsdevMountSdmc) before __libc_init_array runs the
// constructors, so the file is read directly here.
//
// The result goes to logs/oot3d_native_game.log once the logger exists
// (oot3d_demo_host_context.cpp) and code_2mb= is in every perf.txt window.
#if defined(__SWITCH__)

#include "oot3d_switch_code_pages.h"

#include <switch.h>

#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
// The executable's first instruction (crt0), relocated with the image. Not
// __start__, an absolute linker symbol (sa2-nx's first 1.0.1 build used it and
// the swap left the code where it was).
extern char _start;
// newlib's heap end (libnx __libnx_initheap); sbrk(0) is the current break.
extern char* fake_heap_end;
// libnx (applet.c, weak, default 0).
extern u32 __nx_applet_exit_mode;
}

namespace Oot3dNativeGame {
namespace {

// The layout kStub reads.
struct Segment {
    u64 Va;
    u64 OldSource;
    u64 NewSource;
    u64 Size;
    u64 Permission;
};
struct SwapArgs {
    u32 Process;
    u32 Count;
    Segment Segments[2];
};
static_assert(sizeof(Segment) == 40 && __builtin_offsetof(SwapArgs, Segments) == 8,
              "kStub reads this layout");

// u32 stub(SwapArgs*): for each segment, unmap the old copy, map the new one
// and set its permission; returns 0 or the kernel's result. If the new copy is
// refused the old one goes back, so the segment is always mapped when it
// returns; if that fails too nothing can run: brk. sa2-nx's bytes, checked with
// devkitA64's objdump. Data, not a function, so no compiler option can add a
// call into .text to it. 0x78 svcUnmapProcessCodeMemory(proc, dst, src, size),
// 0x77 svcMapProcessCodeMemory, 0x73 svcSetProcessMemoryPermission(proc, addr,
// size, perm).
const u32 kStub[] = {
    0xa9bd53f3,  //   0: stp x19, x20, [sp, #-48]!
    0xa9015bf5,  //   4: stp x21, x22, [sp, #16]
    0xf90013f7,  //   8: str x23, [sp, #32]
    0xaa0003f3,  //   c: mov x19, x0
    0xb9400274,  //  10: ldr w20, [x19]
    0xb9400675,  //  14: ldr w21, [x19, #4]
    0x91002276,  //  18: add x22, x19, #0x8
    // next
    0x34000595,  //  1c: cbz w21, cc <ok>
    0x2a1403e0,  //  20: mov w0, w20
    0xf94002c1,  //  24: ldr x1, [x22]
    0xf94006c2,  //  28: ldr x2, [x22, #8]
    0xf9400ec3,  //  2c: ldr x3, [x22, #24]
    0xd4000f01,  //  30: svc #0x78
    0x350004e0,  //  34: cbnz w0, d0 <out>
    0x2a1403e0,  //  38: mov w0, w20
    0xf94002c1,  //  3c: ldr x1, [x22]
    0xf9400ac2,  //  40: ldr x2, [x22, #16]
    0xf9400ec3,  //  44: ldr x3, [x22, #24]
    0xd4000ee1,  //  48: svc #0x77
    0x2a0003f7,  //  4c: mov w23, w0
    0x350001c0,  //  50: cbnz w0, 88 <put_back>
    0x2a1403e0,  //  54: mov w0, w20
    0xf94002c1,  //  58: ldr x1, [x22]
    0xf9400ec2,  //  5c: ldr x2, [x22, #24]
    0xf94012c3,  //  60: ldr x3, [x22, #32]
    0xd4000e61,  //  64: svc #0x73
    0x2a0003f7,  //  68: mov w23, w0
    0x340002a0,  //  6c: cbz w0, c0 <done_one>
    0x2a1403e0,  //  70: mov w0, w20
    0xf94002c1,  //  74: ldr x1, [x22]
    0xf9400ac2,  //  78: ldr x2, [x22, #16]
    0xf9400ec3,  //  7c: ldr x3, [x22, #24]
    0xd4000f01,  //  80: svc #0x78
    0x350002e0,  //  84: cbnz w0, e0 <dead>
    // put_back
    0x2a1403e0,  //  88: mov w0, w20
    0xf94002c1,  //  8c: ldr x1, [x22]
    0xf94006c2,  //  90: ldr x2, [x22, #8]
    0xf9400ec3,  //  94: ldr x3, [x22, #24]
    0xd4000ee1,  //  98: svc #0x77
    0x35000220,  //  9c: cbnz w0, e0 <dead>
    0x2a1403e0,  //  a0: mov w0, w20
    0xf94002c1,  //  a4: ldr x1, [x22]
    0xf9400ec2,  //  a8: ldr x2, [x22, #24]
    0xf94012c3,  //  ac: ldr x3, [x22, #32]
    0xd4000e61,  //  b0: svc #0x73
    0x35000160,  //  b4: cbnz w0, e0 <dead>
    0x2a1703e0,  //  b8: mov w0, w23
    0x14000005,  //  bc: b d0 <out>
    // done_one
    0x9100a2d6,  //  c0: add x22, x22, #0x28
    0x510006b5,  //  c4: sub w21, w21, #0x1
    0x17ffffd5,  //  c8: b 1c <next>
    // ok
    0x52800000,  //  cc: mov w0, #0x0
    // out
    0xf94013f7,  //  d0: ldr x23, [sp, #32]
    0xa9415bf5,  //  d4: ldp x21, x22, [sp, #16]
    0xa8c353f3,  //  d8: ldp x19, x20, [sp], #48
    0xd65f03c0,  //  dc: ret
    // dead
    0xd4200000,  //  e0: brk #0x0
};

// Constant-initialized: read later from any thread, never written after the
// constructor.
constinit CodePagesStatus gStatus{};

using SwapFunction = u32 (*)(SwapArgs*);

struct CodePage {
    void* Heap = nullptr;
    void* Va = nullptr;
};

u64 Microseconds(u64 fromTick, u64 toTick) {
    return armTicksToNs(toTick - fromTick) / 1000U;
}

// A page of code from the heap, mapped at a free address of the code region.
bool MapCodePage(Handle process, const void* code, size_t size, CodePage* out) {
    void* page = memalign(kCodePagesPage, kCodePagesPage);
    if (page == nullptr) return false;
    std::memset(page, 0, kCodePagesPage);
    std::memcpy(page, code, size);
    armDCacheFlush(page, kCodePagesPage);
    virtmemLock();
    void* va = virtmemFindCodeMemory(kCodePagesPage, kCodePagesPage);
    Result rc = va != nullptr
                    ? svcMapProcessCodeMemory(process, reinterpret_cast<u64>(va),
                                              reinterpret_cast<u64>(page), kCodePagesPage)
                    : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    virtmemUnlock();
    if (R_FAILED(rc)) {
        std::free(page);
        return false;
    }
    rc = svcSetProcessMemoryPermission(process, reinterpret_cast<u64>(va), kCodePagesPage, Perm_Rx);
    if (R_FAILED(rc)) {
        svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(va), reinterpret_cast<u64>(page),
                                  kCodePagesPage);
        std::free(page);
        return false;
    }
    out->Heap = page;
    out->Va = va;
    return true;
}

void UnmapCodePage(Handle process, const CodePage& page) {
    svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(page.Va),
                              reinterpret_cast<u64>(page.Heap), kCodePagesPage);
    std::free(page.Heap);
}

void Fallback(const char* reason, u64 value) {
    gStatus.State = CodePagesState::Fallback;
    gStatus.Reason = reason;
    gStatus.Value = value;
}

// No loader: the swap on two pages of its own (the normal path only; sa2-nx
// found that Ryujinx accepts swaps the console refuses, so the refusal paths
// cannot be tested there).
void SelfTest(Handle process) {
    static const u32 kReturns1[] = {0x52800020, 0xd65f03c0};  // mov w0, #1; ret
    static const u32 kReturns2[] = {0x52800040, 0xd65f03c0};  // mov w0, #2; ret
    gStatus.State = CodePagesState::SelfTest;
    CodePage stub;
    CodePage code;
    if (!MapCodePage(process, kStub, sizeof(kStub), &stub)) {
        gStatus.Reason = "no page for the swap";
        return;
    }
    if (!MapCodePage(process, kReturns1, sizeof(kReturns1), &code)) {
        gStatus.Reason = "no page to swap";
        UnmapCodePage(process, stub);
        return;
    }
    const auto run = reinterpret_cast<int (*)()>(code.Va);
    void* second = run() == 1 ? memalign(kCodePagesPage, kCodePagesPage) : nullptr;
    if (second == nullptr) {
        gStatus.Reason = "the first page does not run, or no second page";
        UnmapCodePage(process, code);
        UnmapCodePage(process, stub);
        return;
    }
    std::memset(second, 0, kCodePagesPage);
    std::memcpy(second, kReturns2, sizeof(kReturns2));
    armDCacheFlush(second, kCodePagesPage);
    SwapArgs args{};
    args.Process = process;
    args.Count = 1;
    args.Segments[0] = {reinterpret_cast<u64>(code.Va), reinterpret_cast<u64>(code.Heap),
                        reinterpret_cast<u64>(second), kCodePagesPage, Perm_Rx};
    const u32 rc = reinterpret_cast<SwapFunction>(stub.Va)(&args);
    const int after = run();
    gStatus.SelfTestPassed = rc == 0 && after == 2;
    gStatus.Value = rc != 0 ? rc : static_cast<u64>(after);
    if (rc == 0) {
        svcUnmapProcessCodeMemory(process, reinterpret_cast<u64>(code.Va),
                                  reinterpret_cast<u64>(second), kCodePagesPage);
        std::free(second);
        std::free(code.Heap);
    } else {
        std::free(second);
        UnmapCodePage(process, code);
    }
    UnmapCodePage(process, stub);
}

void Apply(Handle process) {
    const u64 textVa = reinterpret_cast<u64>(&_start);
    MemoryInfo text{};
    MemoryInfo rodata{};
    MemoryInfo old{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&text, &pageInfo, textVa)) || text.addr != textVa ||
        (text.perm & Perm_Rx) != Perm_Rx)
        return Fallback("the text is not what was expected", text.addr);
    const u32 textType = text.type & 0xFFU;
    if (textType != MemType_ModuleCodeStatic && textType != MemType_ModuleCodeMutable)
        return Fallback("the text is not mapped by a loader", textType);
    if (R_FAILED(svcQueryMemory(&rodata, &pageInfo, textVa + text.size)) ||
        (rodata.perm & Perm_Rw) != Perm_R)
        return Fallback("the read-only data is not what was expected", rodata.perm);
    const u64 total = text.size + rodata.size;

    // hbloader's copy: the start of its heap, lent to the kernel
    // (svcMapProcessCodeMemory marks the source borrowed).
    u64 heapBase = 0;
    if (R_FAILED(svcGetInfo(&heapBase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0)) ||
        R_FAILED(svcQueryMemory(&old, &pageInfo, heapBase)) || (old.attr & MemAttr_IsBorrowed) == 0)
        return Fallback("the loader's copy is not at the start of the heap", old.attr);

    const u64 heapFree = reinterpret_cast<u64>(fake_heap_end) - reinterpret_cast<u64>(sbrk(0));
    if (!CodePagesHeapAllows(heapFree, total))
        return Fallback("heap too small for a second copy", heapFree);

    gStatus.CodeAddress = textVa;
    gStatus.TextBytes = text.size;
    const u64 copyStart = armGetSystemTick();
    // Whole pages are lent to the kernel: the alignment keeps the first clear
    // of this allocation's header, the slack the last one of the next. Never
    // freed after a swap.
    void* raw = memalign(kCodePagesBlock, CodePagesAllocationSize(total));
    if (raw == nullptr) return Fallback("no memory for the copy", total);
    const u64 copy = CodePagesAlignedCopy(reinterpret_cast<u64>(raw), textVa);
    std::memcpy(reinterpret_cast<void*>(copy), reinterpret_cast<const void*>(textVa), total);
    armDCacheFlush(reinterpret_cast<void*>(copy), total);
    const u64 swapStart = armGetSystemTick();
    gStatus.CopyAddress = copy;
    gStatus.CopyMicroseconds = Microseconds(copyStart, swapStart);

    CodePage stub;
    if (!MapCodePage(process, kStub, sizeof(kStub), &stub)) {
        std::free(raw);
        return Fallback("the swap's page could not be mapped", 0);
    }
    const auto swap = reinterpret_cast<SwapFunction>(stub.Va);
    SwapArgs args{};
    args.Process = process;
    args.Count = 1;
    args.Segments[0] = {textVa, heapBase, copy, text.size, Perm_Rx};
    const u32 textResult = swap(&args);
    if (textResult != 0) {
        UnmapCodePage(process, stub);
        std::free(raw);
        return Fallback("the kernel refused the text swap", textResult);
    }
    // From here the text runs from the copy: hbloader must never get control
    // back (see the top of the file).
    __nx_applet_exit_mode = 1;
    gStatus.State = CodePagesState::Remapped;
    gStatus.TextBlocks = CodePagesBlockCount(textVa, text.size);
    args.Segments[0] = {textVa + text.size, heapBase + text.size, copy + text.size, rodata.size,
                        Perm_R};
    const u32 rodataResult = swap(&args);
    gStatus.SwapMicroseconds = Microseconds(swapStart, armGetSystemTick());
    UnmapCodePage(process, stub);
    if (rodataResult != 0) {
        gStatus.Reason = "the kernel refused the read-only data swap";
        gStatus.Value = rodataResult;
        return;
    }
    gStatus.RodataBytes = rodata.size;
    gStatus.RodataBlocks = CodePagesBlockCount(textVa + text.size, rodata.size);
}

// First constructor of the program (see the top of the file); the others that
// must see the final mapping use priority 102.
[[gnu::constructor(101)]] void RemapCodeOnBlocks() {
    struct stat info;
    if (stat(kCodePagesKillSwitchPath, &info) == 0) {
        gStatus.State = CodePagesState::Off;
        return;
    }
    const Handle process = envGetOwnProcessHandle();
    if (process == INVALID_HANDLE) {
        SelfTest(CUR_PROCESS_HANDLE);
        return;
    }
    Apply(process);
}

} // namespace

const CodePagesStatus& CurrentCodePagesStatus() {
    return gStatus;
}

} // namespace Oot3dNativeGame

#endif // __SWITCH__
