// config/list_walk_fast (SWITCH_PORT.md "Command-list walk"): the fast GSP
// command-list walk against the per-word walk, on the same command streams.
//  - Every register id (0..0x2FF and past the register file) written alone,
//    repeated in consecutive mode and in grouped runs, with every byte mask.
//  - Seeded streams: game-shaped draws (material registers, TEV and light
//    runs, texture units, float32/float24 uniform bursts, skinning matrices,
//    attribute loaders, draws) mixed with random commands over the whole
//    register space (LUT, fog and procedural table data, default
//    attributes, shader programs, swizzles, boolean/integer/float uniforms,
//    geometry-shader mirroring, draws in bursts, P3D interrupt requests that
//    stop the list, ids that wrap past 0xFFFF), random byte masks, padding,
//    truncated lists, draws the sink refuses, composition and pass spans,
//    list domains and late lists; capture delta and in-place packets on/off.
//  - Compared after every list: success and error text, every packet the
//    sink received (register file, both shader states with their float
//    uniforms bit for bit, tables, lighting LUT content and hash, default
//    attributes, composition, light binding) with the dirty lines handed
//    along, interrupts sent to the sink and kept pending, the whole frontend
//    savestate (registers, shader state, tables, queues), the composition
//    statistics and notes, and the table/LUT counters.
// --bench [frames]: walk ns per word (sink time excluded), per-word walk vs
// fast walk, on game-shaped and single-write/uniform-heavy lists.
#include "oot3d_native_pica_frontend.h"
#include "oot3d_native_capture_clock.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_pica_list_walk_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// ------------------------------------------------------------------ sinks

struct SinkEvent {
    bool Interrupt = false;
    Oot3dPicaDrawPacket Packet;
    bool HasDirtyLines = false;
    Oot3dPicaStateDirtyLines DirtyLines;
};

// Keeps every draw as a packet and consumes the dirty lines as the capture
// delta queue does; refuses the draw numbered FailAt.
class RecordingSink final : public Oot3dPicaPacketSink {
  public:
    std::vector<SinkEvent> Events;
    uint64_t Draws = 0;
    uint64_t FailAt = std::numeric_limits<uint64_t>::max();

    bool SubmitHardwareRegisterWrite(const Oot3dPicaHardwareRegisterWrite&, std::string*) override {
        return true;
    }
    bool SubmitDrawPacket(const Oot3dPicaDrawPacket& packet, std::string* error) override {
        return Record(packet, nullptr, error);
    }
    bool SubmitDrawPacketSource(const Oot3dPicaDrawPacketSource& source, std::string* error) override {
        return Record(Oot3dPicaDrawPacket(source), source.DirtyLines, error);
    }
    bool SubmitInterruptAfterGpuWork(Oot3dPicaInterruptId, std::string*) override {
        SinkEvent event;
        event.Interrupt = true;
        Events.push_back(std::move(event));
        return true;
    }

  private:
    bool Record(const Oot3dPicaDrawPacket& packet, Oot3dPicaStateDirtyLines* dirty, std::string* error) {
        if (Draws++ == FailAt) {
            if (error != nullptr) *error = "the sink refused the draw";
            return false;
        }
        SinkEvent event;
        event.Packet = packet;
        if (dirty != nullptr) {
            event.HasDirtyLines = true;
            event.DirtyLines = *dirty;
            dirty->Clear();
        }
        Events.push_back(std::move(event));
        return true;
    }
};

// The bench: counts draws and consumes the dirty lines (no packet).
class CountingSink final : public Oot3dPicaPacketSink {
  public:
    uint64_t Draws = 0;
    bool SubmitHardwareRegisterWrite(const Oot3dPicaHardwareRegisterWrite&, std::string*) override {
        return true;
    }
    bool SubmitDrawPacket(const Oot3dPicaDrawPacket&, std::string*) override {
        ++Draws;
        return true;
    }
    bool SubmitDrawPacketSource(const Oot3dPicaDrawPacketSource& source, std::string*) override {
        ++Draws;
        if (source.DirtyLines != nullptr) source.DirtyLines->Clear();
        return true;
    }
};

// ------------------------------------------------------------- comparison

bool SameShader(const Oot3dPicaShaderState& a, const Oot3dPicaShaderState& b) {
    return a.Program.Values() == b.Program.Values() && a.Swizzles.Values() == b.Swizzles.Values() &&
           std::memcmp(a.FloatUniforms.data(), b.FloatUniforms.data(), sizeof(a.FloatUniforms)) == 0 &&
           a.IntegerUniforms == b.IntegerUniforms && a.BooleanUniforms == b.BooleanUniforms &&
           a.ProgramWordCount == b.ProgramWordCount && a.SwizzleWordCount == b.SwizzleWordCount;
}

