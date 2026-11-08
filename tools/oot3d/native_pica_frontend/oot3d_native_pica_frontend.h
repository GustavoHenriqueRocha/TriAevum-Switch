#pragma once

#include "fast/renderer3ds/pica_light_binding.h"
#include "fast/renderer3ds/pica_temporal_identity.h"

#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Oot3dNativeGame {

// Construction tag: a packet's shared tables (shader programs and swizzles,
// fog and procedural texture tables, lighting LUTs) stay empty because the
// capture-delta log that recorded them sets them when it is replayed
// (config/capture_delta with config/texture_snapshot_fast, SWITCH_PORT.md
// "Texture snapshot and packet stages").
struct Oot3dPicaStateTablesDeferred {};

// Shader programs are uploaded rarely and then referenced by every draw.
// Copy-on-write storage keeps each submitted packet as an immutable snapshot
// without copying the complete 16 KiB backing array on the hot draw path.
template <typename Value, size_t Size>
class Oot3dPicaSharedArray final {
  public:
    using Array = std::array<Value, Size>;

    Oot3dPicaSharedArray() : mStorage(std::make_shared<Storage>()) {}

    explicit Oot3dPicaSharedArray(Array values)
        : mStorage(std::make_shared<Storage>(std::move(values))) {}

    // No storage until assigned (Oot3dPicaStateTablesDeferred): nothing may
    // read it before.
    explicit Oot3dPicaSharedArray(Oot3dPicaStateTablesDeferred) noexcept {}

    // The same storage (so the same values: a shared storage is never
    // written in place).
    [[nodiscard]] bool SharesStorageWith(const Oot3dPicaSharedArray& other) const noexcept {
        return mStorage == other.mStorage;
    }

    Oot3dPicaSharedArray& operator=(Array values) {
        mStorage = std::make_shared<Storage>(std::move(values));
        return *this;
    }

    [[nodiscard]] constexpr size_t size() const noexcept { return Size; }

    [[nodiscard]] const Value* data() const noexcept {
        return mStorage->Values.data();
    }

    [[nodiscard]] Value* data() {
        MakeMutable();
        return mStorage->Values.data();
    }

    [[nodiscard]] const Value& operator[](size_t index) const noexcept {
        return mStorage->Values[index];
    }

    [[nodiscard]] Value& operator[](size_t index) {
        MakeMutable();
        return mStorage->Values[index];
    }

    // Range access: const reads the shared storage; the mutable form first
    // takes its own copy, as operator[] does.
    [[nodiscard]] const Value* begin() const noexcept { return data(); }
    [[nodiscard]] const Value* end() const noexcept { return data() + Size; }
    [[nodiscard]] Value* begin() { return data(); }
    [[nodiscard]] Value* end() { return data() + Size; }

    [[nodiscard]] const Array& Values() const noexcept {
        return mStorage->Values;
    }

    // Monotonic within the process and changed before every possible write.
    // It is an exact mutation identity, not a content hash.
    [[nodiscard]] uint64_t MutationIdentity() const noexcept {
        return mStorage->MutationIdentity;
    }

    // A write would copy the storage first (another snapshot shares it).
    [[nodiscard]] bool Shared() const noexcept {
        return mStorage.use_count() != 1;
    }

  private:
    struct Storage final {
        Array Values{};
        uint64_t MutationIdentity = NextMutationIdentity();

        Storage() = default;
        explicit Storage(Array values) : Values(std::move(values)) {}
    };

    static uint64_t NextMutationIdentity() noexcept {
        static std::atomic<uint64_t> next{1U};
        return next.fetch_add(1U, std::memory_order_relaxed);
    }

    void MakeMutable() {
        if (mStorage.use_count() != 1) {
            mStorage = std::make_shared<Storage>(mStorage->Values);
        } else {
            mStorage->MutationIdentity = NextMutationIdentity();
        }
    }

    std::shared_ptr<Storage> mStorage;
};

struct Oot3dPicaHardwareRegisterWrite {
    uint32_t Offset = 0;
    uint32_t PhysicalAddress = 0;
    uint32_t Value = 0;
};

enum class Oot3dGspCommandId : uint8_t {
    RequestDma = 0,
    SubmitCommandList = 1,
    MemoryFill = 2,
    DisplayTransfer = 3,
    TextureCopy = 4,
    CacheFlush = 5,
};

enum class Oot3dPicaInterruptId : uint8_t {
    Psc0 = 0,
    Psc1 = 1,
    Pdc0 = 2,
    Pdc1 = 3,
    Ppf = 4,
    P3d = 5,
    Dma = 6,
};

