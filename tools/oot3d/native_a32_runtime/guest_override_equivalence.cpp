// Teste diferencial das substituições nativas (oot3d_native_guest_overrides)
// contra o C++ gerado pelo whole-AOT. Montado por
// run_guest_override_equivalence.py, que extrai as funções traduzidas (e as
// chamadas que elas fazem) do diretório gerado para guest_override_fixture.inc.
//
// Para cada função: milhões de estados aleatórios + casos de borda. As duas
// versões partem de cópias idênticas (estado guest, memória, orçamento de
// blocos, ganchos, modo de rastreio) e precisam terminar com tudo igual:
// GPRs, CPSR, FPSCR, VFP, fluxo de saída, BlocksRemaining/BlocksConsumed,
// estatísticas, sequência de ganchos, bytes da memória, geração de escrita
// global e por página e impressão digital de escrita rastreada.

#include "oot3d_native_whole_aot_runtime.h"
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_guest_overrides.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace Oot3dNativeGame::GeneratedWholeAot {
namespace a32 = oot3d::recomp::a32;
#include "guest_override_fixture.inc"
} // namespace Oot3dNativeGame::GeneratedWholeAot

using namespace Oot3dNativeGame;
namespace a32 = oot3d::recomp::a32;

namespace {

constexpr uint32_t kTextBase = 0x00100000U, kTextEnd = 0x004CA000U;
constexpr uint32_t kRodataEnd = 0x004F9000U;
constexpr uint32_t kDataBase = 0x004F9000U, kDataEnd = 0x005C7000U;
constexpr uint32_t kRamBase = 0x08000000U, kRamSize = 0x8000U;
constexpr uint32_t kRomBase = 0x08100000U, kRomSize = 0x1000U;
constexpr uint32_t kUnmapped = 0x0A000000U;

std::vector<uint8_t> gCode;

bool BuildMemory(NativeA32Memory& memory) {
    std::vector<uint8_t> text(gCode.begin(), gCode.begin() + (kTextEnd - kTextBase));
    std::vector<uint8_t> rodata(gCode.begin() + (kTextEnd - kTextBase),
                                gCode.begin() + (kRodataEnd - kTextBase));
    std::vector<uint8_t> data(kDataEnd - kDataBase, 0U);
    const size_t fileData = std::min<size_t>(gCode.size() - (kDataBase - kTextBase),
                                             data.size());
    std::copy_n(gCode.begin() + (kDataBase - kTextBase), fileData, data.begin());
    std::vector<uint8_t> ram(kRamSize, 0U), rom(kRomSize, 0x5AU);
    return memory.MapRegion({"text", kTextBase, text.size(), false, true, text}) &&
           memory.MapRegion({"rodata", kTextEnd, rodata.size(), false, false, rodata}) &&
           memory.MapRegion({"data", kDataBase, data.size(), true, false, data}) &&
           memory.MapRegion({"ram", kRamBase, ram.size(), true, false, ram}) &&
           memory.MapRegion({"rom", kRomBase, rom.size(), false, false, rom});
}

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(uint64_t seed) : engine(seed) {}
    uint32_t U32() { return static_cast<uint32_t>(engine()); }
    uint32_t Below(uint32_t n) { return n == 0U ? 0U : static_cast<uint32_t>(engine() % n); }
    bool Chance(uint32_t percent) { return Below(100U) < percent; }

    // Floats: valores "de jogo", inteiros pequenos, casos especiais e bits crus.
    uint32_t Float() {
        static constexpr uint32_t kSpecial[] = {
            0x00000000U, 0x80000000U, 0x3F800000U, 0xBF800000U, 0x7F800000U,
            0xFF800000U, 0x7FC00000U, 0xFFC00000U, 0x7F800001U, 0x7FBFFFFFU,
            0x00000001U, 0x807FFFFFU, 0x00800000U, 0x80800000U, 0x7F7FFFFFU,
            0xFF7FFFFFU, 0x34000000U, 0x33800000U, 0x4B000000U, 0x3EAAAAABU,
            0x3F000000U, 0x40490FDBU, 0x3FC90FDBU, 0x46490E49U, 0x46490E48U,
            0x39800000U, 0x397FFFFFU, 0x3F490FDBU, 0x3F490FDAU, 0x3727C5ACU,
        };
        switch (Below(10U)) {
        case 0: case 1: case 2: case 3: { // magnitudes de jogo
            const uint32_t exponent = 100U + Below(50U);
            return (Chance(50) ? 0x80000000U : 0U) | (exponent << 23U) |
                   (U32() & 0x007FFFFFU);
        }
        case 4: { // inteiros pequenos convertidos
            const int32_t value = static_cast<int32_t>(Below(2001U)) - 1000;
            float f = static_cast<float>(value);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            return bits;
        }
        case 5: case 6:
            return kSpecial[Below(std::size(kSpecial))];
        case 7: { // vizinhança de especiais
            return kSpecial[Below(std::size(kSpecial))] + Below(5U) - 2U;
        }
        default:
            return U32();
        }
    }

