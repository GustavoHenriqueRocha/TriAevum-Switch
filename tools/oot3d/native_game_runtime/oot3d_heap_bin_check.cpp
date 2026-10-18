// Varredura limitada dos bins do malloc do newlib: ver oot3d_heap_bin_check.h.
#include "oot3d_heap_bin_check.h"

#if defined(__SWITCH__)
#include <malloc.h>
#include <reent.h>
#endif

namespace Oot3dNativeGame {
namespace {

struct Chunk {
    size_t PrevSize;
    size_t Size;
    Chunk* Fd;
    Chunk* Bk;
};

constexpr uintptr_t kSizeMask = ~static_cast<uintptr_t>(3U);  // PREV_INUSE | IS_MMAPPED
constexpr uintptr_t kMinimumChunk = sizeof(Chunk);           // MINSIZE (32)

class Walker {
  public:
    Walker(void* const* av, uintptr_t begin, uintptr_t end, uint64_t maxSteps,
           HeapBinCheckResult& result) noexcept
        : mAv(av), mBegin(begin), mEnd(end), mMaxSteps(maxSteps), mResult(result) {}

    // O cabeçalho do bin `bin` (fica dentro do av, não do heap).
    [[nodiscard]] const Chunk* Header(size_t bin) const noexcept {
        return reinterpret_cast<const Chunk*>(reinterpret_cast<const char*>(&mAv[2U * bin + 2U]) -
                                              2U * sizeof(size_t));
    }

    // Um chunk que se pode ler: dentro do heap, alinhado e com os 32 bytes
    // do cabeçalho dentro do heap.
    [[nodiscard]] bool Readable(const Chunk* chunk) const noexcept {
        const auto address = reinterpret_cast<uintptr_t>(chunk);
        return address >= mBegin && address <= mEnd && mEnd - address >= kMinimumChunk &&
               (address & (sizeof(size_t) - 1U)) == 0U;
    }

    bool Fail(const char* reason, size_t bin, const Chunk* chunk) noexcept {
        mResult.Ok = false;
        mResult.Reason = reason;
        mResult.Bin = static_cast<uint32_t>(bin);
        mResult.Chunk = reinterpret_cast<uintptr_t>(chunk);
        if (chunk != nullptr && Readable(chunk)) {
            mResult.ChunkSize = chunk->Size;
            mResult.ChunkFd = reinterpret_cast<uintptr_t>(chunk->Fd);
            mResult.ChunkBk = reinterpret_cast<uintptr_t>(chunk->Bk);
        }
        return false;
    }

    // Um elo: `next` (= p->bk) é o cabeçalho ou um chunk legível com
    // next->fd == p.
    [[nodiscard]] bool Linked(const Chunk* header, const Chunk* p, const Chunk* next) const noexcept {
        if (next == header) return header->Fd == p || p == header;
        return Readable(next) && next->Fd == p;
    }

    bool Bin(size_t bin) noexcept {
        const Chunk* const header = Header(bin);
        const Chunk* p = header->Bk;
        if (p != header && !Readable(p)) return Fail("bin_last_out_of_heap", bin, p);
        if (p != header && p->Fd != header) return Fail("bin_last_fd_not_header", bin, p);
        // Brent: `tortoise` salta para `p` a cada potência de 2 passos; um
        // ciclo que não passa pelo cabeçalho reencontra a tartaruga.
        const Chunk* tortoise = p;
        uint64_t power = 1U;
        uint64_t length = 0U;
        while (p != header) {
            if (++mResult.Steps > mMaxSteps) return Fail("too_many_steps", bin, p);
            const uintptr_t size = p->Size & kSizeMask;
            const auto address = reinterpret_cast<uintptr_t>(p);
            if (size < kMinimumChunk || size > mEnd - address) {
                return Fail("chunk_size", bin, p);
            }
            const Chunk* const next = p->Bk;
            if (next == p) return Fail("self_loop", bin, p);
            if (next != header && !Readable(next)) return Fail("bk_out_of_heap", bin, p);
            if (!Linked(header, p, next)) return Fail("bk_fd_mismatch", bin, p);
            const Chunk* const forward = p->Fd;
            if (forward != header && (!Readable(forward) || forward->Bk != p)) {
                return Fail("fd_bk_mismatch", bin, p);
            }
            mResult.FreeBytes += size;
            ++mResult.FreeChunks;
            p = next;
            if (p == tortoise) return Fail("cycle", bin, p);
            if (++length == power) {
                tortoise = p;
                power <<= 1U;
                length = 0U;
            }
        }
        return true;
    }

  private:
    void* const* mAv;
    uintptr_t mBegin;
    uintptr_t mEnd;
    uint64_t mMaxSteps;
    HeapBinCheckResult& mResult;
};

} // namespace

HeapBinCheckResult CheckNewlibMallocBins(void* const* av, size_t bins, uintptr_t heapBegin,
                                         uintptr_t heapEnd, uint64_t maxSteps) noexcept {
    HeapBinCheckResult result;
    if (av == nullptr || heapEnd <= heapBegin) {
        result.Ok = false;
        result.Reason = "no_heap";
        return result;
    }
    Walker walker(av, heapBegin, heapEnd, maxSteps, result);
    // top (av[2]): conta como livre, como no mallinfo.
    const auto* top = static_cast<const Chunk*>(av[2]);
    // initial_top (o próprio cabeçalho do bin 0): nada foi alocado ainda.
    if (top != walker.Header(0U)) {
        if (!walker.Readable(top)) {
            walker.Fail("top_out_of_heap", 0U, top);
            return result;
        }
        result.TopBytes = top->Size & kSizeMask;
        if (result.TopBytes > heapEnd - reinterpret_cast<uintptr_t>(top)) {
            walker.Fail("top_size", 0U, top);
            return result;
        }
    }
    result.FreeBytes = result.TopBytes;
    for (size_t bin = 1U; bin < bins; ++bin) {
        if (!walker.Bin(bin)) break;
    }
    return result;
}

#if defined(__SWITCH__)
extern "C" {
extern void* __malloc_av_[];
extern char* fake_heap_start;
extern char* fake_heap_end;
extern struct mallinfo __malloc_current_mallinfo;
}

HeapBinCheckResult CheckSwitchMallocHeap(uint64_t* usedBytes) noexcept {
    // Teto: mais chunks livres do que cabem no heap é impossível sem ciclo
    // (o Brent já pega ciclos; isto só limita o pior caso).
    const uintptr_t begin = reinterpret_cast<uintptr_t>(fake_heap_start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(fake_heap_end);
    const uint64_t maxSteps = end > begin ? (end - begin) / 32U + 1U : 1U;
    __malloc_lock(_REENT);
    HeapBinCheckResult result =
        CheckNewlibMallocBins(__malloc_av_, kNewlibMallocBins, begin, end, maxSteps);
    const uint64_t arena = __malloc_current_mallinfo.arena;
    __malloc_unlock(_REENT);
    if (usedBytes != nullptr) {
        *usedBytes = arena > result.FreeBytes ? arena - result.FreeBytes : 0U;
    }
    return result;
}
#endif

} // namespace Oot3dNativeGame
