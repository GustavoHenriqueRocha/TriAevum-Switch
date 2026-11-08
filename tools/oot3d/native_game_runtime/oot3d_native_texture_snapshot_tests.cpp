// config/texture_snapshot_fast (SWITCH_PORT.md "Texture snapshot and packet
// stages"): texture snapshots captured by three submission queues over three
// copies of the same guest memory history: the previous path (the
// reference), the fast path (direct view and certificate, front table of the
// texture cache, page certificates, the next packet prefetched), and the
// switch toggled at random between draws.
//  - Textures of every format and size the decoder accepts (mip chains,
//    type 2), in the linear heap, in VRAM and behind a second guest alias of
//    the heap (the fast path leaves those to the previous one); invalid
//    formats and unmapped textures fail the same way.
//  - Between frames guest memory changes through the store cards: content
//    changes inside textures, same-value rewrites (one byte in a page of
//    many textures, larger spans), escaped write pointers with raw writes
//    the cards never see, deferred device writes.
//  - Every draw: the same success or error and the same snapshots (bytes,
//    hashes, base-level hashes) in the three runs, and the bytes equal to
//    guest memory at the draw (through the payload transform when one is
//    set; a mirror kept by the test provides it).
//  - The same counters and savestates; savestates are restored mid-stream,
//    the write observation is rebound, and the payload transform is set and
//    removed (the cache is emptied under the front table).
// --bench: the texture and packet stages per draw on a steady scene, with
// one byte of every texture rewritten per frame, and with every texture
// rewritten, the switch off and on; the packet stage with capture delta and
// a consumer thread that materializes and frees the drained draws.
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_memory_peek.h"
#include "oot3d_native_capture_clock.h"
#include "oot3d_native_pica_capture_delta.h"
#include "oot3d_native_pica_submission.h"

#include "fast/renderer3ds/pica_scene_water.h"
#include "fast/renderer3ds/room_shadow_shell.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_texture_snapshot_tests: " << message << '\n';
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
constexpr size_t kVramBytes = 0x200000U;
// A second guest view of the first 256 KiB of the linear heap.
constexpr uint32_t kAliasGuest = 0x30000000U;
constexpr size_t kAliasBytes = 0x40000U;
// The draws' one vertex loader (stride 4, three vertices) and nothing else
// below 0x1000 of the heap.
constexpr uint32_t kVertexOffset = 0x800U;

struct Area {
    uint32_t Physical = 0;
    uint32_t Begin = 0;
    uint32_t End = 0;
};
constexpr Area kLinearArea{kLinearPhysical, 0x040000U, 0x1000000U};
constexpr Area kVramArea{kVramPhysical, 0x000000U, 0x200000U};
constexpr Area kAliasArea{kLinearPhysical, 0x001000U, 0x040000U};

struct Texture {
    uint32_t Physical = 0;
    uint16_t Width = 8;
    uint16_t Height = 8;
    uint8_t Format = 0;
    uint8_t Type = 0;
    uint8_t MaxLevel = 0;
    uint32_t Configuration = 0;
};

Oot3dPicaTextureState StateOf(const Texture& texture) {
    Oot3dPicaTextureState state;
    state.Enabled = true;
    state.Width = texture.Width;
    state.Height = texture.Height;
    state.PhysicalAddress = texture.Physical;
    state.Format = texture.Format;
    state.Type = texture.Type;
    state.MaxMipLevel = texture.MaxLevel;
    return state;
}

size_t BytesOf(const Texture& texture) {
    return Oot3dPicaTextureMipChainByteSize(StateOf(texture)).value_or(0U);
}

// A draw: up to three texture units (none, some or all enabled).
struct DrawTemplate {
    std::array<std::optional<Texture>, 3> Units;
};

