// Work taken off the game thread (config/offload_*, oot3d_native_offload.h)
// against the previous code paths, on the same random inputs:
//  1. capture: draw submissions through the frontend-state source (packet
//     built in the queue) with lean capture (one-pass texture hashes, lazy
//     geometry hashes, remembered index ranges) give the same submissions,
//     snapshots, hashes and savestate as packets copied in with the old
//     passes, across guest writes with and without write generations, a
//     payload transform and a savestate round trip;
//  2. plan: placeholders written in place in the accumulator and frames
//     closed by handing the pending storage over give the same frames,
//     pending work and resolved plans as placeholders moved in and frames
//     closed plan by plan (random draws, fills, transfers, raw copies,
//     superseded frames, next-frame draws, continuation canvases);
//  3. perf: the service-thread line writer keeps every line, in order
//     (run with TSan: one producer, the writer thread);
//  4. the config parser.
// --bench: game-thread cost per draw of both plan paths and both captures.
#include "oot3d_native_offload.h"
#include "oot3d_native_pica_deferred_plan.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_native_pica_visual_frame.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using namespace Oot3dNativeGame;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_offload_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// ---------------------------------------------------------------- capture

constexpr uint32_t kGuestBase = 0x14000000U;
constexpr uint32_t kPhysicalBase = 0x20000000U;
constexpr size_t kMemoryBytes = 0x10000U;
constexpr size_t kPageBytes = 256U;

struct GuestMemory {
    std::vector<uint8_t> Bytes = std::vector<uint8_t>(kMemoryBytes);
    std::vector<uint64_t> Generations = std::vector<uint64_t>(kMemoryBytes / kPageBytes, 1U);
    bool GenerationsAvailable = true;

    void Write(size_t offset, std::span<const uint8_t> data) {
        std::memcpy(Bytes.data() + offset, data.data(), data.size());
        for (size_t page = offset / kPageBytes; page <= (offset + data.size() - 1U) / kPageBytes; ++page)
            ++Generations[page];
    }

    Oot3dPicaPhysicalMemoryView View() {
        return Oot3dPicaPhysicalMemoryView(
            {{kPhysicalBase, kGuestBase, kMemoryBytes}},
            [this](uint32_t address, size_t size) -> std::span<const uint8_t> {
                if (address < kGuestBase || address - kGuestBase + size > kMemoryBytes) return {};
                return std::span<const uint8_t>(Bytes).subspan(address - kGuestBase, size);
            },
            [this](uint32_t address, size_t size) -> std::optional<uint64_t> {
                if (!GenerationsAvailable || size == 0U || address < kGuestBase ||
                    address - kGuestBase + size > kMemoryBytes)
                    return std::nullopt;
                uint64_t sum = 0U;
                const size_t first = (address - kGuestBase) / kPageBytes;
                const size_t last = (address - kGuestBase + size - 1U) / kPageBytes;
                for (size_t page = first; page <= last; ++page) sum += Generations[page] * (page + 1U);
                return sum;
            });
    }
};

// An indexed draw over the synthetic memory (registers as in
// oot3d_native_pica_submission_tests): index buffer, one vertex loader and
// an RGBA8 texture, 8x8 or 16x16 with a second mip level.
Oot3dPicaDrawPacket MakePacket(std::mt19937_64& random) {
    Oot3dPicaDrawPacket packet{};
    packet.Indexed = true;
    packet.CommandListAddress = static_cast<uint32_t>(random());
    packet.CommandListOffsetWords = static_cast<uint32_t>(random() % 4096U);
    packet.CompositionDomain = random() % 2U ? Oot3dPicaCompositionDomain::Scene
                                              : Oot3dPicaCompositionDomain::Ui;
    packet.Composition.SourcePc = static_cast<uint32_t>(random());
    packet.VertexShader.FloatUniforms[5] = {static_cast<float>(random() % 100U), 1.0F, 2.0F, 3.0F};
    packet.Registers[0x200] = 0x04000000U;
    packet.Registers[0x201] = 0x00000003U;
    packet.Registers[0x202] = 0x00000000U;
    // Vertex loader at 0x0800..0x47FF (stride 4 bytes, 3 bytes of attribute
    // data plus padding is what 0x10040000 describes in the base test).
    packet.Registers[0x203] = 0x800U + static_cast<uint32_t>(random() % 16U) * 0x100U;
    packet.Registers[0x205] = 0x10040000U;
    const bool sixteenBit = random() % 2U != 0U;
    // Index buffers at 0x0100..0x07FF (aligned), 1..64 indices.
    const uint32_t indexOffset = 0x100U + static_cast<uint32_t>(random() % 24U) * 0x40U;
    packet.Registers[0x227] = indexOffset | (sixteenBit ? 0x80000000U : 0U);
    packet.Registers[0x228] = 1U + static_cast<uint32_t>(random() % 64U);
    packet.Registers[0x11D] = 0x04000040U;
    if (random() % 4U != 0U) {
        packet.Registers[0x080] = 1U;
        const uint32_t textureIndex = static_cast<uint32_t>(random() % 6U);
        // Textures at 0x8000 + n * 0x800 (each up to 1280 bytes).
        packet.Registers[0x085] = (kPhysicalBase + 0x8000U + textureIndex * 0x800U) >> 3U;
        packet.Registers[0x08E] = 0U;
        if (random() % 2U != 0U) {
            packet.Registers[0x082] = (8U << 16U) | 8U;
        } else {
            packet.Registers[0x082] = (16U << 16U) | 16U;
            packet.Registers[0x083] = (1U << 1U) | (1U << 2U) | (1U << 24U);
            packet.Registers[0x084] = 0x1F00U | (1U << 16U) | (1U << 24U);
        }
    }
    return packet;
}

// Index values stay below 0x3FF so every loader range fits in memory.
void FillMemory(GuestMemory& memory, std::mt19937_64& random) {
    for (size_t offset = 0x100U; offset < 0x800U; offset += 2U) {
        const uint16_t value = static_cast<uint16_t>(random() % 0x3FFU);
        memory.Bytes[offset] = static_cast<uint8_t>(value);
        memory.Bytes[offset + 1U] = static_cast<uint8_t>(random() % 4U);
    }
    for (size_t offset = 0x800U; offset < kMemoryBytes; ++offset)
        memory.Bytes[offset] = static_cast<uint8_t>(random());
}