// Composition ownership is supplied by the native runtime at command-list
// submission time. It is intentionally independent from shaders, textures,
// and render-target addresses so renderer extensions can respect the native
// scene/UI boundary without asset-specific classification.
enum class Oot3dPicaCompositionDomain : uint8_t {
    Unknown = 0,
    Scene = 1,
    Ui = 2,
};

// Draw ordering inside the scene remains a native composition concern, not a
// material heuristic. Unknown is a first-class value for native paths whose
// ownership boundary has not been demonstrated yet.
enum class Oot3dPicaCompositionLayer : uint8_t {
    Unknown = 0,
    OpaqueWorld = 1,
    TransparentWorld = 2,
    Atmosphere = 3,
    Ui = 4,
};

enum class Oot3dPicaCompositionProvenance : uint8_t {
    Unknown = 0,
    NativeCmbDrawPass = 1,
    NativeControlFlow = 2,
    NativeUiLifecycle = 3,
};

struct Oot3dPicaCompositionAttribution {
    Oot3dPicaCompositionLayer Layer =
        Oot3dPicaCompositionLayer::Unknown;
    Oot3dPicaCompositionProvenance Provenance =
        Oot3dPicaCompositionProvenance::Unknown;
    uint32_t SourcePc = 0;
    uint32_t NativeValue = 0;
    bool NativeProjectedShadow = false;
    bool NativeActorGeometry = false;
    uint64_t NativeActorIdentity = 0;
    // Renderer3ds::PicaCompositionAttribution::IsolatedLighting.
    bool IsolatedLighting = false;
    Fast::Renderer3ds::PicaTemporalIdentity TemporalIdentity;
    // Transient command destination, used ONLY to derive the source-relative
    // trigger offset. Cleared before publishing a draw packet.
    uint32_t TemporalPacketBeginAddress = 0;

    bool operator==(const Oot3dPicaCompositionAttribution&) const = default;
};

// Absolute guest-memory byte range emitted by one demonstrated native pass.
// The end is exclusive. Ranges are attached to exactly one subsequent GSP
// command-list submission and never inferred from the resulting PICA state.
struct Oot3dPicaCommandListCompositionSpan {
    uint32_t BeginAddress = 0;
    uint32_t EndAddress = 0;
    Oot3dPicaCompositionAttribution Attribution;
    Fast::Renderer3ds::PicaNativeLightBinding LightBinding;
    // The provider's write generation when these bytes were written (one per
    // native Gameplay_Draw; 0 = unknown).
    uint32_t WriteGeneration = 0;
};

class Oot3dPicaCommandListCompositionProvider {
  public:
    virtual ~Oot3dPicaCommandListCompositionProvider() = default;
    virtual bool TakeCommandListCompositionSpans(
        uint32_t commandListAddress, uint32_t commandListSize,
        std::vector<Oot3dPicaCommandListCompositionSpan>& spans,
        std::string* error = nullptr) = 0;
    // The whole native draw passes (a CMB renderer's SubmitDrawHandle) inside
    // the submitted list, sorted and disjoint: the owner of a draw that lies
    // in its pass but in none of its packet spans. None by default.
    virtual bool TakeCommandListPassSpans(
        uint32_t /*commandListAddress*/, uint32_t /*commandListSize*/,
        std::vector<Oot3dPicaCommandListCompositionSpan>& spans) {
        spans.clear();
        return true;
    }
    // The generation spans written now receive (0 = not tracked).
    [[nodiscard]] virtual uint32_t CurrentWriteGeneration() const noexcept {
        return 0U;
    }
};

// A command list the GSP queue ran after the next Gameplay_Draw had already
// switched the frame to its HUD phase (domain Ui): every span in it was
// written in an earlier generation, so its bytes are an earlier frame's
// scene, kicked late (completion-driven), not this frame's HUD. False for
// any other domain, a list without spans or untracked generations.
[[nodiscard]] bool IsLateCompositionCommandList(
    Oot3dPicaCompositionDomain submitted,
    std::span<const Oot3dPicaCommandListCompositionSpan> spans,
    uint32_t currentGeneration) noexcept;

// Per-pixel blink diagnostics (logs/perf.txt "composition:" group and the
// "composition note:" lines): how the frontend attributed draws whose
// composition was not a plain span hit in a scene list.
struct Oot3dPicaCompositionResolveStats {
    uint64_t LateLists = 0;            // IsLateCompositionCommandList
    uint64_t LateListDraws = 0;
    uint64_t UiListWorldSpanDraws = 0; // left UI (HUD-phase list) inside a world span
    uint64_t NoSpanDraws = 0;          // scene/unknown list with spans, draw in none
    uint64_t SameOwnerHoleDraws = 0;   // ... between two spans of one CMB owner
    uint64_t PassSpanDraws = 0;        // ... inside a CMB pass, attributed to it
    uint64_t BareListDraws = 0;        // scene/unknown list without any span
};

