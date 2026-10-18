// Watchdog de travamento (política, texto sem malloc, marcadores) e a
// varredura limitada dos bins do malloc do newlib. Rode também com ASan:
// a varredura nunca pode ler fora do heap sintético, nem com elos
// corrompidos ao acaso.
#include "oot3d_hang_watchdog.h"
#include "oot3d_heap_bin_check.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
using HangWatch::StallTracker;
using HangWatch::Thread;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "oot3d_hang_watchdog_tests: %s\n", message);
        std::exit(1);
    }
}

constexpr size_t kGame = static_cast<size_t>(Thread::Game);
constexpr size_t kRender = static_cast<size_t>(Thread::Render);

StallTracker::Observation Observe(uint64_t now, uint64_t game, uint64_t render,
                                  bool active = true, bool watchRender = true) {
    StallTracker::Observation observation;
    observation.NowMilliseconds = now;
    observation.Active = active;
    observation.Watched[kGame] = true;
    observation.Watched[kRender] = watchRender;
    observation.Progress[kGame] = game;
    observation.Progress[kRender] = render;
    return observation;
}

void TestTrackerReportsOnceAndRecovers() {
    StallTracker tracker;
    uint64_t frames = 0;
    for (uint64_t now = 0; now <= 10000; now += 1000) {
        frames += 60;
        Require(tracker.Observe(Observe(now, frames, frames)).Kind == StallTracker::Verdict::None,
                "progressing threads reported");
    }
    // O jogo para em 10 s; a render thread continua (não fica ociosa).
    StallTracker::Result hang;
    int reports = 0;
    for (uint64_t now = 11000; now <= 20000; now += 1000) {
        const auto verdict = tracker.Observe(Observe(now, frames, now));
        if (verdict.Kind == StallTracker::Verdict::Hang) {
            hang = verdict;
            ++reports;
        }
        Require(verdict.Kind != StallTracker::Verdict::Recovered, "recovered while stalled");
    }
    Require(reports == 1, "a stall must be reported exactly once");
    Require(hang.StalledMask == (1U << kGame), "only the game thread was stalled");
    Require(hang.Milliseconds >= 3000 && hang.Milliseconds < 4000, "report after ~3 s");
    // Volta a andar: "recuperou" uma vez, com a duração do travamento.
    const auto recovered = tracker.Observe(Observe(21000, frames + 1, 21000));
    Require(recovered.Kind == StallTracker::Verdict::Recovered, "recovery not seen");
    Require(recovered.Milliseconds == 11000, "recovered duration");
    // Rearmado: um novo travamento é relatado de novo.
    frames += 1;
    reports = 0;
    for (uint64_t now = 22000; now <= 30000; now += 1000) {
        if (tracker.Observe(Observe(now, frames, frames)).Kind == StallTracker::Verdict::Hang) {
            ++reports;
        }
    }
    Require(reports == 1, "watchdog did not re-arm after recovering");
}

void TestTrackerIgnoresFocusLossAndSuspend() {
    StallTracker tracker;
    Require(tracker.Observe(Observe(0, 1, 1)).Kind == StallTracker::Verdict::None, "start");
    // HOME: nada anda por 20 s, fora de foco.
    for (uint64_t now = 1000; now <= 20000; now += 1000) {
        Require(tracker.Observe(Observe(now, 1, 1, false)).Kind == StallTracker::Verdict::None,
                "stall reported out of focus");
    }
    // De volta ao foco: conta a partir daqui.
    Require(tracker.Observe(Observe(21000, 1, 1)).Kind == StallTracker::Verdict::None,
            "focus regained counted the time away");
    Require(tracker.Observe(Observe(22000, 2, 2)).Kind == StallTracker::Verdict::None,
            "progress after focus");
    // Sleep do console: o watchdog também parou (salto de 60 s).
    Require(tracker.Observe(Observe(82000, 2, 2)).Kind == StallTracker::Verdict::None,
            "a suspension gap was taken as a stall");
    Require(tracker.Observe(Observe(83000, 3, 3)).Kind == StallTracker::Verdict::None,
            "progress after resume");
    // Render thread não vigiada (ociosa): só o jogo conta.
    uint64_t game = 3;
    for (uint64_t now = 84000; now <= 95000; now += 1000) {
        game += 60;
        Require(tracker.Observe(Observe(now, game, 3, true, false)).Kind ==
                    StallTracker::Verdict::None,
                "an idle render thread was reported");
    }
}