void Mutate(GuestMemory& memory, std::mt19937_64& random) {
    const size_t writes = random() % 4U;
    for (size_t write = 0; write < writes; ++write) {
        const size_t region = random() % 3U;
        size_t offset = 0;
        size_t size = 1U + random() % 64U;
        std::vector<uint8_t> data(size);
        if (region == 0U) {
            offset = 0x100U + (random() % 0x600U & ~size_t{1});
            size = std::min(size & ~size_t{1}, 0x800U - offset);
            if (size == 0U) continue;
            data.resize(size);
            for (size_t index = 0; index < size; index += 2U) {
                data[index] = static_cast<uint8_t>(random() % 0x3FFU);
                data[index + 1U] = static_cast<uint8_t>(random() % 4U);
            }
        } else {
            offset = (region == 1U ? 0x800U : 0x8000U) + random() % 0x3000U;
            for (auto& byte : data) byte = static_cast<uint8_t>(random());
        }
        // Same-value rewrites too: the generation changes, the content not.
        if (random() % 5U == 0U)
            std::memcpy(data.data(), memory.Bytes.data() + offset, data.size());
        memory.Write(offset, data);
    }
}

Oot3dPicaDrawPacketSource SourceOf(const Oot3dPicaDrawPacket& packet) {
    Oot3dPicaDrawPacketSource source;
    source.CommandListAddress = packet.CommandListAddress;
    source.CommandListOffsetWords = packet.CommandListOffsetWords;
    source.CompositionDomain = packet.CompositionDomain;
    source.Composition = packet.Composition;
    source.LightBinding = &packet.LightBinding;
    source.Indexed = packet.Indexed;
    source.Registers = &packet.Registers;
    source.VertexShader = &packet.VertexShader;
    source.GeometryShader = &packet.GeometryShader;
    source.FogLut = &packet.FogLut;
    source.ProcTexLuts = &packet.ProcTexLuts;
    source.LightingLuts = packet.LightingLuts;
    source.DefaultAttributes = &packet.DefaultAttributes;
    return source;
}

bool SameShader(const Oot3dPicaShaderState& a, const Oot3dPicaShaderState& b) {
    return a.Program.Values() == b.Program.Values() && a.Swizzles.Values() == b.Swizzles.Values() &&
           a.Program.MutationIdentity() == b.Program.MutationIdentity() &&
           a.Swizzles.MutationIdentity() == b.Swizzles.MutationIdentity() &&
           std::memcmp(a.FloatUniforms.data(), b.FloatUniforms.data(), sizeof(a.FloatUniforms)) == 0 &&
           a.IntegerUniforms == b.IntegerUniforms && a.BooleanUniforms == b.BooleanUniforms &&
           a.ProgramWordCount == b.ProgramWordCount && a.SwizzleWordCount == b.SwizzleWordCount;
}

bool SamePacket(const Oot3dPicaDrawPacket& a, const Oot3dPicaDrawPacket& b) {
    return a.CommandListAddress == b.CommandListAddress &&
           a.CommandListOffsetWords == b.CommandListOffsetWords &&
           a.CompositionDomain == b.CompositionDomain &&
           a.Composition.Layer == b.Composition.Layer &&
           a.Composition.SourcePc == b.Composition.SourcePc &&
           a.Composition.NativeValue == b.Composition.NativeValue &&
           a.LightBinding == b.LightBinding &&
           a.Indexed == b.Indexed && a.Registers == b.Registers &&
           SameShader(a.VertexShader, b.VertexShader) &&
           SameShader(a.GeometryShader, b.GeometryShader) && a.FogLut == b.FogLut &&
           a.ProcTexLuts.Noise == b.ProcTexLuts.Noise && a.ProcTexLuts.ColorMap == b.ProcTexLuts.ColorMap &&
           a.ProcTexLuts.AlphaMap == b.ProcTexLuts.AlphaMap && a.ProcTexLuts.Color == b.ProcTexLuts.Color &&
           a.ProcTexLuts.ColorDifference == b.ProcTexLuts.ColorDifference &&
           a.LightingLuts == b.LightingLuts &&
           std::memcmp(a.DefaultAttributes.data(), b.DefaultAttributes.data(),
                       sizeof(a.DefaultAttributes)) == 0;
}

bool SameResource(const Oot3dPicaResourceSnapshot& a, const Oot3dPicaResourceSnapshot& b) {
    const auto x = a.ResolvedBytes();
    const auto y = b.ResolvedBytes();
    return a.Kind == b.Kind && a.Slot == b.Slot && a.PhysicalAddress == b.PhysicalAddress &&
           a.FirstElement == b.FirstElement && std::equal(x.begin(), x.end(), y.begin(), y.end()) &&
           a.ContentHash == b.ContentHash && a.ContentHashAvailable == b.ContentHashAvailable &&
           a.ContentVersion == b.ContentVersion &&
           a.ContentVersionAvailable == b.ContentVersionAvailable &&
           a.BaseLevelContentHash == b.BaseLevelContentHash &&
           a.BaseLevelContentHashAvailable == b.BaseLevelContentHashAvailable &&
           a.ReplacementWidth == b.ReplacementWidth && a.ReplacementHeight == b.ReplacementHeight &&
           a.CopyReferences.size() == b.CopyReferences.size();
}

bool SameSubmission(const Oot3dPicaDrawSubmission& a, const Oot3dPicaDrawSubmission& b) {
    if (a.Id != b.Id || !SamePacket(a.Packet, b.Packet) ||
        a.MinimumVertexIndex != b.MinimumVertexIndex ||
        a.MaximumVertexIndex != b.MaximumVertexIndex || a.Resources.size() != b.Resources.size())
        return false;
    const auto& s = a.State;
    const auto& t = b.State;
    if (s.VertexInput.VertexCount != t.VertexInput.VertexCount ||
        s.VertexInput.Indexed != t.VertexInput.Indexed ||
        s.VertexInput.IndicesAre16Bit != t.VertexInput.IndicesAre16Bit ||
        s.VertexInput.IndexPhysicalAddress != t.VertexInput.IndexPhysicalAddress ||
        s.Framebuffer.ColorPhysicalAddress != t.Framebuffer.ColorPhysicalAddress)
        return false;
    for (size_t slot = 0; slot < s.Textures.size(); ++slot) {
        if (s.Textures[slot].Enabled != t.Textures[slot].Enabled ||
            s.Textures[slot].PhysicalAddress != t.Textures[slot].PhysicalAddress ||
            s.Textures[slot].Width != t.Textures[slot].Width ||
            s.Textures[slot].Height != t.Textures[slot].Height)
            return false;
    }
    for (size_t index = 0; index < a.Resources.size(); ++index) {
        if (!SameResource(a.Resources[index], b.Resources[index])) return false;
    }
    return true;
}