enum class Oot3dPicaCompositionNoteKind : uint8_t {
    LateList,      // a late list (attributed on time when enabled)
    UiListWorld,   // a HUD-phase list with world spans, kept UI
    SameOwnerHole, // a draw between two spans of the same CMB owner
    NoSpan,        // any other scene draw in no span of a list with spans
};

struct Oot3dPicaCompositionNoteSpan {
    uint32_t BeginAddress = 0;
    uint32_t EndAddress = 0;
    Oot3dPicaCompositionLayer Layer = Oot3dPicaCompositionLayer::Unknown;
    Oot3dPicaCompositionProvenance Provenance = Oot3dPicaCompositionProvenance::Unknown;
    uint32_t SourcePc = 0;
    uint64_t Actor = 0;
    uint64_t Instance = 0; // TemporalIdentity.Instance (the CMB renderer)
    uint32_t WriteGeneration = 0;
};

struct Oot3dPicaCompositionNote {
    Oot3dPicaCompositionNoteKind Kind = Oot3dPicaCompositionNoteKind::LateList;
    uint32_t CommandListAddress = 0;
    uint32_t CommandListSize = 0;
    uint32_t OffsetWords = 0; // SameOwnerHole/NoSpan: the draw
    uint32_t Vertices = 0;    // SameOwnerHole/NoSpan: the draw's vertex count
    Oot3dPicaCompositionDomain Submitted = Oot3dPicaCompositionDomain::Unknown;
    Oot3dPicaCompositionDomain Resolved = Oot3dPicaCompositionDomain::Unknown;
    uint32_t Spans = 0;
    uint32_t OldestGeneration = 0;
    uint32_t NewestGeneration = 0;
    uint32_t CurrentGeneration = 0;
    uint32_t Draws = 0;          // lists: draws in it
    uint32_t WorldSpanDraws = 0; // lists: draws inside a world-layer span
    Oot3dPicaCompositionNoteSpan Before; // SameOwnerHole/NoSpan: the spans around the draw
    Oot3dPicaCompositionNoteSpan After;
    Oot3dPicaCompositionNoteSpan Pass;   // ... and the CMB pass holding it (EndAddress 0: none)
};

// "<kind> list=X+size draw=+N domain=a->b spans=S generations=o..n/c
// draws=D world=W [before=... after=...]" (a "composition note:" line).
[[nodiscard]] std::string FormatOot3dPicaCompositionNote(const Oot3dPicaCompositionNote& note);

struct Oot3dGspCommandPacket {
    uint32_t Control = 0;
    std::array<uint32_t, 7> Parameters{};
};

struct Oot3dPicaDisplayTransfer {
    uint32_t InputAddress = 0;
    uint32_t OutputAddress = 0;
    uint32_t InputSize = 0;
    uint32_t OutputSize = 0;
    uint32_t Flags = 0;
    // Nonzero selects the raw TextureCopy operation of the same GSP engine.
    // InputSize/OutputSize then contain native width/gap pairs, not extents.
    uint32_t TextureCopyBytes = 0;
};

struct Oot3dPicaMemoryFill {
    uint32_t StartAddress = 0;
    uint32_t EndAddress = 0;
    uint32_t Value = 0;
    uint16_t Control = 0;
};

struct Oot3dPicaMemoryFillCommand {
    std::array<Oot3dPicaMemoryFill, 2> Fills{};
};

struct Oot3dPicaRegisterWrite {
    uint32_t CommandListAddress = 0;
    uint32_t CommandListOffsetWords = 0;
    uint16_t RegisterId = 0;
    uint8_t ParameterMask = 0;
    bool RegisterSupported = false;
    uint32_t Value = 0;
    uint32_t FinalValue = 0;
};

// Construction tag: the packet's register file, float uniforms and default
// attributes stay unset (not zeroed) because the sink fills them right after
// (a full copy) or later, off the game thread (config/capture_delta: the
// capture-delta log of oot3d_native_pica_capture_delta.h).
struct Oot3dPicaStateArraysDeferred {};

struct Oot3dPicaShaderState {
    static constexpr size_t MaximumProgramWords = 4096;
    static constexpr size_t MaximumSwizzleWords = 4096;
    static constexpr size_t FloatUniformCount = 96;

