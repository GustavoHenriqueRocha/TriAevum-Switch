// The policy half of oot3d_switch_code_pages.cpp (oot3d_switch_code_pages.h):
// alignment of the copy, 2 MB block count, heap check, log line.
#include "oot3d_switch_code_pages.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_switch_code_pages_tests: " << message << '\n';
        std::exit(1);
    }
}

constexpr uint64_t kMiB = 1024U * 1024U;

void TestAlignedCopy() {
    // Same offset inside a 2 MB block as the code, at or after the allocation.
    const uint64_t code = 0x0000000008123000ULL;
    for (uint64_t raw : {0x0000000400000000ULL, 0x0000000400001000ULL, 0x00000004001FF000ULL,
                         0x0000000400123000ULL, 0x0000000400124000ULL}) {
        const uint64_t copy = CodePagesAlignedCopy(raw, code);
        Require(copy >= raw && copy - raw < kCodePagesBlock, "copy within the first block");
        Require((copy & (kCodePagesBlock - 1U)) == (code & (kCodePagesBlock - 1U)),
                "copy congruent to the code modulo 2 MB");
        // Everything fits in the allocation, slack page included.
        const uint64_t size = 140U * kMiB;
        Require(copy + size + kCodePagesPage <= raw + CodePagesAllocationSize(size),
                "copy and slack page inside the allocation");
    }
    Require(CodePagesAlignedCopy(0x400123000ULL, 0x8123000ULL) == 0x400123000ULL,
            "an already congruent allocation is used as is");
}

void TestBlockCount() {
    Require(CodePagesBlockCount(0x200000U, 0x200000U) == 1U, "one aligned block");
    Require(CodePagesBlockCount(0x201000U, 0x200000U) == 0U, "a misaligned 2 MB range has none");
    Require(CodePagesBlockCount(0x201000U, 0x3FF000U) == 1U, "partial ends are pages");
    Require(CodePagesBlockCount(0x1000U, 0x1000U) == 0U, "a page has none");
    // 133.8 MB of text at a page-aligned address: 65 or 66 whole blocks.
    const uint64_t text = 140304384U;  // 133.8 MB
    const uint64_t blocks = CodePagesBlockCount(0x8123000U, text);
    Require(blocks == text / kCodePagesBlock - 1U || blocks == text / kCodePagesBlock,
            "the game's text is almost all blocks");
}

void TestHeapPolicy() {
    const uint64_t copy = 138U * kMiB;
    Require(CodePagesHeapAllows(3000U * kMiB, copy), "an application heap allows the copy");
    Require(!CodePagesHeapAllows(400U * kMiB, copy), "an applet heap does not");
    Require(!CodePagesHeapAllows(CodePagesAllocationSize(copy) + kCodePagesMinimumHeapLeft - 1U, copy),
            "one byte short of the reserve");
    Require(CodePagesHeapAllows(CodePagesAllocationSize(copy) + kCodePagesMinimumHeapLeft, copy),
            "exactly the reserve");
    Require(!CodePagesHeapAllows(copy, copy), "no wrap-around when the heap is smaller than the copy");
}

bool StartsWith(const std::string& text, const char* prefix) {
    return text.rfind(prefix, 0) == 0;
}

void TestFormat() {
    CodePagesStatus status;
    Require(FormatCodePagesStatus(status) == "code_pages: not run", "not run");
    Require(CodePagesPerfFlag(status) == 0, "not run is 0 in perf.txt");
    status.State = CodePagesState::Off;
    Require(FormatCodePagesStatus(status) == "code_pages: off (config/no_code_hugepages)", "off");
    status.State = CodePagesState::Fallback;
    status.Reason = "heap too small for a second copy";
    status.Value = 0x19000000U;
    Require(FormatCodePagesStatus(status) ==
                "code_pages: left on 4 KB pages: heap too small for a second copy (0x19000000)",
            "fallback names its reason and value");
    Require(CodePagesPerfFlag(status) == 0, "fallback is 0 in perf.txt");

    CodePagesStatus remapped;
    remapped.State = CodePagesState::Remapped;
    remapped.TextBytes = 133U * kMiB;
    remapped.RodataBytes = 4U * kMiB;
    remapped.TextBlocks = 65U;
    remapped.RodataBlocks = 1U;
    remapped.CopyMicroseconds = 41260U;
    remapped.SwapMicroseconds = 840U;
    remapped.CodeAddress = 0x8123000U;
    remapped.CopyAddress = 0x400123000ULL;
    const std::string line = FormatCodePagesStatus(remapped);
    Require(line ==
                "code_pages: remapped text_kb=136192 rodata_kb=4096 blocks_2mb=65+1 copy_ms=41.3 "
                "swap_ms=0.8 code=0x8123000 copy=0x400123000",
            "remapped line has sizes, blocks, times and addresses");
    Require(CodePagesPerfFlag(remapped) == 1, "remapped is 1 in perf.txt");
    remapped.RodataBytes = 0U;
    remapped.RodataBlocks = 0U;
    remapped.Reason = "the kernel refused the read-only data swap";
    remapped.Value = 0xCA01U;
    Require(FormatCodePagesStatus(remapped).find(
                " rodata_kept: the kernel refused the read-only data swap (0xCA01)") !=
                std::string::npos,
            "a text-only swap says why the data stayed");

    CodePagesStatus self;
    self.State = CodePagesState::SelfTest;
    self.SelfTestPassed = true;
    self.Value = 2U;
    Require(StartsWith(FormatCodePagesStatus(self), "code_pages: no loader (emulator)") &&
                FormatCodePagesStatus(self).find("self-test passed (0x2)") != std::string::npos,
            "self-test line");
}

} // namespace

int main() {
    TestAlignedCopy();
    TestBlockCount();
    TestHeapPolicy();
    TestFormat();
    std::cout << "oot3d_switch_code_pages_tests: ok\n";
    return 0;
}