struct CaptureRun {
    GuestMemory Memory;
    std::optional<Oot3dNativePicaSubmissionQueue> Queue;
    bool Lean = false;
    bool Transform = false;

    void Make() {
        Queue.emplace(Memory.View(), true);
        Queue->SetLeanCapture(Lean);
        if (Transform) {
            Queue->SetTexturePayloadTransform(
                [](const Oot3dPicaTextureState&, std::span<uint8_t> payload) {
                    for (auto& byte : payload) byte = static_cast<uint8_t>(byte ^ 0x5AU);
                });
        }
    }
    bool Submit(const Oot3dPicaDrawPacket& packet, std::string* error) {
        return Lean ? Queue->SubmitDrawPacketSource(SourceOf(packet), error)
                    : Queue->SubmitDrawPacket(packet, error);
    }
};

void TestCapture(uint64_t seed, bool transform, size_t steps) {
    std::mt19937_64 random(seed);
    CaptureRun before;
    CaptureRun lean;
    lean.Lean = true;
    before.Transform = lean.Transform = transform;
    FillMemory(before.Memory, random);
    lean.Memory.Bytes = before.Memory.Bytes;
    before.Make();
    lean.Make();
    std::string error;
    std::string leanError;
    uint64_t draws = 0U;
    for (size_t step = 0; step < steps; ++step) {
        std::mt19937_64 mutation(random());
        Mutate(before.Memory, mutation);
        lean.Memory.Bytes = before.Memory.Bytes;
        lean.Memory.Generations = before.Memory.Generations;
        // Some stretches without write generations (content compares).
        const bool generations = (step / 37U) % 3U != 2U;
        before.Memory.GenerationsAvailable = lean.Memory.GenerationsAvailable = generations;
        const size_t count = 1U + random() % 12U;
        for (size_t draw = 0; draw < count; ++draw) {
            const auto packet = MakePacket(random);
            const bool ok = before.Submit(packet, &error);
            const bool leanOk = lean.Submit(packet, &leanError);
            Require(ok == leanOk && error == leanError, "capture paths disagree on a failure");
            ++draws;
        }
        const auto a = before.Queue->TakePendingDraws();
        const auto b = lean.Queue->TakePendingDraws();
        Require(a.size() == b.size(), "capture paths queued different draw counts");
        for (size_t index = 0; index < a.size(); ++index) {
            Require(SameSubmission(a[index], b[index]),
                    "lean capture changed a submission (seed " + std::to_string(seed) +
                        ", step " + std::to_string(step) + ")");
        }
        if (step % 97U == 96U) {
            // Savestate: the same document (geometry hashes computed on
            // demand), and both continue from their restored states.
            const auto stateA = before.Queue->CaptureState();
            const auto stateB = lean.Queue->CaptureState();
            Require(stateA == stateB, "lean capture changed the savestate");
            before.Make();
            lean.Make();
            Require(before.Queue->RestoreState(stateA, &error), error);
            Require(lean.Queue->RestoreState(stateB, &leanError), leanError);
        }
    }
    Require(before.Queue->CaptureState() == lean.Queue->CaptureState(),
            "lean capture changed the final savestate");
    const auto countersA = before.Queue->Counters();
    const auto countersB = lean.Queue->Counters();
    Require(countersA.Draws == countersB.Draws && countersA.CopiedBytes == countersB.CopiedBytes,
            "capture counters disagree");
    Require(countersB.HashedBytes <= countersA.HashedBytes, "lean capture hashed more");
    std::printf("capture seed=%llu transform=%d draws=%llu hashed_kb old=%.0f lean=%.0f copied_kb=%.0f "
                "index_ranges_reused=%llu\n",
                static_cast<unsigned long long>(seed), transform ? 1 : 0,
                static_cast<unsigned long long>(draws), countersA.HashedBytes / 1024.0,
                countersB.HashedBytes / 1024.0, countersA.CopiedBytes / 1024.0,
                static_cast<unsigned long long>(countersB.IndexRangeReuses));
}

void TestPacketSource() {
    std::mt19937_64 random(7U);
    for (int round = 0; round < 64; ++round) {
        auto packet = MakePacket(random);
        packet.LightBinding = {};
        if (round % 2 == 0) {
            std::array<float, 16> view{};
            for (auto& value : view) value = static_cast<float>(random() % 1000U);
            packet.LightBinding.WorldToView = view;
        }
        packet.FogLut[round % 128] = static_cast<uint32_t>(random());
        packet.DefaultAttributes[round % 16][1] = static_cast<float>(round);
        packet.LightingLuts = std::make_shared<Oot3dPicaLightingLutState>();
        const Oot3dPicaDrawPacket copy(packet.CommandListAddress, packet.CommandListOffsetWords,
                                       packet.CompositionDomain, packet.Composition, packet.Indexed,
                                       packet.Registers, packet.VertexShader, packet.GeometryShader,
                                       packet.FogLut, packet.ProcTexLuts, packet.LightingLuts,
                                       packet.DefaultAttributes);
        Oot3dPicaDrawPacket withBinding = copy;
        withBinding.LightBinding = packet.LightBinding;
        Require(SamePacket(Oot3dPicaDrawPacket(SourceOf(packet)), withBinding),
                "a packet built from the frontend source differs");
        auto source = SourceOf(packet);
        source.LightBinding = nullptr;
        Require(SamePacket(Oot3dPicaDrawPacket(source), copy),
                "a packet without a light binding differs");
    }
}

// The frontend's two ways of handing a draw to its sink: the packet it
// builds and copies in, or the source the sink builds it from (here the
// sink's default, which builds the packet and records it).
struct RecordingSink final : Oot3dPicaPacketSink {
    std::vector<Oot3dPicaDrawPacket> Packets;
    uint64_t SourceDraws = 0U;
    bool SubmitHardwareRegisterWrite(const Oot3dPicaHardwareRegisterWrite&, std::string*) override {
        return true;
    }
    bool SubmitDrawPacket(const Oot3dPicaDrawPacket& packet, std::string*) override {
        Packets.push_back(packet);
        return true;
    }
    bool SubmitDrawPacketSource(const Oot3dPicaDrawPacketSource& source, std::string* error) override {
        ++SourceDraws;
        return Oot3dPicaPacketSink::SubmitDrawPacketSource(source, error);
    }
};