    uint32_t Fpscr() {
        static constexpr uint32_t kModes[] = {
            0x03000000U, 0x03000000U, 0x03000000U, 0x00000000U, 0x03C00000U,
            0x00400000U, 0x00800000U, 0x01000000U, 0x02000000U, 0x03400000U,
            0x00000100U, 0x00001000U, 0x00010000U, 0x00200000U, 0x00008000U,
        };
        // 80%: modos sem armadilha (o caminho normal do jogo); o resto exercita
        // LEN/STRIDE e exceções habilitadas, que a versão nativa recusa.
        uint32_t fpscr = Chance(80) ? kModes[Below(10U)] : kModes[Below(std::size(kModes))];
        if (Chance(30)) fpscr |= U32() & 0x9FU;          // exceções acumuladas
        if (Chance(30)) fpscr |= U32() & 0xF0000000U;    // NZCV
        if (Chance(3)) fpscr = U32();
        return fpscr;
    }

    // Ponteiro aleatório; quase sempre em RAM mapeada e alinhada.
    uint32_t Pointer(uint32_t span = 256U, uint32_t align = 4U) {
        const uint32_t kind = Below(100U);
        if (kind < 82U) {
            const uint32_t limit = kRamSize > span ? kRamSize - span : 0U;
            return (kRamBase + Below(limit)) & ~(align - 1U);
        }
        if (kind < 87U) { // perto do fim de página ou da região
            const uint32_t page = kRamBase + 0x1000U * Below(kRamSize / 0x1000U);
            return page + 0x1000U - Below(64U);
        }
        if (kind < 90U) return kRamBase + kRamSize - Below(96U);
        if (kind < 92U) return kRomBase + Below(kRomSize);
        if (kind < 94U) return kUnmapped + Below(0x1000U);
        if (kind < 97U) return kDataBase + (Below(kDataEnd - kDataBase - 512U) & ~3U);
        return U32();
    }
};

struct HookLog {
    std::vector<uint32_t> Pcs;
};

void RecordHook(uint32_t pc, a32::GuestState&, a32::MemoryBus&, void* user) {
    static_cast<HookLog*>(user)->Pcs.push_back(pc);
}

using Fn = Oot3dWholeAotFlow (*)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,
                                 Oot3dAotArchitecturalState&, uint32_t);
using OverrideFn = bool (*)(Oot3dWholeAotFrame&, Oot3dWholeAotContext&,
                            Oot3dAotArchitecturalState&, Oot3dWholeAotFlow&) noexcept;

struct Scenario {
    a32::GuestState Guest{};
    uint32_t Budget = 0;
    uint32_t HookMode = 0; // 0 nenhum, 1 fora, 2 dentro, 3 todos
    bool SkipFirst = false;
    bool Trace = false;
};

struct Outcome {
    a32::GuestState Guest{};
    Oot3dWholeAotFlow Flow{};
    uint32_t Remaining = 0, Consumed = 0;
    Oot3dWholeAotStats Stats{};
    std::vector<uint32_t> Hooks;
    uint64_t Generation = 0;
    uint64_t Trace = 0;
};

Outcome Run(const GeneratedWholeAot::OverrideCase& testCase, bool patched,
            const Scenario& scenario, NativeA32Memory& memory) {
    Outcome out;
    out.Guest = scenario.Guest;
    memory.EnableWriteTraceFingerprint(scenario.Trace);
    Oot3dWholeAotStats stats{};
    Oot3dWholeAotContext context{memory, memory, stats};
    HookLog log;
    const std::array<uint32_t, 2> outside{0x00100000U, 0x00600000U};
    const std::array<uint32_t, 3> inside{0x00100000U, testCase.Entry, 0x00600000U};
    if (scenario.HookMode != 0U) {
        context.BlockEntry = RecordHook;
        context.BlockEntryUser = &log;
        if (scenario.HookMode == 1U) {
            context.BlockEntryPcs = outside.data();
            context.BlockEntryPcCount = outside.size();
        } else if (scenario.HookMode == 2U) {
            context.BlockEntryPcs = inside.data();
            context.BlockEntryPcCount = inside.size();
        }
        context.RefreshBlockEntryGate();
    }
    context.SkipFirstBlockEntry = scenario.SkipFirst;
    context.BlocksRemaining = scenario.Budget;
    Oot3dWholeAotFrame frame(out.Guest);
    Oot3dAotArchitecturalState state(out.Guest);
    Oot3dWholeAotFlow flow{};
    bool done = false;
    if (patched && Oot3dGuestOverridesEnabled()) {
        done = testCase.Native(frame, context, state, flow);
    }
    if (!done) flow = testCase.Translated(frame, context, state, testCase.Entry);
    state.Flush(0x7FFFU, true);
    out.Flow = flow;
    out.Remaining = context.BlocksRemaining;
    out.Consumed = context.BlocksConsumed;
    out.Stats = stats;
    out.Hooks = std::move(log.Pcs);
    out.Generation = memory.WriteGeneration();
    out.Trace = scenario.Trace ? memory.WriteTraceFingerprint() : 0U;
    memory.EnableWriteTraceFingerprint(false);
    return out;
}