std::string PacketDifference(const Oot3dPicaDrawPacket& p, const Oot3dPicaDrawPacket& q) {
    if (p.CommandListAddress != q.CommandListAddress || p.CommandListOffsetWords != q.CommandListOffsetWords)
        return "list position";
    if (p.CompositionDomain != q.CompositionDomain || !(p.Composition == q.Composition)) return "composition";
    if (!(p.LightBinding == q.LightBinding)) return "light binding";
    if (p.Indexed != q.Indexed) return "indexed";
    if (p.Registers != q.Registers) return "register file";
    if (!SameShader(p.VertexShader, q.VertexShader)) return "vertex shader state";
    if (!SameShader(p.GeometryShader, q.GeometryShader)) return "geometry shader state";
    if (p.FogLut.Values() != q.FogLut.Values() || p.ProcTexLuts.Noise.Values() != q.ProcTexLuts.Noise.Values() ||
        p.ProcTexLuts.ColorMap.Values() != q.ProcTexLuts.ColorMap.Values() ||
        p.ProcTexLuts.AlphaMap.Values() != q.ProcTexLuts.AlphaMap.Values() ||
        p.ProcTexLuts.Color.Values() != q.ProcTexLuts.Color.Values() ||
        p.ProcTexLuts.ColorDifference.Values() != q.ProcTexLuts.ColorDifference.Values())
        return "fog or procedural tables";
    if ((p.LightingLuts == nullptr) != (q.LightingLuts == nullptr) ||
        (p.LightingLuts != nullptr && (p.LightingLuts->PackedEntries != q.LightingLuts->PackedEntries ||
                                       p.LightingLuts->ContentHash != q.LightingLuts->ContentHash ||
                                       p.LightingLuts->ContentHashAvailable != q.LightingLuts->ContentHashAvailable)))
        return "lighting LUT";
    if (std::memcmp(p.DefaultAttributes.data(), q.DefaultAttributes.data(), sizeof(p.DefaultAttributes)) != 0)
        return "default attributes";
    return {};
}

bool SameDirtyLines(const Oot3dPicaStateDirtyLines& a, const Oot3dPicaStateDirtyLines& b) {
    return a.Registers == b.Registers && a.VertexUniforms == b.VertexUniforms &&
           a.GeometryUniforms == b.GeometryUniforms && a.DefaultAttributes == b.DefaultAttributes;
}

struct Options {
    bool CaptureDelta = true;
    bool InPlace = true;
    bool PassSpanFallback = true;
    bool LateListsOnTime = true;
};

// The same frontend twice, per-word walk and fast walk.
struct Pair {
    RecordingSink LegacySink;
    RecordingSink FastSink;
    Oot3dNativePicaFrontend Legacy{&LegacySink};
    Oot3dNativePicaFrontend Fast{&FastSink};
    uint64_t Lists = 0;

    explicit Pair(const Options& options) {
        for (Oot3dNativePicaFrontend* frontend : {&Legacy, &Fast}) {
            frontend->SetDiagnosticHistoryEnabled(false);
            frontend->SetInPlaceDrawPackets(options.InPlace);
            frontend->SetCaptureDelta(options.CaptureDelta);
            frontend->SetPassSpanFallback(options.PassSpanFallback);
            frontend->SetLateCommandListsOnTime(options.LateListsOnTime);
        }
        Fast.SetListWalkFast(true);
    }
    Pair(const Pair&) = delete;
    Pair& operator=(const Pair&) = delete;

    struct Composition {
        std::vector<Oot3dPicaCommandListCompositionSpan> Spans;
        std::vector<Oot3dPicaCommandListCompositionSpan> Passes;
        bool HasSpans = false;
        bool HasPasses = false;
        Oot3dPicaCompositionDomain Domain = Oot3dPicaCompositionDomain::Unknown;
        Oot3dPicaCompositionDomain OnTime = Oot3dPicaCompositionDomain::Unknown;
        uint32_t Generation = 0;
    };