void TestFrontendSource(uint64_t seed) {
    std::mt19937_64 random(seed);
    RecordingSink copiedSink;
    RecordingSink sourceSink;
    Oot3dNativePicaFrontend copied(&copiedSink);
    Oot3dNativePicaFrontend source(&sourceSink);
    copied.SetDiagnosticHistoryEnabled(false);
    source.SetDiagnosticHistoryEnabled(false);
    source.SetInPlaceDrawPackets(true);
    // Registers the draw state reads (vertex input, textures, TEV, fog);
    // vertex-shader float uniforms are uploaded as well-formed sequences
    // (setup 0x2C0, then 3 or 4 data words to 0x2C1).
    constexpr std::array<uint16_t, 13> kRegisters{0x040, 0x080, 0x082, 0x085, 0x0C0, 0x0C3,
                                                  0x0E0, 0x200, 0x201, 0x203, 0x205, 0x227,
                                                  0x228};
    std::string error;
    for (int list = 0; list < 300; ++list) {
        std::vector<uint32_t> words;
        const size_t commands = 1U + random() % 40U;
        for (size_t command = 0; command < commands; ++command) {
            if (random() % 6U == 0U) {
                const bool float32 = random() % 2U != 0U;
                words.push_back(static_cast<uint32_t>(random() % 96U) | (float32 ? 0x80000000U : 0U));
                words.push_back(0x2C0U | (0xFU << 16U));
                for (size_t word = 0; word < (float32 ? 4U : 3U); ++word) {
                    words.push_back(static_cast<uint32_t>(random()));
                    words.push_back(0x2C1U | (0xFU << 16U));
                }
                continue;
            }
            const uint64_t kind = random() % 8U;
            const uint16_t id = kind == 0U ? (random() % 2U ? 0x22EU : 0x22FU)
                                           : kRegisters[random() % kRegisters.size()];
            words.push_back(static_cast<uint32_t>(random()));
            words.push_back(static_cast<uint32_t>(id) | (0xFU << 16U));
        }
        const uint32_t address = 0x14000000U + static_cast<uint32_t>(list) * 0x1000U;
        const uint32_t size = static_cast<uint32_t>(words.size() * 4U);
        Oot3dGspCommandPacket submit{};
        submit.Control = 1;
        submit.Parameters[0] = address;
        submit.Parameters[1] = size;
        if (random() % 2U != 0U) {
            Oot3dPicaCommandListCompositionSpan span;
            span.BeginAddress = address;
            span.EndAddress = address + size;
            span.Attribution.Layer = Oot3dPicaCompositionLayer::OpaqueWorld;
            span.Attribution.SourcePc = static_cast<uint32_t>(random());
            std::array<float, 16> view{};
            for (auto& value : view) value = static_cast<float>(random() % 1000U);
            span.LightBinding.WorldToView = view;
            const std::array<Oot3dPicaCommandListCompositionSpan, 1> spans{span};
            Require(copied.SetNextCommandListCompositionSpans(address, size, spans, &error), error);
            Require(source.SetNextCommandListCompositionSpans(address, size, spans, &error), error);
        }
        const bool a = copied.SubmitGspCommand(submit, words, &error);
        const bool b = source.SubmitGspCommand(submit, words, &error);
        Require(a == b, "frontend paths disagree on a command list");
    }
    Require(copiedSink.Packets.size() == sourceSink.Packets.size() && !copiedSink.Packets.empty() &&
                sourceSink.SourceDraws == sourceSink.Packets.size() && copiedSink.SourceDraws == 0U,
            "frontend paths submitted different draws");
    for (size_t index = 0; index < copiedSink.Packets.size(); ++index) {
        const auto& x = copiedSink.Packets[index];
        const auto& y = sourceSink.Packets[index];
        // Shader storage is copy-on-write per frontend: the mutation
        // identities differ between the two frontends, the words not.
        Require(x.Registers == y.Registers && x.CommandListAddress == y.CommandListAddress &&
                    x.CommandListOffsetWords == y.CommandListOffsetWords &&
                    x.CompositionDomain == y.CompositionDomain && x.Indexed == y.Indexed &&
                    x.Composition.Layer == y.Composition.Layer &&
                    x.Composition.SourcePc == y.Composition.SourcePc &&
                    x.LightBinding == y.LightBinding &&
                    x.VertexShader.Program.Values() == y.VertexShader.Program.Values() &&
                    std::memcmp(x.VertexShader.FloatUniforms.data(), y.VertexShader.FloatUniforms.data(),
                                sizeof(x.VertexShader.FloatUniforms)) == 0 &&
                    x.FogLut == y.FogLut &&
                    std::memcmp(x.DefaultAttributes.data(), y.DefaultAttributes.data(),
                                sizeof(x.DefaultAttributes)) == 0 &&
                    x.LightingLuts->PackedEntries == y.LightingLuts->PackedEntries &&
                    x.LightingLuts->ContentHash == y.LightingLuts->ContentHash,
                "a draw built from the frontend source differs from the copied packet");
    }
    std::printf("frontend seed=%llu draws=%zu\n", static_cast<unsigned long long>(seed),
                copiedSink.Packets.size());
}

// ------------------------------------------------------------------- plan

constexpr uint32_t kTopSource = 0x1F000000U;
constexpr uint32_t kBottomSource = 0x1F200000U;
constexpr std::array<uint32_t, 3> kDisplayOutputs{0x1E000000U, 0x1E100000U, 0x1E200000U};

Oot3dPicaVulkanDrawPlan MakePlan(uint64_t id, std::mt19937_64& random) {
    Oot3dPicaVulkanDrawPlan plan;
    plan.SubmissionId = id;
    plan.CommandListAddress = static_cast<uint32_t>(random());
    plan.State.Framebuffer.ColorPhysicalAddress = random() % 4U == 0U ? kBottomSource : kTopSource;
    plan.State.Viewport.CornerX = static_cast<int16_t>((random() % 3U) * 40U);
    plan.State.Viewport.HalfWidth = random() % 2U ? 200.0F : 160.0F;
    plan.State.Viewport.HalfHeight = 120.0F;
    plan.VertexCount = static_cast<uint32_t>(random() % 1000U);
    plan.VertexShader.Uniforms.Floats[3][1] = static_cast<float>(id);
    plan.FragmentShader.Uniforms.FogLut[7][0] = static_cast<float>(random() % 1000U);
    plan.IndexBytes.resize(8U + random() % 24U);
    for (auto& byte : plan.IndexBytes) byte = static_cast<uint8_t>(random());
    std::memcpy(plan.IndexBytes.data(), &id, sizeof(id));
    plan.SharedIndexBytes = std::make_shared<const std::vector<uint8_t>>(plan.IndexBytes);
    plan.GeometryIdentity = random();
    return plan;
}

