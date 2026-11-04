#pragma once

// Bounded walk of newlib's malloc free lists (mallocr.c, the
// dlmalloc 2.6 libnx uses), in place of the mallinfo() of
// config/heap_check_windows.
//
// newlib's mallinfo() (__malloc_update_mallinfo) holds the malloc lock
// and walks each bin by `bk` until it gets back to the header, with no limit or
// checking: with a corrupted list (a double free leaves p->bk == p, a
// use-after-free writes into fd/bk) the loop never ends, the malloc lock
// stays held and the whole process freezes with no crash report — that was the
// 2026-10-17 freeze at the Normal/Master Quest choice (switch-logs/
// crash1: the perf.txt line stopped between "hold_" and the end, in the window that
// would call mallinfo). This walk computes the same sum, but checks each
// link (pointer inside the heap and aligned, plausible size, p->bk->fd == p
// and p->fd->bk == p), detects cycles (Brent) and has a step cap: a corrupted
// list becomes a report instead of an infinite loop.
//
// Layout (newlib, 64-bit): struct malloc_chunk { size_t prev_size; size_t
// size; malloc_chunk* fd; malloc_chunk* bk; }; the __malloc_av_ array has
// 2 * 128 + 2 pointers; the header of bin i is (char*)&av[2i + 2] - 16
// (fd = av[2i + 2], bk = av[2i + 3]) and "top" is av[2]. Checked against the
// disassembly of __malloc_update_mallinfo in devkitA64's libc.a.

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

struct HeapBinCheckResult {
    bool Ok = true;
    const char* Reason = "ok";
    // Totals like mallinfo's: free = sum of the bins + top.
    uint64_t FreeBytes = 0;
    uint64_t FreeChunks = 0;
    uint64_t TopBytes = 0;
    uint64_t Steps = 0;
    // Where it stopped (when !Ok): bin, chunk and the fields read from it.
    uint32_t Bin = 0;
    uintptr_t Chunk = 0;
    uintptr_t ChunkSize = 0;
    uintptr_t ChunkFd = 0;
    uintptr_t ChunkBk = 0;
};

inline constexpr size_t kNewlibMallocBins = 128U;

// `av` points to the 2 * bins + 2 pointers of __malloc_av_; the heap spans
// [heapBegin, heapEnd). Only reads memory inside the heap (or av itself).
// Takes no lock: the caller holds the malloc lock.
HeapBinCheckResult CheckNewlibMallocBins(void* const* av, size_t bins, uintptr_t heapBegin,
                                         uintptr_t heapEnd, uint64_t maxSteps) noexcept;

#if defined(__SWITCH__)
// Takes the malloc lock, walks __malloc_av_ within
// [fake_heap_start, fake_heap_end) and releases the lock. used = arena - free
// (mallinfo's uordblks).
HeapBinCheckResult CheckSwitchMallocHeap(uint64_t* usedBytes) noexcept;
#endif

} // namespace Oot3dNativeGame
