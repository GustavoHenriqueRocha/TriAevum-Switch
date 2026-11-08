// Capture delta (config/capture_delta, SWITCH_PORT.md "Capture delta") against
// the full copy per draw, on the same command streams:
//  - Seeded OoT3D-shaped command lists (material registers with masks and
//    grouped bursts, vertex-uniform bursts in float24/float32, geometry-shader
//    mirroring toggles, default attributes, lighting LUT uploads that repeat
//    and alternate contents, fog/procedural tables, shader program and
//    swizzle uploads with identical and changed words, composition spans,
//    failing draws) and guest memory writes between lists (store cards),
//    replayed through the real frontend and submission queue over the real
//    guest memory: once with capture delta off (the reference), once on with
//    the frontend's source, once on with copied packets (the two capture
//    delta runs also walk the lists with config/list_walk_fast, the
//    reference without).
//  - Every draw compared byte for byte: register file, both float-uniform
//    files, default attributes, shader words, every table, the lighting LUT
//    content and hash, decoded state, vertex/index/texture snapshots and
//    their hashes and versions; also the frontend and queue savestates, which
//    are then restored mid-stream.
//  - The delta draws reach the consumer every way the runtime has: the
//    queue's takes, the log replayed in random prefixes, and the deferred plan
//    builder (worker on and off, plans resolved in random order) whose plans
//    are compared with plans built serially from the reference draws.
// --bench: the capture sub-stages per draw on a steady 100-draw scene.
#include "oot3d_native_a32_memory.h"
#include "oot3d_native_capture_clock.h"
#include "oot3d_native_pica_capture_delta.h"
#include "oot3d_native_pica_deferred_plan.h"
#include "oot3d_native_pica_submission.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_capture_delta_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// ------------------------------------------------------------ guest memory

constexpr uint32_t kGuestBase = 0x14000000U;
constexpr uint32_t kPhysicalBase = 0x20000000U;
constexpr size_t kMemoryBytes = 0x400000U;
constexpr uint32_t kIndexArea = 0x080000U;   // 64 slots of 4 KiB
constexpr uint32_t kVertexArea = 0x100000U;  // 32 slots of 32 KiB
constexpr uint32_t kTextureArea = 0x200000U; // 64 slots of 16 KiB
constexpr size_t kIndexSlots = 64U;
constexpr size_t kVertexSlots = 32U;
constexpr size_t kTextureSlots = 64U;

struct MemoryWrite {
    uint32_t Offset = 0;
    std::vector<uint8_t> Bytes;
};

// Index values stay below 256: every vertex range fits its 32 KiB slot.
void FillIndexBytes(std::mt19937_64& random, std::vector<uint8_t>& bytes) {
    for (size_t index = 0; index < bytes.size(); index += 2U) {
        bytes[index] = static_cast<uint8_t>(random());
        if (index + 1U < bytes.size()) bytes[index + 1U] = 0U;
    }
}

std::vector<MemoryWrite> InitialMemory(std::mt19937_64& random) {
    std::vector<MemoryWrite> writes;
    MemoryWrite indices{kIndexArea, std::vector<uint8_t>(kIndexSlots * 0x1000U)};
    FillIndexBytes(random, indices.Bytes);
    writes.push_back(std::move(indices));
    MemoryWrite rest{kVertexArea, std::vector<uint8_t>(kTextureArea + kTextureSlots * 0x4000U - kVertexArea)};
    for (auto& byte : rest.Bytes) byte = static_cast<uint8_t>(random());
    writes.push_back(std::move(rest));
    return writes;
}

// Between lists: a few writes into index, vertex and texture slots; some
// rewrite the bytes already there (the card is set, the content is not new).
std::vector<MemoryWrite> Mutations(std::mt19937_64& random, const std::vector<uint8_t>& current) {
    std::vector<MemoryWrite> writes;
    for (size_t count = random() % 5U; count != 0U; --count) {
        MemoryWrite write;
        const size_t size = 2U + (random() % 512U & ~size_t{1});
        write.Bytes.resize(size);
        switch (random() % 3U) {
        case 0:
            write.Offset = kIndexArea + static_cast<uint32_t>(random() % (kIndexSlots * 0x1000U - size) & ~size_t{1});
            FillIndexBytes(random, write.Bytes);
            break;
        case 1:
            write.Offset = kVertexArea + static_cast<uint32_t>(random() % (kVertexSlots * 0x8000U - size));
            for (auto& byte : write.Bytes) byte = static_cast<uint8_t>(random());
            break;
        default:
            write.Offset = kTextureArea + static_cast<uint32_t>(random() % (kTextureSlots * 0x4000U - size));
            for (auto& byte : write.Bytes) byte = static_cast<uint8_t>(random());
            break;
        }
        if (random() % 4U == 0U) {
            std::memcpy(write.Bytes.data(), current.data() + write.Offset, size);
        }
        writes.push_back(std::move(write));
    }
    return writes;
}

// ------------------------------------------------------------- the streams

constexpr uint16_t kTriggerDraw = 0x22EU;
constexpr uint16_t kTriggerDrawIndexed = 0x22FU;

struct ListBuilder {
    std::vector<uint32_t> Words;

    void Put(uint16_t id, uint32_t value, uint32_t mask = 0xFU) {
        Words.push_back(value);
        Words.push_back(static_cast<uint32_t>(id) | (mask << 16U));
    }
    // One header and its extra words: consecutive registers (grouped) or the
    // same register again.
    void Burst(uint16_t id, std::span<const uint32_t> values, bool grouped, uint32_t mask = 0xFU) {
        Words.push_back(values[0]);
        Words.push_back(static_cast<uint32_t>(id) | (mask << 16U) |
                        (static_cast<uint32_t>(values.size() - 1U) << 20U) |
                        (grouped ? 0x80000000U : 0U));
        Words.insert(Words.end(), values.begin() + 1, values.end());
        if ((Words.size() & 1U) != 0U) Words.push_back(0U);
    }
};