    Oot3dPicaShaderState() : FloatUniforms{} {}
    // Everything but FloatUniforms (left unset).
    Oot3dPicaShaderState(const Oot3dPicaShaderState& other,
                         Oot3dPicaStateArraysDeferred) noexcept
        : Program(other.Program), Swizzles(other.Swizzles),
          IntegerUniforms(other.IntegerUniforms),
          BooleanUniforms(other.BooleanUniforms),
          ProgramWordCount(other.ProgramWordCount),
          SwizzleWordCount(other.SwizzleWordCount) {}
    // Nor Program and Swizzles (left empty).
    Oot3dPicaShaderState(const Oot3dPicaShaderState& other,
                         Oot3dPicaStateArraysDeferred,
                         Oot3dPicaStateTablesDeferred tables) noexcept
        : Program(tables), Swizzles(tables),
          IntegerUniforms(other.IntegerUniforms),
          BooleanUniforms(other.BooleanUniforms),
          ProgramWordCount(other.ProgramWordCount),
          SwizzleWordCount(other.SwizzleWordCount) {}

    Oot3dPicaSharedArray<uint32_t, MaximumProgramWords> Program;
    Oot3dPicaSharedArray<uint32_t, MaximumSwizzleWords> Swizzles;
    std::array<std::array<float, 4>, FloatUniformCount> FloatUniforms;
    std::array<std::array<uint8_t, 4>, 4> IntegerUniforms{};
    std::array<bool, 16> BooleanUniforms{};
    size_t ProgramWordCount = 0;
    size_t SwizzleWordCount = 0;
};

// The 64-byte lines of the frontend state written since the capture that
// last consumed them (config/capture_delta): register file (48 lines of 16
// registers), vertex and geometry float uniforms (24 lines of 4 vectors
// each), default attributes (4 lines of 4 vectors). Every line starts dirty;
// a line rewritten with its own value is still dirty (the capture copies
// content, the mark only says where to look).
struct Oot3dPicaStateDirtyLines {
    static constexpr size_t RegisterLines = 48U;
    static constexpr size_t UniformLines = 24U;
    static constexpr size_t AttributeLines = 4U;
    static constexpr uint64_t AllRegisters = (uint64_t{1} << RegisterLines) - 1U;
    static constexpr uint32_t AllUniforms = (uint32_t{1} << UniformLines) - 1U;
    static constexpr uint32_t AllAttributes = (uint32_t{1} << AttributeLines) - 1U;

    uint64_t Registers = AllRegisters;
    uint32_t VertexUniforms = AllUniforms;
    uint32_t GeometryUniforms = AllUniforms;
    uint32_t DefaultAttributes = AllAttributes;

    void MarkRegister(size_t id) noexcept { Registers |= uint64_t{1} << (id >> 4U); }
    void MarkAll() noexcept { *this = Oot3dPicaStateDirtyLines{}; }
    void Clear() noexcept {
        Registers = 0U;
        VertexUniforms = GeometryUniforms = DefaultAttributes = 0U;
    }
};

// Game-thread cost of the frontend's part of the capture (logs/perf.txt
// "capture:" group), cumulative. Ticks: CaptureTicks().
struct Oot3dPicaFrontendCaptureStats {
    uint64_t Lists = 0;
    uint64_t ListWords = 0;
    // Whole command lists, the sink's draw captures included.
    uint64_t ListTicks = 0;
    // The part of ListTicks inside the sink's draw and interrupt submissions
    // (its capture, lock included): ListTicks - SinkTicks is the walk itself.
    uint64_t SinkTicks = 0;
    // config/list_walk_fast: extra words of a command written by a batched
    // loop (a register repeated in consecutive mode, float uniform bursts).
    uint64_t BatchedWords = 0;
    // Lighting LUT tables: the 24 KiB content hash at a draw after an
    // upload, and the 24 KiB copy-on-write at the first upload after a draw.
    uint64_t LutTicks = 0;
    uint64_t LutHashes = 0;
    uint64_t LutCopies = 0;
    // Capture delta: hashes avoided because the uploads restored the content
    // of a table seen before (the same bytes, so the same hash).
    uint64_t LutReuses = 0;
    // Copy-on-write copies of the shared tables (shader programs, swizzles,
    // fog and procedural texture tables) made by a write after a draw.
    uint64_t TableCopies = 0;
    // Capture delta: table writes of the value already there, skipped (no
    // copy-on-write and the content hash stays valid).
    uint64_t SameValueWrites = 0;
};

// Procedural texture and fog tables: uploaded rarely and referenced by every
// draw, so each packet shares them copy-on-write (Oot3dPicaSharedArray)
// instead of copying their 4 KiB per draw on the guest's GX command.
struct Oot3dPicaProcTexLutState {
    Oot3dPicaProcTexLutState() = default;
    // Every table empty (Oot3dPicaStateTablesDeferred).
    explicit Oot3dPicaProcTexLutState(Oot3dPicaStateTablesDeferred tables) noexcept
        : Noise(tables), ColorMap(tables), AlphaMap(tables), Color(tables),
          ColorDifference(tables) {}

