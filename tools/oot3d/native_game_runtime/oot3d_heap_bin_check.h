#pragma once

// Varredura limitada das listas livres do malloc do newlib (mallocr.c, o
// dlmalloc 2.6 que a libnx usa), no lugar do mallinfo() de
// config/heap_check_windows.
//
// O mallinfo() do newlib (__malloc_update_mallinfo) segura o lock do malloc
// e percorre cada bin por `bk` até voltar ao cabeçalho, sem limite nem
// checagem: com uma lista corrompida (um double free deixa p->bk == p, um
// use-after-free escreve no fd/bk) o laço não termina, o lock do malloc
// fica preso e o processo inteiro congela sem crash report — foi o
// congelamento de 2026-10-17 na escolha Normal/Master Quest (switch-logs/
// crash1: a linha do perf.txt parou entre "hold_" e o fim, na janela que
// chamaria o mallinfo). Esta varredura faz a mesma soma, mas confere cada
// elo (ponteiro dentro do heap e alinhado, tamanho plausível, p->bk->fd == p
// e p->fd->bk == p), detecta ciclo (Brent) e tem teto de passos: uma lista
// corrompida vira um relatório em vez de um laço infinito.
//
// Layout (newlib, 64 bits): struct malloc_chunk { size_t prev_size; size_t
// size; malloc_chunk* fd; malloc_chunk* bk; }; o vetor __malloc_av_ tem
// 2 * 128 + 2 ponteiros; o cabeçalho do bin i é (char*)&av[2i + 2] - 16
// (fd = av[2i + 2], bk = av[2i + 3]) e o "top" é av[2]. Conferido no
// desassembly de __malloc_update_mallinfo do libc.a do devkitA64.

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

struct HeapBinCheckResult {
    bool Ok = true;
    const char* Reason = "ok";
    // Totais como os do mallinfo: livres = soma dos bins + top.
    uint64_t FreeBytes = 0;
    uint64_t FreeChunks = 0;
    uint64_t TopBytes = 0;
    uint64_t Steps = 0;
    // Onde parou (quando !Ok): bin, chunk e os campos lidos dele.
    uint32_t Bin = 0;
    uintptr_t Chunk = 0;
    uintptr_t ChunkSize = 0;
    uintptr_t ChunkFd = 0;
    uintptr_t ChunkBk = 0;
};

inline constexpr size_t kNewlibMallocBins = 128U;

// `av` aponta para os 2 * bins + 2 ponteiros de __malloc_av_; o heap ocupa
// [heapBegin, heapEnd). Só lê memória dentro do heap (ou do próprio av).
// Não toma lock: quem chama segura o lock do malloc.
HeapBinCheckResult CheckNewlibMallocBins(void* const* av, size_t bins, uintptr_t heapBegin,
                                         uintptr_t heapEnd, uint64_t maxSteps) noexcept;

#if defined(__SWITCH__)
// Toma o lock do malloc, varre __malloc_av_ dentro de
// [fake_heap_start, fake_heap_end) e solta o lock. used = arena - livres
// (o uordblks do mallinfo).
HeapBinCheckResult CheckSwitchMallocHeap(uint64_t* usedBytes) noexcept;
#endif

} // namespace Oot3dNativeGame