bool SameStats(const Oot3dWholeAotStats& a, const Oot3dWholeAotStats& b) {
    return a.Calls == b.Calls && a.DirectCalls == b.DirectCalls &&
           a.IndirectCalls == b.IndirectCalls &&
           a.ResolvedIndirectCalls == b.ResolvedIndirectCalls &&
           a.ExternalCalls == b.ExternalCalls && a.SvcExits == b.SvcExits &&
           a.BlockLimitExits == b.BlockLimitExits &&
           a.MemoryFaults == b.MemoryFaults &&
           a.UnsupportedExits == b.UnsupportedExits;
}

std::string Describe(const Outcome& o) {
    char buffer[2048];
    int n = std::snprintf(buffer, sizeof(buffer),
        "flow=%u pc=%08X det=%08X rem=%u cons=%u gen=%" PRIu64 " trace=%016" PRIx64
        " faults=%" PRIu64 " limits=%" PRIu64 " calls=%" PRIu64 " hooks=%zu\n  r:",
        static_cast<unsigned>(o.Flow.Kind), o.Flow.Pc, o.Flow.Detail, o.Remaining,
        o.Consumed, o.Generation, o.Trace, o.Stats.MemoryFaults,
        o.Stats.BlockLimitExits, o.Stats.DirectCalls, o.Hooks.size());
    for (int i = 0; i < 16; ++i) n += std::snprintf(buffer + n, sizeof(buffer) - n, " %08X", o.Guest.r[i]);
    n += std::snprintf(buffer + n, sizeof(buffer) - n, "\n  cpsr=%08X fpscr=%08X\n  s:",
                       o.Guest.cpsr, o.Guest.fpscr);
    for (int i = 0; i < 32; ++i) n += std::snprintf(buffer + n, sizeof(buffer) - n, " %08X", o.Guest.vfp[i]);
    return buffer;
}

std::string Mismatch(const Outcome& a, const Outcome& b) {
    if (a.Guest.r != b.Guest.r) return "registradores";
    if (a.Guest.cpsr != b.Guest.cpsr) return "cpsr";
    if (a.Guest.fpscr != b.Guest.fpscr) return "fpscr";
    if (a.Guest.vfp != b.Guest.vfp) return "vfp";
    if (a.Flow.Kind != b.Flow.Kind || a.Flow.Pc != b.Flow.Pc ||
        a.Flow.Detail != b.Flow.Detail) return "fluxo";
    if (a.Remaining != b.Remaining || a.Consumed != b.Consumed) return "orcamento";
    if (!SameStats(a.Stats, b.Stats)) return "estatisticas";
    if (a.Hooks != b.Hooks) return "ganchos";
    if (a.Generation != b.Generation) return "geracao";
    if (a.Trace != b.Trace) return "rastreio";
    return {};
}

bool SameMemory(NativeA32Memory& a, NativeA32Memory& b, bool full,
                std::string* where) {
    const auto compare = [&](uint32_t base, uint32_t size) {
        const uint8_t* pa = a.GetReadPointer(base, size);
        const uint8_t* pb = b.GetReadPointer(base, size);
        if (pa == nullptr || pb == nullptr || std::memcmp(pa, pb, size) != 0) {
            char text[64];
            std::snprintf(text, sizeof(text), "bytes em %08X", base);
            *where = text;
            return false;
        }
        for (uint32_t page = base; page < base + size; page += 0x1000U) {
            if (a.RangeWriteGeneration(page, 0x1000U) != b.RangeWriteGeneration(page, 0x1000U)) {
                char text[64];
                std::snprintf(text, sizeof(text), "geracao da pagina %08X", page);
                *where = text;
                return false;
            }
        }
        return true;
    };
    if (!compare(kRamBase, kRamSize)) return false;
    if (full && !compare(kDataBase, kDataEnd - kDataBase)) return false;
    return true;
}

