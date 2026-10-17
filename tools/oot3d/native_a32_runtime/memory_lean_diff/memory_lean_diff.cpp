// Teste diferencial de memória (run_memory_lean_diff.sh): NativeA32Memory atual contra a
// implementação anterior (HEAD, renomeada para OldMem). Sequências aleatórias
// de operações aplicadas às duas; depois de cada uma compara retorno, valores
// lidos, endereço de falha, geração arquitetural, gerações por página
// (RangeWriteGeneration), impressão digital de conteúdo/estado/rastreio,
// contadores de vigia, divergências de leitura rápida, eventos do observador,
// exclusivos, spans rápidos, CaptureState e cópias/movimentos/restauração.
#include "oot3d_native_a32_memory.h"
#include "old_memory.h"

#include <nlohmann/json.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

using New = Oot3dNativeGame::NativeA32Memory;
using Old = OldMem::NativeA32Memory;

namespace {

std::mt19937_64 rng;
uint64_t gOps = 0;
std::string gLastOp;

[[noreturn]] void Fail(const char* what, uint64_t a = 0, uint64_t b = 0) {
    std::fprintf(stderr, "DIVERGENCIA op#%" PRIu64 " (%s): %s  nova=0x%" PRIx64
                 " antiga=0x%" PRIx64 "\n", gOps, gLastOp.c_str(), what, a, b);
    std::exit(1);
}

template <typename A, typename B>
void Eq(const char* what, const A& a, const B& b) {
    if (!(a == b)) Fail(what, static_cast<uint64_t>(a), static_cast<uint64_t>(b));
}

uint32_t Rand(uint32_t n) { return static_cast<uint32_t>(rng() % n); }

struct Events {
    std::vector<std::pair<uint32_t, size_t>> Observed;
};

// Mapa: RAM gravável 64 KiB, ROM de 8 KiB, região que não cobre páginas
// inteiras (sem entrada rápida), RAM pequena gravável, e buracos.
template <typename M>
void Map(M& memory) {
    std::vector<uint8_t> ram(0x10000), rom(0x2000), odd(0x1800), small(0x3000);
    for (size_t i = 0; i < ram.size(); ++i) ram[i] = uint8_t(i * 7 + 3);
    for (size_t i = 0; i < rom.size(); ++i) rom[i] = uint8_t(i * 13 + 1);
    for (size_t i = 0; i < odd.size(); ++i) odd[i] = uint8_t(i ^ 0x5A);
    for (size_t i = 0; i < small.size(); ++i) small[i] = uint8_t(i * 3);
    std::string error;
    if (!memory.MapRegion({"ram", 0x10000, ram.size(), true, false, ram}, &error) ||
        !memory.MapRegion({"rom", 0x30000, rom.size(), false, true, rom}, &error) ||
        !memory.MapRegion({"odd", 0x40800, odd.size(), true, false, odd}, &error) ||
        !memory.MapRegion({"small", 0x50000, small.size(), true, false, small}, &error)) {
        std::fprintf(stderr, "map: %s\n", error.c_str());
        std::exit(2);
    }
}

uint32_t RandomAddress() {
    static const uint32_t bases[] = {0x10000, 0x30000, 0x40800, 0x50000, 0x0F000, 0x20000,
                                     0x42000, 0x53000};
    const uint32_t base = bases[Rand(8)];
    switch (Rand(4)) {
    case 0: return base + Rand(0x10000);
    case 1: return base + (Rand(16) << 12) - Rand(9);  // borda de página
    case 2: return base + Rand(0x3000);
    default: return base + (Rand(0x4000) & ~3U);
    }
}

std::vector<uint8_t> RandomBytes(size_t n) {
    std::vector<uint8_t> bytes(n);
    for (auto& b : bytes) b = uint8_t(rng());
    return bytes;
}

struct Pair {
    std::unique_ptr<New> N = std::make_unique<New>();
    std::unique_ptr<Old> O = std::make_unique<Old>();
    Events NE, OE;
    uint64_t NToken = 0, OToken = 0;
    bool HasToken = false;

