#pragma once

// Alocador com páginas de guarda (diagnóstico de corrupção de heap; Switch).
//
// O heap corrompido dos tours 16 a 20 é sempre o mesmo: o chunk livre de
// 0x50 bytes (bin 10) em 0x27fe173a0 com o bk (byte 8 do bloco do usuário)
// trocado por 0x00ed5391'0000001a e o fd intacto. Escrita de 8 bytes num
// objeto de 57 a 72 bytes que já tinha sido liberado (uso após o free), e
// determinística.
//
// Com config/guard_alloc no SD, a partir do início da sessão de jogo (Arm, no
// HangWatch::Start; o boot fica de fora: com guarda desde o construtor o boot
// ficou sem memória, "Not enough space", em 26/10), todo pedido ao malloc com
// tamanho na faixa configurada (padrão 57..72) ganha páginas próprias:
//   [página 0: cabeçalho ... objeto no fim][página 1: folga]
// No free a página 0 perde o acesso (svcSetMemoryPermission) e o bloco entra
// numa quarentena FIFO; só volta ao malloc de verdade quando sai dela. Uma
// escrita (ou leitura) num bloco liberado vira exceção no PC exato; o handler
// de exceção grava em logs/hang.txt o PC, o endereço da falha, as pilhas de
// alocação e de free do bloco atingido e o estado das threads
// (oot3d_hang_watchdog). A página 1 existe porque o cabeçalho do chunk
// seguinte do newlib fica 16 bytes antes do ponteiro alinhado dele: sem ela
// cairia na página 0 (sem acesso) do bloco anterior. Não há página de guarda
// para estouro: a corrupção vista é de uso após o free (fd intacto).
//
// Ideia das páginas de guarda: TheMoonPeople/Chronicle PR #205 (arenas do port
// de Switch com páginas de guarda via svcSetMemoryPermission).
//
// Ligado com -Wl,--wrap=_malloc_r (e _memalign_r, _free_r, _realloc_r,
// _calloc_r, _malloc_usable_size_r): no newlib do devkitA64 cada um mora num
// objeto próprio, então malloc/free/new/delete do processo inteiro passam por
// aqui. O malloc() do devkitA64 manda todo pedido de 32 bytes ou mais para
// _memalign_r(32, n), por isso o objeto fica alinhado a 32 e o _memalign_r
// (alinhamento até 32) também ganha guarda.
// Custo: 8 KiB por bloco na faixa e uma chamada ao kernel no free; só para
// diagnóstico.
//
// config/guard_alloc: "min max quarentena vivos" (bytes, bytes, blocos,
// blocos); vazio = 57 72 4096 32768. A quarentena segura blocos liberados sem
// acesso (4096 = 32 MiB): quanto maior, mais tarde um uso após o free ainda é
// pego. Com `vivos` blocos com guarda vivos (32768 = 256 MiB), os pedidos
// seguintes vão para o malloc normal até algum ser liberado.

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame::GuardAlloc {

inline constexpr size_t kPageSize = 0x1000U;
inline constexpr size_t kBlockPages = 2U;
inline constexpr size_t kBlockBytes = kBlockPages * kPageSize;
inline constexpr uint64_t kMagic = 0x434c414452415547ULL;  // "GUARDALC"
inline constexpr size_t kStackDepth = 12U;
// Alinhamento do objeto: o do malloc() do devkitA64 (_memalign_r(32, n)).
inline constexpr size_t kAlignment = 32U;

struct Config {
    size_t MinSize = 57U;
    size_t MaxSize = 72U;
    size_t QuarantineBlocks = 4096U;
    size_t MaxLive = 32768U;
};

// "min max quarentena vivos"; o que faltar fica no padrão. Falso = texto inválido
// (número ruim, min > max, min 0 ou max que não cabe na página 0).
bool ParseConfig(const char* text, Config& config) noexcept;

// Cabeçalho no início da página 0 (lido de volta pelo free e pelo handler).
struct BlockHeader {
    uint64_t Magic;
    uint64_t Size;
    uint64_t Serial;
    uint64_t AllocTicks;
    uint64_t AllocStack[kStackDepth];
};

constexpr bool InRange(const Config& config, size_t size) noexcept {
    return size >= config.MinSize && size <= config.MaxSize &&
           size <= kPageSize - sizeof(BlockHeader);
}

// Deslocamento do objeto na página 0: o mais perto do fim que o alinhamento
// de 32 deixa (longe do cabeçalho).
constexpr size_t ObjectOffset(size_t size) noexcept {
    return (kPageSize - size) & ~(kAlignment - 1U);
}

// Bytes utilizáveis (malloc_usable_size): até o fim da página 0.
constexpr size_t UsableSize(size_t size) noexcept {
    return kPageSize - ObjectOffset(size);
}

// Bloco na quarentena: o que o handler precisa para dizer quem alocou e
// quem liberou o endereço atingido.
struct QuarantineEntry {
    uintptr_t Page = 0;  // início da página 0 (= ponteiro do memalign)
    uint64_t Size = 0;
    uint64_t Serial = 0;
    uint64_t AllocTicks = 0;
    uint64_t FreeTicks = 0;
    uint32_t FreeThread = 0;
    uint64_t AllocStack[kStackDepth] = {};
    uint64_t FreeStack[kStackDepth] = {};
};

// FIFO de capacidade fixa sobre memória do chamador (sem alocar).
class QuarantineRing {
  public:
    QuarantineRing() noexcept = default;
    QuarantineRing(QuarantineEntry* storage, size_t capacity) noexcept
        : mEntries(storage), mCapacity(capacity) {}

    // Guarda `entry`; se estava cheio, devolve o mais antigo em `evicted`.
    bool Push(const QuarantineEntry& entry, QuarantineEntry& evicted) noexcept {
        if (mCapacity == 0U) {
            evicted = entry;
            return true;
        }
        bool full = mCount == mCapacity;
        if (full) {
            evicted = mEntries[mHead];
            mHead = (mHead + 1U) % mCapacity;
            --mCount;
        }
        mEntries[(mHead + mCount) % mCapacity] = entry;
        ++mCount;
        return full;
    }

    // Bloco cujas páginas contêm `address` (busca linear; só no handler).
    const QuarantineEntry* Find(uintptr_t address) const noexcept {
        for (size_t index = 0; index < mCount; ++index) {
            const QuarantineEntry& entry = mEntries[(mHead + index) % mCapacity];
            if (address >= entry.Page && address < entry.Page + kBlockBytes) return &entry;
        }
        return nullptr;
    }

    [[nodiscard]] size_t Count() const noexcept { return mCount; }

  private:
    QuarantineEntry* mEntries = nullptr;
    size_t mCapacity = 0U;
    size_t mHead = 0U;
    size_t mCount = 0U;
};

struct Stats {
    bool Enabled = false;
    uint64_t Guarded = 0;    // blocos com guarda criados
    uint64_t Live = 0;       // vivos agora
    uint64_t Quarantined = 0;
    uint64_t Fallbacks = 0;  // memalign ou svc falhou: foi para o malloc normal
    uint64_t Skipped = 0;    // teto de vivos: foi para o malloc normal
};

// Lê config/guard_alloc (Switch; construtor, antes do jogo). Não liga.
void Start() noexcept;
// Liga, se o arquivo existia (início da sessão de jogo, depois do boot).
void Arm() noexcept;
[[nodiscard]] Stats CurrentStats() noexcept;

} // namespace Oot3dNativeGame::GuardAlloc
