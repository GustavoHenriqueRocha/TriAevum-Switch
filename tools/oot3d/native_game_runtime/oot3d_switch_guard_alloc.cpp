// Alocador com páginas de guarda: ver oot3d_switch_guard_alloc.h.
#include "oot3d_switch_guard_alloc.h"

#include <cstdlib>

namespace Oot3dNativeGame::GuardAlloc {

namespace {

// Próximo número sem sinal de `text` (pula espaços); falso se não há número
// ou ele é inválido. `text` avança.
bool NextNumber(const char*& text, size_t& value, bool& found) noexcept {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    found = false;
    if (*text == '\0') return true;
    if (*text < '0' || *text > '9') return false;
    size_t parsed = 0;
    while (*text >= '0' && *text <= '9') {
        const size_t digit = static_cast<size_t>(*text - '0');
        if (parsed > (static_cast<size_t>(-1) - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        ++text;
    }
    value = parsed;
    found = true;
    return true;
}

} // namespace

bool ParseConfig(const char* text, Config& config) noexcept {
    Config parsed;
    if (text != nullptr) {
        size_t* const fields[] = {&parsed.MinSize, &parsed.MaxSize, &parsed.QuarantineBlocks};
        for (size_t* field : fields) {
            bool found = false;
            if (!NextNumber(text, *field, found)) return false;
            if (!found) break;
        }
        while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
        if (*text != '\0') return false;
    }
    if (parsed.MinSize == 0U || parsed.MinSize > parsed.MaxSize ||
        !InRange(parsed, parsed.MaxSize)) {
        return false;
    }
    config = parsed;
    return true;
}

} // namespace Oot3dNativeGame::GuardAlloc

#if defined(__SWITCH__)
#include <switch.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <reent.h>

#include "oot3d_hang_watchdog.h"

extern "C" {
void* __real__malloc_r(struct _reent* reent, size_t size);
void __real__free_r(struct _reent* reent, void* pointer);
void* __real__realloc_r(struct _reent* reent, void* pointer, size_t size);
void* __real__calloc_r(struct _reent* reent, size_t count, size_t size);
size_t __real__malloc_usable_size_r(struct _reent* reent, void* pointer);
void* _memalign_r(struct _reent* reent, size_t alignment, size_t size);
}

namespace Oot3dNativeGame::GuardAlloc {
namespace {

constexpr const char* kConfigPath = "sdmc:/switch/oot3dre/config/guard_alloc";

Config gConfig;
std::atomic<bool> gEnabled{false};
std::atomic<bool> gUsed{false};  // já existe algum bloco com guarda
std::atomic<uint64_t> gSerial{0};
std::atomic<uint64_t> gGuarded{0};
std::atomic<uint64_t> gLive{0};
std::atomic<uint64_t> gFallbacks{0};
Mutex gRingMutex;
QuarantineRing gRing;

// Chamadas ao newlib que, por dentro, chamam o _malloc_r embrulhado: o
// calloc/realloc reais leem o cabeçalho do chunk que o malloc devolveu, que
// num bloco com guarda é lixo. Nelas o malloc embrulhado vai direto ao real.
thread_local bool tBypass = false;

// Limites da pilha da thread (para seguir os frame pointers sem sair dela).
thread_local uintptr_t tStackBegin = 0;
thread_local uintptr_t tStackEnd = 0;

[[gnu::noinline]] void CaptureStack(uint64_t (&out)[kStackDepth]) noexcept {
    uintptr_t frame = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    if (tStackEnd == 0U) {
        MemoryInfo info{};
        u32 page = 0;
        tStackEnd = 1U;  // nada legível: não segue
        if (R_SUCCEEDED(svcQueryMemory(&info, &page, frame)) && (info.perm & Perm_R) != 0) {
            tStackBegin = info.addr;
            tStackEnd = info.addr + info.size;
        }
    }
    size_t count = 0;
    while (count < kStackDepth) {
        if ((frame & 7U) != 0U || frame < tStackBegin || frame + 16U > tStackEnd) break;
        const uintptr_t next = reinterpret_cast<const uintptr_t*>(frame)[0];
        out[count++] = reinterpret_cast<const uint64_t*>(frame)[1];
        if (next <= frame) break;
        frame = next;
    }
    for (; count < kStackDepth; ++count) out[count] = 0;
}

BlockHeader* HeaderOf(void* pointer) noexcept {
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t offset = address & (kPageSize - 1U);
    if ((offset & 15U) != 0U || offset < sizeof(BlockHeader)) return nullptr;
    auto* header = reinterpret_cast<BlockHeader*>(address - offset);
    if (header->Magic != kMagic || ObjectOffset(header->Size) != offset) return nullptr;
    return header;
}

void* GuardedAlloc(struct _reent* reent, size_t size) noexcept {
    // _memalign_r não é embrulhado; o malloc que ele faz por dentro (12 KiB+)
    // fica fora da faixa e vai direto ao real.
    void* const base = _memalign_r(reent, kPageSize, kBlockBytes);
    if (base == nullptr) {
        gFallbacks.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    auto* const header = static_cast<BlockHeader*>(base);
    header->Magic = kMagic;
    header->Size = size;
    header->Serial = gSerial.fetch_add(1U, std::memory_order_relaxed) + 1U;
    header->AllocTicks = armGetSystemTick();
    CaptureStack(header->AllocStack);
    if (R_FAILED(svcSetMemoryPermission(static_cast<u8*>(base) + kPageSize, kPageSize,
                                        Perm_None))) {
        // Sem blocos de memória livres no kernel, por exemplo.
        header->Magic = 0;
        __real__free_r(reent, base);
        gFallbacks.fetch_add(1U, std::memory_order_relaxed);
        return nullptr;
    }
    gUsed.store(true, std::memory_order_relaxed);
    gGuarded.fetch_add(1U, std::memory_order_relaxed);
    gLive.fetch_add(1U, std::memory_order_relaxed);
    return static_cast<u8*>(base) + ObjectOffset(size);
}

// Devolve as páginas ao malloc de verdade.
void Release(struct _reent* reent, uintptr_t page) noexcept {
    svcSetMemoryPermission(reinterpret_cast<void*>(page), 2U * kPageSize, Perm_Rw);
    reinterpret_cast<BlockHeader*>(page)->Magic = 0;
    __real__free_r(reent, reinterpret_cast<void*>(page));
}

void GuardedFree(struct _reent* reent, BlockHeader* header) noexcept {
    QuarantineEntry entry;
    entry.Page = reinterpret_cast<uintptr_t>(header);
    entry.Size = header->Size;
    entry.Serial = header->Serial;
    entry.AllocTicks = header->AllocTicks;
    entry.FreeTicks = armGetSystemTick();
    entry.FreeThread = threadGetCurHandle();
    std::memcpy(entry.AllocStack, header->AllocStack, sizeof(entry.AllocStack));
    CaptureStack(entry.FreeStack);
    gLive.fetch_sub(1U, std::memory_order_relaxed);
    if (R_FAILED(svcSetMemoryPermission(header, kPageSize, Perm_None))) {
        Release(reent, entry.Page);
        return;
    }
    QuarantineEntry evicted;
    mutexLock(&gRingMutex);
    const bool full = gRing.Push(entry, evicted);
    mutexUnlock(&gRingMutex);
    // Fora do lock: o free real pega o lock do malloc.
    if (full) Release(reent, evicted.Page);
}

void AppendStack(HangWatch::Text& text, const char* label, const uint64_t (&stack)[kStackDepth]) {
    text.Str(label);
    for (uint64_t address : stack) {
        if (address == 0U) break;
        text.Char(' ');
        HangWatch::AppendCodeAddress(text, address);
    }
    text.Char('\n');
}

// Classifica o endereço da falha contra os blocos com guarda. Sem lock: quem
// falhou pode estar com ele (free); o anel só cresce até a capacidade.
void AppendGuardVerdict(HangWatch::Text& text, uintptr_t address) {
    if (const QuarantineEntry* entry = gRing.Find(address)) {
        const uintptr_t object = entry->Page + ObjectOffset(entry->Size);
        text.Str("USO APÓS FREE: bloco serial=").Dec(entry->Serial).Str(" tamanho=")
            .Dec(entry->Size).Str(" objeto=").Hex(object).Str(" deslocamento=")
            .SignedDec(static_cast<int64_t>(address) - static_cast<int64_t>(object))
            .Str(" liberado_ha_ms=")
            .Dec(armTicksToNs(armGetSystemTick() - entry->FreeTicks) / 1000000U)
            .Str(" vida_ms=").Dec(armTicksToNs(entry->FreeTicks - entry->AllocTicks) / 1000000U)
            .Str(" thread_do_free=").Hex(entry->FreeThread).Char('\n');
        AppendStack(text, "pilha_alloc:", entry->AllocStack);
        AppendStack(text, "pilha_free:", entry->FreeStack);
        return;
    }
    // Página de guarda de um bloco vivo: o cabeçalho está uma página antes.
    const uintptr_t page = address & ~(kPageSize - 1U);
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (page >= kPageSize && R_SUCCEEDED(svcQueryMemory(&info, &pageInfo, page - kPageSize)) &&
        (info.perm & Perm_R) != 0) {
        const auto* header = reinterpret_cast<const BlockHeader*>(page - kPageSize);
        if (header->Magic == kMagic) {
            const uintptr_t object = page - kPageSize + ObjectOffset(header->Size);
            text.Str("ESTOURO: bloco vivo serial=").Dec(header->Serial).Str(" tamanho=")
                .Dec(header->Size).Str(" objeto=").Hex(object).Str(" deslocamento=")
                .SignedDec(static_cast<int64_t>(address) - static_cast<int64_t>(object))
                .Char('\n');
            AppendStack(text, "pilha_alloc:", header->AllocStack);
            return;
        }
    }
    text.Str("fora dos blocos com guarda\n");
}

char gFaultDetails[8 * 1024];

void HandleException(ThreadExceptionDump* context) {
    HangWatch::Text text(gFaultDetails, sizeof(gFaultDetails));
    text.Str("tipo=").Hex(context->error_desc).Str(" far=").Hex(context->far.x)
        .Str(" esr=").Hex(context->esr).Char('\n');
    if (gUsed.load(std::memory_order_relaxed)) {
        AppendGuardVerdict(text, context->far.x);
    }
    const Stats stats = CurrentStats();
    text.Str("guard_alloc=").Dec(stats.Enabled ? 1U : 0U).Str(" faixa=").Dec(gConfig.MinSize)
        .Char('-').Dec(gConfig.MaxSize).Str(" criados=").Dec(stats.Guarded)
        .Str(" vivos=").Dec(stats.Live).Str(" quarentena=").Dec(stats.Quarantined)
        .Str(" falhas=").Dec(stats.Fallbacks);
    HangWatch::FaultFrame frame;
    frame.Pc = context->pc.x;
    frame.Lr = context->lr.x;
    frame.Sp = context->sp.x;
    frame.Fp = context->fp.x;
    frame.X0 = context->cpu_gprs[0].x;
    frame.X1 = context->cpu_gprs[1].x;
    frame.X2 = context->cpu_gprs[2].x;
    frame.X19 = context->cpu_gprs[19].x;
    HangWatch::ReportFault("EXCEÇÃO", text.Data(), frame);
}

// Antes dos construtores do jogo (a libnx já montou o sdmc no __appInit).
[[gnu::constructor(101)]] void StartEarly() {
    Start();
}

} // namespace

void Start() noexcept {
    if (gEnabled.load(std::memory_order_acquire)) return;
    std::FILE* file = std::fopen(kConfigPath, "rb");
    if (file == nullptr) return;
    char text[128] = {};
    const size_t read = std::fread(text, 1, sizeof(text) - 1U, file);
    std::fclose(file);
    text[read] = '\0';
    Config config;
    if (!ParseConfig(text, config)) return;
    auto* storage = static_cast<QuarantineEntry*>(
        __real__malloc_r(_REENT, sizeof(QuarantineEntry) * config.QuarantineBlocks));
    if (storage == nullptr && config.QuarantineBlocks != 0U) return;
    for (size_t index = 0; index < config.QuarantineBlocks; ++index) {
        new (&storage[index]) QuarantineEntry();
    }
    mutexInit(&gRingMutex);
    gRing = QuarantineRing(storage, config.QuarantineBlocks);
    gConfig = config;
    gEnabled.store(true, std::memory_order_release);
}

Stats CurrentStats() noexcept {
    Stats stats;
    stats.Enabled = gEnabled.load(std::memory_order_relaxed);
    stats.Guarded = gGuarded.load(std::memory_order_relaxed);
    stats.Live = gLive.load(std::memory_order_relaxed);
    stats.Quarantined = gRing.Count();
    stats.Fallbacks = gFallbacks.load(std::memory_order_relaxed);
    return stats;
}

} // namespace Oot3dNativeGame::GuardAlloc

using namespace Oot3dNativeGame::GuardAlloc;

extern "C" {

void* __wrap__malloc_r(struct _reent* reent, size_t size) {
    // gEnabled antes do TLS: a libnx chama o malloc antes de tudo estar de pé.
    if (gEnabled.load(std::memory_order_relaxed) && InRange(gConfig, size) && !tBypass) {
        if (void* pointer = GuardedAlloc(reent, size)) return pointer;
    }
    return __real__malloc_r(reent, size);
}

void __wrap__free_r(struct _reent* reent, void* pointer) {
    if (pointer != nullptr && gUsed.load(std::memory_order_relaxed)) {
        if (BlockHeader* header = HeaderOf(pointer)) {
            GuardedFree(reent, header);
            return;
        }
    }
    __real__free_r(reent, pointer);
}

void* __wrap__calloc_r(struct _reent* reent, size_t count, size_t size) {
    if (!gEnabled.load(std::memory_order_relaxed)) return __real__calloc_r(reent, count, size);
    size_t total = 0;
    if (!__builtin_mul_overflow(count, size, &total) && InRange(gConfig, total) && !tBypass) {
        if (void* pointer = GuardedAlloc(reent, total)) {
            std::memset(pointer, 0, total);
            return pointer;
        }
    }
    tBypass = true;
    void* const pointer = __real__calloc_r(reent, count, size);
    tBypass = false;
    return pointer;
}

void* __wrap__realloc_r(struct _reent* reent, void* pointer, size_t size) {
    if (pointer == nullptr) return __wrap__malloc_r(reent, size);
    if (!gUsed.load(std::memory_order_relaxed) && !gEnabled.load(std::memory_order_relaxed)) {
        return __real__realloc_r(reent, pointer, size);
    }
    BlockHeader* const header = gUsed.load(std::memory_order_relaxed) ? HeaderOf(pointer)
                                                                       : nullptr;
    const bool guardNew = gEnabled.load(std::memory_order_relaxed) && InRange(gConfig, size);
    if (header == nullptr && !guardNew) {
        tBypass = true;
        void* const moved = __real__realloc_r(reent, pointer, size);
        tBypass = false;
        return moved;
    }
    const size_t oldSize =
        header != nullptr ? header->Size : __real__malloc_usable_size_r(reent, pointer);
    void* const fresh = __wrap__malloc_r(reent, size);
    if (fresh == nullptr) return nullptr;
    std::memcpy(fresh, pointer, oldSize < size ? oldSize : size);
    __wrap__free_r(reent, pointer);
    return fresh;
}

size_t __wrap__malloc_usable_size_r(struct _reent* reent, void* pointer) {
    if (pointer != nullptr && gUsed.load(std::memory_order_relaxed)) {
        if (const BlockHeader* header = HeaderOf(pointer)) return RoundedSize(header->Size);
    }
    return __real__malloc_usable_size_r(reent, pointer);
}

// Handler de exceção da libnx (sempre ligado: grava o relatório e devolve,
// então o crash segue como antes e o Atmosphère ainda gera o crash report).
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump* context) {
    HandleException(context);
}

} // extern "C"

#endif