// Preparação específica por função: dirige as entradas para os caminhos que
// importam (sem impedir os caminhos aleatórios).
void Prepare(uint32_t entry, Rng& rng, Scenario& s,
             const std::function<void(uint32_t, uint32_t)>& poke) {
    auto& g = s.Guest;
    // Alocador simples na metade baixa da RAM (a pilha fica na metade alta).
    uint32_t next = kRamBase + (rng.Below(64U) << 2U);
    const auto alloc = [&](uint32_t bytes) {
        const uint32_t address = next;
        next += (bytes + 3U) & ~3U;
        return address;
    };
    // Amostras de áudio: pequenas, grandes (estouro em mul/mla), extremos.
    const auto sample = [&]() -> uint32_t {
        switch (rng.Below(8U)) {
        case 0: return 0U;
        case 1: return rng.Chance(50) ? 0x80000000U : 0x7FFFFFFFU;
        case 2: case 3: return rng.U32();
        default: return static_cast<uint32_t>(static_cast<int32_t>(rng.Below(65536U)) - 32768);
        }
    };
    const auto coefficient = [&]() -> uint32_t {
        return rng.Chance(70) ? rng.Below(256U) : (rng.Chance(50) ? rng.U32() : sample());
    };
    const auto samples = [&](uint32_t address, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) poke(address + 4U * i, sample());
    };
    // Buffer de 640 bytes: quase sempre o alocado; às vezes em outra região
    // rápida (dados), atravessando página, desalinhado, somente leitura, não
    // mapeado, sobre o objeto ou aleatório (esses últimos devem ser recusados).
    std::vector<uint32_t> buffers;
    const auto buffer = [&](uint32_t normal, uint32_t obj) -> uint32_t {
        uint32_t chosen = normal;
        if (rng.Chance(95)) {
        } else switch (rng.Below(9U)) {
        case 0: chosen = kDataBase + (rng.Below(kDataEnd - kDataBase - 640U) & ~3U); break;
        case 1: chosen = kRamBase + 0x1000U * (1U + rng.Below(3U)) - 4U * (1U + rng.Below(159U)); break;
        case 2: chosen = normal + 1U + rng.Below(3U); break;
        case 3: chosen = kRomBase + (rng.Below(kRomSize) & ~3U); break;
        case 4: chosen = kUnmapped + (rng.Below(0x1000U) & ~3U); break;
        case 5: chosen = obj + 4U * rng.Below(0x40U) - 600U; break;
        case 6: if (!buffers.empty()) chosen = buffers[rng.Below(static_cast<uint32_t>(buffers.size()))]; break;
        case 7: chosen = kTextBase + (rng.Below(0x1000U) & ~3U); break;
        default: chosen = rng.U32(); break;
        }
        buffers.push_back(chosen);
        return chosen;
    };
    const auto objectAddress = [&](uint32_t size) {
        // Às vezes o objeto encosta no fim de uma página (campos em duas páginas).
        if (rng.Chance(8)) return kRamBase + 0x3000U - 4U * rng.Below(size / 4U);
        return alloc(size);
    };
    switch (entry) {
    case 0x00371738U: { // Runtime_Memcpy
        if (!rng.Chance(94)) break;
        uint32_t len = 0U;
        switch (rng.Below(8U)) {
        case 0: len = rng.Below(32U); break;
        case 1: len = rng.Below(160U); break;
        case 2: len = 32U * rng.Below(64U); break;
        case 3: len = 32U * rng.Below(64U) + rng.Below(32U); break;
        case 4: len = rng.Below(0x1800U); break;
        case 5: len = 0x1000U + rng.Below(0x2000U); break;
        case 6: len = rng.Chance(50) ? rng.U32() : 0xFFFFFFE0U + rng.Below(32U); break;
        default: len = rng.Below(700U); break;
        }
        const auto place = [&](uint32_t other) -> uint32_t {
            const uint32_t room = len < 0x3800U ? 0x3800U - len : 4U;
            switch (rng.Below(20U)) {
            case 0: return kDataBase + (rng.Below(kDataEnd - kDataBase - 0x4000U) & ~3U);
            case 1: return kRamBase + 0x1000U * (1U + rng.Below(3U)) - 4U * rng.Below(40U);
            case 2: return other + 4U * rng.Below(64U) - 128U;          // sobreposição
            case 3: return (kRamBase + rng.Below(room)) | (1U + rng.Below(3U)); // desalinhado
            case 4: return g.r[13] - 4U * rng.Below(48U);                // sobre a pilha
            case 5: return kRomBase + (rng.Below(kRomSize) & ~3U);
            case 6: return kTextBase + (rng.Below(0x1000U) & ~3U);
            case 7: return kUnmapped + (rng.Below(0x1000U) & ~3U);
            case 8: return rng.U32() & ~3U;
            default: return (kRamBase + rng.Below(room)) & ~3U;
            }
        };
        const uint32_t dst = place(g.r[1]);
        const uint32_t src = place(dst);
        g.r[0] = dst;
        g.r[1] = src;
        g.r[2] = len;
        if (rng.Chance(30)) s.Budget = rng.Below(len / 32U + 6U);
        break;
    }
    case 0x004A022CU: { // atraso de 4 canais
        if (!rng.Chance(92)) break;
        const uint32_t obj = objectAddress(0x60U);
        const uint32_t channels = rng.Chance(95) ? rng.Below(5U) : 5U + rng.Below(3U);
        const uint32_t enabled = rng.Chance(90) ? 1U + rng.Below(255U) : 0U;
        poke(obj + 0x54U, (rng.U32() & 0xFF0000FFU) | (channels << 8U) | (enabled << 16U));
        const uint32_t frameIndex = rng.Below(4U);
        poke(obj + 0x40U, frameIndex);
        poke(obj + 0x3CU, rng.Chance(80) ? frameIndex + 1U + rng.Below(3U) : rng.Below(4U));
        for (uint32_t offset : {0x44U, 0x48U, 0x4CU}) poke(obj + offset, coefficient());
        const uint32_t list = alloc(16U);
        for (uint32_t ch = 0; ch < 4U; ++ch) {
            poke(obj + 0x2CU + 4U * ch, sample());
            const uint32_t io = alloc(640U);
            const uint32_t delay = alloc(640U);
            samples(io, 160U);
            samples(delay, 160U);
            uint32_t ioPointer = io;
            if (rng.Chance(3)) ioPointer = delay;              // processamento no lugar
            if (rng.Chance(2)) ioPointer = g.r[13] - 256U;     // sobre a pilha: recusa
            poke(list + 4U * ch, buffer(ioPointer, obj));
            poke(obj + 0x1CU + 4U * ch, buffer(delay, obj) - frameIndex * 640U);
        }
        g.r[0] = obj;
        g.r[1] = list;
        if (rng.Chance(40)) s.Budget = rng.Below(2200U);
        break;
    }
    case 0x004A0338U: { // reverberação estéreo
        if (!rng.Chance(92)) break;
        const uint32_t obj = objectAddress(0x104U);
        poke(obj + 0x100U, (rng.U32() & 0xFFFFFF00U) | (rng.Chance(90) ? 1U + rng.Below(255U) : 0U));
        // (índice, campo do índice, campo do limite, base do ponteiro, passo)
        const uint32_t fields[5][4] = {
            {0x9CU, 0x98U, 0x38U, 4U}, {0xA4U, 0xA0U, 0x48U, 4U}, {0xB0U, 0xA8U, 0x58U, 8U},
            {0xB4U, 0xACU, 0x5CU, 8U}, {0xC4U, 0xC0U, 0x78U, 4U},
        };
        for (const auto& field : fields) {
            const uint32_t index = rng.Below(40U);
            poke(obj + field[0], index);
            poke(obj + field[1], rng.Chance(70) ? index + 160U + rng.Below(40U) : rng.Below(400U));
            for (uint32_t ch = 0; ch < 2U; ++ch) {
                const uint32_t line = alloc(640U);
                samples(line, 160U);
                poke(obj + field[2] + field[3] * ch, buffer(line, obj) - index * 4U);
            }
        }
        for (uint32_t offset : {0xB8U, 0xBCU, 0xC8U, 0xDCU, 0xE0U, 0xE8U}) poke(obj + offset, coefficient());
        for (uint32_t ch = 0; ch < 2U; ++ch) poke(obj + 0xCCU + 4U * ch, sample());
        const uint32_t list = alloc(16U);
        for (uint32_t ch = 0; ch < 4U; ++ch) {
            const uint32_t io = alloc(640U);
            samples(io, 160U);
            poke(list + 4U * ch, buffer(rng.Chance(2) ? g.r[13] - 300U : io, obj));
        }
        g.r[0] = obj;
        g.r[1] = list;
        if (rng.Chance(40)) s.Budget = rng.Below(3200U);
        break;
    }
    case 0x002F9430U: { // PauseRenderBuffer_SetVec2Range
        if (rng.Chance(85)) {
            g.r[2] = rng.Chance(10) ? 0U : (rng.Chance(5) ? 0x80000000U | rng.U32() : rng.Below(24U) + 1U);
            g.r[3] = rng.Below(32U);
            g.r[1] = rng.Pointer(0x200U);
            if (rng.Chance(90)) poke(g.r[0] + 0x1CU, kRamBase + (rng.Below(kRamSize - 0x400U) & ~3U));
        }
        break;
    }
    case 0x003094CCU: // índice pequeno dentro da estrutura
        if (rng.Chance(85)) g.r[1] = rng.Below(8U);
        break;
    case 0x0036E168U: { // Math_SmoothStepToF: contexto global com taxa de atualização
        if (rng.Chance(90)) {
            const uint32_t context = kRamBase + (rng.Below(kRamSize - 0x200U) & ~3U);
            poke(0x0051B2F4U, context);
            const uint32_t rate = rng.Chance(70) ? 1U + rng.Below(3U) : rng.U32() & 0xFFFFU;
            poke(context + 0x110U, (rng.U32() & 0xFFFF0000U) | rate);
        }
        if (rng.Chance(15)) poke(g.r[0], g.vfp[0]); // alvo igual ao valor atual
        if (rng.Chance(30)) { // alvo perto do valor atual (caminhos de limite)
            float current = static_cast<float>(static_cast<int>(rng.Below(2001U)) - 1000) * 0.01f;
            float target = current + (static_cast<float>(static_cast<int>(rng.Below(201U)) - 100) *
                                      (rng.Chance(50) ? 1.0e-7f : 0.01f));
            uint32_t bits;
            std::memcpy(&bits, &current, 4);
            poke(g.r[0], bits);
            std::memcpy(&g.vfp[0], &target, 4);
        }
        break;
    }
    case 0x003325F0U: { // Math3D_CylOutsideCylDist: dois cilindros com floats de jogo
        if (rng.Chance(85)) {
            g.r[0] = rng.Pointer(64U);
            g.r[1] = rng.Pointer(64U);
            g.r[2] = rng.Pointer(16U);
            g.r[3] = rng.Pointer(16U);
            for (uint32_t base : {g.r[0], g.r[1]}) {
                for (uint32_t i = 0; i < 6U; ++i) {
                    // raio, altura, deslocamento, posição: valores de jogo,
                    // às vezes próximos para exercitar a sobreposição.
                    const float value = static_cast<float>(static_cast<int>(rng.Below(401U)) - 200) *
                                        (rng.Chance(30) ? 0.05f : 1.0f);
                    uint32_t bits;
                    std::memcpy(&bits, &value, 4);
                    poke(base + 4U * i, rng.Chance(90) ? bits : rng.Float());
                }
            }
        }
        break;
    }
    case 0x00307C94U: { // UploadVertexFloatUniforms: escritor, lista e origem em RAM
        if (rng.Chance(85)) {
            g.r[2] = rng.Chance(8) ? (rng.Chance(50) ? 0U : rng.U32()) : 1U + rng.Below(12U);
            g.r[3] = rng.Pointer(0x100U);
            const uint32_t list = kRamBase + (rng.Below(kRamSize - 0x400U) & ~3U);
            if (rng.Chance(95)) poke(g.r[0] + 8U, rng.Chance(97) ? list : rng.Pointer());
        }
        break;
    }
    case 0x003727F0U: { // Math_SinF: ângulos típicos, minúsculos, zero e enormes
        const uint32_t kind = rng.Below(100U);
        float angle = 0.0f;
        if (kind < 55U) {
            angle = (static_cast<float>(rng.Below(2000001U)) - 1000000.0f) * 1.0e-5f *
                    (rng.Chance(20) ? 1000.0f : 10.0f);
        } else if (kind < 65U) {
            angle = (static_cast<float>(rng.Below(2001U)) - 1000.0f) * 1.0e-7f; // < 2^-12
        } else if (kind < 70U) {
            angle = rng.Chance(50) ? 0.0f : -0.0f;
        } else if (kind < 75U) {
            angle = (static_cast<float>(rng.Below(2001U)) - 1000.0f) * 20.0f; // perto de 12867
        }
        if (kind < 75U) std::memcpy(&g.vfp[0], &angle, 4);
        break;
    }
    default:
        break;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s code.bin [tentativas] [semente] [entrada]\n", argv[0]);
        return 2;
    }
    {
        std::ifstream file(argv[1], std::ios::binary);
        gCode.assign(std::istreambuf_iterator<char>(file), {});
        if (gCode.size() < kDataBase - kTextBase) {
            std::fprintf(stderr, "code.bin curto demais\n");
            return 2;
        }
    }
    const uint64_t trials = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1000000ULL;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 0x5EEDULL;
    const uint32_t only = argc > 4 ? static_cast<uint32_t>(std::strtoul(argv[4], nullptr, 16)) : 0U;

    NativeA32Memory memoryA, memoryB;
    if (!BuildMemory(memoryA) || !BuildMemory(memoryB)) {
        std::fprintf(stderr, "falha ao mapear a memoria\n");
        return 2;
    }
    int failures = 0;
    for (const auto& testCase : GeneratedWholeAot::kOverrideCases) {
        if (only != 0U && testCase.Entry != only) continue;
        Rng rng(seed ^ (uint64_t{testCase.Entry} << 20U));
        const auto* table = Oot3dGuestOverrideTable();
        const Oot3dGuestOverrideInfo* info = nullptr;
        for (size_t i = 0; i < Oot3dGuestOverrideCount(); ++i) {
            if (table[i].Entry == testCase.Entry) info = &table[i];
        }
        const uint64_t callsBefore = info ? info->Calls : 0U;
        const uint64_t declinedBefore = info ? info->Declined : 0U;
        uint64_t flows[6] = {};
        // Para o microbenchmark: cenários aceitos pela versão nativa, com a RAM
        // exatamente como estava antes da chamada e sem orçamento curto.
        std::vector<std::pair<Scenario, std::vector<uint8_t>>> accepted;
        const bool benchEnabled = std::getenv("GUEST_OVERRIDE_BENCH") != nullptr;
        bool failed = false;
        for (uint64_t trial = 0; trial < trials && !failed; ++trial) {
            Scenario s;
            auto& g = s.Guest;
            for (int i = 0; i < 13; ++i) g.r[i] = rng.Chance(i < 4 ? 85 : 50) ? rng.Pointer() : rng.U32();
            g.r[13] = rng.Chance(92) ? ((kRamBase + 0x4000U + rng.Below(0x3800U)) & ~7U) : rng.Pointer();
            g.r[14] = rng.Chance(97) ? ((0x00100000U + rng.Below(0x3C0000U)) & ~3U)
                                     : testCase.Entry + 4U * rng.Below(64U);
            g.r[15] = testCase.Entry;
            g.cpsr = (rng.U32() & 0xF80F0000U) | 0x10U;
            g.fpscr = rng.Fpscr();
            for (auto& v : g.vfp) v = rng.Float();
            s.Budget = rng.Chance(80) ? 1U << 20U : rng.Below(48U);
            const uint32_t hook = rng.Below(100U);
            s.HookMode = hook < 85U ? 0U : hook < 90U ? 1U : hook < 95U ? 2U : 3U;
            s.SkipFirst = rng.Chance(20);
            s.Trace = rng.Chance(4);
            // Conteúdo aleatório perto de cada ponteiro (escrito igual nas duas memórias).
            std::vector<std::pair<uint32_t, uint32_t>> pokes;
            const auto poke = [&](uint32_t address, uint32_t value) {
                pokes.emplace_back(address, value);
            };
            Prepare(testCase.Entry, rng, s, poke);
            std::array<uint8_t, 512> window{};
            const auto fill = [&](uint32_t address) {
                for (size_t i = 0; i < window.size(); i += 4) {
                    const uint32_t word = rng.Chance(75) ? rng.Float() : rng.U32();
                    std::memcpy(window.data() + i, &word, 4);
                }
                const uint32_t start = address - 64U;
                for (auto* memory : {&memoryA, &memoryB}) {
                    if (memory->IsWritable(start, window.size())) {
                        memory->WriteBytes(start, window);
                    }
                }
            };
            // Primeiro o conteúdo aleatório (perto dos registradores e dos
            // ponteiros plantados), depois os valores plantados por Prepare.
            for (int i = 0; i < 4; ++i) fill(g.r[i]);
            fill(g.r[13] - 256U);
            for (const auto& [address, value] : pokes) {
                if (value >= kRamBase && value < kRamBase + kRamSize) fill(value);
            }
            for (const auto& [address, value] : pokes) {
                for (auto* memory : {&memoryA, &memoryB}) {
                    if (memory->IsWritable(address, 4U)) memory->WriteHost<uint32_t>(address, value);
                }
            }
            std::vector<uint8_t> snapshot;
            if (benchEnabled && accepted.size() < 256U && !s.Trace && s.HookMode == 0U &&
                s.Budget == (1U << 20U) && (s.Guest.fpscr & 0x00C00000U) == 0U) {
                snapshot.resize(kRamSize);
                memoryA.ReadBytes(kRamBase, snapshot);
            }
            const Outcome a = Run(testCase, false, s, memoryA);
            const uint64_t callsBeforeTrial = info ? info->Calls : 0U;
            const Outcome b = Run(testCase, true, s, memoryB);
            if (!snapshot.empty() && info && info->Calls != callsBeforeTrial &&
                a.Consumed > 3U) { // caminho completo, não só o retorno antecipado
                accepted.emplace_back(s, std::move(snapshot));
            }
            ++flows[std::min<size_t>(static_cast<size_t>(a.Flow.Kind), 5U)];
            std::string why = Mismatch(a, b);
            std::string where;
            if (why.empty() && !SameMemory(memoryA, memoryB, (trial & 255U) == 0U, &where)) {
                why = "memoria (" + where + ")";
            }
            if (!why.empty()) {
                std::printf("FALHA %08X %s tentativa %" PRIu64 ": %s\n", testCase.Entry,
                            testCase.Name, trial, why.c_str());
                std::printf(" entrada budget=%u hook=%u skip=%d trace=%d\n", s.Budget,
                            s.HookMode, s.SkipFirst, s.Trace);
                Outcome input;
                input.Guest = s.Guest;
                std::printf(" estado inicial:\n  %s\n", Describe(input).c_str());
                std::printf(" traduzida:\n  %s\n", Describe(a).c_str());
                std::printf(" nativa:\n  %s\n", Describe(b).c_str());
                failed = true;
                ++failures;
            }
        }
        if (!failed) {
            std::string where;
            if (!SameMemory(memoryA, memoryB, true, &where)) {
                std::printf("FALHA %08X %s memoria final (%s)\n", testCase.Entry,
                            testCase.Name, where.c_str());
                ++failures;
                failed = true;
            }
        }
        const uint64_t calls = info ? info->Calls - callsBefore : 0U;
        const uint64_t declined = info ? info->Declined - declinedBefore : 0U;
        std::printf("%s %08X %-36s tentativas=%" PRIu64 " nativa=%" PRIu64 " recusou=%" PRIu64
                    " fluxos[ret=%" PRIu64 " br=%" PRIu64 " lim=%" PRIu64 " falta=%" PRIu64
                    " nsup=%" PRIu64 "]\n",
                    failed ? "FALHOU" : "OK    ", testCase.Entry, testCase.Name, trials,
                    calls, declined, flows[0], flows[1], flows[3], flows[4], flows[5]);
        if (benchEnabled && !accepted.empty() && !failed) {
            // Microbenchmark no host (x86): mesmo conjunto de entradas aceitas,
            // tradução contra substituição; só indicativo da diferença relativa.
            // A tradução roda na memória A e a nativa na B: como as duas fazem
            // as mesmas escritas, as memórias continuam iguais para o próximo caso.
            // Cada chamada parte da mesma RAM (restaurada fora da medição), e o
            // tempo é medido chamada a chamada com o contador de ciclos.
            std::vector<uint8_t> ram(kRamSize);
            const auto bench = [&](bool native) {
                NativeA32Memory& memory = native ? memoryB : memoryA;
                memory.ReadBytes(kRamBase, ram);
                uint64_t cycles = 0U, calls = 0U;
                for (int round = 0; round < 20; ++round) {
                    for (const auto& [scenario, image] : accepted) {
                        memory.WriteBytes(kRamBase, image);
                        a32::GuestState guest = scenario.Guest;
                        Oot3dWholeAotStats stats{};
                        Oot3dWholeAotContext context{memory, memory, stats};
                        context.BlocksRemaining = 1U << 20U;
                        Oot3dWholeAotFrame frame(guest);
                        Oot3dAotArchitecturalState state(guest);
                        Oot3dWholeAotFlow flow{};
                        const uint64_t start = __builtin_ia32_rdtsc();
                        if (!native || !testCase.Native(frame, context, state, flow)) {
                            flow = testCase.Translated(frame, context, state, testCase.Entry);
                        }
                        asm volatile("" : : "r"(&flow), "r"(&guest) : "memory");
                        cycles += __builtin_ia32_rdtsc() - start;
                        ++calls;
                    }
                }
                memory.WriteBytes(kRamBase, ram);
                return static_cast<double>(cycles) / static_cast<double>(calls);
            };
            const double translated = bench(false);
            const double native = bench(true);
            std::printf("  bench %08X: traduzida %.0f ciclos TSC/chamada, nativa %.0f (%.2fx)\n",
                        testCase.Entry, translated, native, translated / native);
            std::string where;
            if (!SameMemory(memoryA, memoryB, true, &where) ||
                memoryA.WriteGeneration() != memoryB.WriteGeneration()) {
                std::printf("FALHA %08X memoria diverge depois do bench (%s)\n",
                            testCase.Entry, where.c_str());
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
