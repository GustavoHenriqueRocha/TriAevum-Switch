#pragma once

// The NRO's code and read-only data on 2 MB blocks (oot3d_switch_code_pages.cpp,
// Switch only). This header is the part that does not touch the kernel: the
// alignment of the copy, the block count, the heap check and the log line, so
// the host tests (oot3d_switch_code_pages_tests) cover them.

#include <cstdint>
#include <cstdio>
#include <string>

namespace Oot3dNativeGame {

inline constexpr uint64_t kCodePagesBlock = 0x200000U;  // one AArch64 level-2 block
inline constexpr uint64_t kCodePagesPage = 0x1000U;
// Heap that must stay free after the copy. The game's newlib heap peaked at
// 892 MB in the 0.1.1 logs (perf.txt heap_arena_peak_mb); an application has
// about 3 GB, so the copy (~140 MB) leaves plenty. A smaller heap (an applet,
// or a future build that needs more) keeps the code where it is.
inline constexpr uint64_t kCodePagesMinimumHeapLeft = 1536ULL * 1024U * 1024U;
// SD file whose presence turns the remap off (read before the swap: sdmc is
// mounted by libnx's __appInit, which runs before every constructor).
inline constexpr const char* kCodePagesKillSwitchPath =
    "sdmc:/switch/oot3dre/config/no_code_hugepages";

// Bytes the allocation needs so that a copy of `size` bytes can start at an
// address congruent to the code modulo 2 MB, with whole pages on both ends
// (the slack page keeps the last one clear of newlib's next chunk header).
constexpr uint64_t CodePagesAllocationSize(uint64_t size) {
    return size + kCodePagesBlock + kCodePagesPage;
}

// The first address at or after `raw` with the same offset as `code` inside a
// 2 MB block: virtual and physical addresses of the heap line up modulo 2 MB,
// so the kernel can map the copy at `code` with blocks wherever both are
// aligned.
constexpr uint64_t CodePagesAlignedCopy(uint64_t raw, uint64_t code) {
    return raw + ((code - raw) & (kCodePagesBlock - 1U));
}

// Whole 2 MB blocks inside [va, va + size).
constexpr uint64_t CodePagesBlockCount(uint64_t va, uint64_t size) {
    const uint64_t low = (va + kCodePagesBlock - 1U) & ~(kCodePagesBlock - 1U);
    const uint64_t high = (va + size) & ~(kCodePagesBlock - 1U);
    return high > low ? (high - low) / kCodePagesBlock : 0U;
}

// Whether a heap with `heapFree` bytes left can give `copySize` bytes (plus the
// alignment slack) and still keep kCodePagesMinimumHeapLeft.
constexpr bool CodePagesHeapAllows(uint64_t heapFree, uint64_t copySize) {
    const uint64_t needed = CodePagesAllocationSize(copySize);
    return heapFree >= needed && heapFree - needed >= kCodePagesMinimumHeapLeft;
}

enum class CodePagesState : uint8_t {
    NotRun,    // not a Switch build, or the constructor did not run
    Remapped,  // the text (and the read-only data, unless Reason says why not) moved
    Off,       // config/no_code_hugepages
    Fallback,  // left where the loader put it; Reason and Value say why
    SelfTest,  // no loader (an emulator): the swap was tested on pages of its own
};

struct CodePagesStatus {
    CodePagesState State = CodePagesState::NotRun;
    const char* Reason = "";  // static text; empty when nothing went wrong
    uint64_t Value = 0;       // the result code or value behind Reason
    uint64_t TextBytes = 0;
    uint64_t RodataBytes = 0;  // 0 when only the text moved
    uint64_t TextBlocks = 0;
    uint64_t RodataBlocks = 0;
    uint64_t CodeAddress = 0;
    uint64_t CopyAddress = 0;
    uint64_t CopyMicroseconds = 0;  // memcpy + data cache flush
    uint64_t SwapMicroseconds = 0;  // the unmap/map/permission calls
    bool SelfTestPassed = false;
};

// 1 when the text runs from the 2 MB-aligned copy, else 0 (perf.txt code_2mb=).
inline int CodePagesPerfFlag(const CodePagesStatus& status) {
    return status.State == CodePagesState::Remapped ? 1 : 0;
}

// What the start-up constructor did (Switch); NotRun elsewhere.
const CodePagesStatus& CurrentCodePagesStatus();

// One line for logs/oot3d_native_game.log, starting with "code_pages: ".
inline std::string FormatCodePagesStatus(const CodePagesStatus& status) {
    const auto hex = [](uint64_t value) {
        char text[24];
        std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(value));
        return std::string(text);
    };
    const auto ms = [](uint64_t microseconds) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(microseconds) / 1000.0);
        return std::string(text);
    };
    std::string line = "code_pages: ";
    switch (status.State) {
    case CodePagesState::NotRun:
        return line + "not run";
    case CodePagesState::Off:
        return line + "off (config/no_code_hugepages)";
    case CodePagesState::Fallback:
        return line + "left on 4 KB pages: " + status.Reason + " (" + hex(status.Value) + ")";
    case CodePagesState::SelfTest:
        line += "no loader (emulator), left where it is; swap self-test ";
        line += status.SelfTestPassed ? "passed" : "FAILED";
        if (status.Reason[0] != '\0') line += std::string(": ") + status.Reason;
        return line + " (" + hex(status.Value) + ")";
    case CodePagesState::Remapped:
        break;
    }
    line += "remapped text_kb=" + std::to_string(status.TextBytes >> 10U) +
            " rodata_kb=" + std::to_string(status.RodataBytes >> 10U) +
            " blocks_2mb=" + std::to_string(status.TextBlocks) + '+' +
            std::to_string(status.RodataBlocks) + " copy_ms=" + ms(status.CopyMicroseconds) +
            " swap_ms=" + ms(status.SwapMicroseconds) + " code=" + hex(status.CodeAddress) +
            " copy=" + hex(status.CopyAddress);
    if (status.Reason[0] != '\0')
        line += std::string(" rodata_kept: ") + status.Reason + " (" + hex(status.Value) + ")";
    return line;
}

} // namespace Oot3dNativeGame