void TestText() {
    char buffer[64];
    HangWatch::Text text(buffer, sizeof(buffer));
    text.Str("a=").Dec(0).Str(" b=").Dec(18446744073709551615ULL).Str(" c=").Hex(0xBEEF)
        .Str(" d=").SignedDec(-42);
    Require(std::string(text.Data()) == "a=0 b=18446744073709551615 c=0xbeef d=-42",
            "text formatting");
    char tiny[8];
    HangWatch::Text small(tiny, sizeof(tiny));
    small.Str("0123456789").Dec(12345);
    Require(small.Size() == 7 && std::strlen(tiny) == 7, "text must truncate in place");
}

void TestStageScope() {
    HangWatch::Bind(Thread::Game, 7U);
    HangWatch::Mark(HangWatch::Stage::GuestRefresh);
    {
        HangWatch::StageScope scope(HangWatch::Stage::WaitDrain);
        Require(HangWatch::CurrentStage() == HangWatch::Stage::WaitDrain, "scope stage");
    }
    Require(HangWatch::CurrentStage() == HangWatch::Stage::GuestRefresh, "scope restore");
    for (size_t stage = 0; stage < static_cast<size_t>(HangWatch::Stage::Count); ++stage) {
        Require(HangWatch::StageName(static_cast<uint8_t>(stage))[0] != '?', "stage name");
    }
    // Uma thread sem slot não escreve nada.
    std::thread([] {
        HangWatch::Mark(HangWatch::Stage::RenderSwap);
        HangWatch::Advance();
    }).join();
    Require(HangWatch::gSlots[kGame].CurrentStage.load() ==
                static_cast<uint8_t>(HangWatch::Stage::GuestRefresh),
            "an unbound thread wrote a slot");
}

// ---- heap sintético no layout do newlib ----------------------------------------

struct Chunk {
    size_t PrevSize;
    size_t Size;
    Chunk* Fd;
    Chunk* Bk;
};

struct SyntheticHeap {
    std::vector<uint64_t> Storage = std::vector<uint64_t>(1U << 16);  // 512 KiB
    std::vector<void*> Av = std::vector<void*>(2U * kNewlibMallocBins + 2U);
    size_t Next = 0;

    SyntheticHeap() {
        for (size_t bin = 0; bin < kNewlibMallocBins; ++bin) {
            Chunk* header = Header(bin);
            Av[2U * bin + 2U] = header;  // fd
            Av[2U * bin + 3U] = header;  // bk
        }
    }
    [[nodiscard]] uintptr_t Begin() const { return reinterpret_cast<uintptr_t>(Storage.data()); }
    [[nodiscard]] uintptr_t End() const {
        return reinterpret_cast<uintptr_t>(Storage.data() + Storage.size());
    }
    Chunk* Header(size_t bin) {
        return reinterpret_cast<Chunk*>(reinterpret_cast<char*>(&Av[2U * bin + 2U]) -
                                        2U * sizeof(size_t));
    }
    Chunk* Allocate(size_t bytes) {
        auto* chunk = reinterpret_cast<Chunk*>(reinterpret_cast<char*>(Storage.data()) + Next);
        Next += bytes;
        Require(Next <= Storage.size() * sizeof(uint64_t), "synthetic heap full");
        chunk->PrevSize = 0;
        chunk->Size = bytes | 1U;  // PREV_INUSE
        chunk->Fd = chunk->Bk = nullptr;
        return chunk;
    }
    // frontlink: insere no começo do bin.
    void Free(size_t bin, Chunk* chunk) {
        Chunk* header = Header(bin);
        chunk->Bk = header;
        chunk->Fd = header->Fd;
        header->Fd->Bk = chunk;
        header->Fd = chunk;
    }
    void Top(size_t bytes) { Av[2] = Allocate(bytes); }
    HeapBinCheckResult Check(uint64_t maxSteps = 1U << 20) {
        return CheckNewlibMallocBins(Av.data(), kNewlibMallocBins, Begin(), End(), maxSteps);
    }
};

void TestHeapWalkMatchesMallinfo() {
    SyntheticHeap heap;
    auto result = heap.Check();
    Require(result.Ok && result.FreeBytes == 0 && result.FreeChunks == 0, "empty heap");
    uint64_t expected = 0;
    for (size_t index = 0; index < 50; ++index) {
        Chunk* chunk = heap.Allocate(32U + 16U * (index % 7U));
        heap.Free(1U + index % 9U, chunk);
        expected += chunk->Size & ~uintptr_t{3};
    }
    heap.Top(4096);
    result = heap.Check();
    Require(result.Ok, "valid heap reported corrupt");
    Require(result.FreeChunks == 50 && result.TopBytes == 4096, "chunk count / top");
    Require(result.FreeBytes == expected + 4096, "free bytes (mallinfo fordblks)");
}