// Registers the draw state does not validate (TEV, lighting parameters, fog
// colour, shader output maps): a few preset values each, so the plans of the
// replayed scene share shader programs as a game's do.
constexpr std::array<uint16_t, 40> kFreeRegisters{
    0x0C0, 0x0C1, 0x0C2, 0x0C3, 0x0C4, 0x0C8, 0x0C9, 0x0CA, 0x0CB, 0x0CC,
    0x0D0, 0x0D1, 0x0D2, 0x0D3, 0x0D4, 0x0D8, 0x0D9, 0x0DA, 0x0DB, 0x0DC,
    0x0E0, 0x0E1, 0x0F0, 0x0F1, 0x0F2, 0x0F3, 0x0F4, 0x0F8, 0x0F9, 0x0FA,
    0x0FB, 0x0FC, 0x0FD, 0x103, 0x104, 0x105, 0x106, 0x107, 0x140, 0x141};

class Stream final {
  public:
    explicit Stream(uint64_t seed) : mRandom(seed) {
        for (auto& table : mLutContents)
            for (auto& content : table)
                for (auto& word : content) word = static_cast<uint32_t>(mRandom());
        for (auto& program : mPrograms) {
            program[0] = 0x88000000U;  // END: every variant builds
            for (size_t word = 1; word < program.size(); ++word) program[word] = static_cast<uint32_t>(mRandom());
        }
        for (auto& values : mFreeValues)
            for (auto& value : values) value = static_cast<uint32_t>(mRandom());
        // Register 0x0E0's fog mode: off or fog (gas has no plan).
        const size_t fog = static_cast<size_t>(
            std::find(kFreeRegisters.begin(), kFreeRegisters.end(), uint16_t{0x0E0U}) - kFreeRegisters.begin());
        for (auto& value : mFreeValues[fog]) value &= ~7U;
        mFreeValues[fog][1] |= 5U;
        // TEV stages: sources, modifiers, operations and scales the
        // combiner accepts (any constant colour).
        const auto nibbles = [&](std::initializer_list<uint32_t> allowed, size_t count) {
            uint32_t value = 0U;
            for (size_t nibble = 0; nibble < count; ++nibble)
                value |= allowed.begin()[mRandom() % allowed.size()] << (nibble * 4U);
            return value;
        };
        for (size_t index = 0; index < kFreeRegisters.size(); ++index) {
            const uint16_t id = kFreeRegisters[index];
            if (id < 0x0C0U || id > 0x0FCU || (id >= 0x0E0U && id < 0x0F0U)) continue;
            for (auto& value : mFreeValues[index]) {
                switch ((id - 0x0C0U) % 8U) {
                case 0: value = nibbles({0U, 1U, 2U, 3U, 4U, 0xEU, 0xFU}, 3) |
                                (nibbles({0U, 1U, 2U, 3U, 4U, 0xEU, 0xFU}, 3) << 16U); break;
                case 1: value = nibbles({0U, 1U, 2U, 3U}, 6); break;
                case 2: value = nibbles({0U, 1U, 2U, 3U, 8U}, 1) | (nibbles({0U, 1U, 2U, 3U, 8U}, 1) << 16U); break;
                case 4: value = static_cast<uint32_t>(mRandom() % 3U) | (static_cast<uint32_t>(mRandom() % 3U) << 16U); break;
                default: break;
                }
            }
        }
    }

    std::mt19937_64& Random() noexcept { return mRandom; }

    // One command list of 1..`maxDraws` draws. `fail` makes one draw use an
    // invalid colour format (the capture fails; the next list repairs it).
    std::vector<uint32_t> NextList(size_t maxDraws, bool fail) {
        ListBuilder list;
        // Repair a format a previous list broke, and the steady pipeline.
        list.Put(0x117, (static_cast<uint32_t>(mRandom() % 5U) << 16U) | 2U);
        list.Put(0x04F, 1U);
        list.Put(0x050, 0x03020100U);
        list.Put(0x2BD, 1U);
        list.Put(0x2BA, 0x7FFF0000U);
        list.Put(0x200, 0x04000000U);
        list.Put(0x229, 0U);
        if (mFirstList) {
            // The first program upload (one END at the entry point).
            list.Put(0x2CB, 0U);
            list.Burst(0x2CC, mPrograms[0], false);
            list.Put(0x2D5, 0U);
            list.Burst(0x2D6, std::span<const uint32_t>(mPrograms[0]).first(4), false);
            mFirstList = false;
        }
        const size_t draws = 1U + mRandom() % maxDraws;
        const size_t failing = fail ? mRandom() % draws : draws;
        for (size_t draw = 0; draw < draws; ++draw) {
            Material(list);
            Uniforms(list);
            Tables(list);
            if (draw == failing) list.Put(0x117, 7U << 16U);
            Geometry(list);
        }
        return list.Words;
    }

    // A finite float24 (sign, 7-bit exponent away from 0x7F, mantissa): the
    // savestate documents compare NaN-free.
    uint32_t Float24() {
        return (static_cast<uint32_t>(mRandom() % 2U) << 23U) |
               ((0x30U + static_cast<uint32_t>(mRandom() % 0x20U)) << 16U) |
               static_cast<uint32_t>(mRandom() & 0xFFFFU);
    }
    // Four float24 values in the three words of a packed upload (the
    // inverse of the frontend's DecodePackedFloat24).
    std::array<uint32_t, 3> PackedFloat24() {
        const uint32_t v0 = Float24();
        const uint32_t v1 = Float24();
        const uint32_t v2 = Float24();
        const uint32_t v3 = Float24();
        return {(v3 << 8U) | (v2 >> 16U), ((v2 & 0xFFFFU) << 16U) | (v1 >> 8U),
                ((v1 & 0xFFU) << 24U) | v0};
    }

  private:
    uint32_t Pick(size_t registerIndex) {
        return mFreeValues[registerIndex][mRandom() % mFreeValues[registerIndex].size()];
    }