bool SamePlan(const Oot3dPicaVulkanDrawPlan& a, const Oot3dPicaVulkanDrawPlan& b) {
    return a.SubmissionId == b.SubmissionId && a.CommandListAddress == b.CommandListAddress &&
           a.CommandListOffsetWords == b.CommandListOffsetWords &&
           a.CompositionDomain == b.CompositionDomain &&
           a.State.Framebuffer.ColorPhysicalAddress == b.State.Framebuffer.ColorPhysicalAddress &&
           a.State.Viewport.CornerX == b.State.Viewport.CornerX &&
           a.State.Viewport.HalfWidth == b.State.Viewport.HalfWidth &&
           a.VertexCount == b.VertexCount && a.IndexBytes == b.IndexBytes &&
           (a.SharedIndexBytes == nullptr) == (b.SharedIndexBytes == nullptr) &&
           (a.SharedIndexBytes == nullptr || *a.SharedIndexBytes == *b.SharedIndexBytes) &&
           a.GeometryIdentity == b.GeometryIdentity &&
           std::memcmp(a.VertexShader.Uniforms.Floats.data(), b.VertexShader.Uniforms.Floats.data(),
                       sizeof(a.VertexShader.Uniforms.Floats)) == 0 &&
           std::memcmp(a.FragmentShader.Uniforms.FogLut.data(), b.FragmentShader.Uniforms.FogLut.data(),
                       sizeof(a.FragmentShader.Uniforms.FogLut)) == 0 &&
           a.ResolvedVertexShaderSource() == b.ResolvedVertexShaderSource() &&
           a.ResolvedFragmentShaderSource() == b.ResolvedFragmentShaderSource() &&
           a.VertexBindings.size() == b.VertexBindings.size() &&
           a.Textures.size() == b.Textures.size() && a.BaseVertex == b.BaseVertex &&
           (a.Deferred == nullptr) == (b.Deferred == nullptr);
}

bool SameTransfer(const Oot3dPicaDisplayTransferSubmission& a,
                  const Oot3dPicaDisplayTransferSubmission& b) {
    return a.CompletionId == b.CompletionId && a.AfterDrawSubmissionId == b.AfterDrawSubmissionId &&
           a.InputPhysicalAddress == b.InputPhysicalAddress &&
           a.OutputPhysicalAddress == b.OutputPhysicalAddress &&
           a.Transfer.TextureCopyBytes == b.Transfer.TextureCopyBytes &&
           a.SignalInterrupt == b.SignalInterrupt;
}

bool SameFrame(const Oot3dPicaVisualFrame& a, const Oot3dPicaVisualFrame& b) {
    if (a.Sequence != b.Sequence || !SameTransfer(a.TopTransfer, b.TopTransfer) ||
        a.Draws.size() != b.Draws.size() || a.StrictDrawIdentities != b.StrictDrawIdentities ||
        a.MemoryFills.size() != b.MemoryFills.size() ||
        a.DisplayTransfers.size() != b.DisplayTransfers.size() ||
        a.DependencyExecutedThroughDraw != b.DependencyExecutedThroughDraw ||
        a.DependencyExecutedTransfers != b.DependencyExecutedTransfers)
        return false;
    for (size_t index = 0; index < a.Draws.size(); ++index)
        if (!SamePlan(a.Draws[index], b.Draws[index])) return false;
    for (size_t index = 0; index < a.MemoryFills.size(); ++index) {
        const auto& x = a.MemoryFills[index];
        const auto& y = b.MemoryFills[index];
        if (x.CompletionId != y.CompletionId || x.BeforeDrawSubmissionId != y.BeforeDrawSubmissionId ||
            x.StartPhysicalAddress != y.StartPhysicalAddress || x.Value != y.Value)
            return false;
    }
    for (size_t index = 0; index < a.DisplayTransfers.size(); ++index)
        if (!SameTransfer(a.DisplayTransfers[index], b.DisplayTransfers[index])) return false;
    return true;
}

// The accumulator alone: random guest work closed into frames by both
// FinishFrames paths; frames are kept alive for a while and released in
// random order (the render thread's history), so pooled storage cycles.
void TestAccumulator(uint64_t seed, size_t steps) {
    std::mt19937_64 random(seed);
    Oot3dPicaVisualFrameAccumulator before;
    Oot3dPicaVisualFrameAccumulator after;
    before.SetMoveFreeFinish(false);
    after.SetMoveFreeFinish(true);
    std::vector<Oot3dPicaVisualFrame> heldBefore;
    std::vector<Oot3dPicaVisualFrame> heldAfter;
    uint64_t nextDraw = 1U;
    uint64_t nextCompletion = 1U;
    uint64_t frames = 0U;
    uint64_t multiFrame = 0U;
    for (size_t step = 0; step < steps; ++step) {
        const size_t actions = random() % 40U;
        for (size_t action = 0; action < actions; ++action) {
            const uint64_t kind = random() % 16U;
            if (kind < 11U) {
                auto plan = MakePlan(nextDraw++, random);
                before.Append(plan);
                if (random() % 2U) {
                    after.EmplaceDraw() = std::move(plan);
                } else {
                    after.Append(std::move(plan));
                }
            } else if (kind < 13U) {
                Oot3dPicaMemoryFillSubmission fill;
                fill.BeforeDrawSubmissionId = nextDraw;
                fill.StartPhysicalAddress = static_cast<uint32_t>(random());
                fill.Value = static_cast<uint32_t>(random());
                before.Append(fill);
                after.Append(fill);
            } else {
                Oot3dPicaDisplayTransferSubmission transfer;
                transfer.CompletionId = nextCompletion++;
                transfer.AfterDrawSubmissionId = nextDraw - 1U;
                transfer.InputPhysicalAddress = random() % 3U ? kTopSource : kBottomSource;
                transfer.OutputPhysicalAddress = kDisplayOutputs[random() % kDisplayOutputs.size()];
                if (random() % 6U == 0U) {
                    transfer.Transfer.TextureCopyBytes = 256U;
                    transfer.SignalInterrupt = random() % 2U != 0U;
                }
                before.Append(transfer);
                after.Append(transfer);
            }
        }
        if (random() % 3U != 0U) {
            Oot3dPicaDisplayTransferSubmission top;
            top.CompletionId = nextCompletion++;
            top.AfterDrawSubmissionId = nextDraw - 1U - std::min<uint64_t>(nextDraw - 1U, random() % 4U);
            top.InputPhysicalAddress = kTopSource;
            top.OutputPhysicalAddress = kDisplayOutputs[random() % kDisplayOutputs.size()];
            std::vector<uint32_t> outputs(kDisplayOutputs.begin(),
                                          kDisplayOutputs.begin() + 1 + random() % kDisplayOutputs.size());
            std::vector<Oot3dPicaViewportState> canvases;
            if (random() % 4U == 0U) {
                Oot3dPicaViewportState canvas;
                canvas.CornerX = 40;
                canvas.HalfWidth = 160.0F;
                canvas.HalfHeight = 120.0F;
                canvases.push_back(canvas);
            }
            auto a = before.FinishFrames(top, outputs, canvases);
            auto b = after.FinishFrames(top, outputs, canvases);
            Require(a.size() == b.size(), "FinishFrames paths closed different frame counts");
            for (size_t index = 0; index < a.size(); ++index) {
                Require(SameFrame(a[index], b[index]),
                        "move-free FinishFrames changed a frame (seed " + std::to_string(seed) +
                            ", step " + std::to_string(step) + ")");
            }
            frames += a.size();
            multiFrame += a.size() > 1U ? 1U : 0U;
            for (auto& frame : a) heldBefore.push_back(std::move(frame));
            for (auto& frame : b) heldAfter.push_back(std::move(frame));
        }
        const auto pendingA = before.PendingDraws();
        const auto pendingB = after.PendingDraws();
        Require(pendingA.size() == pendingB.size(), "pending draw counts differ");
        for (size_t index = 0; index < pendingA.size(); ++index)
            Require(SamePlan(pendingA[index], pendingB[index]), "pending draws differ");
        while (heldBefore.size() > 3U) {
            const size_t victim = random() % heldBefore.size();
            heldBefore.erase(heldBefore.begin() + static_cast<std::ptrdiff_t>(victim));
            heldAfter.erase(heldAfter.begin() + static_cast<std::ptrdiff_t>(victim));
        }
    }
    std::printf("accumulator seed=%llu frames=%llu multi_frame_closes=%llu\n",
                static_cast<unsigned long long>(seed), static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(multiFrame));
}