    Oot3dPicaSharedArray<uint32_t, 128> Noise;
    Oot3dPicaSharedArray<uint32_t, 128> ColorMap;
    Oot3dPicaSharedArray<uint32_t, 128> AlphaMap;
    Oot3dPicaSharedArray<uint32_t, 256> Color;
    Oot3dPicaSharedArray<uint32_t, 256> ColorDifference;
};

using Oot3dPicaFogLut = Oot3dPicaSharedArray<uint32_t, 128>;

struct Oot3dPicaLightingLutState {
    static constexpr size_t TableCount = 24U;
    static constexpr size_t EntryCount = 256U;
    static constexpr size_t PackedEntryCount = TableCount * EntryCount;

    std::array<uint32_t, PackedEntryCount> PackedEntries{};
    uint64_t ContentHash = 0;
    bool ContentHashAvailable = false;

    [[nodiscard]] uint32_t& Entry(size_t table, size_t index) {
        return PackedEntries[table * EntryCount + index];
    }

    [[nodiscard]] const uint32_t& Entry(size_t table, size_t index) const {
        return PackedEntries[table * EntryCount + index];
    }
};

[[nodiscard]] uint64_t ComputeOot3dPicaLightingLutContentHash(
    const Oot3dPicaLightingLutState& state);

// The frontend state a draw packet is made of, by reference: a sink that
// keeps packets constructs one directly where it stores it.
struct Oot3dPicaDrawPacketSource {
    uint32_t CommandListAddress = 0;
    uint32_t CommandListOffsetWords = 0;
    Oot3dPicaCompositionDomain CompositionDomain =
        Oot3dPicaCompositionDomain::Unknown;
    Oot3dPicaCompositionAttribution Composition;
    // nullptr: the packet keeps its default light binding.
    const Fast::Renderer3ds::PicaNativeLightBinding* LightBinding = nullptr;
    bool Indexed = false;
    const std::array<uint32_t, 0x300>* Registers = nullptr;
    const Oot3dPicaShaderState* VertexShader = nullptr;
    const Oot3dPicaShaderState* GeometryShader = nullptr;
    const Oot3dPicaFogLut* FogLut = nullptr;
    const Oot3dPicaProcTexLutState* ProcTexLuts = nullptr;
    std::shared_ptr<const Oot3dPicaLightingLutState> LightingLuts;
    const std::array<std::array<float, 4>, 16>* DefaultAttributes = nullptr;
    // The frontend's dirty lines (config/capture_delta); a sink that
    // captures only what changed clears the lines it consumed.
    Oot3dPicaStateDirtyLines* DirtyLines = nullptr;
};

struct Oot3dPicaDrawPacket {
    Oot3dPicaDrawPacket() : Registers{}, DefaultAttributes{} {}

    explicit Oot3dPicaDrawPacket(const Oot3dPicaDrawPacketSource& source)
        : Oot3dPicaDrawPacket(source.CommandListAddress,
                              source.CommandListOffsetWords,
                              source.CompositionDomain, source.Composition,
                              source.Indexed, *source.Registers,
                              *source.VertexShader, *source.GeometryShader,
                              *source.FogLut, *source.ProcTexLuts,
                              source.LightingLuts, *source.DefaultAttributes) {
        if (source.LightBinding != nullptr) LightBinding = *source.LightBinding;
    }

    // Everything but the register file, float uniforms and default
    // attributes, which stay unset (Oot3dPicaStateArraysDeferred).
    Oot3dPicaDrawPacket(const Oot3dPicaDrawPacketSource& source,
                        Oot3dPicaStateArraysDeferred deferred)
        : CommandListAddress(source.CommandListAddress),
          CommandListOffsetWords(source.CommandListOffsetWords),
          CompositionDomain(source.CompositionDomain),
          Composition(source.Composition), Indexed(source.Indexed),
          VertexShader(*source.VertexShader, deferred),
          GeometryShader(*source.GeometryShader, deferred),
          FogLut(*source.FogLut), ProcTexLuts(*source.ProcTexLuts),
          LightingLuts(source.LightingLuts) {
        if (source.LightBinding != nullptr) LightBinding = *source.LightBinding;
    }

    // Nor the shared tables (Oot3dPicaStateTablesDeferred): empty, set with
    // the arrays when the capture-delta log is replayed.
    Oot3dPicaDrawPacket(const Oot3dPicaDrawPacketSource& source,
                        Oot3dPicaStateArraysDeferred deferred,
                        Oot3dPicaStateTablesDeferred tables)
        : CommandListAddress(source.CommandListAddress),
          CommandListOffsetWords(source.CommandListOffsetWords),
          CompositionDomain(source.CompositionDomain),
          Composition(source.Composition), Indexed(source.Indexed),
          VertexShader(*source.VertexShader, deferred, tables),
          GeometryShader(*source.GeometryShader, deferred, tables),
          FogLut(tables), ProcTexLuts(tables) {
        if (source.LightBinding != nullptr) LightBinding = *source.LightBinding;
    }