    void Material(ListBuilder& list) {
        for (size_t count = mRandom() % 24U; count != 0U; --count) {
            const size_t index = mRandom() % kFreeRegisters.size();
            const uint32_t mask = mRandom() % 4U == 0U ? static_cast<uint32_t>(1U + mRandom() % 15U) : 0xFU;
            list.Put(kFreeRegisters[index], Pick(index), mask);
        }
        if (mRandom() % 3U == 0U) {
            // A grouped run over a TEV stage.
            static constexpr std::array<uint16_t, 6> kStages{0x0C0U, 0x0C8U, 0x0D0U, 0x0D8U, 0x0F0U, 0x0F8U};
            const uint16_t base = kStages[mRandom() % kStages.size()];
            std::array<uint32_t, 5> values{};
            for (size_t index = 0; index < values.size(); ++index) values[index] = Pick(index);
            list.Burst(base, values, true);
        }
        // Validated state: only values the decoder accepts.
        list.Put(0x040, static_cast<uint32_t>(mRandom() % 3U));
        list.Put(0x065, std::array<uint32_t, 3>{0U, 1U, 3U}[mRandom() % 3U]);
        // Default fragment mode mostly (gas and shadow modes have no plan).
        list.Put(0x100, (mRandom() % 16U == 0U ? 3U : 0U) | (mRandom() % 2U != 0U ? 0x100U : 0U));
        const auto equation = [&] { return static_cast<uint32_t>(mRandom() % 5U); };
        const auto factor = [&] { return static_cast<uint32_t>(mRandom() % 15U); };
        list.Put(0x101, equation() | (equation() << 8U) | (factor() << 16U) | (factor() << 20U) |
                            (factor() << 24U) | (factor() << 28U));
        list.Put(0x116, std::array<uint32_t, 3>{0U, 2U, 3U}[mRandom() % 3U]);
        list.Put(0x11D, (0x18000000U + static_cast<uint32_t>(mRandom() % 2U) * 0x100000U) >> 3U);
        list.Put(0x11C, 0x18200000U >> 3U);
        list.Put(0x11E, (239U << 12U) | 400U | (1U << 24U));
        list.Put(0x113, 0xFU);
        list.Put(0x115, 3U);
        list.Put(0x041, Float24());
        if (mRandom() % 8U == 0U) {
            // Geometry-shader mirroring on/off.
            list.Put(0x244, static_cast<uint32_t>(mRandom() % 2U));
            list.Put(0x229, mRandom() % 16U == 0U ? 2U : 0U);
        }
        // Textures: none, one, two or three units, every native format.
        const uint32_t enabled = std::array<uint32_t, 4>{0U, 1U, 3U, 7U}[mRandom() % 4U];
        list.Put(0x080, enabled | (mRandom() % 2U != 0U ? (1U << 13U) : 0U));
        static constexpr std::array<uint16_t, 3> kConfig{0x081U, 0x091U, 0x099U};
        static constexpr std::array<uint16_t, 3> kFormat{0x08EU, 0x096U, 0x09EU};
        for (size_t unit = 0; unit < 3U; ++unit) {
            if ((enabled & (1U << unit)) == 0U) continue;
            const uint32_t width = 8U << (mRandom() % 4U);
            const uint32_t height = 8U << (mRandom() % 4U);
            const uint32_t slot = static_cast<uint32_t>(mRandom() % kTextureSlots);
            std::array<uint32_t, 5> values{
                static_cast<uint32_t>(mRandom()),
                (width << 16U) | height,
                ((static_cast<uint32_t>(mRandom()) & 0x3306U)),
                static_cast<uint32_t>(mRandom() % 0x2000U),
                (kPhysicalBase + kTextureArea + slot * 0x4000U) >> 3U};
            list.Burst(kConfig[unit], values, true);
            list.Put(kFormat[unit], static_cast<uint32_t>(mRandom() % 14U));
        }
    }

    void Uniforms(ListBuilder& list) {
        // Matrices (and bones): a burst of vectors from a start index.
        for (size_t burst = mRandom() % 3U; burst != 0U; --burst) {
            const bool geometry = mRandom() % 6U == 0U;
            const uint16_t setup = geometry ? 0x290U : 0x2C0U;
            const bool float32 = mRandom() % 2U != 0U;
            const uint32_t start = static_cast<uint32_t>(mRandom() % 96U);
            const size_t vectors = 1U + mRandom() % 12U;
            list.Put(setup, start | (float32 ? 0x80000000U : 0U));
            std::vector<uint32_t> data;
            for (size_t vector = 0; vector < vectors; ++vector) {
                if (float32) {
                    for (size_t word = 0; word < 4U; ++word)
                        data.push_back(std::bit_cast<uint32_t>(static_cast<float>(mRandom() % 2000U) / 7.0F));
                } else {
                    const auto packed = PackedFloat24();
                    data.insert(data.end(), packed.begin(), packed.end());
                }
            }
            list.Burst(static_cast<uint16_t>(setup + 1U), data, false);
        }
        if (mRandom() % 4U == 0U) {
            list.Put(0x2B0, static_cast<uint32_t>(mRandom() % 0x10000U));  // booleans
            list.Put(static_cast<uint16_t>(0x2B1U + mRandom() % 4U), static_cast<uint32_t>(mRandom()));  // integers
        }
        if (mRandom() % 6U == 0U) {
            list.Put(0x232, static_cast<uint32_t>(mRandom() % 15U));
            const size_t vectors = 1U + mRandom() % 3U;
            std::vector<uint32_t> data;
            for (size_t vector = 0; vector < vectors; ++vector) {
                const auto packed = PackedFloat24();
                data.insert(data.end(), packed.begin(), packed.end());
            }
            list.Burst(0x233, data, false);
        }
    }