// Submissions the plan builder can build (as in the deferred-plan soak).
struct PlanFrontend {
    Oot3dPicaDrawPacket Live;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Vertices;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Textures;
    uint64_t NextId = 1U;

    PlanFrontend() {
        Live.Indexed = true;
        Live.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
        Live.VertexShader.Program[0] = 0x88000000U;
        Live.VertexShader.ProgramWordCount = 1U;
        Live.VertexShader.SwizzleWordCount = 1U;
        Live.Registers[0x04F] = 1U;
        Live.Registers[0x050] = 0x03020100U;
        Live.Registers[0x2BD] = 1U;
        for (uint32_t index = 0; index < 8U; ++index) {
            Vertices.push_back(std::make_shared<const std::vector<uint8_t>>(
                480U, static_cast<uint8_t>(index)));
            Textures.push_back(std::make_shared<const std::vector<uint8_t>>(
                4096U, static_cast<uint8_t>(0x40U + index)));
        }
    }

    Oot3dPicaDrawSubmission Submit(size_t draw, std::mt19937_64& random) {
        Live.VertexShader.FloatUniforms[3] = {static_cast<float>(draw), 0.5F, 0.25F, 1.0F};
        Live.Registers[0x0C3U + 1U] = static_cast<uint32_t>(random() % 7U);
        Oot3dPicaDrawSubmission submission(NextId++, Live);
        auto& state = submission.State;
        state.Framebuffer.ColorPhysicalAddress = random() % 9U == 8U ? kBottomSource : kTopSource;
        state.Viewport.HalfWidth = 200.0F;
        state.Viewport.HalfHeight = 120.0F;
        state.VertexInput.Indexed = true;
        state.VertexInput.VertexCount = 3U;
        state.VertexInput.AttributeCount = 2U;
        state.VertexInput.Attributes[0] = {Oot3dPicaVertexFormat::Float, 3U, false};
        state.VertexInput.Attributes[1] = {Oot3dPicaVertexFormat::Float, 4U, true};
        state.VertexInput.Loaders[0].ByteStride = 12U;
        state.VertexInput.Loaders[0].ComponentCount = 1U;
        state.VertexInput.Loaders[0].Components[0] = 0U;
        state.ShaderInterface.OutputMask = 1U;
        state.ShaderInterface.InputRegisterByAttribute[1] = 2U;
        submission.MinimumVertexIndex = 0U;
        submission.MaximumVertexIndex = 3U;
        Oot3dPicaResourceSnapshot indices;
        indices.Kind = Oot3dPicaResourceKind::IndexBuffer;
        indices.Bytes = {0U, 1U, 2U};
        indices.ContentVersion = 5U;
        indices.ContentVersionAvailable = true;
        submission.Resources.push_back(std::move(indices));
        Oot3dPicaResourceSnapshot vertices;
        vertices.Kind = Oot3dPicaResourceKind::VertexLoader;
        vertices.SharedBytes = Vertices[draw % Vertices.size()];
        vertices.ContentVersion = 100U + draw % Vertices.size();
        vertices.ContentVersionAvailable = true;
        submission.Resources.push_back(std::move(vertices));
        if (random() % 2U == 0U) {
            state.Textures[0].Enabled = true;
            state.Textures[0].Width = 32U;
            state.Textures[0].Height = 32U;
            state.Textures[0].PhysicalAddress = 0x18000000U + static_cast<uint32_t>(draw % 8U) * 0x1000U;
            Oot3dPicaResourceSnapshot texture;
            texture.Kind = Oot3dPicaResourceKind::Texture;
            texture.SharedBytes = Textures[draw % Textures.size()];
            texture.ContentHash = 0x1234U + draw % Textures.size();
            texture.ContentHashAvailable = true;
            texture.BaseLevelContentHash = texture.ContentHash;
            texture.BaseLevelContentHashAvailable = true;
            submission.Resources.push_back(std::move(texture));
        }
        return submission;
    }
};