    Oot3dPicaDrawPacket(
        uint32_t commandListAddress, uint32_t commandListOffsetWords,
        Oot3dPicaCompositionDomain compositionDomain,
        Oot3dPicaCompositionAttribution composition, bool indexed,
        const std::array<uint32_t, 0x300>& registers,
        const Oot3dPicaShaderState& vertexShader,
        const Oot3dPicaShaderState& geometryShader,
        const Oot3dPicaFogLut& fogLut,
        const Oot3dPicaProcTexLutState& procTexLuts,
        std::shared_ptr<const Oot3dPicaLightingLutState> lightingLuts,
        const std::array<std::array<float, 4>, 16>& defaultAttributes)
        : CommandListAddress(commandListAddress),
          CommandListOffsetWords(commandListOffsetWords),
          CompositionDomain(compositionDomain), Composition(composition),
          Indexed(indexed), Registers(registers), VertexShader(vertexShader),
          GeometryShader(geometryShader), FogLut(fogLut),
          ProcTexLuts(procTexLuts), LightingLuts(std::move(lightingLuts)),
          DefaultAttributes(defaultAttributes) {}

    uint32_t CommandListAddress = 0;
    uint32_t CommandListOffsetWords = 0;
    Oot3dPicaCompositionDomain CompositionDomain =
        Oot3dPicaCompositionDomain::Unknown;
    Oot3dPicaCompositionAttribution Composition;
    Fast::Renderer3ds::PicaNativeLightBinding LightBinding;
    bool Indexed = false;
    std::array<uint32_t, 0x300> Registers;
    Oot3dPicaShaderState VertexShader;
    Oot3dPicaShaderState GeometryShader;
    Oot3dPicaFogLut FogLut;
    Oot3dPicaProcTexLutState ProcTexLuts;
    std::shared_ptr<const Oot3dPicaLightingLutState> LightingLuts;
    std::array<std::array<float, 4>, 16> DefaultAttributes;
};

class Oot3dPicaPacketSink {
  public:
    virtual ~Oot3dPicaPacketSink() = default;
    virtual bool SubmitHardwareRegisterWrite(
        const Oot3dPicaHardwareRegisterWrite& write,
        std::string* error = nullptr) = 0;
    virtual bool SubmitDrawPacket(const Oot3dPicaDrawPacket& packet,
                                  std::string* error = nullptr) = 0;
    // The same draw from the frontend state itself; a sink that stores
    // packets may construct it in place (one copy less per draw).
    virtual bool SubmitDrawPacketSource(const Oot3dPicaDrawPacketSource& source,
                                        std::string* error = nullptr) {
        const Oot3dPicaDrawPacket packet(source);
        return SubmitDrawPacket(packet, error);
    }
    virtual bool SubmitInterruptAfterGpuWork(
        Oot3dPicaInterruptId interrupt, std::string* error = nullptr) {
        (void)interrupt;
        (void)error;
        return true;
    }
    virtual bool SubmitDisplayTransfer(
        const Oot3dPicaDisplayTransfer&, bool* deferredToGpu,
        std::string* error = nullptr,
        bool* cpuCopySuppressed = nullptr) {
        if (deferredToGpu != nullptr) {
            *deferredToGpu = false;
        }
        if (cpuCopySuppressed != nullptr) {
            *cpuCopySuppressed = false;
        }
        (void)error;
        return true;
    }
    virtual bool SubmitMemoryFill(
        const Oot3dPicaMemoryFillCommand&, bool* deferredToGpu,
        std::string* error = nullptr) {
        if (deferredToGpu != nullptr) {
            *deferredToGpu = false;
        }
        (void)error;
        return true;
    }
};