    void Tables(ListBuilder& list) {
        if (mRandom() % 5U == 0U) {
            // A lighting LUT table from a small pool of contents: repeats
            // the content already there, or restores an earlier one.
            const uint32_t table = static_cast<uint32_t>(mRandom() % 24U);
            const auto& content = mLutContents[table][mRandom() % mLutContents[table].size()];
            const bool partial = mRandom() % 4U == 0U;
            const uint32_t first = partial ? static_cast<uint32_t>(mRandom() % 200U) : 0U;
            const uint32_t count = partial ? 1U + static_cast<uint32_t>(mRandom() % 56U) : 256U;
            list.Put(0x1C5, (table << 8U) | first);
            list.Burst(0x1C8, std::span<const uint32_t>(content).subspan(first, count), false);
        }
        if (mRandom() % 25U == 0U) {
            list.Put(0x0AF, (static_cast<uint32_t>(mRandom() % 6U) << 8U) | static_cast<uint32_t>(mRandom() % 64U));
            std::vector<uint32_t> data(1U + mRandom() % 32U, static_cast<uint32_t>(mRandom() % 3U));
            list.Burst(0x0B0, data, false);
        }
        if (mRandom() % 25U == 0U) {
            list.Put(0x0E6, static_cast<uint32_t>(mRandom() % 128U));
            std::vector<uint32_t> data(1U + mRandom() % 16U, static_cast<uint32_t>(mRandom() % 3U));
            list.Burst(0x0E8, data, false);
        }
        if (mRandom() % 30U == 0U) {
            // A shader re-upload: often the very words already there.
            const auto& program = mPrograms[mRandom() % mPrograms.size()];
            list.Put(0x2CB, 0U);
            list.Burst(0x2CC, program, false);
            list.Put(0x2D5, 0U);
            list.Burst(0x2D6, std::span<const uint32_t>(program).first(4), false);
        }
    }

    void Geometry(ListBuilder& list) {
        const uint32_t attributes = 1U + static_cast<uint32_t>(mRandom() % 3U);
        const uint32_t formats = static_cast<uint32_t>(mRandom()) & 0x0FFFU;
        list.Put(0x201, formats);
        list.Put(0x202, ((attributes - 1U) << 28U) | ((static_cast<uint32_t>(mRandom()) & 0x6U) << 16U));
        const size_t loaders = 1U + mRandom() % 3U;
        for (size_t loader = 0; loader < 12U; ++loader) {
            const uint16_t base = static_cast<uint16_t>(0x203U + loader * 3U);
            if (loader >= loaders) {
                if (mRandom() % 4U == 0U) list.Put(static_cast<uint16_t>(base + 2U), 0U);
                continue;
            }
            const uint32_t stride = std::array<uint32_t, 6>{8U, 12U, 16U, 20U, 24U, 32U}[mRandom() % 6U];
            const uint32_t slot = static_cast<uint32_t>(mRandom() % kVertexSlots);
            std::array<uint32_t, 3> values{
                kVertexArea + slot * 0x8000U + static_cast<uint32_t>(mRandom() % 64U) * 4U,
                static_cast<uint32_t>(mRandom() % 3U),
                (stride << 16U) | (1U << 28U)};
            list.Burst(base, values, true);
        }
        list.Put(0x2B9, static_cast<uint32_t>(mRandom() % 3U));
        list.Put(0x2BB, 0x76543210U);
        const bool indexed = mRandom() % 5U != 0U;
        if (indexed) {
            const bool sixteen = mRandom() % 2U != 0U;
            const uint32_t slot = static_cast<uint32_t>(mRandom() % kIndexSlots);
            list.Put(0x227, (kIndexArea + slot * 0x1000U) | (sixteen ? 0x80000000U : 0U));
            list.Put(0x228, 3U + static_cast<uint32_t>(mRandom() % 300U));
        } else {
            list.Put(0x228, 3U + static_cast<uint32_t>(mRandom() % 150U));
            list.Put(0x22A, static_cast<uint32_t>(mRandom() % 100U));
        }
        list.Put(0x25E, static_cast<uint32_t>(mRandom() % 3U) << 8U, 0x2U);
        list.Put(indexed ? kTriggerDrawIndexed : kTriggerDraw, 1U);
    }

    std::mt19937_64 mRandom;
    bool mFirstList = true;
    std::array<std::array<std::array<uint32_t, 256>, 3>, 24> mLutContents{};
    std::array<std::array<uint32_t, 24>, 3> mPrograms{};
    std::array<std::array<uint32_t, 3>, kFreeRegisters.size()> mFreeValues{};
};

// ---------------------------------------------------------------- the runs

struct Run {
    NativeA32Memory Memory;
    std::vector<uint8_t> Mirror = std::vector<uint8_t>(kMemoryBytes);
    std::unique_ptr<Oot3dNativePicaSubmissionQueue> Queue;
    Oot3dNativePicaFrontend Frontend;