// The guest thread's plan path of RunGuestRefresh in both modes (placeholder
// moved through Capture, closed plan by plan; or written in place, closed
// by handing the storage over), then resolution as on the render thread.
// Oot3dPicaPresentationScheduler only forwards Capture/CaptureInPlace/
// FinishFrames to its accumulator (and counts); the accumulator stands in.
struct SchedulerStandIn {
    Oot3dPicaVisualFrameAccumulator Accumulator;
    uint64_t ProducedGuestDraws = 0U;
    void Capture(Oot3dPicaVulkanDrawPlan plan) {
        ++ProducedGuestDraws;
        Accumulator.Append(std::move(plan));
    }
    Oot3dPicaVulkanDrawPlan& CaptureInPlace() {
        ++ProducedGuestDraws;
        return Accumulator.EmplaceDraw();
    }
    void Capture(Oot3dPicaDisplayTransferSubmission transfer) { Accumulator.Append(std::move(transfer)); }
    std::vector<Oot3dPicaVisualFrame> FinishFrames(const Oot3dPicaDisplayTransferSubmission& top,
                                                   std::span<const uint32_t> outputs) {
        return Accumulator.FinishFrames(top, outputs);
    }
    std::span<const Oot3dPicaVulkanDrawPlan> PendingDraws() const { return Accumulator.PendingDraws(); }
    bool ResolveDeferredDraws(uint64_t through,
                              const std::function<bool(Oot3dPicaVulkanDrawPlan&)>& resolve) {
        return Accumulator.ResolveDeferredDraws(through, resolve);
    }
};

struct PlanPath {
    Oot3dPicaVulkanShaderSourceCache Cache;
    Oot3dPicaDeferredPlanBuilder Builder{Cache};
    SchedulerStandIn Scheduler;
    std::vector<Oot3dPicaDrawSubmission> Drained;
    bool InPlace = false;

    // worker: the plan worker builds published batches while the guest
    // side fills the next placeholders (TSan sees both threads).
    explicit PlanPath(bool inPlace, bool worker = false) : InPlace(inPlace) {
        Builder.Configure(true, worker);
        Scheduler.Accumulator.SetMoveFreeFinish(inPlace);
    }

    void Drain(std::vector<Oot3dPicaDrawSubmission> submissions, const std::vector<bool>& suppressed) {
        Drained = std::move(submissions);
        auto batch = Builder.BeginBatch(Drained);
        for (size_t index = 0; index < batch->Submissions.size(); ++index) {
            if (InPlace) {
                if (!suppressed[index])
                    Builder.FillDeferredPlan(batch, index, Scheduler.CaptureInPlace());
            } else {
                Oot3dPicaVulkanDrawPlan plan;
                if (!suppressed[index]) plan = Builder.MakeDeferredPlan(batch, index);
                if (!suppressed[index]) Scheduler.Capture(std::move(plan));
            }
        }
        Builder.Publish(std::move(batch));
    }
};

void TestPlanPaths(uint64_t seed, size_t frames) {
    std::mt19937_64 random(seed);
    const bool worker = seed % 2U == 1U;
    PlanPath before(false, worker);
    PlanPath after(true, worker);
    PlanFrontend frontend;
    std::string error;
    uint64_t completion = 1U;
    uint64_t resolved = 0U;
    std::vector<Oot3dPicaVisualFrame> historyBefore;
    std::vector<Oot3dPicaVisualFrame> historyAfter;
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t passes = 1U + random() % 3U;
        for (size_t pass = 0; pass < passes; ++pass) {
            const size_t count = random() % 70U;
            std::vector<Oot3dPicaDrawSubmission> submissions;
            std::vector<bool> suppressed;
            for (size_t draw = 0; draw < count; ++draw) {
                submissions.push_back(frontend.Submit(draw, random));
                suppressed.push_back(random() % 23U == 0U);
            }
            auto copy = submissions;
            before.Drain(std::move(submissions), suppressed);
            after.Drain(std::move(copy), suppressed);
            // A dependency flush resolves a prefix in place (guest stopped).
            if (random() % 5U == 0U) {
                const auto pendingA = before.Scheduler.PendingDraws();
                const uint64_t through = pendingA.empty() ? 0U : pendingA[pendingA.size() / 2U].SubmissionId;
                const auto resolve = [&](Oot3dPicaVulkanDrawPlan& plan) {
                    return ResolveOot3dPicaDeferredDrawPlan(plan, &error);
                };
                Require(before.Scheduler.ResolveDeferredDraws(through, resolve), error);
                Require(after.Scheduler.ResolveDeferredDraws(through, resolve), error);
            }
        }
        Oot3dPicaDisplayTransferSubmission top;
        top.CompletionId = completion++;
        top.AfterDrawSubmissionId = frontend.NextId - 1U;
        top.InputPhysicalAddress = kTopSource;
        top.OutputPhysicalAddress = kDisplayOutputs[0];
        before.Scheduler.Capture(top);
        after.Scheduler.Capture(top);
        const std::array<uint32_t, 1> outputs{kDisplayOutputs[0]};
        auto a = before.Scheduler.FinishFrames(top, outputs);
        auto b = after.Scheduler.FinishFrames(top, outputs);
        Require(a.size() == b.size(), "plan paths closed different frame counts");
        for (size_t index = 0; index < a.size(); ++index) {
            Require(SameFrame(a[index], b[index]), "in-place placeholders changed a closed frame");
            Require(ResolveOot3dPicaDeferredVisualFrame(a[index], &error), error);
            Require(ResolveOot3dPicaDeferredVisualFrame(b[index], &error), error);
            Require(SameFrame(a[index], b[index]), "in-place placeholders changed a resolved frame");
            for (const auto& plan : b[index].Draws)
                Require(plan.Deferred == nullptr && !plan.VertexBindings.empty(),
                        "an in-place placeholder did not resolve");
            resolved += b[index].Draws.size();
            historyBefore.push_back(std::move(a[index]));
            historyAfter.push_back(std::move(b[index]));
        }
        if (historyBefore.size() > 2U) {
            historyBefore.erase(historyBefore.begin());
            historyAfter.erase(historyAfter.begin());
        }
        Require(before.Scheduler.ProducedGuestDraws == after.Scheduler.ProducedGuestDraws,
                "produced draw counts differ");
    }
    before.Builder.StopWorker();
    after.Builder.StopWorker();
    std::printf("plan seed=%llu worker=%d frames=%zu resolved_draws=%llu\n",
                static_cast<unsigned long long>(seed), worker ? 1 : 0, frames,
                static_cast<unsigned long long>(resolved));
}

// ------------------------------------------------------------------- perf