class Oot3dNativePicaFrontend {
  public:
    explicit Oot3dNativePicaFrontend(
        Oot3dPicaPacketSink* packetSink = nullptr);
    void SetPacketSink(Oot3dPicaPacketSink* packetSink);
    void SetDiagnosticHistoryEnabled(bool enabled);
    // config/offload_capture: without diagnostic history, draws reach the
    // sink as SubmitDrawPacketSource (built in place by the queue).
    void SetInPlaceDrawPackets(bool enabled) noexcept {
        mInPlaceDrawPackets = enabled;
    }
    // config/list_walk_fast (SWITCH_PORT.md "Command-list walk"): without
    // diagnostic history, lists are walked with a per-register dispatch table
    // and batched consecutive-mode commands. The registers, shader state,
    // tables, queues, dirty lines, packets and composition are the same.
    void SetListWalkFast(bool enabled) noexcept { mListWalkFast = enabled; }
    [[nodiscard]] bool ListWalkFast() const noexcept { return mListWalkFast; }
    // config/capture_delta: the source handed to the sink carries the dirty
    // lines of the register file, uniforms and default attributes (the sink
    // captures only those); table writes of the value already present are
    // skipped (no copy-on-write; the lighting LUT hash stays valid) and a
    // lighting LUT whose uploads restored an earlier table's content takes
    // that table and its hash instead of hashing 24 KiB again. Every packet
    // keeps the same content and hashes as with it off.
    void SetCaptureDelta(bool enabled);
    [[nodiscard]] bool CaptureDelta() const noexcept { return mCaptureDelta; }
    [[nodiscard]] const Oot3dPicaFrontendCaptureStats& CaptureStats() const noexcept {
        return mCaptureStats;
    }
    void SetCommandListCompositionDomain(
        Oot3dPicaCompositionDomain domain) noexcept;
    bool SetNextCommandListCompositionSpans(
        uint32_t commandListAddress, uint32_t commandListSize,
        std::span<const Oot3dPicaCommandListCompositionSpan> spans,
        std::string* error = nullptr);
    // The provider's CMB passes for the same next list (TakeCommandListPassSpans).
    // A scene draw in none of the list's spans but inside a pass takes that
    // pass's attribution and light binding (no temporal identity) when
    // SetPassSpanFallback is on (default; off = config/no_pass_span_fallback).
    void SetNextCommandListPassSpans(
        uint32_t commandListAddress, uint32_t commandListSize,
        std::span<const Oot3dPicaCommandListCompositionSpan> spans);
    void SetPassSpanFallback(bool enabled) noexcept { mPassSpanFallback = enabled; }
    // For the next submissions: the domain the last guest refresh gave the
    // lists (what a list kicked on time gets) and the provider's current
    // write generation. A late list (IsLateCompositionCommandList) gets
    // the on-time domain when SetLateCommandListsOnTime is on (default; off
    // = config/late_lists_as_ui, the old HUD-phase attribution).
    void SetCommandListTiming(Oot3dPicaCompositionDomain onTimeDomain,
                              uint32_t currentGeneration) noexcept {
        mOnTimeCompositionDomain = onTimeDomain;
        mCurrentWriteGeneration = currentGeneration;
    }
    void SetLateCommandListsOnTime(bool enabled) noexcept {
        mLateCommandListsOnTime = enabled;
    }
    [[nodiscard]] const Oot3dPicaCompositionResolveStats&
    CompositionResolveStats() const noexcept {
        return mCompositionResolveStats;
    }
    // Notes recorded since the last call (at most 64 in all, of them at most
    // 8 late_list: a late list comes every frame).
    std::vector<Oot3dPicaCompositionNote> TakeCompositionNotes();

    bool WriteHardwareRegisters(uint32_t baseOffset,
                                std::span<const uint32_t> values,
                                std::string* error = nullptr);
    bool WriteHardwareRegistersWithMask(
        uint32_t baseOffset, std::span<const uint32_t> values,
        std::span<const uint32_t> masks, std::string* error = nullptr);
    bool SubmitGspCommand(const Oot3dGspCommandPacket& command,
                          std::span<const uint32_t> commandListWords = {},
                          std::string* error = nullptr,
                          bool* displayTransferDeferredToGpu = nullptr,
                          bool* memoryFillDeferredToGpu = nullptr,
                          bool* displayTransferCpuCopySuppressed = nullptr);
    std::optional<uint32_t> ReadHardwareRegister(uint32_t offset) const;
    size_t HardwareWriteCount() const;
    std::span<const Oot3dPicaHardwareRegisterWrite> PendingWrites() const;
    std::vector<Oot3dPicaHardwareRegisterWrite> TakePendingWrites();
    std::optional<uint32_t> ReadPicaRegister(uint16_t registerId) const;
    std::span<const Oot3dGspCommandPacket> PendingGspCommands() const;
    std::span<const Oot3dPicaRegisterWrite> PendingRegisterWrites() const;
    std::span<const Oot3dPicaDrawPacket> PendingDrawPackets() const;
    std::vector<Oot3dPicaInterruptId> TakePendingInterrupts();
    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

  private:
    struct PackedAttributeQueue {
        std::array<uint32_t, 4> Words{};
        size_t Count = 0;
    };