    // fastWalk: config/list_walk_fast (the reference keeps the per-word walk).
    Run(bool delta, bool inPlace, bool fastWalk = false) {
        std::string error;
        Require(Memory.MapRegion({"linear", kGuestBase, kMemoryBytes, true, false, {}}, &error), error);
        Queue = std::make_unique<Oot3dNativePicaSubmissionQueue>(
            Oot3dPicaPhysicalMemoryView(Memory, {{kPhysicalBase, kGuestBase, kMemoryBytes}}), true);
        Queue->SetLeanCapture(true);
        Queue->SetCaptureDelta(delta);
        Frontend.SetPacketSink(Queue.get());
        Frontend.SetDiagnosticHistoryEnabled(false);
        Frontend.SetInPlaceDrawPackets(inPlace);
        Frontend.SetCaptureDelta(delta);
        Frontend.SetListWalkFast(fastWalk);
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    void Write(const MemoryWrite& write) {
        Require(Memory.WriteBytes(kGuestBase + write.Offset, write.Bytes), "guest write failed");
        std::memcpy(Mirror.data() + write.Offset, write.Bytes.data(), write.Bytes.size());
    }
};

bool SameShader(const Oot3dPicaShaderState& a, const Oot3dPicaShaderState& b) {
    return a.Program.Values() == b.Program.Values() && a.Swizzles.Values() == b.Swizzles.Values() &&
           std::memcmp(a.FloatUniforms.data(), b.FloatUniforms.data(), sizeof(a.FloatUniforms)) == 0 &&
           a.IntegerUniforms == b.IntegerUniforms && a.BooleanUniforms == b.BooleanUniforms &&
           a.ProgramWordCount == b.ProgramWordCount && a.SwizzleWordCount == b.SwizzleWordCount;
}

// Everything but the register file, float uniforms and default attributes
// (the arrays capture delta fills later) and, if `resources`, the snapshots.
void RequireSameSubmission(const Oot3dPicaDrawSubmission& a, const Oot3dPicaDrawSubmission& b,
                           bool arrays, bool resources, std::string_view what) {
    const auto fail = [&](std::string_view field) {
        Fail(std::string(what) + ": draw " + std::to_string(a.Id) + " differs in " + std::string(field));
    };
    const auto& p = a.Packet;
    const auto& q = b.Packet;
    if (a.Id != b.Id) fail("id");
    if (p.CommandListAddress != q.CommandListAddress || p.CommandListOffsetWords != q.CommandListOffsetWords ||
        p.CompositionDomain != q.CompositionDomain || !(p.Composition == q.Composition) ||
        !(p.LightBinding == q.LightBinding) || p.Indexed != q.Indexed)
        fail("packet identity");
    if (p.VertexShader.Program.Values() != q.VertexShader.Program.Values() ||
        p.VertexShader.Swizzles.Values() != q.VertexShader.Swizzles.Values() ||
        p.GeometryShader.Program.Values() != q.GeometryShader.Program.Values() ||
        p.VertexShader.ProgramWordCount != q.VertexShader.ProgramWordCount ||
        p.VertexShader.IntegerUniforms != q.VertexShader.IntegerUniforms ||
        p.VertexShader.BooleanUniforms != q.VertexShader.BooleanUniforms ||
        p.GeometryShader.IntegerUniforms != q.GeometryShader.IntegerUniforms ||
        p.GeometryShader.BooleanUniforms != q.GeometryShader.BooleanUniforms)
        fail("shader words or integer/boolean uniforms");
    if (p.FogLut.Values() != q.FogLut.Values() ||
        p.ProcTexLuts.Noise.Values() != q.ProcTexLuts.Noise.Values() ||
        p.ProcTexLuts.ColorMap.Values() != q.ProcTexLuts.ColorMap.Values() ||
        p.ProcTexLuts.AlphaMap.Values() != q.ProcTexLuts.AlphaMap.Values() ||
        p.ProcTexLuts.Color.Values() != q.ProcTexLuts.Color.Values() ||
        p.ProcTexLuts.ColorDifference.Values() != q.ProcTexLuts.ColorDifference.Values())
        fail("fog or procedural tables");
    if ((p.LightingLuts == nullptr) != (q.LightingLuts == nullptr) ||
        (p.LightingLuts != nullptr &&
         (p.LightingLuts->PackedEntries != q.LightingLuts->PackedEntries ||
          p.LightingLuts->ContentHash != q.LightingLuts->ContentHash ||
          p.LightingLuts->ContentHashAvailable != q.LightingLuts->ContentHashAvailable)))
        fail("lighting LUT");
    if (arrays) {
        if (p.Registers != q.Registers) fail("register file");
        if (!SameShader(p.VertexShader, q.VertexShader)) fail("vertex float uniforms");
        if (!SameShader(p.GeometryShader, q.GeometryShader)) fail("geometry float uniforms");
        if (std::memcmp(p.DefaultAttributes.data(), q.DefaultAttributes.data(), sizeof(p.DefaultAttributes)) != 0)
            fail("default attributes");
    }
    const auto& s = a.State;
    const auto& t = b.State;
    if (s.VertexInput.VertexCount != t.VertexInput.VertexCount || s.VertexInput.Indexed != t.VertexInput.Indexed ||
        s.VertexInput.IndexPhysicalAddress != t.VertexInput.IndexPhysicalAddress ||
        s.VertexInput.AttributeCount != t.VertexInput.AttributeCount ||
        s.Framebuffer.ColorPhysicalAddress != t.Framebuffer.ColorPhysicalAddress ||
        s.Framebuffer.ColorFormat != t.Framebuffer.ColorFormat || s.CullMode != t.CullMode ||
        s.OutputMerger.Blend.SourceColor != t.OutputMerger.Blend.SourceColor ||
        s.ShaderInterface.GeometryShaderEnabled != t.ShaderInterface.GeometryShaderEnabled ||
        s.Topology != t.Topology || a.MinimumVertexIndex != b.MinimumVertexIndex ||
        a.MaximumVertexIndex != b.MaximumVertexIndex)
        fail("decoded state");
    for (size_t slot = 0; slot < s.Textures.size(); ++slot) {
        if (s.Textures[slot].Enabled != t.Textures[slot].Enabled ||
            s.Textures[slot].PhysicalAddress != t.Textures[slot].PhysicalAddress ||
            s.Textures[slot].Format != t.Textures[slot].Format ||
            s.Textures[slot].Width != t.Textures[slot].Width ||
            s.Textures[slot].Height != t.Textures[slot].Height)
            fail("decoded texture");
    }
    if (!resources) return;
    if (a.Resources.size() != b.Resources.size()) fail("snapshot count");
    for (size_t index = 0; index < a.Resources.size(); ++index) {
        const auto& x = a.Resources[index];
        const auto& y = b.Resources[index];
        if (x.Kind != y.Kind || x.Slot != y.Slot || x.PhysicalAddress != y.PhysicalAddress ||
            x.FirstElement != y.FirstElement || !std::ranges::equal(x.ResolvedBytes(), y.ResolvedBytes()) ||
            x.ContentHash != y.ContentHash || x.ContentHashAvailable != y.ContentHashAvailable ||
            x.ContentVersion != y.ContentVersion || x.ContentVersionAvailable != y.ContentVersionAvailable ||
            x.BaseLevelContentHash != y.BaseLevelContentHash ||
            x.CopyReferences.size() != y.CopyReferences.size())
            fail("snapshot " + std::to_string(index));
    }
}

void RequireSameDraws(const std::vector<Oot3dPicaDrawSubmission>& reference,
                      const std::vector<Oot3dPicaDrawSubmission>& other, std::string_view what) {
    Require(reference.size() == other.size(), std::string(what) + ": draw counts differ");
    for (size_t index = 0; index < reference.size(); ++index) {
        Require(other[index].DeltaEntry == kOot3dPicaNoDeltaEntry,
                std::string(what) + ": a taken draw was left unmaterialized");
        RequireSameSubmission(reference[index], other[index], true, true, what);
    }
}


// config/plan_full_identity, plan_copy_build, plan_one_lock (as the builder's
// SetBuildOptions takes them): set per replay by main.
struct BuildMode {
    bool CanonicalIdentity = true;
    bool InPlace = false;
    bool Parallel = false;
};
BuildMode gBuild;

// One batch built through a deferred builder (kept alive for comparison).
struct DeferredResult {
    Oot3dPicaVulkanShaderSourceCache Cache;
    std::shared_ptr<Oot3dPicaDeferredPlanBatch> Batch;
    std::vector<Oot3dPicaVulkanDrawPlan> Plans;
    std::vector<char> Built;
    std::vector<std::string> Errors;
};

// How the delta run's draws of one drain pass reach their consumer.
enum class Consumer { Take, Prefixes, Builder, BuilderWorker };

struct ReplayStats {
    uint64_t Draws = 0;
    uint64_t Failures = 0;
    uint64_t Plans = 0;
    uint64_t PlanErrors = 0;
    std::string FirstPlanError;
};

void Drain(Run& reference, Run& delta, Run& packets, Consumer consumer, std::mt19937_64& random,
           ReplayStats& stats) {
    std::vector<Oot3dPicaDrawSubmission> expected;
    reference.Queue->TakePendingDraws(expected);
    {
        std::vector<Oot3dPicaDrawSubmission> copied;
        packets.Queue->TakePendingDraws(copied);
        RequireSameDraws(expected, copied, "capture delta with copied packets");
    }
    stats.Draws += expected.size();
    if (consumer == Consumer::Take) {
        std::vector<Oot3dPicaDrawSubmission> taken;
        delta.Queue->TakePendingDraws(taken);
        RequireSameDraws(expected, taken, "capture delta, queue take");
        return;
    }
    std::vector<Oot3dPicaDrawSubmission> draws;
    std::shared_ptr<Oot3dPicaCaptureDeltaLog> log;
    delta.Queue->TakePendingDraws(draws, log);
    Require(draws.size() == expected.size(), "capture delta: drained draw counts differ");
    Require((log != nullptr) == !draws.empty(), "capture delta: the log did not come with the draws");
    for (size_t index = 0; index < draws.size(); ++index) {
        RequireSameSubmission(expected[index], draws[index], false, true, "capture delta before replay");
        Require(draws[index].DeltaEntry != kOot3dPicaNoDeltaEntry, "a source draw has no log entry");
    }
    if (consumer == Consumer::Prefixes) {
        size_t through = 0U;
        while (log != nullptr && through < draws.size()) {
            through = std::min(draws.size() - 1U, through + random() % 7U);
            log->MaterializeThrough(draws, through);
            if (random() % 3U == 0U) log->MaterializeThrough(draws, random() % (through + 1U));  // already done
            ++through;
        }
        RequireSameDraws(expected, draws, "capture delta, log replayed in prefixes");
        return;
    }
    // The runtime's path: placeholders, publish, then plans resolved by
    // several threads at once, each on its own slots in random order (render
    // thread and dependency flushes) while the worker builds in order. With
    // parallel builds (gBuild.Parallel) two threads build slots N and N+1 at
    // the same time, each replaying the log up to its slot first. The delta
    // draws (with their log) and a copy of the reference draws (complete)
    // both go this way.
    const auto deferred = [&](std::vector<Oot3dPicaDrawSubmission>& submissions,
                              std::shared_ptr<Oot3dPicaCaptureDeltaLog> deltaLog,
                              std::string_view what) {
        auto owned = std::make_unique<DeferredResult>();
        auto& result = *owned;
        Oot3dPicaDeferredPlanBuilder builder(result.Cache);
        builder.SetBuildOptions(gBuild.CanonicalIdentity, gBuild.InPlace, gBuild.Parallel);
        builder.Configure(true, consumer == Consumer::BuilderWorker);
        result.Batch = builder.BeginBatch(submissions, std::move(deltaLog));
        result.Plans.resize(result.Batch->Submissions.size());
        for (size_t index = 0; index < result.Plans.size(); ++index)
            builder.FillDeferredPlan(result.Batch, index, result.Plans[index]);
        builder.Publish(result.Batch);
        std::vector<size_t> order(result.Plans.size());
        std::iota(order.begin(), order.end(), 0U);
        std::shuffle(order.begin(), order.end(), random);
        result.Built.assign(result.Plans.size(), 0);
        result.Errors.assign(result.Plans.size(), {});
        const size_t threads = 1U + random() % 3U;
        std::vector<std::thread> resolvers;
        for (size_t thread = 0; thread < threads; ++thread) {
            resolvers.emplace_back([&, thread] {
                for (size_t position = thread; position < order.size(); position += threads) {
                    const size_t index = order[position];
                    result.Built[index] = ResolveOot3dPicaDeferredDrawPlan(result.Plans[index],
                                                                           &result.Errors[index]) ? 1 : 0;
                }
            });
        }
        for (auto& resolver : resolvers) resolver.join();
        builder.StopWorker();
        // Built plans consumed the snapshots; everything else must match.
        for (size_t index = 0; index < result.Plans.size(); ++index) {
            Require(result.Batch->Submissions[index].DeltaEntry == kOot3dPicaNoDeltaEntry,
                    std::string(what) + ": the plan builder left a draw unmaterialized");
            RequireSameSubmission(expected[index], result.Batch->Submissions[index], true, false, what);
        }
        return owned;
    };
    std::vector<Oot3dPicaDrawSubmission> referenceCopy = expected;
    const auto fromDelta = deferred(draws, std::move(log), "capture delta through the plan builder");
    const auto fromReference = deferred(referenceCopy, nullptr, "full capture through the plan builder");
    Oot3dPicaVulkanShaderSourceCache serialCache;
    Oot3dPicaDeferredPlanBuilder serial(serialCache);
    serial.SetBuildOptions(gBuild.CanonicalIdentity, gBuild.InPlace, gBuild.Parallel);
    for (size_t index = 0; index < expected.size(); ++index) {
        Oot3dPicaVulkanDrawPlan plan;
        std::string error;
        const bool ok = serial.Build(expected[index], plan, &error);
        for (const DeferredResult* result : {fromDelta.get(), fromReference.get()}) {
            const auto& other = result->Plans[index];
            // The deferred path prefixes the builder's message.
            Require(ok == (result->Built[index] != 0) &&
                        std::string_view(result->Errors[index]).ends_with(error),
                    "a deferred plan succeeded or failed differently: serial " + std::to_string(ok) + " '" +
                        error + "', deferred '" + result->Errors[index] + "'");
            if (!ok) continue;
            Require(ComputeOot3dPicaVisualDrawIdentity(plan) == ComputeOot3dPicaVisualDrawIdentity(other),
                    "a deferred plan differs from the serial plan");
            Require(std::memcmp(plan.VertexShader.Uniforms.Floats.data(), other.VertexShader.Uniforms.Floats.data(),
                                sizeof(plan.VertexShader.Uniforms.Floats)) == 0 &&
                        plan.ResolvedVertexShaderSource() == other.ResolvedVertexShaderSource() &&
                        plan.ResolvedFragmentShaderSource() == other.ResolvedFragmentShaderSource(),
                    "a deferred plan has other uniforms or shaders");
        }
        ++stats.Plans;
        stats.PlanErrors += ok ? 0U : 1U;
        if (!ok && stats.FirstPlanError.empty()) stats.FirstPlanError = error;
    }
}

void TestReplay(uint64_t seed, size_t lists) {
    Stream stream(seed);
    auto& random = stream.Random();
    Run reference(false, true);
    Run delta(true, true, true);
    Run packets(true, false, true);
    const auto write = [&](const MemoryWrite& memoryWrite) {
        reference.Write(memoryWrite);
        delta.Write(memoryWrite);
        packets.Write(memoryWrite);
    };
    for (const auto& memoryWrite : InitialMemory(random)) write(memoryWrite);
    ReplayStats stats;
    std::string errorA;
    std::string errorB;
    std::string errorC;
    for (size_t list = 0; list < lists; ++list) {
        for (const auto& memoryWrite : Mutations(random, reference.Mirror)) write(memoryWrite);
        const bool fail = random() % 40U == 0U;
        const auto words = stream.NextList(24U, fail);
        Oot3dGspCommandPacket command{};
        command.Control = 1U;
        command.Parameters[0] = kGuestBase + 0x10000U + static_cast<uint32_t>(list % 16U) * 0x4000U;
        command.Parameters[1] = static_cast<uint32_t>(words.size() * 4U);
        if (random() % 2U != 0U) {
            Oot3dPicaCommandListCompositionSpan span;
            span.BeginAddress = command.Parameters[0];
            span.EndAddress = command.Parameters[0] + command.Parameters[1];
            span.Attribution.Layer = Oot3dPicaCompositionLayer::OpaqueWorld;
            span.Attribution.SourcePc = static_cast<uint32_t>(random());
            std::array<float, 16> view{};
            for (auto& value : view) value = static_cast<float>(random() % 1000U);
            span.LightBinding.WorldToView = view;
            const std::array<Oot3dPicaCommandListCompositionSpan, 1> spans{span};
            for (Run* run : {&reference, &delta, &packets}) {
                Require(run->Frontend.SetNextCommandListCompositionSpans(command.Parameters[0],
                                                                         command.Parameters[1], spans, &errorA),
                        errorA);
            }
        }
        errorA.clear();
        errorB.clear();
        errorC.clear();
        const bool a = reference.Frontend.SubmitGspCommand(command, words, &errorA);
        const bool b = delta.Frontend.SubmitGspCommand(command, words, &errorB);
        const bool c = packets.Frontend.SubmitGspCommand(command, words, &errorC);
        Require(a == b && a == c && errorA == errorB && errorA == errorC,
                "capture delta changed whether a command list succeeds (" + errorA + " / " + errorB + ")");
        stats.Failures += a ? 0U : 1U;
        if (random() % 3U == 0U || list + 1U == lists) {
            const Consumer consumer = static_cast<Consumer>(random() % 4U);
            Drain(reference, delta, packets, consumer, random, stats);
        }
        if (list % 61U == 60U) {
            // Savestate mid-stream: the same documents, restored in place.
            Drain(reference, delta, packets, Consumer::Take, random, stats);
            const auto frontendState = reference.Frontend.CaptureState();
            const auto queueState = reference.Queue->CaptureState();
            Require(delta.Frontend.CaptureState() == frontendState &&
                        packets.Frontend.CaptureState() == frontendState,
                    "capture delta changed the frontend savestate");
            Require(delta.Queue->CaptureState() == queueState && packets.Queue->CaptureState() == queueState,
                    "capture delta changed the queue savestate");
            for (Run* run : {&reference, &delta, &packets}) {
                Require(run->Frontend.RestoreState(frontendState, &errorA), errorA);
                Require(run->Queue->RestoreState(queueState, &errorA), errorA);
            }
        }
    }
    const auto counters = delta.Queue->Counters();
    const auto referenceCounters = reference.Queue->Counters();
    const auto& frontend = delta.Frontend.CaptureStats();
    const auto& referenceFrontend = reference.Frontend.CaptureStats();
    Require(counters.Draws == referenceCounters.Draws && counters.Keyframes != 0U && counters.DeltaDraws != 0U &&
                counters.Keyframes + counters.DeltaDraws + stats.Failures == counters.Draws,
            "capture delta did not record its draws as keyframes and deltas");
    Require(counters.StateBytes < referenceCounters.StateBytes, "capture delta did not copy less state");
    // A restored LUT content needs a long enough stream (short TSan runs).
    Require(frontend.SameValueWrites != 0U && (frontend.LutReuses != 0U || lists < 200U) &&
                frontend.LutHashes < referenceFrontend.LutHashes &&
                frontend.TableCopies <= referenceFrontend.TableCopies,
            "capture delta did not skip same-value writes or reuse LUT hashes");
    std::printf("replay seed=%llu lists=%zu draws=%llu failed_lists=%llu plans=%llu (errors %llu) "
                "state_kb ref=%.0f delta=%.0f keyframes=%llu lut_hashes ref=%llu delta=%llu reuses=%llu "
                "table_copies ref=%llu delta=%llu same_value_writes=%llu\n",
                static_cast<unsigned long long>(seed), lists, static_cast<unsigned long long>(stats.Draws),
                static_cast<unsigned long long>(stats.Failures), static_cast<unsigned long long>(stats.Plans),
                static_cast<unsigned long long>(stats.PlanErrors), referenceCounters.StateBytes / 1024.0,
                counters.StateBytes / 1024.0, static_cast<unsigned long long>(counters.Keyframes),
                static_cast<unsigned long long>(referenceFrontend.LutHashes),
                static_cast<unsigned long long>(frontend.LutHashes),
                static_cast<unsigned long long>(frontend.LutReuses),
                static_cast<unsigned long long>(referenceFrontend.TableCopies),
                static_cast<unsigned long long>(frontend.TableCopies),
                static_cast<unsigned long long>(frontend.SameValueWrites));
    if (!stats.FirstPlanError.empty()) std::printf("  first plan error: %s\n", stats.FirstPlanError.c_str());
}

// ------------------------------------------------------------------- bench

// A steady room: the same lists every frame over unchanged memory, drained
// once per frame (the log replayed after the timed part, as the worker
// would).
void Bench(bool captureDelta, size_t frames) {
    Stream stream(99U);
    auto& random = stream.Random();
    Run run(captureDelta, true);
    for (const auto& memoryWrite : InitialMemory(random)) run.Write(memoryWrite);
    std::vector<std::vector<uint32_t>> lists;
    for (size_t list = 0; list < 8U; ++list) lists.push_back(stream.NextList(24U, false));
    std::vector<Oot3dPicaDrawSubmission> draws;
    std::shared_ptr<Oot3dPicaCaptureDeltaLog> log;
    std::string error;
    Oot3dNativePicaSubmissionQueue::CaptureCounters before{};
    Oot3dPicaFrontendCaptureStats frontendBefore{};
    Oot3dPicaCaptureDeltaLog::Totals materializeBefore{};
    for (size_t frame = 0; frame < frames + 10U; ++frame) {
        if (frame == 10U) {
            before = run.Queue->Counters();
            frontendBefore = run.Frontend.CaptureStats();
            materializeBefore = Oot3dPicaCaptureDeltaLog::MaterializeTotals();
        }
        for (size_t list = 0; list < lists.size(); ++list) {
            Oot3dGspCommandPacket command{};
            command.Control = 1U;
            command.Parameters[0] = kGuestBase + 0x10000U + static_cast<uint32_t>(list) * 0x4000U;
            command.Parameters[1] = static_cast<uint32_t>(lists[list].size() * 4U);
            Require(run.Frontend.SubmitGspCommand(command, lists[list], &error), error);
        }
        run.Queue->TakePendingDraws(draws, log);
        if (log != nullptr) log->MaterializeAll(draws);
        log.reset();
    }
    const auto after = run.Queue->Counters();
    const auto& frontend = run.Frontend.CaptureStats();
    const auto materialize = Oot3dPicaCaptureDeltaLog::MaterializeTotals();
    const double count = static_cast<double>(after.Draws - before.Draws);
    const auto us = [count](uint64_t now, uint64_t then) {
        return CaptureTicksToNanoseconds(now - then) / 1.0e3 / count;
    };
    std::printf("bench capture_delta=%d draws/frame=%.0f us/draw: list %.3f capture %.3f (regs %.3f uniforms %.3f "
                "packet %.3f decode %.3f vertex %.3f texture %.3f hash %.3f enqueue %.3f) lut %.3f | state %.2f KiB/draw"
                " | materialize %.3f us/draw elsewhere\n",
                captureDelta ? 1 : 0, count / static_cast<double>(frames),
                us(frontend.ListTicks, frontendBefore.ListTicks),
                static_cast<double>(after.Nanoseconds - before.Nanoseconds) / 1.0e3 / count,
                us(after.RegisterTicks, before.RegisterTicks), us(after.UniformTicks, before.UniformTicks),
                us(after.PacketTicks, before.PacketTicks), us(after.DecodeTicks, before.DecodeTicks),
                us(after.VertexTicks, before.VertexTicks), us(after.TextureTicks, before.TextureTicks),
                us(after.HashTicks, before.HashTicks), us(after.EnqueueTicks, before.EnqueueTicks),
                us(frontend.LutTicks, frontendBefore.LutTicks),
                static_cast<double>(after.StateBytes - before.StateBytes) / 1024.0 / count,
                materialize.Draws != materializeBefore.Draws
                    ? CaptureTicksToNanoseconds(materialize.Ticks - materializeBefore.Ticks) / 1.0e3 /
                          static_cast<double>(materialize.Draws - materializeBefore.Draws)
                    : 0.0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bench") {
        const size_t frames = argc > 2 ? static_cast<size_t>(std::strtoul(argv[2], nullptr, 10)) : 300U;
        for (int round = 0; round < 2; ++round) {
            Bench(false, frames);
            Bench(true, frames);
        }
        return 0;
    }
    const size_t lists = argc > 1 ? static_cast<size_t>(std::strtoul(argv[1], nullptr, 10)) : 200U;
    // Every build-option combination (8), delta on and off in each replay.
    for (uint64_t combination = 0U; combination < 8U; ++combination) {
        gBuild.CanonicalIdentity = (combination & 1U) == 0U;
        gBuild.InPlace = (combination & 2U) != 0U;
        gBuild.Parallel = (combination & 4U) != 0U;
        std::printf("build options: canonical_identity=%d in_place=%d parallel=%d\n",
                    gBuild.CanonicalIdentity ? 1 : 0, gBuild.InPlace ? 1 : 0, gBuild.Parallel ? 1 : 0);
        TestReplay(combination + 1U, lists);
    }
    std::cout << "oot3d_native_capture_delta_tests: ok\n";
    return 0;
}