    void Submit(uint32_t address, std::span<const uint32_t> words, const Composition& composition,
                uint64_t failAfterDraws, std::string_view what) {
        Oot3dGspCommandPacket command{};
        command.Control = 1U;
        command.Parameters[0] = address;
        command.Parameters[1] = static_cast<uint32_t>(words.size() * 4U);
        bool spansAccepted[2]{};
        std::string spanErrors[2];
        for (int side = 0; side < 2; ++side) {
            Oot3dNativePicaFrontend& frontend = side == 0 ? Legacy : Fast;
            RecordingSink& sink = side == 0 ? LegacySink : FastSink;
            frontend.SetCommandListCompositionDomain(composition.Domain);
            frontend.SetCommandListTiming(composition.OnTime, composition.Generation);
            if (composition.HasSpans)
                spansAccepted[side] = frontend.SetNextCommandListCompositionSpans(
                    address, command.Parameters[1], composition.Spans, &spanErrors[side]);
            if (composition.HasPasses)
                frontend.SetNextCommandListPassSpans(address, command.Parameters[1], composition.Passes);
            sink.FailAt = failAfterDraws == std::numeric_limits<uint64_t>::max()
                              ? failAfterDraws
                              : sink.Draws + failAfterDraws;
        }
        Require(spansAccepted[0] == spansAccepted[1] && spanErrors[0] == spanErrors[1],
                std::string(what) + ": composition spans accepted differently");
        std::string legacyError;
        std::string fastError;
        const bool legacyOk = Legacy.SubmitGspCommand(command, words, &legacyError);
        const bool fastOk = Fast.SubmitGspCommand(command, words, &fastError);
        ++Lists;
        const std::string where = std::string(what) + " (list " + std::to_string(Lists) + ")";
        Require(legacyOk == fastOk && legacyError == fastError,
                where + ": the list succeeded or failed differently ('" + legacyError + "' / '" + fastError + "')");
        Compare(where);
    }

    void Compare(const std::string& where) {
        auto& a = LegacySink.Events;
        auto& b = FastSink.Events;
        Require(a.size() == b.size(), where + ": sink event counts differ (" + std::to_string(a.size()) + " / " +
                                          std::to_string(b.size()) + ")");
        for (size_t index = 0; index < a.size(); ++index) {
            const std::string event = where + ", sink event " + std::to_string(index);
            Require(a[index].Interrupt == b[index].Interrupt, event + ": kind differs");
            if (a[index].Interrupt) continue;
            const std::string difference = PacketDifference(a[index].Packet, b[index].Packet);
            Require(difference.empty(), event + ": packet differs in " + difference);
            Require(a[index].HasDirtyLines == b[index].HasDirtyLines &&
                        (!a[index].HasDirtyLines || SameDirtyLines(a[index].DirtyLines, b[index].DirtyLines)),
                    event + ": dirty lines differ");
        }
        a.clear();
        b.clear();
        Require(LegacySink.Draws == FastSink.Draws, where + ": draw counts differ");
        Require(Legacy.CaptureState().dump() == Fast.CaptureState().dump(), where + ": frontend savestate differs");
        Require(Legacy.TakePendingInterrupts() == Fast.TakePendingInterrupts(), where + ": pending interrupts differ");
        const auto& r = Legacy.CompositionResolveStats();
        const auto& s = Fast.CompositionResolveStats();
        Require(r.LateLists == s.LateLists && r.LateListDraws == s.LateListDraws &&
                    r.UiListWorldSpanDraws == s.UiListWorldSpanDraws && r.NoSpanDraws == s.NoSpanDraws &&
                    r.SameOwnerHoleDraws == s.SameOwnerHoleDraws && r.PassSpanDraws == s.PassSpanDraws &&
                    r.BareListDraws == s.BareListDraws,
                where + ": composition statistics differ");
        const auto legacyNotes = Legacy.TakeCompositionNotes();
        const auto fastNotes = Fast.TakeCompositionNotes();
        Require(legacyNotes.size() == fastNotes.size(), where + ": composition note counts differ");
        for (size_t index = 0; index < legacyNotes.size(); ++index)
            Require(FormatOot3dPicaCompositionNote(legacyNotes[index]) ==
                        FormatOot3dPicaCompositionNote(fastNotes[index]),
                    where + ": composition notes differ");
        const auto& x = Legacy.CaptureStats();
        const auto& y = Fast.CaptureStats();
        Require(x.Lists == y.Lists && x.ListWords == y.ListWords && x.LutHashes == y.LutHashes &&
                    x.LutCopies == y.LutCopies && x.LutReuses == y.LutReuses && x.TableCopies == y.TableCopies &&
                    x.SameValueWrites == y.SameValueWrites,
                where + ": table or LUT counters differ");
        Require(x.BatchedWords == 0U, where + ": the per-word walk batched words");
    }
};

// ------------------------------------------------------------- the streams

constexpr uint16_t kVertexShaderBase = 0x2B0U;
constexpr uint16_t kGeometryShaderBase = 0x280U;

// Builds command lists. Every float uniform setup write goes through the
// stage tracker: a setup that would switch a stage to float24 while a whole
// float32 vector's worth (3 words) is queued is made float32 instead (the
// frontend's queue holds 4 words; both walks would overrun it the same way,
// which no test can compare).
class Generator {
  public:
    explicit Generator(uint64_t seed) : mRandom(seed) {}