Oot3dPicaDrawPacket MakePacket(const DrawTemplate& draw) {
    Oot3dPicaDrawPacket packet{};
    packet.Indexed = false;
    packet.Registers[0x200] = kLinearPhysical >> 3U;
    packet.Registers[0x201] = 0x00000003U;
    packet.Registers[0x202] = 0x00000000U;
    packet.Registers[0x203] = kVertexOffset;
    packet.Registers[0x205] = (4U << 16U) | (1U << 28U);
    packet.Registers[0x228] = 3U;
    packet.Registers[0x22A] = 0U;
    packet.Registers[0x11D] = 0x04000040U;
    static constexpr std::array<uint16_t, 3> kConfig{0x081U, 0x091U, 0x099U};
    static constexpr std::array<uint16_t, 3> kFormat{0x08EU, 0x096U, 0x09EU};
    uint32_t enabled = 0U;
    for (size_t unit = 0; unit < draw.Units.size(); ++unit) {
        if (!draw.Units[unit].has_value()) continue;
        const Texture& texture = *draw.Units[unit];
        enabled |= 1U << unit;
        const uint16_t base = kConfig[unit];
        packet.Registers[base + 1U] = (uint32_t{texture.Width} << 16U) | texture.Height;
        packet.Registers[base + 2U] = texture.Configuration | (uint32_t{texture.Type} << 28U);
        packet.Registers[base + 3U] = uint32_t{texture.MaxLevel} << 16U;
        packet.Registers[base + 4U] = texture.Physical >> 3U;
        packet.Registers[kFormat[unit]] = texture.Format;
    }
    packet.Registers[0x080] = enabled;
    return packet;
}