    void ApplyShaderRegisterSideEffects(uint16_t registerId,
                                        uint32_t writeValue);
    void ApplyShaderStageRegisterSideEffects(
        uint16_t registerId, uint32_t writeValue, uint16_t stageBase,
        Oot3dPicaShaderState& shader, PackedAttributeQueue& uniformQueue,
        bool mirrorToGeometryShader);
    // config/list_walk_fast: the extra words of a consecutive-mode command
    // to one float uniform data register, as ApplyShaderStageRegisterSideEffects
    // would take them one by one. False (nothing written) when the queue
    // already holds a whole vector for the current mode (the per-word path).
    bool WriteFloatUniformBurst(uint16_t registerId, bool vertexStage,
                                uint32_t mask, std::span<const uint32_t> words);
    // Stores into a shared table (copy-on-write); with capture delta a write
    // of the value already there is skipped.
    template <typename Table, typename Value>
    void WriteTable(Table& table, size_t index, Value value);
    void MarkUniform(const Oot3dPicaShaderState& shader, size_t index) noexcept {
        if (&shader == &mVertexShader) {
            mDirtyLines.VertexUniforms |= uint32_t{1} << (index >> 2U);
        } else {
            mDirtyLines.GeometryUniforms |= uint32_t{1} << (index >> 2U);
        }
    }
    void WriteLightingLut(size_t table, size_t index, uint32_t value);
    // Before a draw: the LUT's content hash, computed or (capture delta)
    // taken over from an earlier table with the same content.
    void SettleLightingLutHash();
    // XOR of one mixed word per LUT entry (position and value), kept up to
    // date by every LUT write while capture delta is on: equal contents have
    // equal fingerprints, so only those are compared byte for byte.
    static uint64_t LutEntryFingerprint(size_t position, uint32_t value) noexcept;
    void RecomputeLutFingerprint() noexcept;

    Oot3dPicaPacketSink* mPacketSink = nullptr;
    std::unordered_map<uint32_t, uint32_t> mHardwareRegisters;
    std::vector<Oot3dPicaHardwareRegisterWrite> mPendingWrites;
    std::array<uint32_t, 0x300> mPicaRegisters{};
    std::vector<Oot3dGspCommandPacket> mPendingGspCommands;
    std::vector<Oot3dPicaRegisterWrite> mPendingRegisterWrites;
    std::vector<Oot3dPicaDrawPacket> mPendingDrawPackets;
    std::vector<Oot3dPicaInterruptId> mPendingInterrupts;
    Oot3dPicaShaderState mVertexShader;
    Oot3dPicaShaderState mGeometryShader;
    Oot3dPicaFogLut mFogLut;
    Oot3dPicaProcTexLutState mProcTexLuts;
    std::shared_ptr<Oot3dPicaLightingLutState> mLightingLuts =
        std::make_shared<Oot3dPicaLightingLutState>();
    std::array<std::array<float, 4>, 16> mDefaultAttributes{};
    PackedAttributeQueue mVertexUniformQueue;
    PackedAttributeQueue mGeometryUniformQueue;
    PackedAttributeQueue mDefaultAttributeQueue;
    size_t mHardwareWriteCount = 0;
    bool mDiagnosticHistoryEnabled = true;
    bool mInPlaceDrawPackets = false;
    bool mListWalkFast = false;
    bool mCaptureDelta = false;
    Oot3dPicaStateDirtyLines mDirtyLines;
    Oot3dPicaFrontendCaptureStats mCaptureStats;
    uint64_t mLutFingerprint = 0;
    // Recently hashed LUT contents (capture delta), round robin.
    struct LutCacheEntry {
        uint64_t Fingerprint = 0;
        std::shared_ptr<Oot3dPicaLightingLutState> State;
    };
    // At most 8 x 24 KiB pinned.
    static constexpr size_t kLutCacheEntries = 8U;
    std::array<LutCacheEntry, kLutCacheEntries> mLutCache{};
    size_t mLutCacheNext = 0;
    Oot3dPicaCompositionDomain mCommandListCompositionDomain =
        Oot3dPicaCompositionDomain::Unknown;
    uint32_t mCompositionSpanCommandListAddress = 0;
    uint32_t mCompositionSpanCommandListSize = 0;
    std::vector<Oot3dPicaCommandListCompositionSpan>
        mNextCommandListCompositionSpans;
    uint32_t mPassSpanCommandListAddress = 0;
    uint32_t mPassSpanCommandListSize = 0;
    std::vector<Oot3dPicaCommandListCompositionSpan> mNextCommandListPassSpans;
    bool mPassSpanFallback = true;
    Oot3dPicaCompositionDomain mOnTimeCompositionDomain =
        Oot3dPicaCompositionDomain::Unknown;
    uint32_t mCurrentWriteGeneration = 0;
    bool mLateCommandListsOnTime = true;
    Oot3dPicaCompositionResolveStats mCompositionResolveStats;
    std::vector<Oot3dPicaCompositionNote> mCompositionNotes;
    size_t mCompositionNotesRecorded = 0;
    size_t mLateListNotesRecorded = 0;
    void NoteComposition(const Oot3dPicaCompositionNote& note);
};

} // namespace Oot3dNativeGame
