// config/vertex_snapshot_fast (SWITCH_PORT.md "Vertex snapshot hit path"):
// index buffers and vertex loaders captured by three submission queues over
// three copies of the same guest memory history: the previous path (the
// reference), the fast path (direct view and certificate, page certificates,
// the front table of the cache, the card peek of RangeWriteGeneration), and
// the switch toggled at random between draws.
//  - Between frames guest memory changes through the store cards: content
//    changes inside snapshot ranges, same-value rewrites, same-value writes
//    into one page of many ranges (the card is set, the bytes are not new),
//    escaped write pointers with raw writes the cards never see, deferred
//    device writes, and a physical range with a second guest alias (the
//    fast path leaves it to the previous one).
//  - Every draw: the same success or error, index range and snapshots
//    (bytes, versions, hashes) in the three runs, and the bytes equal to
//    guest memory at the draw (a mirror kept by the test).
//  - The same counters (resources, version hits and misses, uncertified,
//    misses, evictions, copied and hashed bytes) and savestates; the fast run
//    compares fewer bytes. Savestates are restored mid-stream, the write
//    observation is rebound (every certificate forgotten) and the cache
//    overflows its 4096 entries (evictions under the front table).
// --bench: the vertex stage per draw on a steady scene, with one page of
// every buffer rewritten per frame, and with every page rewritten, fast path
// off and on.
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_memory_peek.h"
#include "oot3d_native_capture_clock.h"
#include "oot3d_native_pica_submission.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_vertex_snapshot_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// ------------------------------------------------------------ guest memory

constexpr uint32_t kLinearGuest = 0x14000000U;
constexpr uint32_t kLinearPhysical = 0x20000000U;
constexpr size_t kLinearBytes = 0x1000000U;
constexpr uint32_t kVramGuest = 0x1F000000U;
constexpr uint32_t kVramPhysical = 0x18000000U;
constexpr size_t kVramBytes = 0x100000U;
// A second guest view of the first 256 KiB of the linear heap.
constexpr uint32_t kAliasGuest = 0x30000000U;
constexpr size_t kAliasBytes = 0x40000U;
constexpr uint32_t kMaximumIndex = 1024U;
constexpr uint32_t kMaximumStride = 48U;

// Where a template's buffers live (offsets from the region's base).
struct Area {
    uint32_t Physical = 0;
    uint32_t IndexBegin = 0;
    uint32_t IndexEnd = 0;
    uint32_t VertexBegin = 0;
    uint32_t VertexEnd = 0;
};
constexpr Area kLinearArea{kLinearPhysical, 0x040000U, 0x0C0000U, 0x100000U, 0x1000000U};
constexpr Area kVramArea{kVramPhysical, 0x000000U, 0x010000U, 0x010000U, 0x100000U};
constexpr Area kAliasArea{kLinearPhysical, 0x000000U, 0x008000U, 0x008000U, 0x040000U};

struct Loader {
    uint32_t Offset = 0;
    uint32_t Stride = 0;
};

struct DrawTemplate {
    uint32_t Base = 0;
    bool Indexed = true;
    bool Sixteen = false;
    uint32_t IndexOffset = 0;
    uint32_t Count = 0;
    uint32_t VertexOffset = 0;
    std::vector<Loader> Loaders;
};

Oot3dPicaDrawPacket MakePacket(const DrawTemplate& draw) {
    Oot3dPicaDrawPacket packet{};
    packet.Indexed = draw.Indexed;
    packet.Registers[0x200] = draw.Base >> 3U;
    packet.Registers[0x201] = 0x00000003U;
    packet.Registers[0x202] = 0x00000000U;
    for (size_t index = 0; index < draw.Loaders.size(); ++index) {
        const uint16_t base = static_cast<uint16_t>(0x203U + index * 3U);
        packet.Registers[base] = draw.Loaders[index].Offset;
        packet.Registers[base + 1U] = 0U;
        packet.Registers[base + 2U] = (draw.Loaders[index].Stride << 16U) | (1U << 28U);
    }
    packet.Registers[0x227] = draw.IndexOffset | (draw.Sixteen ? 0x80000000U : 0U);
    packet.Registers[0x228] = draw.Count;
    packet.Registers[0x22A] = draw.VertexOffset;
    packet.Registers[0x11D] = 0x04000040U;
    packet.Registers[0x080] = 0U;
    return packet;
}