void TestHeapWalkStopsOnCorruption() {
    struct Case {
        const char* Name;
        void (*Corrupt)(SyntheticHeap&, std::vector<Chunk*>&);
    };
    const Case cases[] = {
        // double free: frontlink outra vez deixa a lista com p->bk == p
        {"double_free", [](SyntheticHeap& h, std::vector<Chunk*>& c) { h.Free(3, c[1]); }},
        {"self_loop", [](SyntheticHeap&, std::vector<Chunk*>& c) { c[2]->Bk = c[2]; }},
        {"cycle", [](SyntheticHeap&, std::vector<Chunk*>& c) {
             c[3]->Bk = c[1];
             c[1]->Fd = c[3];
         }},
        {"wild_bk", [](SyntheticHeap&, std::vector<Chunk*>& c) {
             c[2]->Bk = reinterpret_cast<Chunk*>(uintptr_t{0x1234560});
         }},
        {"misaligned_bk", [](SyntheticHeap&, std::vector<Chunk*>& c) {
             c[2]->Bk = reinterpret_cast<Chunk*>(reinterpret_cast<char*>(c[1]) + 3);
         }},
        {"bad_size", [](SyntheticHeap&, std::vector<Chunk*>& c) { c[2]->Size = 8; }},
        {"huge_size", [](SyntheticHeap&, std::vector<Chunk*>& c) { c[2]->Size = ~size_t{0}; }},
        {"fd_mismatch", [](SyntheticHeap&, std::vector<Chunk*>& c) { c[2]->Fd = c[0]; }},
        {"wild_top", [](SyntheticHeap& h, std::vector<Chunk*>&) {
             h.Av[2] = reinterpret_cast<void*>(uintptr_t{16});
         }},
    };
    for (const auto& item : cases) {
        SyntheticHeap heap;
        std::vector<Chunk*> chunks;
        for (size_t index = 0; index < 6; ++index) {
            chunks.push_back(heap.Allocate(64));
            heap.Free(3, chunks.back());
        }
        heap.Top(1024);
        Require(heap.Check().Ok, "setup heap corrupt");
        item.Corrupt(heap, chunks);
        const auto result = heap.Check();
        if (result.Ok) {
            std::fprintf(stderr, "case %s\n", item.Name);
            Require(false, "corruption not detected");
        }
        Require(result.Steps <= 64, "walk did not stop early on corruption");
    }
}

// Elos e tamanhos aleatórios: a varredura sempre termina e nunca lê fora do
// heap (ASan acusaria).
void TestHeapWalkFuzz() {
    std::mt19937_64 random(20261017U);
    uint64_t corrupt = 0;
    for (int round = 0; round < 3000; ++round) {
        SyntheticHeap heap;
        std::vector<Chunk*> chunks;
        const size_t count = 1U + random() % 40U;
        for (size_t index = 0; index < count; ++index) {
            chunks.push_back(heap.Allocate(32U + 16U * (random() % 8U)));
            heap.Free(1U + random() % 20U, chunks.back());
        }
        heap.Top(256);
        const size_t mutations = random() % 4U;
        for (size_t mutation = 0; mutation < mutations; ++mutation) {
            Chunk* chunk = chunks[random() % chunks.size()];
            const uintptr_t pick = random() % 5U;
            const uintptr_t value =
                pick == 0   ? reinterpret_cast<uintptr_t>(chunks[random() % chunks.size()])
                : pick == 1 ? reinterpret_cast<uintptr_t>(heap.Header(random() % 20U))
                : pick == 2 ? heap.Begin() + (random() % (heap.End() - heap.Begin()))
                : pick == 3 ? static_cast<uintptr_t>(random())
                            : reinterpret_cast<uintptr_t>(chunk);
            switch (random() % 3U) {
            case 0: chunk->Fd = reinterpret_cast<Chunk*>(value); break;
            case 1: chunk->Bk = reinterpret_cast<Chunk*>(value); break;
            default: chunk->Size = static_cast<size_t>(value); break;
            }
        }
        const auto result = heap.Check(100000U);
        Require(result.Steps <= 100000U, "fuzz walk exceeded its step cap");
        corrupt += result.Ok ? 0U : 1U;
    }
    Require(corrupt > 1000U, "fuzz mutations were not detected");
}

} // namespace

int main() {
    TestTrackerReportsOnceAndRecovers();
    TestTrackerIgnoresFocusLossAndSuspend();
    TestText();
    TestStageScope();
    TestHeapWalkMatchesMallinfo();
    TestHeapWalkStopsOnCorruption();
    TestHeapWalkFuzz();
    std::puts("oot3d_hang_watchdog_tests: ok");
    return 0;
}