void TestLineWriter() {
    const auto path = (std::filesystem::temp_directory_path() /
                       ("oot3d_offload_writer_" + std::to_string(::getpid()) + ".txt"))
                          .string();
    std::filesystem::remove(path);
    std::string expected;
    {
        AsyncLineWriter writer(path, 1U << 20U);
        for (int line = 0; line < 20000; ++line) {
            std::string text = "host_frames=" + std::to_string(line * 150) + " fps=59.9 x=" +
                               std::string(static_cast<size_t>(line % 300), 'a') + '\n';
            expected += text;
            Require(writer.Push(std::move(text)), "a line was dropped below the limit");
            if (line % 1000 == 0) std::this_thread::yield();
        }
        writer.Stop();
        Require(writer.Written() == 20000U && writer.Dropped() == 0U, "writer lost lines");
    }
    std::ifstream input(path, std::ios::binary);
    std::stringstream content;
    content << input.rdbuf();
    Require(content.str() == expected, "the written file is not the lines in order");
    std::filesystem::remove(path);
    // A full queue drops (never waits) and keeps the order of what it took.
    {
        AsyncLineWriter writer(path, 2U);
        uint64_t taken = 0U;
        for (int line = 0; line < 5000; ++line) taken += writer.Push(std::to_string(line) + '\n') ? 1U : 0U;
        writer.Stop();
        Require(writer.Written() == taken && writer.Dropped() == 5000U - taken,
                "dropped and written lines do not add up");
    }
    std::ifstream again(path);
    long previous = -1;
    for (long value; again >> value;) {
        Require(value > previous, "the writer reordered lines");
        previous = value;
    }
    std::filesystem::remove(path);
    std::printf("line writer: ok\n");
}

void TestParser() {
    Require(ParseOffloadSwitch(""), "an empty file enables");
    Require(ParseOffloadSwitch("1\n"), "1 enables");
    Require(!ParseOffloadSwitch("0"), "0 disables");
    Require(!ParseOffloadSwitch(" 0 \r\n"), "0 with spaces disables");
    Require(!ParseOffloadSwitch("off"), "off disables");
    Require(ParseOffloadSwitch("on"), "on enables");
    Require(ParseOffloadSwitch("10"), "10 is not 0");
}

// ------------------------------------------------------------------ bench

double MicrosecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

void BenchPlan(size_t drawsPerFrame, size_t frames) {
    for (const bool inPlace : {false, true}) {
        PlanPath path(inPlace);
        PlanFrontend frontend;
        std::mt19937_64 random(3U);
        double placeholderUs = 0.0;
        double finishUs = 0.0;
        double timedSectionUs = 0.0;  // what pica_plan measures (old path)
        uint64_t completion = 1U;
        for (size_t frame = 0; frame < frames; ++frame) {
            std::vector<Oot3dPicaDrawSubmission> submissions;
            for (size_t draw = 0; draw < drawsPerFrame; ++draw) submissions.push_back(frontend.Submit(draw, random));
            path.Drained = std::move(submissions);
            auto batch = path.Builder.BeginBatch(path.Drained);
            const auto started = Clock::now();
            for (size_t index = 0; index < batch->Submissions.size(); ++index) {
                if (inPlace) {
                    path.Builder.FillDeferredPlan(batch, index, path.Scheduler.CaptureInPlace());
                } else {
                    Oot3dPicaVulkanDrawPlan plan;
                    const auto section = Clock::now();
                    plan = path.Builder.MakeDeferredPlan(batch, index);
                    timedSectionUs += MicrosecondsSince(section);
                    path.Scheduler.Capture(std::move(plan));
                }
            }
            placeholderUs += MicrosecondsSince(started);
            path.Builder.Publish(std::move(batch));
            Oot3dPicaDisplayTransferSubmission top;
            top.CompletionId = completion++;
            top.AfterDrawSubmissionId = frontend.NextId - 1U;
            top.InputPhysicalAddress = kTopSource;
            top.OutputPhysicalAddress = kDisplayOutputs[0];
            path.Scheduler.Capture(top);
            const std::array<uint32_t, 1> outputs{kDisplayOutputs[0]};
            const auto finishStart = Clock::now();
            auto closed = path.Scheduler.FinishFrames(top, outputs);
            finishUs += MicrosecondsSince(finishStart);
            // Frames die elsewhere (render thread): not timed.
        }
        const double perDraw = 1.0 / static_cast<double>(frames * drawsPerFrame);
        std::printf("bench plan %-9s placeholder+capture %.3f us/draw  finish %.3f us/draw  total %.3f us/draw"
                    "  (old pica_plan section %.3f us/draw)\n",
                    inPlace ? "in-place" : "moved", placeholderUs * perDraw, finishUs * perDraw,
                    (placeholderUs + finishUs) * perDraw, timedSectionUs * perDraw);
    }
}

void BenchCapture(size_t draws) {
    for (const bool lean : {false, true}) {
        std::mt19937_64 random(11U);
        CaptureRun run;
        run.Lean = lean;
        FillMemory(run.Memory, random);
        run.Make();
        std::string error;
        double us = 0.0;
        std::mt19937_64 packets(5U);
        for (size_t draw = 0; draw < draws; ++draw) {
            if (draw % 16U == 0U) Mutate(run.Memory, random);
            const auto packet = MakePacket(packets);
            const auto started = Clock::now();
            (void)run.Submit(packet, &error);
            us += MicrosecondsSince(started);
            if (draw % 64U == 63U) (void)run.Queue->TakePendingDraws();
        }
        const auto counters = run.Queue->Counters();
        std::printf("bench capture %-4s %.3f us/draw (hashed %.1f KiB/draw, copied %.1f KiB/draw)\n",
                    lean ? "lean" : "old", us / draws, counters.HashedBytes / 1024.0 / draws,
                    counters.CopiedBytes / 1024.0 / draws);
    }
}

} // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::string_view(argv[1]) == "--bench";
    if (bench) {
        BenchPlan(200U, 300U);
        BenchCapture(60000U);
        return 0;
    }
    TestParser();
    TestPacketSource();
    for (uint64_t seed = 1U; seed <= 3U; ++seed) TestFrontendSource(seed);
    for (uint64_t seed = 1U; seed <= 4U; ++seed) TestCapture(seed, seed % 2U == 0U, 600U);
    for (uint64_t seed = 1U; seed <= 4U; ++seed) TestAccumulator(seed, 1500U);
    for (uint64_t seed = 1U; seed <= 3U; ++seed) TestPlanPaths(seed, 200U);
    TestLineWriter();
    std::cout << "oot3d_native_offload_tests: ok\n";
    return 0;
}