    std::mt19937_64& Random() noexcept { return mRandom; }
    uint32_t Next() { return static_cast<uint32_t>(mRandom()); }
    uint32_t Below(uint32_t bound) { return static_cast<uint32_t>(mRandom() % bound); }

    // The stages' uniform modes and queued words as the frontend has them.
    void Sync(const nlohmann::json& state) {
        const auto& registers = state.at("pica_registers");
        mStages[0].Float32 = (registers.at(kVertexShaderBase + 0x10U).get<uint32_t>() >> 31U) != 0U;
        mStages[0].Pending = state.at("vertex_uniform_queue").at("count").get<size_t>();
        mStages[1].Float32 = (registers.at(kGeometryShaderBase + 0x10U).get<uint32_t>() >> 31U) != 0U;
        mStages[1].Pending = state.at("geometry_uniform_queue").at("count").get<size_t>();
    }

    // One command: values[0] with the header, the rest as extra words (at
    // most 256 values), padded to an even word count.
    void Emit(std::vector<uint32_t>& words, uint16_t id, std::vector<uint32_t> values, uint32_t mask,
              bool grouped) {
        for (size_t index = 0; index < values.size(); ++index) {
            const uint16_t target = static_cast<uint16_t>(id + (grouped ? index : 0U));
            for (size_t stage = 0; stage < 2U; ++stage) {
                auto& tracked = mStages[stage];
                const uint16_t base = stage == 0U ? kVertexShaderBase : kGeometryShaderBase;
                if (target == base + 0x10U) {
                    if ((mask & 8U) != 0U) {
                        if ((values[index] >> 31U) == 0U && tracked.Pending == 3U) values[index] |= 0x80000000U;
                        tracked.Float32 = (values[index] >> 31U) != 0U;
                    }
                } else if (target >= base + 0x11U && target <= base + 0x18U) {
                    if (++tracked.Pending == (tracked.Float32 ? 4U : 3U)) tracked.Pending = 0U;
                }
            }
        }
        words.push_back(values[0]);
        words.push_back(static_cast<uint32_t>(id) | (mask << 16U) |
                        (static_cast<uint32_t>(values.size() - 1U) << 20U) | (grouped ? 0x80000000U : 0U));
        words.insert(words.end(), values.begin() + 1, values.end());
        if ((words.size() & 1U) != 0U) words.push_back(Next());  // padding, never read
    }
    void Put(std::vector<uint32_t>& words, uint16_t id, uint32_t value, uint32_t mask = 0xFU) {
        Emit(words, id, {value}, mask, false);
    }

    // A finite float24 (no NaN: exponent away from 0x7F).
    uint32_t Float24() {
        return (Below(2U) << 23U) | ((0x30U + Below(0x20U)) << 16U) | (Next() & 0xFFFFU);
    }
    std::array<uint32_t, 3> PackedFloat24() {
        const uint32_t v0 = Float24(), v1 = Float24(), v2 = Float24(), v3 = Float24();
        return {(v3 << 8U) | (v2 >> 16U), ((v2 & 0xFFFFU) << 16U) | (v1 >> 8U), ((v1 & 0xFFU) << 24U) | v0};
    }
    uint32_t Float32Word() { return std::bit_cast<uint32_t>(static_cast<float>(Below(4000U)) / 7.0F - 200.0F); }