DrawTemplate RandomTemplate(std::mt19937_64& random) {
    const unsigned choice = static_cast<unsigned>(random() % 8U);
    const Area& area = choice == 0U ? kVramArea : choice == 1U ? kAliasArea : kLinearArea;
    DrawTemplate draw;
    draw.Base = area.Physical;
    draw.Indexed = random() % 6U != 0U;
    draw.Sixteen = random() % 3U != 0U;
    const uint32_t indexSize = draw.Sixteen ? 2U : 1U;
    draw.Count = 3U + static_cast<uint32_t>(random() % 600U);
    const uint32_t indexSpan = area.IndexEnd - area.IndexBegin - draw.Count * indexSize;
    draw.IndexOffset = area.IndexBegin + static_cast<uint32_t>(random() % indexSpan & ~uint64_t{1});
    draw.VertexOffset = static_cast<uint32_t>(random() % 300U);
    const size_t loaders = 1U + random() % 4U;
    const uint32_t vertexReach = (kMaximumIndex + 600U) * kMaximumStride;
    for (size_t index = 0; index < loaders; ++index) {
        Loader loader;
        loader.Stride = 4U + static_cast<uint32_t>(random() % (kMaximumStride - 3U));
        loader.Offset = area.VertexBegin +
                        static_cast<uint32_t>(random() % (area.VertexEnd - area.VertexBegin - vertexReach));
        draw.Loaders.push_back(loader);
    }
    // Now and then a range that leaves mapped memory: the same error.
    if (random() % 97U == 0U) draw.Loaders.back().Offset = 0x0FF00000U;
    return draw;
}

struct Mirror {
    std::vector<uint8_t> Linear = std::vector<uint8_t>(kLinearBytes);
    std::vector<uint8_t> Vram = std::vector<uint8_t>(kVramBytes);

    std::span<const uint8_t> Physical(uint32_t address, size_t size) const {
        if (address >= kLinearPhysical && address - kLinearPhysical + size <= kLinearBytes)
            return std::span<const uint8_t>(Linear).subspan(address - kLinearPhysical, size);
        if (address >= kVramPhysical && address - kVramPhysical + size <= kVramBytes)
            return std::span<const uint8_t>(Vram).subspan(address - kVramPhysical, size);
        return {};
    }
    std::span<uint8_t> Guest(uint32_t address, size_t size) {
        if (address >= kLinearGuest && address - kLinearGuest + size <= kLinearBytes)
            return std::span<uint8_t>(Linear).subspan(address - kLinearGuest, size);
        if (address >= kVramGuest && address - kVramGuest + size <= kVramBytes)
            return std::span<uint8_t>(Vram).subspan(address - kVramGuest, size);
        Fail("mirror write outside the test regions");
    }
};

// ---------------------------------------------------------------- the runs

struct Run {
    NativeA32Memory Memory;
    std::optional<Oot3dPicaPhysicalMemoryView> View;
    std::unique_ptr<Oot3dNativePicaSubmissionQueue> Queue;
    struct EscapedRange {
        uint8_t* Pointer = nullptr;
        uint32_t Guest = 0;
        size_t Size = 0;
    };
    std::vector<EscapedRange> Escaped;