Texture RandomTexture(std::mt19937_64& random) {
    const unsigned choice = static_cast<unsigned>(random() % 10U);
    const Area& area = choice < 2U ? kVramArea : choice == 2U ? kAliasArea : kLinearArea;
    Texture texture;
    static constexpr std::array<uint16_t, 6> kSides{8U, 16U, 32U, 64U, 128U, 256U};
    texture.Width = kSides[random() % (area.End - area.Begin < 0x80000U ? 4U : 6U)];
    texture.Height = kSides[random() % (area.End - area.Begin < 0x80000U ? 4U : 6U)];
    texture.Format = static_cast<uint8_t>(random() % 14U);
    texture.Type = random() % 13U == 0U ? 2U : 0U;
    texture.MaxLevel = static_cast<uint8_t>(random() % 4U == 0U ? 0U : random() % 6U);
    texture.Configuration = static_cast<uint32_t>(random() % 4U) << 1U | (static_cast<uint32_t>(random() % 8U) << 8U);
    const size_t bytes = BytesOf(texture);
    texture.Physical = area.Physical + area.Begin +
                       (static_cast<uint32_t>(random() % (area.End - area.Begin - bytes)) & ~7U);
    // Now and then an invalid format or a texture that leaves mapped memory:
    // the same error in every run.
    if (random() % 211U == 0U) texture.Format = 14U + static_cast<uint8_t>(random() % 2U);
    if (random() % 199U == 0U) texture.Physical = 0x0FF00000U;
    return texture;
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

uint32_t GuestOf(uint32_t physical) {
    return physical >= kLinearPhysical ? kLinearGuest + (physical - kLinearPhysical)
                                       : kVramGuest + (physical - kVramPhysical);
}

// ---------------------------------------------------------------- the runs

// The payload transform of the runs that set one: the base level of every
// texture, every byte flipped by its offset.
void Transform(const Oot3dPicaTextureState&, std::span<uint8_t> payload) {
    for (size_t at = 0; at < payload.size(); ++at) payload[at] = static_cast<uint8_t>(payload[at] ^ (at * 7U + 0x5AU));
}

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
        Queue->SetVertexSnapshotFast(true);
        Queue->SetTextureSnapshotFast(fast);
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

void RequireSameDraw(const Oot3dPicaDrawSubmission& a, const Oot3dPicaDrawSubmission& b, std::string_view what) {
    const auto fail = [&](std::string_view field) {
        Fail(std::string(what) + ": draw " + std::to_string(a.Id) + " differs in " + std::string(field));
    };
    if (a.Id != b.Id || a.MinimumVertexIndex != b.MinimumVertexIndex ||
        a.MaximumVertexIndex != b.MaximumVertexIndex)
        fail("identity or index range");
    if (a.Packet.Registers != b.Packet.Registers || a.Packet.DefaultAttributes != b.Packet.DefaultAttributes ||
        a.Packet.VertexShader.FloatUniforms != b.Packet.VertexShader.FloatUniforms ||
        a.Packet.GeometryShader.FloatUniforms != b.Packet.GeometryShader.FloatUniforms ||
        a.Packet.CommandListAddress != b.Packet.CommandListAddress || a.Packet.Indexed != b.Packet.Indexed ||
        a.Packet.Composition != b.Packet.Composition || a.Packet.LightBinding != b.Packet.LightBinding ||
        a.DeltaEntry != b.DeltaEntry)
        fail("packet");
    if (a.Resources.size() != b.Resources.size()) fail("snapshot count");
    for (size_t index = 0; index < a.Resources.size(); ++index) {
        const auto& x = a.Resources[index];
        const auto& y = b.Resources[index];
        if (x.Kind != y.Kind || x.Slot != y.Slot || x.PhysicalAddress != y.PhysicalAddress ||
            x.FirstElement != y.FirstElement || !std::ranges::equal(x.ResolvedBytes(), y.ResolvedBytes()) ||
            x.ContentHash != y.ContentHash || x.ContentHashAvailable != y.ContentHashAvailable ||
            x.ContentVersion != y.ContentVersion || x.ContentVersionAvailable != y.ContentVersionAvailable ||
            x.BaseLevelContentHash != y.BaseLevelContentHash ||
            x.BaseLevelContentHashAvailable != y.BaseLevelContentHashAvailable ||
            x.ReplacementWidth != y.ReplacementWidth || x.ReplacementHeight != y.ReplacementHeight ||
            x.CopyReferences.size() != y.CopyReferences.size())
            fail("snapshot " + std::to_string(index));
    }
}

// The texture snapshots hold guest memory as it was at the draw (the base
// level through the payload transform), and their hashes are its hashes.
void RequireMemoryContent(const Oot3dPicaDrawSubmission& draw, const DrawTemplate& source, const Mirror& mirror,
                          bool transformed) {
    for (const auto& resource : draw.Resources) {
        if (resource.Kind != Oot3dPicaResourceKind::Texture) continue;
        const Texture& texture = source.Units.at(resource.Slot).value();
        const size_t size = BytesOf(texture);
        Require(resource.PhysicalAddress == texture.Physical && resource.ResolvedBytes().size() == size,
                "texture snapshot of another range");
        std::vector<uint8_t> expected(mirror.Physical(texture.Physical, size).begin(),
                                      mirror.Physical(texture.Physical, size).end());
        const size_t base = Oot3dPicaTextureMipLevelByteSize(StateOf(texture), 0U).value();
        if (transformed) Transform(StateOf(texture), std::span<uint8_t>(expected).first(base));
        Require(std::ranges::equal(resource.ResolvedBytes(), expected), "a texture snapshot differs from guest memory at its draw");
        Require(resource.ContentHash == HashOot3dPicaSnapshot(expected) &&
                    resource.BaseLevelContentHash == HashOot3dPicaSnapshot(std::span<const uint8_t>(expected).first(base)),
                "a texture snapshot's hashes are not its content's");
    }
}

void RequireSameCounters(const Oot3dNativePicaSubmissionQueue::CaptureCounters& a,
                         const Oot3dNativePicaSubmissionQueue::CaptureCounters& b, std::string_view what) {
    Require(a.Draws == b.Draws && a.CopiedBytes == b.CopiedBytes && a.HashedBytes == b.HashedBytes &&
                a.GeometryMisses == b.GeometryMisses && a.GeometryResources == b.GeometryResources &&
                a.TextureResources == b.TextureResources && a.TextureVersionHits == b.TextureVersionHits &&
                a.TextureVersionMisses == b.TextureVersionMisses && a.TextureUncertified == b.TextureUncertified &&
                a.TextureUnprovenBytes == b.TextureUnprovenBytes && a.TextureMisses == b.TextureMisses &&
                a.TextureCopyReferenced == b.TextureCopyReferenced,
            std::string(what) + ": the capture counters differ");
}

uint8_t DeviceByte(uint64_t seed, uint32_t address) {
    uint64_t value = (seed ^ address) * 0x9E3779B97F4A7C15ULL;
    return static_cast<uint8_t>(value >> 56U);
}

void TestReplay(uint64_t seed, size_t frames, bool lean) {
    std::mt19937_64 random(seed);
    Run reference(false, lean);
    Run fast(true, lean);
    Run toggled(random() % 2U != 0U, lean);
    const std::array<Run*, 3> runs{&reference, &fast, &toggled};
    Mirror mirror;
    uint64_t draws = 0U;
    uint64_t failures = 0U;
    uint64_t escapedRanges = 0U;
    uint64_t deferredWrites = 0U;
    bool transformed = false;

    const auto write = [&](uint32_t guest, std::span<const uint8_t> bytes) {
        for (Run* run : runs) Require(run->Memory.WriteBytes(guest, bytes), "guest write failed");
        std::ranges::copy(bytes, mirror.Guest(guest, bytes.size()).begin());
    };
    {
        std::vector<uint8_t> bytes(kLinearBytes);
        for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
        write(kLinearGuest, bytes);
        std::vector<uint8_t> vram(kVramBytes);
        for (auto& byte : vram) byte = static_cast<uint8_t>(random());
        write(kVramGuest, vram);
    }
    std::vector<Texture> pool;
    for (size_t index = 0; index < 160U; ++index) pool.push_back(RandomTexture(random));
    const auto randomDraw = [&]() {
        DrawTemplate draw;
        for (auto& unit : draw.Units)
            if (random() % 3U != 0U) unit = pool[random() % pool.size()];
        return draw;
    };
    std::vector<DrawTemplate> scene;
    for (size_t index = 0; index < 96U; ++index) scene.push_back(randomDraw());
    // A guest address inside a random pooled texture (mapped, not aliased).
    const auto insideTexture = [&](size_t& room) -> uint32_t {
        for (;;) {
            const Texture& texture = pool[random() % pool.size()];
            const size_t bytes = BytesOf(texture);
            if (bytes == 0U || texture.Physical == 0x0FF00000U) continue;
            const size_t at = random() % bytes;
            room = bytes - at;
            return GuestOf(texture.Physical) + static_cast<uint32_t>(at);
        }
    };

    for (size_t frame = 0; frame < frames; ++frame) {
        // Content changes inside textures.
        for (size_t count = random() % 5U; count != 0U; --count) {
            size_t room = 0U;
            const uint32_t guest = insideTexture(room);
            std::vector<uint8_t> bytes(std::min<size_t>(room, 1U + random() % 700U));
            for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
            write(guest, bytes);
        }
        // Same-value writes: one byte in a page of many textures, and a few
        // larger spans.
        for (size_t count = random() % 40U; count != 0U; --count) {
            size_t room = 0U;
            const uint32_t guest = insideTexture(room);
            const size_t size = std::min<size_t>(room, random() % 8U == 0U ? 1U + random() % 20000U : 1U);
            const std::vector<uint8_t> same(mirror.Guest(guest, size).begin(), mirror.Guest(guest, size).end());
            write(guest, same);
        }
        // Escaped write pointers: raw writes the cards never see.
        if (random() % 53U == 0U) {
            size_t room = 0U;
            const uint32_t guest = insideTexture(room);
            const size_t size = std::min<size_t>(room, 64U + random() % 8192U);
            for (Run* run : runs) {
                uint8_t* pointer = run->Memory.GetWritePointer(guest, size);
                Require(pointer != nullptr, "no write pointer");
                run->Escaped.push_back({pointer, guest, size});
            }
            ++escapedRanges;
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
            size_t room = 0U;
            const uint32_t guest = insideTexture(room);
            const size_t size = std::min<size_t>(room, 16U + random() % 9000U);
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
                ++deferredWrites;
            }
        }
        // New textures and draws now and then (misses).
        for (size_t count = random() % 3U; count != 0U; --count) pool[random() % pool.size()] = RandomTexture(random);
        for (size_t count = random() % 3U; count != 0U; --count) scene[random() % scene.size()] = randomDraw();
        // The payload transform set or removed (the cache is emptied).
        if (frame % 151U == 150U) {
            transformed = !transformed;
            for (Run* run : runs) run->Queue->SetTexturePayloadTransform(
                    transformed ? Oot3dNativePicaSubmissionQueue::TexturePayloadTransform(Transform)
                                : Oot3dNativePicaSubmissionQueue::TexturePayloadTransform{});
        }
        std::vector<const DrawTemplate*> submitted;
        for (const auto& draw : scene) {
            if (random() % 8U == 0U) continue;
            const auto packet = MakePacket(draw);
            std::array<Outcome, 3> outcomes;
            for (size_t index = 0; index < runs.size(); ++index) {
                if (index == 2U && random() % 4U == 0U) toggled.Queue->SetTextureSnapshotFast(random() % 2U != 0U);
                outcomes[index].Ok = runs[index]->Queue->SubmitDrawPacket(packet, &outcomes[index].Error);
            }
            Require(outcomes[0].Ok == outcomes[1].Ok && outcomes[0].Ok == outcomes[2].Ok &&
                        outcomes[0].Error == outcomes[1].Error && outcomes[0].Error == outcomes[2].Error,
                    "a draw succeeded or failed differently: '" + outcomes[0].Error + "' / '" +
                        outcomes[1].Error + "' / '" + outcomes[2].Error + "'");
            if (outcomes[0].Ok) {
                submitted.push_back(&draw);
            } else {
                ++failures;
            }
        }
        std::array<std::vector<Oot3dPicaDrawSubmission>, 3> taken;
        for (size_t index = 0; index < runs.size(); ++index) runs[index]->Queue->TakePendingDraws(taken[index]);
        Require(taken[0].size() == submitted.size() && taken[1].size() == submitted.size() &&
                    taken[2].size() == submitted.size(),
                "draw counts differ");
        for (size_t index = 0; index < submitted.size(); ++index) {
            RequireMemoryContent(taken[0][index], *submitted[index], mirror, transformed);
            RequireSameDraw(taken[0][index], taken[1][index], "fast path");
            RequireSameDraw(taken[0][index], taken[2][index], "switch toggled");
        }
        draws += submitted.size();
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
    Require(counters.TextureUncertified != 0U && counters.TextureVersionHits != 0U &&
                counters.TextureUnprovenBytes != 0U && counters.TextureMisses != 0U &&
                fastCounters.TextureFallbacks != 0U && fastCounters.TextureDirtyPages != 0U &&
                escapedRanges != 0U && deferredWrites != 0U && failures != 0U,
            "the replay did not reach every case");
    Require(fastCounters.TextureComparedBytes < counters.TextureComparedBytes,
            "the fast path did not compare fewer bytes");
    std::printf("replay seed=%llu lean=%d frames=%zu draws=%llu failed=%llu textures=%llu version_hits=%llu "
                "uncertified=%llu misses=%llu fallbacks=%llu escaped=%llu deferred=%llu "
                "compared_kb off=%.0f on=%.0f (unproven %.0f) pages=%llu changed=%llu\n",
                static_cast<unsigned long long>(seed), lean ? 1 : 0, frames,
                static_cast<unsigned long long>(draws), static_cast<unsigned long long>(failures),
                static_cast<unsigned long long>(counters.TextureResources),
                static_cast<unsigned long long>(counters.TextureVersionHits),
                static_cast<unsigned long long>(counters.TextureUncertified),
                static_cast<unsigned long long>(counters.TextureMisses),
                static_cast<unsigned long long>(fastCounters.TextureFallbacks),
                static_cast<unsigned long long>(escapedRanges), static_cast<unsigned long long>(deferredWrites),
                counters.TextureComparedBytes / 1024.0, fastCounters.TextureComparedBytes / 1024.0,
                counters.TextureUnprovenBytes / 1024.0, static_cast<unsigned long long>(fastCounters.TexturePages),
                static_cast<unsigned long long>(fastCounters.TextureDirtyPages));
}

// ------------------------------------------------------------------- bench

enum class Pattern { Steady, OnePage, AllPages };

// The frontend state a draw packet is made of (as the frontend keeps it):
// every shared table its own storage, scene lights bound.
struct FrontendState {
    Oot3dPicaDrawPacket Packet;
    Oot3dPicaStateDirtyLines Dirty;
    Fast::Renderer3ds::PicaNativeLightBinding Lights;

    FrontendState() {
        Packet.LightingLuts = std::make_shared<Oot3dPicaLightingLutState>();
        Lights.ScenePoints = std::make_shared<Fast::Renderer3ds::PicaScenePointLights>();
        Lights.SceneDirectionals = std::make_shared<Fast::Renderer3ds::PicaSceneDirectionalLights>();
        Lights.Water = std::make_shared<Fast::Renderer3ds::PicaSceneWaterBodies>();
        Lights.RoomShadows = std::make_shared<Fast::Renderer3ds::RoomShadowShellScene>();
    }

    Oot3dPicaDrawPacketSource Source() {
        Oot3dPicaDrawPacketSource source;
        source.CommandListAddress = Packet.CommandListAddress;
        source.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
        source.Composition = Packet.Composition;
        source.LightBinding = &Lights;
        source.Indexed = Packet.Indexed;
        source.Registers = &Packet.Registers;
        source.VertexShader = &Packet.VertexShader;
        source.GeometryShader = &Packet.GeometryShader;
        source.FogLut = &Packet.FogLut;
        source.ProcTexLuts = &Packet.ProcTexLuts;
        source.LightingLuts = Packet.LightingLuts;
        source.DefaultAttributes = &Packet.DefaultAttributes;
        source.DirtyLines = &Dirty;
        return source;
    }
};

// The plan builder's side: drained draws and their log, materialized and
// freed on another thread; the emptied vectors come back for the queue.
class Consumer {
  public:
    Consumer() : mThread([this] { Loop(); }) {}
    ~Consumer() {
        {
            std::lock_guard lock(mMutex);
            mStop = true;
        }
        mWake.notify_all();
        mThread.join();
    }
    void Hand(std::vector<Oot3dPicaDrawSubmission> draws, std::shared_ptr<Oot3dPicaCaptureDeltaLog> log) {
        {
            std::lock_guard lock(mMutex);
            mWork.emplace_back(std::move(draws), std::move(log));
        }
        mWake.notify_all();
    }
    // An emptied vector (with its capacity) once the ring is full.
    std::vector<Oot3dPicaDrawSubmission> Recycled(size_t ring) {
        std::unique_lock lock(mMutex);
        if (mFree.empty() && mCreated < ring) {
            ++mCreated;
            return {};
        }
        mWake.wait(lock, [this] { return !mFree.empty(); });
        auto draws = std::move(mFree.front());
        mFree.pop_front();
        return draws;
    }

  private:
    void Loop() {
        std::unique_lock lock(mMutex);
        for (;;) {
            mWake.wait(lock, [this] { return mStop || !mWork.empty(); });
            if (mWork.empty()) return;
            auto [draws, log] = std::move(mWork.front());
            mWork.pop_front();
            lock.unlock();
            if (log != nullptr) log->MaterializeAll(draws);
            draws.clear();
            log.reset();
            lock.lock();
            mFree.push_back(std::move(draws));
            mWake.notify_all();
        }
    }
    std::mutex mMutex;
    std::condition_variable mWake;
    std::deque<std::pair<std::vector<Oot3dPicaDrawSubmission>, std::shared_ptr<Oot3dPicaCaptureDeltaLog>>> mWork;
    std::deque<std::vector<Oot3dPicaDrawSubmission>> mFree;
    size_t mCreated = 1U;
    bool mStop = false;
    std::thread mThread;
};

// A room: 110 draws per frame, each with one or two of 120 textures of
// 4-64 KiB (shared between draws), from the frontend's state with capture
// delta; between frames nothing, one same-value byte in every texture, or
// every byte of every texture rewritten with the same value. The texture
// and packet stages per draw.
void Bench(Pattern pattern, bool fastPath, size_t frames) {
    std::mt19937_64 random(11U);
    Run run(fastPath, true);
    run.Queue->SetCaptureDelta(true);
    std::vector<uint8_t> bytes(kLinearBytes);
    for (auto& byte : bytes) byte = static_cast<uint8_t>(random());
    Require(run.Memory.WriteBytes(kLinearGuest, bytes), "bench memory");
    std::vector<Texture> textures;
    uint32_t next = 0x100000U;
    for (size_t index = 0; index < 120U; ++index) {
        Texture texture;
        static constexpr std::array<uint16_t, 3> kSides{32U, 64U, 128U};
        texture.Width = kSides[random() % 3U];
        texture.Height = kSides[random() % 3U];
        texture.Format = static_cast<uint8_t>(std::array<uint8_t, 4>{0U, 2U, 12U, 13U}[random() % 4U]);
        texture.MaxLevel = static_cast<uint8_t>(random() % 3U);
        texture.Physical = kLinearPhysical + next;
        next += (static_cast<uint32_t>(BytesOf(texture)) + 0x3FFU) & ~0x3FFU;
        textures.push_back(texture);
    }
    std::vector<DrawTemplate> scene;
    for (size_t index = 0; index < 110U; ++index) {
        DrawTemplate draw;
        draw.Units[0] = textures[random() % textures.size()];
        if (random() % 2U == 0U) draw.Units[1] = textures[random() % textures.size()];
        scene.push_back(draw);
    }
    FrontendState frontend;
    Consumer consumer;
    std::vector<Oot3dPicaDrawSubmission> drained;
    Oot3dNativePicaSubmissionQueue::CaptureCounters before{};
    std::string error;
    // The command lists the walk reads between two draws (about 1.5 KiB
    // each, 2 MiB in all, as the game's lists stream through the caches).
    std::vector<uint32_t> lists(512U * 1024U);
    for (auto& word : lists) word = static_cast<uint32_t>(random());
    size_t listWord = 0U;
    uint32_t walked = 0U;
    for (size_t frame = 0; frame < frames + 10U; ++frame) {
        if (frame == 10U) before = run.Queue->Counters();
        if (pattern != Pattern::Steady) {
            for (const auto& texture : textures) {
                const uint32_t guest = GuestOf(texture.Physical);
                const size_t size = BytesOf(texture);
                const uint32_t at = pattern == Pattern::OnePage ? guest + static_cast<uint32_t>(random() % size) : guest;
                Require(run.Memory.WriteBytes(at, std::span<const uint8_t>(bytes).subspan(
                                                      at - kLinearGuest, pattern == Pattern::OnePage ? 1U : size)),
                        "bench write");
            }
        }
        for (const auto& draw : scene) {
            for (size_t word = 0; word < 390U; ++word, listWord = (listWord + 1U) % lists.size())
                walked = walked * 31U + lists[listWord];
            const auto packet = MakePacket(draw);
            // The frontend's registers change on a few lines per draw.
            for (size_t id : {size_t{0x080}, size_t{0x081}, size_t{0x091}, size_t{0x08E}, size_t{0x096}}) {
                if (frontend.Packet.Registers[id] != packet.Registers[id]) frontend.Dirty.MarkRegister(id);
            }
            frontend.Packet.Registers = packet.Registers;
            frontend.Dirty.VertexUniforms |= 0x7U;
            Require(run.Queue->SubmitDrawPacketSource(frontend.Source(), &error), error);
        }
        // The session's drain: the queue takes the recycled vector.
        std::shared_ptr<Oot3dPicaCaptureDeltaLog> log;
        run.Queue->TakePendingDraws(drained, log);
        consumer.Hand(std::move(drained), std::move(log));
        drained = consumer.Recycled(4U);
    }
    const auto after = run.Queue->Counters();
    const double count = static_cast<double>(after.Draws - before.Draws);
    const auto us = [count](uint64_t now, uint64_t then) {
        return CaptureTicksToNanoseconds(now - then) / 1.0e3 / count;
    };
    const char* name = pattern == Pattern::Steady ? "steady" : pattern == Pattern::OnePage ? "one_page" : "all_pages";
    std::printf("bench %-9s fast=%d us/draw: texture %.3f (view %.3f compare %.3f copy %.3f) packet %.3f decode %.3f "
                "vertex %.3f regs+uniforms %.3f | per frame: textures %.0f version_hits %.0f unproven %.0f KiB "
                "compared %.0f KiB pages %.0f changed %.0f (submission %zu B, walk %u)\n",
                name, fastPath ? 1 : 0, us(after.TextureTicks, before.TextureTicks),
                us(after.TextureViewTicks, before.TextureViewTicks),
                us(after.TextureCompareTicks, before.TextureCompareTicks),
                us(after.TextureCopyTicks, before.TextureCopyTicks), us(after.PacketTicks, before.PacketTicks),
                us(after.DecodeTicks, before.DecodeTicks), us(after.VertexTicks, before.VertexTicks),
                us(after.RegisterTicks + after.UniformTicks, before.RegisterTicks + before.UniformTicks),
                static_cast<double>(after.TextureResources - before.TextureResources) / frames,
                static_cast<double>(after.TextureVersionHits - before.TextureVersionHits) / frames,
                static_cast<double>(after.TextureUnprovenBytes - before.TextureUnprovenBytes) / 1024.0 / frames,
                static_cast<double>(after.TextureComparedBytes - before.TextureComparedBytes) / 1024.0 / frames,
                static_cast<double>(after.TexturePages - before.TexturePages) / frames,
                static_cast<double>(after.TextureDirtyPages - before.TextureDirtyPages) / frames,
                sizeof(Oot3dPicaDrawSubmission), walked & 0xFU);
}

// The packet constructor alone, single-threaded, hot: 110 draws into a
// reserved vector from the bench's frontend state, with the shared tables
// copied (one reference each) or deferred to the log. The destruction (on
// the consumer's thread in the runtime) is timed apart.
void BenchConstructor(size_t rounds) {
    FrontendState frontend;
    const auto source = frontend.Source();
    std::vector<Oot3dPicaDrawSubmission> draws;
    draws.reserve(110U);
    for (bool deferred : {false, true}) {
        uint64_t constructed = 0U;
        uint64_t destroyed = 0U;
        for (size_t round = 0; round < rounds; ++round) {
            const auto started = CaptureTicks();
            for (uint64_t id = 0; id < 110U; ++id) {
                if (deferred) {
                    draws.emplace_back(id, source, Oot3dPicaStateArraysDeferred{}, Oot3dPicaStateTablesDeferred{});
                } else {
                    draws.emplace_back(id, source, Oot3dPicaStateArraysDeferred{});
                }
            }
            const auto built = CaptureTicks();
            draws.clear();
            constructed += built - started;
            destroyed += CaptureTicks() - built;
        }
        const double count = static_cast<double>(rounds * 110U);
        std::printf("bench constructor tables %s: %.1f ns/draw (destruction %.1f)\n",
                    deferred ? "deferred" : "copied  ", CaptureTicksToNanoseconds(constructed) / count,
                    CaptureTicksToNanoseconds(destroyed) / count);
    }
}

} // namespace

int main(int argc, char** argv) {
    SetRangeWriteGenerationCardPeek(true);
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        const size_t frames = argc > 2 ? static_cast<size_t>(std::strtoul(argv[2], nullptr, 10)) : 300U;
        for (int round = 0; round < 2; ++round) {
            for (Pattern pattern : {Pattern::Steady, Pattern::OnePage, Pattern::AllPages})
                for (bool fastPath : {false, true}) Bench(pattern, fastPath, frames);
            BenchConstructor(20U * frames);
        }
        return 0;
    }
    const size_t frames = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 400U;
    for (uint64_t seed = 1U; seed <= 4U; ++seed) TestReplay(seed, frames, seed % 2U == 0U);
    std::cout << "oot3d_native_texture_snapshot_tests: ok\n";
    return 0;
}