    // A uniform upload: setup, then `vectors` vectors in one consecutive
    // command to a data register of the stage.
    void Uniforms(std::vector<uint32_t>& words, uint16_t stageBase, bool float32, uint32_t start, size_t vectors,
                  uint32_t dataMask = 0xFU) {
        Put(words, static_cast<uint16_t>(stageBase + 0x10U), (start & 0x7FU) | (float32 ? 0x80000000U : 0U));
        std::vector<uint32_t> data;
        for (size_t vector = 0; vector < vectors; ++vector) {
            if (float32) {
                for (int word = 0; word < 4; ++word) data.push_back(Float32Word());
            } else {
                const auto packed = PackedFloat24();
                data.insert(data.end(), packed.begin(), packed.end());
            }
        }
        while (!data.empty()) {
            const size_t count = std::min<size_t>(data.size(), 256U);
            Emit(words, static_cast<uint16_t>(stageBase + 0x11U + Below(8U)),
                 std::vector<uint32_t>(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count)), dataMask,
                 false);
            data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    // One draw as the game's renderers write it (bench shape "game").
    void GameDraw(std::vector<uint32_t>& words) {
        static constexpr std::array<uint16_t, 40> kMaterial{
            0x0C0, 0x0C1, 0x0C2, 0x0C3, 0x0C4, 0x0C8, 0x0C9, 0x0CA, 0x0CB, 0x0CC, 0x0E0, 0x0E1, 0x0FD, 0x100,
            0x101, 0x102, 0x103, 0x104, 0x105, 0x106, 0x107, 0x110, 0x111, 0x112, 0x113, 0x114, 0x115, 0x116,
            0x1C0, 0x1C1, 0x1C2, 0x1C3, 0x1C4, 0x1C6, 0x1C7, 0x1D0, 0x1D1, 0x1D2, 0x1D3, 0x1D9};
        for (int write = 0; write < 80; ++write) {
            const uint16_t id = kMaterial[Below(kMaterial.size())];
            Put(words, id, Next(), Below(6U) == 0U ? 1U + Below(15U) : 0xFU);
        }
        // TEV stages.
        for (const uint16_t stage : {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8}) {
            std::vector<uint32_t> values(5U);
            for (auto& value : values) value = Next();
            Emit(words, stage, values, 0xFU, true);
        }
        // Fragment lights.
        for (uint16_t light = 0; light < 1U + Below(2U); ++light) {
            std::vector<uint32_t> values(16U);
            for (auto& value : values) value = Next();
            Emit(words, static_cast<uint16_t>(0x140U + light * 0x10U), values, 0xFU, true);
        }
        // Two texture units.
        for (const uint16_t unit : {0x081, 0x091}) {
            std::vector<uint32_t> values(5U);
            for (auto& value : values) value = Next();
            Emit(words, unit, values, 0xFU, true);
            Put(words, static_cast<uint16_t>(unit + 0x0DU), Below(14U));
        }
        // Model-view, texture matrices, material colours; skinned meshes
        // add their bone matrices.
        Uniforms(words, kVertexShaderBase, true, 0U, 3U);
        Uniforms(words, kVertexShaderBase, true, 8U, 6U);
        Uniforms(words, kVertexShaderBase, Below(2U) != 0U, 20U, 4U);
        if (Below(10U) < 3U) Uniforms(words, kVertexShaderBase, false, 32U, 3U * (4U + Below(17U)));
        if (Below(4U) == 0U) {
            Put(words, 0x2B0, Next() & 0xFFFFU);
            Put(words, static_cast<uint16_t>(0x2B1U + Below(4U)), Next());
        }
        // Attribute loaders.
        std::vector<uint32_t> loaders(40U);
        for (auto& value : loaders) value = Next();
        Emit(words, 0x200, loaders, 0xFU, true);
        Put(words, 0x227, Next());
        Put(words, 0x228, 3U + Below(600U));
        Put(words, 0x25E, Below(3U) << 8U, 0x2U);
        Put(words, 0x22F, 1U);
        Put(words, 0x231, 1U);
        Put(words, 0x111, 1U);
    }

    // Single writes only (bench shape "singles").
    void SingleWritesDraw(std::vector<uint32_t>& words) {
        for (int write = 0; write < 200; ++write) {
            uint16_t id = static_cast<uint16_t>(Below(0x280U));
            if (id == 0x010U || id == 0x22EU || id == 0x22FU || (id >= 0x1C8U && id <= 0x1CFU) ||
                (id >= 0x0B0U && id <= 0x0B7U) || (id >= 0x0E8U && id <= 0x0EFU) || (id >= 0x232U && id <= 0x235U))
                id = 0x0C0U;
            Put(words, id, Next(), Below(6U) == 0U ? 1U + Below(15U) : 0xFU);
        }
        Put(words, 0x22F, 1U);
    }

    // Uniform-heavy (bench shape "uniforms"): skinned meshes.
    void UniformDraw(std::vector<uint32_t>& words) {
        for (int write = 0; write < 20; ++write) Put(words, static_cast<uint16_t>(0x0C0U + Below(0x40U)), Next());
        Uniforms(words, kVertexShaderBase, true, 0U, 3U);
        Uniforms(words, kVertexShaderBase, false, 16U, 60U);
        Put(words, 0x22F, 1U);
    }

    // A random command anywhere in the register space.
    void Noise(std::vector<uint32_t>& words) {
        static constexpr std::array<uint16_t, 22> kInteresting{
            0x1C5, 0x1C8, 0x1CF, 0x0AF, 0x0B0, 0x0B7, 0x0E6, 0x0E8, 0x0EF, 0x232, 0x233,
            0x235, 0x229, 0x244, 0x2C0, 0x2C1, 0x2C8, 0x290, 0x291, 0x2CB, 0x2CC, 0x2D5};
        uint16_t id;
        switch (Below(10U)) {
        case 0: case 1: case 2: id = kInteresting[Below(kInteresting.size())]; break;
        case 3: case 4: id = static_cast<uint16_t>(0x280U + Below(0x60U)); break;
        case 5: id = static_cast<uint16_t>(0x300U + Below(0xFD00U)); break;
        case 6: id = static_cast<uint16_t>(0xFF00U + Below(0x100U)); break;
        default: id = static_cast<uint16_t>(Below(0x300U)); break;
        }
        // Draws, interrupt requests and the auto-stop only now and then.
        if ((id == 0x22EU || id == 0x22FU || id == 0x010U || id == 0x034U || id == 0x020U) && Below(4U) != 0U)
            id = 0x0C0U;
        const uint32_t roll = Below(10U);
        const size_t count = roll < 6U ? 1U : roll < 9U ? 2U + Below(16U) : 17U + Below(240U);
        std::vector<uint32_t> values(count);
        for (auto& value : values) value = Below(3U) == 0U ? Below(8U) : Next();
        if (id == 0x229U || id == 0x244U) values[0] = Below(4U);
        const uint32_t mask = Below(10U) < 7U ? 0xFU : Below(16U);
        bool grouped = Below(2U) != 0U;
        // A grouped run over the interrupt registers (0x010 request, 0x020
        // compare, 0x034 auto-stop) mostly stops the list: only now and then.
        if (grouped && Below(8U) != 0U) {
            for (size_t index = 0; index < count; ++index) {
                const uint16_t target = static_cast<uint16_t>(id + index);
                if (target == 0x010U || target == 0x020U || target == 0x034U) grouped = false;
            }
        }
        Emit(words, id, std::move(values), mask, grouped);
    }

    // Rarely: an interrupt request that matches the compare register's
    // reset value (stops the list when auto-stop is set).
    void Interrupt(std::vector<uint32_t>& words) { Put(words, 0x010, 0x12345678U); }

  private:
    struct Stage {
        bool Float32 = false;
        size_t Pending = 0;
    };
    std::mt19937_64 mRandom;
    std::array<Stage, 2> mStages{};
};

// Random spans over the list: sorted, disjoint, every layer and provenance.
Pair::Composition RandomComposition(Generator& generator, uint32_t address, size_t words) {
    Pair::Composition composition;
    composition.Domain = static_cast<Oot3dPicaCompositionDomain>(generator.Below(3U));
    composition.OnTime = static_cast<Oot3dPicaCompositionDomain>(generator.Below(3U));
    composition.Generation = generator.Below(6U);
    const auto spans = [&](std::vector<Oot3dPicaCommandListCompositionSpan>& out, size_t maxSpans) {
        std::vector<uint32_t> cuts;
        for (size_t cut = generator.Below(static_cast<uint32_t>(maxSpans * 2U)); cut != 0U; --cut)
            cuts.push_back(generator.Below(static_cast<uint32_t>(words + 1U)));
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
        for (size_t index = 0; index + 1U < cuts.size(); index += 2U) {
            Oot3dPicaCommandListCompositionSpan span;
            span.BeginAddress = address + cuts[index] * 4U;
            span.EndAddress = address + cuts[index + 1U] * 4U;
            span.Attribution.Layer = static_cast<Oot3dPicaCompositionLayer>(generator.Below(5U));
            span.Attribution.Provenance = static_cast<Oot3dPicaCompositionProvenance>(generator.Below(4U));
            span.Attribution.SourcePc = generator.Next();
            span.Attribution.NativeActorIdentity = generator.Below(3U);
            if (generator.Below(2U) != 0U) {
                span.Attribution.TemporalIdentity = {1U + generator.Below(3U), 1U + generator.Below(3U),
                                                     generator.Below(3U), generator.Below(100U), generator.Below(3U)};
                span.Attribution.TemporalPacketBeginAddress =
                    generator.Below(3U) == 0U ? 0U : span.BeginAddress + generator.Below(3U) * 4U;
            }
            span.WriteGeneration = generator.Below(6U);
            if (generator.Below(2U) != 0U) {
                std::array<float, 16> view{};
                for (auto& value : view) value = static_cast<float>(generator.Below(1000U));
                span.LightBinding.WorldToView = view;
            }
            out.push_back(span);
        }
        // Unsorted on purpose: the frontend sorts the packet spans.
        std::shuffle(out.begin(), out.end(), generator.Random());
    };
    composition.HasSpans = generator.Below(3U) != 0U;
    if (composition.HasSpans) spans(composition.Spans, 12U);
    composition.HasPasses = generator.Below(3U) == 0U;
    if (composition.HasPasses) {
        spans(composition.Passes, 4U);
        std::sort(composition.Passes.begin(), composition.Passes.end(),
                  [](const auto& a, const auto& b) { return a.BeginAddress < b.BeginAddress; });
    }
    return composition;
}

// ------------------------------------------------------------------ tests

// Every register id alone, repeated and grouped, under every byte mask, on a
// fresh pair each (the stage queues start empty: no setup can overrun them).
void TestEveryRegister() {
    const Options options;
    Generator generator(7U);
    std::vector<uint16_t> ids;
    for (uint32_t id = 0; id < 0x300U; ++id) ids.push_back(static_cast<uint16_t>(id));
    for (const uint16_t id : {0x2FEU, 0x2FFU, 0x300U, 0x301U, 0x3FFU, 0x7FFFU, 0xFFF8U, 0xFFFEU, 0xFFFFU})
        ids.push_back(static_cast<uint16_t>(id));
    uint64_t lists = 0;
    for (const uint16_t id : ids) {
        Pair pair(options);
        for (uint32_t mask = 0; mask < 16U; ++mask) {
            std::vector<uint32_t> words;
            generator.Sync(pair.Legacy.CaptureState());
            generator.Put(words, id, generator.Next(), mask);
            std::vector<uint32_t> values(1U + generator.Below(12U));
            for (auto& value : values) value = generator.Next();
            generator.Emit(words, id, values, mask, false);
            generator.Emit(words, id, values, mask, true);
            generator.Put(words, 0x22F, 1U);  // a probe draw: the whole state into a packet
            pair.Submit(0x18000000U, words, {}, std::numeric_limits<uint64_t>::max(),
                        "register " + std::to_string(id) + " mask " + std::to_string(mask));
            ++lists;
        }
    }
    std::printf("every register: %zu ids x 16 masks, %llu lists identical\n", ids.size(),
                static_cast<unsigned long long>(lists));
}

void TestStreams(uint64_t seed, size_t lists, const Options& options) {
    Generator generator(seed);
    Pair pair(options);
    uint64_t words = 0;
    uint64_t failures = 0;
    for (size_t list = 0; list < lists; ++list) {
        generator.Sync(pair.Legacy.CaptureState());
        std::vector<uint32_t> listWords;
        const size_t draws = 1U + generator.Below(12U);
        // The interrupt compare register back to its reset value mostly.
        if (generator.Below(4U) != 0U) generator.Put(listWords, 0x020, 0x12345678U);
        for (size_t draw = 0; draw < draws; ++draw) {
            for (size_t noise = generator.Below(8U); noise != 0U; --noise) generator.Noise(listWords);
            if (generator.Below(3U) != 0U) {
                generator.GameDraw(listWords);
            } else {
                generator.Put(listWords, generator.Below(2U) != 0U ? 0x22E : 0x22F, 1U);
            }
            if (generator.Below(40U) == 0U) generator.Interrupt(listWords);
        }
        if (generator.Below(30U) == 0U && !listWords.empty())
            listWords.resize(generator.Below(static_cast<uint32_t>(listWords.size())));
        const uint32_t address = 0x18000000U + generator.Below(64U) * 0x10000U;
        const auto composition = RandomComposition(generator, address, listWords.size());
        const uint64_t fail = generator.Below(20U) == 0U ? generator.Below(6U) : std::numeric_limits<uint64_t>::max();
        const uint64_t before = pair.LegacySink.Draws;
        pair.Submit(address, listWords, composition, fail, "seed " + std::to_string(seed));
        failures += fail != std::numeric_limits<uint64_t>::max() && pair.LegacySink.Draws > before + fail ? 1U : 0U;
        words += listWords.size();
    }
    const auto& stats = pair.Fast.CaptureStats();
    std::printf("seed %llu: %zu lists, %llu words (%.0f%% batched), %llu draws, %llu refused draws; delta=%d "
                "in_place=%d pass_fallback=%d late_on_time=%d: identical\n",
                static_cast<unsigned long long>(seed), lists, static_cast<unsigned long long>(words),
                100.0 * static_cast<double>(stats.BatchedWords) / static_cast<double>(words),
                static_cast<unsigned long long>(pair.LegacySink.Draws), static_cast<unsigned long long>(failures),
                options.CaptureDelta ? 1 : 0, options.InPlace ? 1 : 0, options.PassSpanFallback ? 1 : 0,
                options.LateListsOnTime ? 1 : 0);
    Require(stats.BatchedWords != 0U, "the fast walk batched nothing");
    Require(pair.LegacySink.Draws != 0U, "no draws");
}

// Diagnostic history keeps the per-word walk (it records every write).
void TestDiagnosticHistory() {
    Generator generator(3U);
    RecordingSink legacySink;
    RecordingSink fastSink;
    Oot3dNativePicaFrontend legacy(&legacySink);
    Oot3dNativePicaFrontend fast(&fastSink);
    fast.SetListWalkFast(true);
    std::vector<uint32_t> words;
    generator.GameDraw(words);
    Oot3dGspCommandPacket command{};
    command.Control = 1U;
    command.Parameters[0] = 0x18000000U;
    command.Parameters[1] = static_cast<uint32_t>(words.size() * 4U);
    std::string error;
    Require(legacy.SubmitGspCommand(command, words, &error) && fast.SubmitGspCommand(command, words, &error), error);
    Require(fast.CaptureStats().BatchedWords == 0U &&
                fast.PendingRegisterWrites().size() == legacy.PendingRegisterWrites().size() &&
                fast.PendingDrawPackets().size() == 1U && legacy.CaptureState().dump() == fast.CaptureState().dump(),
            "diagnostic history did not keep the per-word walk");
    std::printf("diagnostic history: per-word walk kept (%zu writes recorded)\n", fast.PendingRegisterWrites().size());
}

// ------------------------------------------------------------------ bench

void Bench(const char* shape, void (Generator::*draw)(std::vector<uint32_t>&), size_t frames) {
    Generator generator(99U);
    // A Kokiri-like frame: 4 lists of 25 draws.
    std::vector<std::vector<uint32_t>> lists(4U);
    uint64_t frameWords = 0;
    for (auto& list : lists) {
        for (int index = 0; index < 25; ++index) (generator.*draw)(list);
        frameWords += list.size();
    }
    double nsPerWord[2]{};
    double batched = 0.0;
    for (int side = 0; side < 2; ++side) {
        CountingSink sink;
        Oot3dNativePicaFrontend frontend(&sink);
        frontend.SetDiagnosticHistoryEnabled(false);
        frontend.SetInPlaceDrawPackets(true);
        frontend.SetCaptureDelta(true);
        frontend.SetListWalkFast(side == 1);
        Oot3dPicaFrontendCaptureStats before{};
        std::string error;
        for (size_t frame = 0; frame < frames + 20U; ++frame) {
            if (frame == 20U) before = frontend.CaptureStats();
            for (size_t list = 0; list < lists.size(); ++list) {
                Oot3dGspCommandPacket command{};
                command.Control = 1U;
                command.Parameters[0] = 0x18000000U + static_cast<uint32_t>(list) * 0x40000U;
                command.Parameters[1] = static_cast<uint32_t>(lists[list].size() * 4U);
                Require(frontend.SubmitGspCommand(command, lists[list], &error), error);
            }
        }
        const auto& after = frontend.CaptureStats();
        const double walk = CaptureTicksToNanoseconds((after.ListTicks - after.SinkTicks) -
                                                      (before.ListTicks - before.SinkTicks));
        nsPerWord[side] = walk / static_cast<double>(after.ListWords - before.ListWords);
        if (side == 1)
            batched = static_cast<double>(after.BatchedWords - before.BatchedWords) /
                      static_cast<double>(after.ListWords - before.ListWords);
    }
    std::printf("bench %-8s %4.0f words/draw: walk %.2f -> %.2f ns/word (%.2fx, -%.0f%%), %.0f%% of words batched; "
                "%.2f -> %.2f us/draw\n",
                shape, static_cast<double>(frameWords) / 100.0, nsPerWord[0], nsPerWord[1],
                nsPerWord[0] / nsPerWord[1], 100.0 * (1.0 - nsPerWord[1] / nsPerWord[0]), 100.0 * batched,
                nsPerWord[0] * static_cast<double>(frameWords) / 100.0 / 1.0e3,
                nsPerWord[1] * static_cast<double>(frameWords) / 100.0 / 1.0e3);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        const size_t frames = argc > 2 ? static_cast<size_t>(std::strtoul(argv[2], nullptr, 10)) : 2000U;
        for (int round = 0; round < 3; ++round) {
            Bench("game", &Generator::GameDraw, frames);
            Bench("singles", &Generator::SingleWritesDraw, frames);
            Bench("uniforms", &Generator::UniformDraw, frames);
        }
        return 0;
    }
    // run.sh runs 100 lists per combination (a Debug run of 400 takes about
    // 9 min, near run.sh's 10-minute limit); pass 400 for the full campaign.
    const size_t lists = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 100U;
    TestEveryRegister();
    TestDiagnosticHistory();
    for (uint64_t combination = 0; combination < 16U; ++combination) {
        Options options;
        options.CaptureDelta = (combination & 1U) == 0U;
        options.InPlace = (combination & 2U) == 0U;
        options.PassSpanFallback = (combination & 4U) == 0U;
        options.LateListsOnTime = (combination & 8U) == 0U;
        TestStreams(combination + 1U, lists, options);
    }
    std::cout << "oot3d_native_pica_list_walk_tests: ok\n";
    return 0;
}