    Run(bool fast, bool lean) {
        std::string error;
        Require(Memory.MapRegion({"linear", kLinearGuest, kLinearBytes, true, false, {}}, &error), error);
        Require(Memory.MapRegion({"vram", kVramGuest, kVramBytes, true, false, {}}, &error), error);
        Require(Memory.MapRegion({"alias", kAliasGuest, kAliasBytes, true, false, {}}, &error), error);
        const std::vector<Oot3dPicaPhysicalMemoryRegion> regions{
            {kLinearPhysical, kLinearGuest, kLinearBytes},
            {kVramPhysical, kVramGuest, kVramBytes},
            {kLinearPhysical, kAliasGuest, kAliasBytes}};
        View.emplace(Memory, regions);
        Queue = std::make_unique<Oot3dNativePicaSubmissionQueue>(*View, true);
        Queue->SetLeanCapture(lean);
        Queue->SetVertexSnapshotFast(fast);
        // As the runtime binds it (oot3d_native_a32_window_setup.cpp).
        Memory.SetWriteObserver([this](uint32_t address, size_t size) {
            Require(View->SynchronizeGuestWrite(Memory, address, size), "alias synchronization failed");
            Queue->NotifyGuestWrite(address, size);
        });
        Bind();
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    void Bind() {
        Queue->BindGuestWriteObservation(
            [this](uint32_t address, size_t size) { return Memory.ObserveWrites(address, size); });
    }
};

struct Outcome {
    bool Ok = false;
    std::string Error;
};

void RequireSameResources(const Oot3dPicaDrawSubmission& a, const Oot3dPicaDrawSubmission& b,
                          std::string_view what) {
    const auto fail = [&](std::string_view field) {
        Fail(std::string(what) + ": draw " + std::to_string(a.Id) + " differs in " + std::string(field));
    };
    if (a.Id != b.Id || a.MinimumVertexIndex != b.MinimumVertexIndex ||
        a.MaximumVertexIndex != b.MaximumVertexIndex)
        fail("identity or index range");
    if (a.Resources.size() != b.Resources.size()) fail("snapshot count");
    for (size_t index = 0; index < a.Resources.size(); ++index) {
        const auto& x = a.Resources[index];
        const auto& y = b.Resources[index];
        if (x.Kind != y.Kind || x.Slot != y.Slot || x.PhysicalAddress != y.PhysicalAddress ||
            x.FirstElement != y.FirstElement || !std::ranges::equal(x.ResolvedBytes(), y.ResolvedBytes()) ||
            x.ContentHash != y.ContentHash || x.ContentHashAvailable != y.ContentHashAvailable ||
            x.ContentVersion != y.ContentVersion || x.ContentVersionAvailable != y.ContentVersionAvailable)
            fail("snapshot " + std::to_string(index));
    }
}

// The snapshots hold guest memory as it was at the draw.
void RequireMemoryContent(const Oot3dPicaDrawSubmission& draw, const DrawTemplate& source, const Mirror& mirror) {
    for (const auto& resource : draw.Resources) {
        size_t size = resource.ResolvedBytes().size();
        uint32_t address = resource.PhysicalAddress;
        if (resource.Kind == Oot3dPicaResourceKind::IndexBuffer) {
            Require(address == source.Base + source.IndexOffset &&
                        size == source.Count * (source.Sixteen ? 2U : 1U),
                    "index snapshot of another range");
        } else {
            const auto& loader = source.Loaders.at(resource.Slot);
            Require(address == source.Base + loader.Offset + draw.MinimumVertexIndex * loader.Stride &&
                        size == (draw.MaximumVertexIndex - draw.MinimumVertexIndex + 1U) * loader.Stride,
                    "vertex snapshot of another range");
        }
        Require(std::ranges::equal(resource.ResolvedBytes(), mirror.Physical(address, size)),
                "a snapshot differs from guest memory at its draw");
    }
}

void RequireSameCounters(const Oot3dNativePicaSubmissionQueue::CaptureCounters& a,
                         const Oot3dNativePicaSubmissionQueue::CaptureCounters& b, std::string_view what) {
    Require(a.Draws == b.Draws && a.CopiedBytes == b.CopiedBytes && a.HashedBytes == b.HashedBytes &&
                a.IndexRangeReuses == b.IndexRangeReuses && a.GeometryMisses == b.GeometryMisses &&
                a.GeometryEvictions == b.GeometryEvictions && a.GeometryResources == b.GeometryResources &&
                a.GeometryVersionHits == b.GeometryVersionHits &&
                a.GeometryVersionMisses == b.GeometryVersionMisses &&
                a.GeometryUncertified == b.GeometryUncertified &&
                a.GeometryUnprovenBytes == b.GeometryUnprovenBytes,
            std::string(what) + ": the capture counters differ");
}

// A deterministic device write: the bytes a reader produces for an address.
uint8_t DeviceByte(uint64_t seed, uint32_t address) {
    uint64_t value = (seed ^ address) * 0x9E3779B97F4A7C15ULL;
    return static_cast<uint8_t>(value >> 56U);
}

struct Totals {
    uint64_t Draws = 0;
    uint64_t Failures = 0;
    uint64_t EscapedRanges = 0;
    uint64_t DeferredWrites = 0;
};

void TestReplay(uint64_t seed, size_t frames, bool lean) {
    std::mt19937_64 random(seed);
    Run reference(false, lean);
    Run fast(true, lean);
    Run toggled(random() % 2U != 0U, lean);
    const std::array<Run*, 3> runs{&reference, &fast, &toggled};
    Mirror mirror;
    Totals totals;

    const auto write = [&](uint32_t guest, std::span<const uint8_t> bytes) {
        for (Run* run : runs) Require(run->Memory.WriteBytes(guest, bytes), "guest write failed");
        std::ranges::copy(bytes, mirror.Guest(guest, bytes.size()).begin());
    };
    // Initial content: index areas hold indices below kMaximumIndex.
    {
        std::vector<uint8_t> bytes(kLinearBytes);
        for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
        for (const Area* area : {&kLinearArea, &kAliasArea})
            for (uint32_t offset = area->IndexBegin; offset < area->IndexEnd; offset += 2U)
                bytes[offset + 1U] = static_cast<uint8_t>(random() % (kMaximumIndex / 256U));
        write(kLinearGuest, bytes);
        std::vector<uint8_t> vram(kVramBytes);
        for (auto& byte : vram) byte = static_cast<uint8_t>(random());
        for (uint32_t offset = kVramArea.IndexBegin; offset < kVramArea.IndexEnd; offset += 2U)
            vram[offset + 1U] = static_cast<uint8_t>(random() % (kMaximumIndex / 256U));
        write(kVramGuest, vram);
    }
    std::vector<DrawTemplate> scene;
    for (size_t index = 0; index < 96U; ++index) scene.push_back(RandomTemplate(random));

    const auto guestOf = [](const Area& area, uint32_t offset) {
        return (area.Physical == kVramPhysical ? kVramGuest : kLinearGuest) + offset;
    };
    const auto areaOf = [&]() -> const Area& {
        const unsigned choice = static_cast<unsigned>(random() % 6U);
        return choice == 0U ? kVramArea : choice == 1U ? kAliasArea : kLinearArea;
    };

    for (size_t frame = 0; frame < frames; ++frame) {
        // Content changes inside index and vertex areas (indices stay small).
        for (size_t count = random() % 6U; count != 0U; --count) {
            const Area& area = areaOf();
            const bool index = random() % 3U == 0U;
            const size_t size = 2U + (random() % 600U & ~size_t{1});
            const uint32_t begin = index ? area.IndexBegin : area.VertexBegin;
            const uint32_t end = index ? area.IndexEnd : area.VertexEnd;
            const uint32_t offset = begin + static_cast<uint32_t>(random() % (end - begin - size) & ~uint64_t{1});
            std::vector<uint8_t> bytes(size);
            for (size_t at = 0; at < size; ++at)
                bytes[at] = index && at % 2U == 1U ? static_cast<uint8_t>(random() % (kMaximumIndex / 256U))
                                                   : static_cast<uint8_t>(random());
            write(guestOf(area, offset), bytes);
        }
        // Same-value writes: the card is set, the content is not new. Many
        // single bytes (one page of a range, as dynamic data sharing a page
        // with a model) and a few larger spans.
        for (size_t count = random() % 40U; count != 0U; --count) {
            const Area& area = areaOf();
            const size_t size = random() % 8U == 0U ? 1U + random() % 9000U : 1U;
            const uint32_t offset =
                area.IndexBegin + static_cast<uint32_t>(random() % (area.VertexEnd - area.IndexBegin - size));
            const uint32_t guest = guestOf(area, offset);
            const std::vector<uint8_t> same(mirror.Guest(guest, size).begin(), mirror.Guest(guest, size).end());
            write(guest, same);
        }
        // Escaped write pointers: raw writes the cards never see.
        if (random() % 53U == 0U) {
            const Area& area = areaOf();
            const size_t size = 64U + random() % 4096U;
            const uint32_t offset =
                area.VertexBegin + static_cast<uint32_t>(random() % (area.VertexEnd - area.VertexBegin - size));
            const uint32_t guest = guestOf(area, offset);
            for (Run* run : runs) {
                uint8_t* pointer = run->Memory.GetWritePointer(guest, size);
                Require(pointer != nullptr, "no write pointer");
                run->Escaped.push_back({pointer, guest, size});
            }
            ++totals.EscapedRanges;
        }
        if (!reference.Escaped.empty() && random() % 3U == 0U) {
            const size_t which = random() % reference.Escaped.size();
            const size_t at = random() % reference.Escaped[which].Size;
            const uint8_t value = static_cast<uint8_t>(random());
            for (Run* run : runs) run->Escaped[which].Pointer[at] = value;
            mirror.Guest(reference.Escaped[which].Guest + static_cast<uint32_t>(at), 1U)[0] = value;
        }
        // Deferred device writes, materialized when a reader needs them.
        if (random() % 7U == 0U) {
            const Area& area = areaOf();
            const size_t size = 16U + random() % 6000U;
            const uint32_t offset =
                area.VertexBegin + static_cast<uint32_t>(random() % (area.VertexEnd - area.VertexBegin - size));
            const uint32_t guest = guestOf(area, offset);
            const uint64_t device = random();
            const std::array<std::pair<uint32_t, size_t>, 1> ranges{{{guest, size}}};
            bool accepted = false;
            for (size_t index = 0; index < runs.size(); ++index) {
                const bool ok = runs[index]->Memory.DeferWrites(ranges, [device](uint32_t address, std::span<uint8_t> out) {
                    for (size_t at = 0; at < out.size(); ++at) out[at] = DeviceByte(device, address + static_cast<uint32_t>(at));
                    return true;
                });
                Require(index == 0U || ok == accepted, "a deferred write was accepted differently");
                accepted = ok;
            }
            if (accepted) {
                auto target = mirror.Guest(guest, size);
                for (size_t at = 0; at < size; ++at) target[at] = DeviceByte(device, guest + static_cast<uint32_t>(at));
                ++totals.DeferredWrites;
            }
        }
        // The scene moves now and then (new ranges: misses).
        for (size_t count = random() % 3U; count != 0U; --count) scene[random() % scene.size()] = RandomTemplate(random);
        // Once: more distinct ranges than the cache holds.
        std::vector<DrawTemplate> draws;
        for (const auto& draw : scene)
            if (random() % 8U != 0U) draws.push_back(draw);
        if (frame == frames / 2U) {
            for (size_t index = 0; index < 800U; ++index) {
                DrawTemplate draw;
                draw.Base = kLinearPhysical;
                draw.Indexed = false;
                draw.Count = 3U + static_cast<uint32_t>(random() % 30U);
                draw.VertexOffset = static_cast<uint32_t>(random() % 300U);
                for (size_t loader = 0; loader < 6U; ++loader) {
                    draw.Loaders.push_back(
                        {kLinearArea.VertexBegin +
                             static_cast<uint32_t>(random() % (kLinearArea.VertexEnd - kLinearArea.VertexBegin - 0x10000U)),
                         4U + static_cast<uint32_t>(random() % (kMaximumStride - 3U))});
                }
                draws.push_back(std::move(draw));
            }
        }
        std::vector<const DrawTemplate*> submitted;
        for (const auto& draw : draws) {
            const auto packet = MakePacket(draw);
            std::array<Outcome, 3> outcomes;
            for (size_t index = 0; index < runs.size(); ++index) {
                if (index == 2U && random() % 4U == 0U) toggled.Queue->SetVertexSnapshotFast(random() % 2U != 0U);
                // The card peek follows the run's switch, as the runtime sets it.
                SetRangeWriteGenerationCardPeek(runs[index]->Queue->VertexSnapshotFast());
                outcomes[index].Ok = runs[index]->Queue->SubmitDrawPacket(packet, &outcomes[index].Error);
            }
            Require(outcomes[0].Ok == outcomes[1].Ok && outcomes[0].Ok == outcomes[2].Ok &&
                        outcomes[0].Error == outcomes[1].Error && outcomes[0].Error == outcomes[2].Error,
                    "a draw succeeded or failed differently: '" + outcomes[0].Error + "' / '" +
                        outcomes[1].Error + "' / '" + outcomes[2].Error + "'");
            if (outcomes[0].Ok) {
                submitted.push_back(&draw);
            } else {
                ++totals.Failures;
            }
        }
        std::array<std::vector<Oot3dPicaDrawSubmission>, 3> taken;
        for (size_t index = 0; index < runs.size(); ++index) runs[index]->Queue->TakePendingDraws(taken[index]);
        Require(taken[0].size() == submitted.size() && taken[1].size() == submitted.size() &&
                    taken[2].size() == submitted.size(),
                "draw counts differ");
        for (size_t index = 0; index < submitted.size(); ++index) {
            RequireMemoryContent(taken[0][index], *submitted[index], mirror);
            RequireSameResources(taken[0][index], taken[1][index], "fast path");
            RequireSameResources(taken[0][index], taken[2][index], "switch toggled");
        }
        totals.Draws += submitted.size();
        RequireSameCounters(reference.Queue->Counters(), fast.Queue->Counters(), "fast path");
        RequireSameCounters(reference.Queue->Counters(), toggled.Queue->Counters(), "switch toggled");
        if (frame % 97U == 96U) {
            const auto state = reference.Queue->CaptureState();
            Require(fast.Queue->CaptureState() == state && toggled.Queue->CaptureState() == state,
                    "the queue savestates differ");
            std::string error;
            for (Run* run : runs) Require(run->Queue->RestoreState(state, &error), error);
        }
        if (frame % 131U == 130U) {
            for (Run* run : runs) run->Bind();
        }
    }
    const auto counters = reference.Queue->Counters();
    const auto fastCounters = fast.Queue->Counters();
    Require(counters.GeometryEvictions != 0U && counters.GeometryUncertified != 0U &&
                counters.GeometryVersionHits != 0U && counters.GeometryUnprovenBytes != 0U &&
                fastCounters.GeometryFallbacks != 0U && fastCounters.GeometryDirtyPages != 0U &&
                totals.EscapedRanges != 0U && totals.DeferredWrites != 0U && totals.Failures != 0U,
            "the replay did not reach every case");
    Require(fastCounters.GeometryComparedBytes < counters.GeometryComparedBytes,
            "the fast path did not compare fewer bytes");
    std::printf("replay seed=%llu lean=%d frames=%zu draws=%llu failed=%llu resources=%llu version_hits=%llu "
                "uncertified=%llu misses=%llu evictions=%llu fallbacks=%llu escaped=%llu deferred=%llu "
                "compared_kb off=%.0f on=%.0f (unproven %.0f) pages=%llu changed=%llu\n",
                static_cast<unsigned long long>(seed), lean ? 1 : 0, frames,
                static_cast<unsigned long long>(totals.Draws), static_cast<unsigned long long>(totals.Failures),
                static_cast<unsigned long long>(counters.GeometryResources),
                static_cast<unsigned long long>(counters.GeometryVersionHits),
                static_cast<unsigned long long>(counters.GeometryUncertified),
                static_cast<unsigned long long>(counters.GeometryMisses),
                static_cast<unsigned long long>(counters.GeometryEvictions),
                static_cast<unsigned long long>(fastCounters.GeometryFallbacks),
                static_cast<unsigned long long>(totals.EscapedRanges),
                static_cast<unsigned long long>(totals.DeferredWrites), counters.GeometryComparedBytes / 1024.0,
                fastCounters.GeometryComparedBytes / 1024.0, counters.GeometryUnprovenBytes / 1024.0,
                static_cast<unsigned long long>(fastCounters.GeometryPages),
                static_cast<unsigned long long>(fastCounters.GeometryDirtyPages));
}

// ------------------------------------------------------------------- bench

enum class Pattern { Steady, OnePage, AllPages };

// A room: 90 indexed draws per frame, each with its own index buffer and
// three loaders, vertex ranges of 0.5-32 KiB (1-9 pages); between frames
// nothing, one same-value byte in every vertex range (a page shared with
// data the game writes), or every byte of every range rewritten with the
// same value. The vertex stage per draw and its steps.
void Bench(Pattern pattern, bool fastPath, size_t frames) {
    std::mt19937_64 random(7U);
    Run run(fastPath, true);
    SetRangeWriteGenerationCardPeek(fastPath);
    std::vector<uint8_t> bytes(kLinearBytes);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
    std::vector<DrawTemplate> scene;
    std::vector<uint32_t> vertexCounts;
    constexpr size_t kDraws = 90U;
    for (size_t index = 0; index < kDraws; ++index) {
        DrawTemplate draw;
        draw.Base = kLinearPhysical;
        draw.Indexed = true;
        draw.Sixteen = true;
        draw.Count = 300U;
        draw.IndexOffset = 0x040000U + static_cast<uint32_t>(index) * 0x400U;
        const uint32_t vertices = 128U + static_cast<uint32_t>(random() % 1920U);
        vertexCounts.push_back(vertices);
        for (uint32_t at = 0; at < draw.Count; ++at) {
            const uint32_t value = at == 0U ? 0U : at == 1U ? vertices - 1U : static_cast<uint32_t>(random() % vertices);
            bytes[draw.IndexOffset + at * 2U] = static_cast<uint8_t>(value);
            bytes[draw.IndexOffset + at * 2U + 1U] = static_cast<uint8_t>(value >> 8U);
        }
        for (uint32_t loader = 0; loader < 3U; ++loader) {
            const uint32_t stride = std::array<uint32_t, 3>{12U, 16U, 4U}[loader];
            draw.Loaders.push_back({0x100000U + static_cast<uint32_t>(index * 3U + loader) * 0x9000U, stride});
        }
        scene.push_back(std::move(draw));
    }
    Require(run.Memory.WriteBytes(kLinearGuest, bytes), "bench memory");
    std::vector<Oot3dPicaDrawSubmission> taken;
    Oot3dNativePicaSubmissionQueue::CaptureCounters before{};
    std::string error;
    for (size_t frame = 0; frame < frames + 10U; ++frame) {
        if (frame == 10U) before = run.Queue->Counters();
        if (pattern != Pattern::Steady) {
            for (size_t draw = 0; draw < scene.size(); ++draw) {
                for (const auto& loader : scene[draw].Loaders) {
                    const uint32_t guest = kLinearGuest + loader.Offset;
                    const uint32_t range = vertexCounts[draw] * loader.Stride;
                    const size_t size = pattern == Pattern::OnePage ? 1U : range;
                    const uint32_t at = pattern == Pattern::OnePage
                                            ? guest + static_cast<uint32_t>(random() % range)
                                            : guest;
                    Require(run.Memory.WriteBytes(at, std::span<const uint8_t>(bytes).subspan(at - kLinearGuest, size)),
                            "bench write");
                }
            }
        }
        for (const auto& draw : scene) Require(run.Queue->SubmitDrawPacket(MakePacket(draw), &error), error);
        run.Queue->TakePendingDraws(taken);
    }
    const auto after = run.Queue->Counters();
    const double count = static_cast<double>(after.Draws - before.Draws);
    const auto us = [count](uint64_t now, uint64_t then) {
        return CaptureTicksToNanoseconds(now - then) / 1.0e3 / count;
    };
    const char* name = pattern == Pattern::Steady ? "steady" : pattern == Pattern::OnePage ? "one_page" : "all_pages";
    std::printf("bench %-9s fast=%d us/draw: vertex %.3f (view %.3f compare %.3f copy %.3f) | per frame: "
                "resources %.0f version_hits %.0f unproven %.0f KiB compared %.0f KiB pages %.0f changed %.0f\n",
                name, fastPath ? 1 : 0, us(after.VertexTicks, before.VertexTicks),
                us(after.GeometryViewTicks, before.GeometryViewTicks),
                us(after.GeometryCompareTicks, before.GeometryCompareTicks),
                us(after.GeometryCopyTicks, before.GeometryCopyTicks),
                static_cast<double>(after.GeometryResources - before.GeometryResources) / frames,
                static_cast<double>(after.GeometryVersionHits - before.GeometryVersionHits) / frames,
                static_cast<double>(after.GeometryUnprovenBytes - before.GeometryUnprovenBytes) / 1024.0 / frames,
                static_cast<double>(after.GeometryComparedBytes - before.GeometryComparedBytes) / 1024.0 / frames,
                static_cast<double>(after.GeometryPages - before.GeometryPages) / frames,
                static_cast<double>(after.GeometryDirtyPages - before.GeometryDirtyPages) / frames);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        const size_t frames = argc > 2 ? static_cast<size_t>(std::strtoul(argv[2], nullptr, 10)) : 300U;
        for (int round = 0; round < 2; ++round)
            for (Pattern pattern : {Pattern::Steady, Pattern::OnePage, Pattern::AllPages})
                for (bool fastPath : {false, true}) Bench(pattern, fastPath, frames);
        return 0;
    }
    const size_t frames = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 400U;
    for (uint64_t seed = 1U; seed <= 4U; ++seed) TestReplay(seed, frames, seed % 2U == 0U);
    std::cout << "oot3d_native_vertex_snapshot_tests: ok\n";
    return 0;
}