    void InstallObservers() {
        N->SetWriteObserver([this](uint32_t a, size_t s) { NE.Observed.emplace_back(a, s); });
        O->SetWriteObserver([this](uint32_t a, size_t s) { OE.Observed.emplace_back(a, s); });
    }
};

void CompareState(Pair& p, bool full) {
    New& n = *p.N;
    Old& o = *p.O;
    Eq("WriteGeneration", n.WriteGeneration(), o.WriteGeneration());
    Eq("FastReadMismatchCount", n.FastReadMismatchCount(), o.FastReadMismatchCount());
    Eq("AllowsUnobservedAccess", n.AllowsUnobservedAccess(), o.AllowsUnobservedAccess());
    Eq("RangeWatchActive", n.RangeWatchActive(), o.RangeWatchActive());
    const auto& nc = n.WatchCounters();
    const auto& oc = o.WatchCounters();
    Eq("watch.TypedReads", nc.TypedReads, oc.TypedReads);
    Eq("watch.TypedWrites", nc.TypedWrites, oc.TypedWrites);
    Eq("watch.ByteReads", nc.ByteReads, oc.ByteReads);
    Eq("watch.ByteWrites", nc.ByteWrites, oc.ByteWrites);
    Eq("watch.SuppressedByteWrites", nc.SuppressedByteWrites, oc.SuppressedByteWrites);
    Eq("watch.HostByteWrites", nc.HostByteWrites, oc.HostByteWrites);
    Eq("watch.Fills", nc.Fills, oc.Fills);
    Eq("watch.PointerReads", nc.PointerReads, oc.PointerReads);
    Eq("watch.PointerWrites", nc.PointerWrites, oc.PointerWrites);
    Eq("observed.size", p.NE.Observed.size(), p.OE.Observed.size());
    if (p.NE.Observed != p.OE.Observed) Fail("observed events");
    // Gerações por página e por intervalos aleatórios.
    for (uint32_t page = 0x0F000; page < 0x56000; page += 0x1000) {
        const auto a = n.RangeWriteGeneration(page, 0x1000);
        const auto b = o.RangeWriteGeneration(page, 0x1000);
        Eq("RangeWriteGeneration.has", a.has_value(), b.has_value());
        if (a) Eq("RangeWriteGeneration", *a, *b);
        Eq("SpanOnFastReadPage", n.SpanOnFastReadPage(page + 8, 16), o.SpanOnFastReadPage(page + 8, 16));
        Eq("SpanOnFastWritePage", n.SpanOnFastWritePage(page + 8, 16), o.SpanOnFastWritePage(page + 8, 16));
    }
    for (int i = 0; i < 4; ++i) {
        const uint32_t address = RandomAddress();
        const size_t size = 1 + Rand(0x3000);
        const auto a = n.RangeWriteGeneration(address, size);
        const auto b = o.RangeWriteGeneration(address, size);
        Eq("RangeWriteGeneration(range).has", a.has_value(), b.has_value());
        if (a) Eq("RangeWriteGeneration(range)", *a, *b);
        Eq("HasEscapedWritePointer", n.HasEscapedWritePointer(address, size),
           o.HasEscapedWritePointer(address, size));
    }
    if (full) {
        // Materializa (as duas do mesmo jeito) e compara conteúdo/estado/rastreio.
        Eq("ContentFingerprint", n.ContentFingerprint(), o.ContentFingerprint());
        Eq("StateFingerprint", n.StateFingerprint(), o.StateFingerprint());
        Eq("WriteTraceFingerprint", n.WriteTraceFingerprint(), o.WriteTraceFingerprint());
        if (n.CaptureState() != o.CaptureState()) Fail("CaptureState");
    }
}

template <typename T>
void TypedOp(Pair& p, bool write, bool virt) {
    const uint32_t address = RandomAddress();
    uint32_t nf = 0xEEEEEEEE, of = 0xEEEEEEEE;
    if (write) {
        const T value = static_cast<T>(rng());
        bool a, b;
        if (virt) {
            if constexpr (sizeof(T) == 1) { a = p.N->Write8(address, value); b = p.O->Write8(address, value); }
            else if constexpr (sizeof(T) == 2) { a = p.N->Write16(address, value); b = p.O->Write16(address, value); }
            else if constexpr (sizeof(T) == 4) { a = p.N->Write32(address, value); b = p.O->Write32(address, value); }
            else { a = p.N->Write64(address, value, &nf); b = p.O->Write64(address, value, &of); }
        } else {
            a = p.N->template WriteFast<T>(address, value, &nf);
            b = p.O->template WriteFast<T>(address, value, &of);
        }
        Eq("write.ok", a, b);
    } else {
        T x = 0, y = 0;
        bool a, b;
        if (virt) {
            if constexpr (sizeof(T) == 1) { a = p.N->Read8(address, &x); b = p.O->Read8(address, &y); }
            else if constexpr (sizeof(T) == 2) { a = p.N->Read16(address, &x); b = p.O->Read16(address, &y); }
            else if constexpr (sizeof(T) == 4) { a = p.N->Read32(address, &x); b = p.O->Read32(address, &y); }
            else { a = p.N->Read64(address, &x, &nf); b = p.O->Read64(address, &y, &of); }
        } else {
            a = p.N->template ReadFast<T>(address, &x, &nf);
            b = p.O->template ReadFast<T>(address, &y, &of);
        }
        Eq("read.ok", a, b);
        Eq("read.value", x, y);
    }
    Eq("faultAddress", nf, of);
}

void Step(Pair& p) {
    const uint32_t kind = Rand(100);
    if (kind < 40) {
        const bool write = Rand(2) == 0;
        const bool virt = Rand(4) == 0;
        switch (Rand(4)) {
        case 0: gLastOp = "typed8"; TypedOp<uint8_t>(p, write, virt); break;
        case 1: gLastOp = "typed16"; TypedOp<uint16_t>(p, write, virt); break;
        case 2: gLastOp = "typed32"; TypedOp<uint32_t>(p, write, virt); break;
        default: gLastOp = "typed64"; TypedOp<uint64_t>(p, write, virt); break;
        }
    } else if (kind < 50) {
        gLastOp = "words";
        const uint32_t address = RandomAddress() & ~3U;
        const uint32_t count = Rand(9);
        if (Rand(2)) {
            uint32_t a[8] = {}, b[8] = {};
            Eq("ReadWordsFast", p.N->ReadWordsFast(address, a, count), p.O->ReadWordsFast(address, b, count));
            for (uint32_t i = 0; i < 8; ++i) Eq("ReadWordsFast.value", a[i], b[i]);
        } else {
            uint32_t v[8];
            for (auto& w : v) w = uint32_t(rng());
            Eq("WriteWordsFast", p.N->WriteWordsFast(address, v, count), p.O->WriteWordsFast(address, v, count));
        }
    } else if (kind < 58) {
        const uint32_t address = RandomAddress();
        const size_t size = Rand(5) == 0 ? 0 : 1 + Rand(300);
        switch (Rand(6)) {
        case 0: {
            gLastOp = "ReadBytes";
            std::vector<uint8_t> a(size), b(size);
            Eq("ReadBytes", p.N->ReadBytes(address, a), p.O->ReadBytes(address, b));
            if (a != b) Fail("ReadBytes.value");
            break;
        }
        case 1: {
            gLastOp = "WriteBytes";
            const auto bytes = RandomBytes(size);
            Eq("WriteBytes", p.N->WriteBytes(address, bytes), p.O->WriteBytes(address, bytes));
            break;
        }
        case 2: {
            gLastOp = "WriteHostBytes";
            const auto bytes = RandomBytes(size);
            Eq("WriteHostBytes", p.N->WriteHostBytes(address, bytes), p.O->WriteHostBytes(address, bytes));
            break;
        }
        case 3: {
            gLastOp = "Fill";
            const uint8_t value = uint8_t(rng());
            Eq("Fill", p.N->Fill(address, size, value), p.O->Fill(address, size, value));
            break;
        }
        case 4: {
            gLastOp = "ReadBytesFast";
            std::vector<uint8_t> a(size), b(size);
            Eq("ReadBytesFast", p.N->ReadBytesFast(address, a), p.O->ReadBytesFast(address, b));
            if (a != b) Fail("ReadBytesFast.value");
            break;
        }
        default: {
            gLastOp = "WriteBytesFast";
            const auto bytes = RandomBytes(size);
            Eq("WriteBytesFast", p.N->WriteBytesFast(address, bytes), p.O->WriteBytesFast(address, bytes));
            break;
        }
        }
    } else if (kind < 63) {
        gLastOp = "exclusive";
        const uint32_t address = RandomAddress() & ~3U;
        const uint8_t size = uint8_t(1U << Rand(4));
        uint32_t nf = 0, of = 0;
        if (!p.HasToken || Rand(2)) {
            uint64_t x = 0, y = 0;
            const bool a = p.N->LoadExclusive(address, size, &x, &p.NToken, &nf);
            const bool b = p.O->LoadExclusive(address, size, &y, &p.OToken, &of);
            Eq("LoadExclusive", a, b);
            Eq("LoadExclusive.value", x, y);
            Eq("LoadExclusive.token", p.NToken, p.OToken);
            p.HasToken = a;
        } else {
            const uint64_t value = rng();
            const auto a = p.N->StoreExclusive(address, size, value, p.NToken, &nf);
            const auto b = p.O->StoreExclusive(address, size, value, p.OToken, &of);
            Eq("StoreExclusive", static_cast<int>(a), static_cast<int>(b));
        }
        if (Rand(4) == 0) {
            uint32_t x = 0, y = 0;
            const uint32_t replacement = uint32_t(rng());
            Eq("AtomicSwap", p.N->AtomicSwap(address, 4, replacement, &x, &nf),
               p.O->AtomicSwap(address, 4, replacement, &y, &of));
            Eq("AtomicSwap.previous", x, y);
        }
        Eq("exclusive.fault", nf, of);
    } else if (kind < 66) {
        gLastOp = "trace";
        const bool enabled = Rand(2) == 0;
        p.N->EnableWriteTraceFingerprint(enabled);
        p.O->EnableWriteTraceFingerprint(enabled);
    } else if (kind < 69) {
        gLastOp = "pointer";
        const uint32_t address = RandomAddress();
        const size_t size = 1 + Rand(64);
        if (Rand(2)) {
            const uint8_t* a = p.N->GetReadPointer(address, size);
            const uint8_t* b = p.O->GetReadPointer(address, size);
            Eq("GetReadPointer.null", a == nullptr, b == nullptr);
            if (a && std::memcmp(a, b, size) != 0) Fail("GetReadPointer.bytes");
        } else {
            uint8_t* a = p.N->GetWritePointer(address, size);
            uint8_t* b = p.O->GetWritePointer(address, size);
            Eq("GetWritePointer.null", a == nullptr, b == nullptr);
            if (a) {
                const auto bytes = RandomBytes(size);
                std::memcpy(a, bytes.data(), size);
                std::memcpy(b, bytes.data(), size);
            }
        }
    } else if (kind < 72) {
        gLastOp = "observe";
        if (Rand(6) == 0) {
            p.N->SetWriteObserver({});
            p.O->SetWriteObserver({});
        } else {
            p.InstallObservers();
            const uint32_t address = RandomAddress();
            const size_t size = 1 + Rand(0x1800);
            Eq("ObserveWrites", p.N->ObserveWrites(address, size), p.O->ObserveWrites(address, size));
        }
    } else if (kind < 75) {
        gLastOp = "watch";
        if (Rand(3) == 0) {
            p.N->ClearRangeWatch();
            p.O->ClearRangeWatch();
        } else {
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            for (uint32_t i = 0, n = 1 + Rand(2); i < n; ++i) {
                const uint32_t begin = RandomAddress();
                ranges.emplace_back(begin, begin + 1 + Rand(0x1400));
            }
            p.N->SetRangeWatch(ranges);
            p.O->SetRangeWatch(ranges);
        }
        const bool suppressed = Rand(4) == 0;
        p.N->SetRangeWatchSuppressed(suppressed);
        p.O->SetRangeWatchSuppressed(suppressed);
    } else if (kind < 79) {
        gLastOp = "defer";
        std::vector<std::pair<uint32_t, size_t>> ranges;
        for (uint32_t i = 0, n = 1 + Rand(2); i < n; ++i)
            ranges.emplace_back(RandomAddress(), 1 + Rand(0x900));
        const uint8_t seed = uint8_t(rng());
        auto reader = [seed](uint32_t address, std::span<uint8_t> bytes) {
            for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(seed + address + i);
            return true;
        };
        Eq("DeferWrites", p.N->DeferWrites(ranges, reader), p.O->DeferWrites(ranges, reader));
    } else if (kind < 81) {
        gLastOp = "materialize";
        Eq("MaterializeDeferredWrites", p.N->MaterializeDeferredWrites(), p.O->MaterializeDeferredWrites());
    } else if (kind < 83) {
        gLastOp = "restore";
        // CaptureState/RestoreState (materializa antes, como o savestate).
        p.N->MaterializeDeferredWrites();
        p.O->MaterializeDeferredWrites();
        const auto ns = p.N->CaptureState();
        const auto os = p.O->CaptureState();
        if (ns != os) Fail("CaptureState(restore)");
        // Às vezes restaura um estado com outra geração.
        auto changed = ns;
        if (Rand(2)) changed["write_generation"] = ns["write_generation"].get<uint64_t>() + 1 + Rand(5);
        std::string e1, e2;
        Eq("RestoreState", p.N->RestoreState(changed, &e1), p.O->RestoreState(changed, &e2));
    } else if (kind < 85) {
        gLastOp = "copy";
        p.N->MaterializeDeferredWrites();
        p.O->MaterializeDeferredWrites();
        // Imagem copiada e atribuída de volta (épocas de certificado novas).
        if (Rand(2)) {
            New nc(*p.N);
            Old oc(*p.O);
            nc.WriteFast<uint32_t>(0x10010, 0x12345678U);
            oc.WriteFast<uint32_t>(0x10010, 0x12345678U);
            *p.N = nc;
            *p.O = oc;
        } else {
            New nc(*p.N);
            Old oc(*p.O);
            *p.N = std::move(nc);
            *p.O = std::move(oc);
        }
        p.HasToken = false;
    } else {
        gLastOp = "range";
        // Só consultas (CompareState faz o resto).
    }
}

} // namespace

int main(int argc, char** argv) {
    const uint64_t steps = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 200000;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 0x51A7;
    const uint64_t episodes = argc > 3 ? std::strtoull(argv[3], nullptr, 0) : 50;
    rng.seed(seed);
    for (uint64_t episode = 0; episode < episodes; ++episode) {
        Pair p;
        Map(*p.N);
        Map(*p.O);
        if (Rand(2)) {
            const bool enabled = Rand(2) == 0;
            p.N->EnableWriteTraceFingerprint(enabled);
            p.O->EnableWriteTraceFingerprint(enabled);
        }
        for (uint64_t i = 0; i < steps / episodes; ++i) {
            ++gOps;
            Step(p);
            CompareState(p, (i % 64) == 63);
        }
        gLastOp = "final";
        CompareState(p, true);
    }
    std::printf("memdiff: %" PRIu64 " operações, %" PRIu64 " episódios, zero divergências\n",
                gOps, episodes);
    return 0;
}
