#pragma once

// Alocador com páginas de guarda (diagnóstico de corrupção de heap; Switch).
//
// O heap corrompido dos tours 16 a 20 é sempre o mesmo: o chunk livre de
// 0x50 bytes (bin 10) em 0x27fe173a0 com o bk (byte 8 do bloco do usuário)
// trocado por 0x00ed5391'0000001a e o fd intacto. Escrita de 8 bytes num
// objeto de 57 a 72 bytes que já tinha sido liberado (uso após o free), e
// determinística.
//
// Com config/guard_alloc no SD, todo pedido ao malloc com tamanho na faixa
// configurada (padrão 57..72) ganha páginas próprias:
//   [página 0: cabeçalho ... objeto no fim][página 1: guarda][página 2: folga]
// O objeto termina colado na página de guarda (sem acesso), então um estouro
// para a frente falha na hora. No free a página 0 também perde o acesso e o
// bloco entra numa quarentena FIFO; só volta ao malloc de verdade quando sai
// dela. Uma escrita (ou leitura) num bloco liberado vira exceção no PC exato;
// o handler de exceção grava em logs/hang.txt o PC, o endereço da falha, as
// pilhas de alocação e de free do bloco atingido e o estado das threads
// (oot3d_hang_watchdog). A página 2 existe porque o cabeçalho do chunk
// seguinte do newlib fica 16 bytes antes do ponteiro alinhado dele: sem ela
// cairia na página de guarda do bloco anterior.
//
// Ideia das páginas de guarda: TheMoonPeople/Chronicle PR #205 (arenas do port
// de Switch com páginas de guarda via svcSetMemoryPermission).
//
// Ligado com -Wl,--wrap=_malloc_r (e _free_r, _realloc_r, _calloc_r,
// _malloc_usable_size_r): no newlib do devkitA64 cada um mora num objeto
// próprio, então malloc/free/new/delete do processo inteiro passam por aqui.
// Custo: 12 KiB e uma chamada ao kernel por bloco na faixa, mais uma no free;
// só para diagnóstico.
//
// config/guard_alloc: "min max quarentena" (bytes, bytes, blocos); vazio =
// 57 72 4096. A quarentena segura blocos liberados sem acesso: quanto maior,
// mais tarde um uso após o free ainda é pego (4096 blocos = 48 MiB).

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame::GuardAlloc {

inline constexpr size_t kPageSize = 0x1000U;
inline constexpr size_t kBlockPages = 3U;
inline constexpr size_t kBlockBytes = kBlockPages * kPageSize;
inline constexpr uint64_t kMagic = 0x434c414452415547ULL;  // "GUARDALC"
inline constexpr size_t kStackDepth = 12U;

struct Config {
    size_t MinSize = 57U;
    size_t MaxSize = 72U;
    size_t QuarantineBlocks = 4096U;
};

// "min max quarentena"; o que faltar fica no padrão. Falso = texto inválido
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

constexpr size_t RoundedSize(size_t size) noexcept {
    return (size + 15U) & ~static_cast<size_t>(15U);
}

constexpr bool InRange(const Config& config, size_t size) noexcept {
    return size >= config.MinSize && size <= config.MaxSize &&
           RoundedSize(size) <= kPageSize - sizeof(BlockHeader);
}

// Deslocamento do objeto na página 0: termina no limite da página de guarda.
constexpr size_t ObjectOffset(size_t size) noexcept {
    return kPageSize - RoundedSize(size);
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
};

// Lê config/guard_alloc e liga (Switch; chamado por um construtor bem cedo).
void Start() noexcept;
[[nodiscard]] Stats CurrentStats() noexcept;

} // namespace Oot3dNativeGame::GuardAlloc
