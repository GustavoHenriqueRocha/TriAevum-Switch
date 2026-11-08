#pragma once

#include "oot3d_native_pica_capture_delta.h"
#include "oot3d_native_pica_draw_state.h"

#include "fast/renderer3ds/pica_native_state.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Oot3dNativeGame {

inline uint64_t HashOot3dPicaSnapshot(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const auto value : bytes) hash = (hash ^ value) * 1099511628211ULL;
    return hash;
}

class NativeA32Memory;

struct Oot3dPicaPhysicalMemoryRegion {
    uint32_t PhysicalBaseAddress = 0;
    uint32_t GuestBaseAddress = 0;
    size_t Size = 0;
};

class Oot3dPicaPhysicalMemoryView {
  public:
    using ReadView = std::function<std::span<const uint8_t>(
        uint32_t guestAddress, size_t size)>;
    using WriteGeneration = std::function<std::optional<uint64_t>(
        uint32_t guestAddress, size_t size)>;
    using MappingCheck = std::function<bool(uint32_t guestAddress, size_t size)>;
    using GuestAliasVisitor = std::function<bool(uint32_t guestAddress, size_t size)>;

    Oot3dPicaPhysicalMemoryView(
        const NativeA32Memory& memory,
        std::vector<Oot3dPicaPhysicalMemoryRegion> regions);
    Oot3dPicaPhysicalMemoryView(
        std::vector<Oot3dPicaPhysicalMemoryRegion> regions,
        ReadView readView, WriteGeneration writeGeneration = {},
        MappingCheck mappingCheck = {});

    std::optional<uint32_t> Translate(uint32_t physicalAddress,
                                      size_t size) const;
    std::optional<uint32_t> TranslateGuest(uint32_t guestAddress,
                                           size_t size) const;
    // Visit every mapped alias without allocating. False means no alias exists
    // or a visitor rejected one; observation must cover every alias.
    bool ForEachGuestAlias(uint32_t physicalAddress, size_t size,
                           const GuestAliasVisitor& visitor) const;
    // Longest mapped segment starting at guestAddress: a guest write may cross
    // the boundary between two mapped views, and each segment still has to be
    // attributed to its own physical range.
    std::optional<std::pair<uint32_t, size_t>> GuestSegment(
        uint32_t guestAddress, size_t size) const;
    // Mirror a completed CPU write through its physical aliases before GPU
    // ownership is invalidated. Never reads the untouched device-owned bytes.
    bool SynchronizeGuestWrite(NativeA32Memory& memory,
                               uint32_t guestAddress, size_t size) const;
    std::optional<std::span<const uint8_t>> View(
        uint32_t physicalAddress, size_t size) const;
    std::optional<uint64_t> RangeWriteGeneration(uint32_t physicalAddress,
                                                 size_t size) const;
    bool Read(uint32_t physicalAddress, std::span<uint8_t> output) const;
    bool ReadGuest(uint32_t guestAddress,
                   std::span<uint8_t> output) const;
    std::span<const uint8_t> ViewGuest(uint32_t address, size_t size) const {
        if (!mReadView) return {};
        const auto view = mReadView(address, size);
        return view.size() == size ? view : std::span<const uint8_t>{};
    }
    // config/vertex_snapshot_fast: View followed by the certificate of
    // SnapshotWriteGeneration for a range exactly one region covers, as two
    // direct calls to the NativeA32Memory behind this view
    // (SynchronousReadView, RangeWriteGeneration) instead of the callbacks,
    // their mapping checks and the alias walks: the same calls into the
    // memory, in the same order. Fallback: no such memory (callback views) or
    // several covering regions (aliases need the write observation of
    // SnapshotWriteGeneration); the caller takes View and the generation as
    // before. Unmapped: View would fail.
    enum class DirectView : uint8_t { Viewed, Unmapped, Fallback };
    DirectView ViewCertified(uint32_t physicalAddress, size_t size,
                             std::span<const uint8_t>& bytes,
                             uint32_t& guestAddress,
                             std::optional<uint64_t>& generation) const;
    // The certificate of each guest page of [guestAddress, guestAddress +
    // size) (NativeA32Memory::RangeWriteGeneration of the page's part of the
    // range), in order; false (and `generations` emptied) when one has none.
    // Only after ViewCertified returned Viewed for that range.
    bool GuestPageGenerations(uint32_t guestAddress, size_t size,
                              std::vector<uint64_t>& generations) const;

  private:
    std::vector<Oot3dPicaPhysicalMemoryRegion> mRegions;
    ReadView mReadView;
    WriteGeneration mWriteGeneration;
    MappingCheck mMappingCheck;
    const NativeA32Memory* mDirectMemory = nullptr;
};

enum class Oot3dPicaResourceKind : uint8_t {
    IndexBuffer,
    VertexLoader,
    Texture,
};

struct Oot3dPicaResourceSnapshot {
    Oot3dPicaResourceKind Kind = Oot3dPicaResourceKind::VertexLoader;
    uint8_t Slot = 0;
    uint32_t PhysicalAddress = 0;
    uint32_t FirstElement = 0;
    std::vector<uint8_t> Bytes;
    std::shared_ptr<const std::vector<uint8_t>> SharedBytes;
    uint64_t ContentHash = 0;
    bool ContentHashAvailable = false;
    uint64_t ContentVersion = 0;
    bool ContentVersionAvailable = false;
    uint64_t BaseLevelContentHash = 0;
    bool BaseLevelContentHashAvailable = false;
    // Optional presentation texture extent; native decoded PICA state stays intact.
    uint16_t ReplacementWidth = 0;
    uint16_t ReplacementHeight = 0;
    // Explicit GPU producers of this immutable snapshot. Referenced bytes are
    // not CPU payload: their slots in ResolvedBytes are zeroed and must be
    // replaced by the immutable GPU version. Only uncovered bytes come from RAM.
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference> CopyReferences;

    [[nodiscard]] std::span<const uint8_t> ResolvedBytes() const {
        return SharedBytes != nullptr
                   ? std::span<const uint8_t>(*SharedBytes)
                   : std::span<const uint8_t>(Bytes);
    }
};

struct Oot3dPicaDrawSubmission {
    Oot3dPicaDrawSubmission() = default;

    Oot3dPicaDrawSubmission(uint64_t id,
                            const Oot3dPicaDrawPacket& packet)
        : Id(id), Packet(packet) {
    }

    Oot3dPicaDrawSubmission(uint64_t id,
                            const Oot3dPicaDrawPacketSource& source)
        : Id(id), Packet(source) {
    }

    // Register file, float uniforms and default attributes left unset: the
    // queue copies them next, or a capture-delta log fills them later.
    Oot3dPicaDrawSubmission(uint64_t id,
                            const Oot3dPicaDrawPacketSource& source,
                            Oot3dPicaStateArraysDeferred deferred)
        : Id(id), Packet(source, deferred) {
    }

    // Nor the packet's shared tables: the capture-delta log sets them too.
    Oot3dPicaDrawSubmission(uint64_t id,
                            const Oot3dPicaDrawPacketSource& source,
                            Oot3dPicaStateArraysDeferred deferred,
                            Oot3dPicaStateTablesDeferred tables)
        : Id(id), Packet(source, deferred, tables) {
    }

    uint64_t Id = 0;
    Oot3dPicaDrawPacket Packet;
    Oot3dPicaDecodedDrawState State;
    uint32_t MinimumVertexIndex = 0;
    uint32_t MaximumVertexIndex = 0;
    std::vector<Oot3dPicaResourceSnapshot> Resources;
    // config/capture_delta: the entry of the queue's capture-delta log that
    // still has to be materialized into Packet's register file, float
    // uniforms and default attributes (kOot3dPicaNoDeltaEntry: they hold the
    // draw's state).
    uint32_t DeltaEntry = kOot3dPicaNoDeltaEntry;
};

struct Oot3dPicaCompletionSubmission {
    uint64_t Id = 0;
    uint64_t AfterDrawSubmissionId = 0;
    Oot3dPicaInterruptId Interrupt = Oot3dPicaInterruptId::P3d;
};

struct Oot3dPicaDisplayTransferSubmission {
    uint64_t CompletionId = 0;
    uint64_t AfterDrawSubmissionId = 0;
    uint32_t InputPhysicalAddress = 0;
    uint32_t OutputPhysicalAddress = 0;
    Oot3dPicaDisplayTransfer Transfer;
    // A restored render target may need one GPU replay after its portable CPU
    // fallback already delivered the guest PPF interrupt.
    bool SignalInterrupt = true;
};

struct Oot3dPicaMemoryFillSubmission {
    uint64_t CompletionId = 0;
    uint64_t BeforeDrawSubmissionId = 0;
    uint32_t StartPhysicalAddress = 0;
    uint32_t EndPhysicalAddress = 0;
    uint32_t Value = 0;
    uint16_t Control = 0;
    std::optional<Oot3dPicaInterruptId> Interrupt;
};

struct Oot3dPicaSubmissionBatch {
    std::vector<Oot3dPicaDrawSubmission> Draws;
    std::vector<Oot3dPicaCompletionSubmission> Completions;
    std::vector<Oot3dPicaDisplayTransferSubmission> DisplayTransfers;
    std::vector<Oot3dPicaMemoryFillSubmission> MemoryFills;

    bool Empty() const noexcept {
        return Draws.empty() && Completions.empty() &&
               DisplayTransfers.empty() && MemoryFills.empty();
    }
};

struct Oot3dPicaSubmissionRuntimeProfile {
    uint64_t DrawCalls = 0;
    uint64_t TotalNanoseconds = 0;
    uint64_t PacketCopyNanoseconds = 0;
    uint64_t DecodeNanoseconds = 0;
    uint64_t VertexCaptureNanoseconds = 0;
    uint64_t TextureCaptureNanoseconds = 0;
    uint64_t EnqueueNanoseconds = 0;
    uint64_t IndexBytes = 0;
    uint64_t VertexBytes = 0;
    uint64_t TextureBytes = 0;
    uint64_t TextureSnapshotCacheHits = 0;
    uint64_t TextureSnapshotCacheMisses = 0;
    uint64_t TextureSnapshotComparedBytes = 0;
    uint64_t TextureSnapshotCopiedBytes = 0;
    uint64_t GeometrySnapshotCacheHits = 0;
    uint64_t GeometrySnapshotCacheMisses = 0;
    uint64_t GeometrySnapshotVersionHits = 0;
    uint64_t GeometrySnapshotVersionMisses = 0;
    uint64_t GeometrySnapshotComparedBytes = 0;
    uint64_t GeometrySnapshotCopiedBytes = 0;
    uint64_t TextureCopyReferences = 0;
    uint64_t TextureCopyReferencedBytes = 0;
};

class Oot3dNativePicaSubmissionQueue final : public Oot3dPicaPacketSink {
  public:
    using TexturePayloadTransform =
        std::function<void(const Oot3dPicaTextureState&,
                           std::span<std::uint8_t>)>;
    using TextureSnapshotTransform = std::function<void(
        const Oot3dPicaTextureState&, Oot3dPicaResourceSnapshot&)>;
    void SetTextureSnapshotTransform(TextureSnapshotTransform transform) {
        mTextureSnapshotTransform = std::move(transform);
    }

    explicit Oot3dNativePicaSubmissionQueue(
        Oot3dPicaPhysicalMemoryView memory,
        bool deferGpuBackedDisplayTransfers = false);
    void SetRuntimeProfilingEnabled(bool enabled);
    // config/offload_capture: texture hashes in one pass over the bytes,
    // geometry content hashes only when a savestate needs them, index
    // ranges remembered with the cached index bytes. Same snapshots, same
    // hashes; false keeps the previous passes.
    void SetLeanCapture(bool enabled) noexcept { mLeanCapture = enabled; }
    [[nodiscard]] bool LeanCapture() const noexcept { return mLeanCapture; }
    // config/capture_delta: a draw from a source with dirty lines records
    // only those lines (Oot3dPicaCaptureDeltaLog) instead of copying the
    // 6.4 KiB register file, float uniforms and default attributes into its
    // packet; the draw state is decoded from the frontend's registers. The
    // arrays are materialized by the consumer: every take below does it
    // before returning, except TakePendingDraws(destination, log), which
    // hands the log over (the plan builder replays it off this thread).
    void SetCaptureDelta(bool enabled);
    [[nodiscard]] bool CaptureDelta() const noexcept { return mCaptureDelta; }
    // config/vertex_snapshot_fast (SWITCH_PORT.md "Vertex snapshot hit
    // path"): an index buffer or vertex loader of one guest mapping is viewed
    // and certified by direct calls into guest memory
    // (Oot3dPicaPhysicalMemoryView::ViewCertified). When a multi-page range's
    // certificate changed, the certificate of each page is taken too and
    // kept with the cache entry; the next time only the pages whose own
    // certificate changed are compared with the cached bytes (the others are
    // proven unchanged as a whole range is on a version hit). The same hits,
    // misses, snapshots, versions and savestates; false keeps the callback
    // view, one certificate per range and a compare of every byte.
    void SetVertexSnapshotFast(bool enabled);
    [[nodiscard]] bool VertexSnapshotFast() const noexcept { return mVertexSnapshotFast; }
    // config/texture_snapshot_fast (SWITCH_PORT.md "Texture snapshot and
    // packet stages"): a texture of one guest mapping is viewed and
    // certified by the direct calls of the vertex path (ViewCertified),
    // found through a front table of the texture cache, and, when the
    // certificate of a multi-page texture changed, compared only on the
    // pages whose own certificate changed. A capture-delta draw leaves its
    // packet's shared tables to the log (Oot3dPicaStateTablesDeferred: one
    // reference per change instead of eleven per draw), and the next draw's
    // packet is prefetched for writing at the end of each draw. The same
    // snapshots, hashes, materialized packets, counters and savestates;
    // false keeps the callback view, the map lookup, a compare of every byte
    // and the tables copied into every packet.
    void SetTextureSnapshotFast(bool enabled);
    [[nodiscard]] bool TextureSnapshotFast() const noexcept { return mTextureSnapshotFast; }
    // Always counted, cumulative (logs/perf.txt): time inside the draw
    // submissions (the guest's GX command, on the guest thread) and the bytes
    // copied into snapshots and hashed there.
    struct CaptureCounters {
        uint64_t Draws = 0;
        uint64_t Nanoseconds = 0;
        uint64_t CopiedBytes = 0;
        uint64_t HashedBytes = 0;
        uint64_t IndexRangeReuses = 0;
        // Geometry snapshots made anew (cache misses) and entries evicted
        // to make room for them (the cache is full: kGeometrySnapshotCacheLimit).
        uint64_t GeometryMisses = 0;
        uint64_t GeometryEvictions = 0;
        // Sub-stages of Nanoseconds, in CaptureTicks() (perf.txt "capture:"):
        // the register file into the packet (or the log), the float uniforms
        // and default attributes, the rest of the packet (shared tables,
        // composition), the state decode, index/vertex and texture
        // snapshots, and the enqueue bookkeeping. HashTicks is the part of
        // the snapshots spent hashing content.
        uint64_t RegisterTicks = 0;
        uint64_t UniformTicks = 0;
        uint64_t PacketTicks = 0;
        uint64_t DecodeTicks = 0;
        uint64_t VertexTicks = 0;
        uint64_t TextureTicks = 0;
        uint64_t HashTicks = 0;
        uint64_t EnqueueTicks = 0;
        // Register file, uniforms and attributes written on this thread
        // (6.4 KiB per draw without capture delta), and the draws recorded
        // as a delta / as a keyframe.
        uint64_t StateBytes = 0;
        uint64_t DeltaDraws = 0;
        uint64_t Keyframes = 0;
        // Geometry snapshots (index buffers and vertex loaders, perf.txt
        // "vsnap:"): resources looked up; proven unchanged by their
        // certificate (version hits) or not (version misses: compared, or
        // made anew); without a certificate (escaped pointer, unobservable
        // alias); the bytes of the compared resources and the bytes actually
        // compared; the pages of certified resources, those whose
        // certificate changed on a compared entry (fast path), and resources
        // the fast path left to the previous one (aliases).
        uint64_t GeometryResources = 0;
        uint64_t GeometryVersionHits = 0;
        uint64_t GeometryVersionMisses = 0;
        uint64_t GeometryUncertified = 0;
        uint64_t GeometryUnprovenBytes = 0;
        uint64_t GeometryComparedBytes = 0;
        uint64_t GeometryPages = 0;
        uint64_t GeometryDirtyPages = 0;
        uint64_t GeometryFallbacks = 0;
        // Parts of VertexTicks: the memory view with the certificate, the
        // comparison with the cached bytes, and the copy of new bytes.
        uint64_t GeometryViewTicks = 0;
        uint64_t GeometryCompareTicks = 0;
        uint64_t GeometryCopyTicks = 0;
        // Textures (perf.txt "tsnap:"), as the geometry fields above:
        // looked up (textures without GPU copy references), proven
        // unchanged by their certificate or not, without a certificate, KiB
        // of the unproven ones and actually compared, pages of certified
        // multi-page textures and those whose certificate changed on a
        // compared entry, left to the previous path (aliases), made anew
        // (copied, transformed and hashed), and textures with GPU copy
        // references (their CPU gaps read every draw).
        uint64_t TextureResources = 0;
        uint64_t TextureVersionHits = 0;
        uint64_t TextureVersionMisses = 0;
        uint64_t TextureUncertified = 0;
        uint64_t TextureUnprovenBytes = 0;
        uint64_t TextureComparedBytes = 0;
        uint64_t TexturePages = 0;
        uint64_t TextureDirtyPages = 0;
        uint64_t TextureFallbacks = 0;
        uint64_t TextureMisses = 0;
        uint64_t TextureCopyReferenced = 0;
        // Parts of TextureTicks for the looked-up textures: the copy-reference
        // check, the memory view, the certificate and the cache lookup; the
        // comparison; and a new snapshot (copy, payload transform, hashes).
        uint64_t TextureViewTicks = 0;
        uint64_t TextureCompareTicks = 0;
        uint64_t TextureCopyTicks = 0;
    };
    [[nodiscard]] CaptureCounters Counters() const noexcept {
        std::lock_guard guard(mQueueMutex);
        return mCounters;
    }
    void SetTexturePayloadTransform(TexturePayloadTransform transform);
    Oot3dPicaSubmissionRuntimeProfile RuntimeProfile() const noexcept;

    // Guest-write observation for raw copy destinations. `registrar` traps
    // future writes into [guestAddress, guestAddress + byteCount) and returns
    // false when an escaped mutable pointer makes that range unobservable.
    // Without a registrar a raw copy cannot be assigned to the GPU: the queue
    // reports the failure instead of assuming ownership.
    using GuestWriteRegistrar =
        std::function<bool(uint32_t guestAddress, size_t byteCount)>;
    void BindGuestWriteObservation(GuestWriteRegistrar registrar);
    [[nodiscard]] bool HasGuestWriteObservation() const noexcept;
    // Entry point for the process memory's write observer. A guest write into a
    // producer's owned subrange removes exactly the bytes it covers, including a
    // same-value write, so a later snapshot never samples stale GPU bytes. Must
    // not be reached from the renderer's own materialization: that scope is
    // suppressed by the memory object.
    void NotifyGuestWrite(uint32_t guestAddress, size_t byteCount);
    // Appends the producers that still own copy destination bytes right now:
    // the versions a future snapshot can reference. The caller combines them
    // with the references of retained plans to pin the versions the backend
    // must keep alive; a superseded or CPU-overwritten producer is absent.
    void CollectLiveCopyReferences(
        std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out) const;

    bool SubmitHardwareRegisterWrite(
        const Oot3dPicaHardwareRegisterWrite& write,
        std::string* error = nullptr) override;
    bool SubmitDrawPacket(const Oot3dPicaDrawPacket& packet,
                          std::string* error = nullptr) override;
    // The packet is constructed in the queue's own submission (one copy of
    // the frontend state instead of two).
    bool SubmitDrawPacketSource(const Oot3dPicaDrawPacketSource& source,
                                std::string* error = nullptr) override;
    bool SubmitInterruptAfterGpuWork(
        Oot3dPicaInterruptId interrupt,
        std::string* error = nullptr) override;
    bool SubmitDisplayTransfer(
        const Oot3dPicaDisplayTransfer& transfer, bool* deferredToGpu,
        std::string* error = nullptr,
        bool* cpuCopySuppressed = nullptr) override;
    bool SubmitMemoryFill(
        const Oot3dPicaMemoryFillCommand& command, bool* deferredToGpu,
        std::string* error = nullptr) override;

    // Identities, decoded state and resources. With capture delta the
    // register file, uniforms and default attributes of these draws (and,
    // with config/texture_snapshot_fast, the packet's shared tables) may not
    // be filled yet (DeltaEntry); the takes below fill them.
    std::span<const Oot3dPicaDrawSubmission> PendingDraws() const;
    std::vector<Oot3dPicaDrawSubmission> TakePendingDraws();
    void TakePendingDraws(
        std::vector<Oot3dPicaDrawSubmission>& destination);
    // The same, but the draws' capture-delta log (if any draw has an entry)
    // comes along unreplayed in `deltaLog`: the caller materializes the draws
    // (Oot3dPicaCaptureDeltaLog::MaterializeThrough) before reading those
    // arrays and tables, or hands both to Oot3dPicaDeferredPlanBuilder::BeginBatch.
    void TakePendingDraws(
        std::vector<Oot3dPicaDrawSubmission>& destination,
        std::shared_ptr<Oot3dPicaCaptureDeltaLog>& deltaLog);
    std::span<const Oot3dPicaCompletionSubmission> PendingCompletions() const;
    std::vector<Oot3dPicaCompletionSubmission> TakePendingCompletions();
    std::span<const Oot3dPicaDisplayTransferSubmission>
    PendingDisplayTransfers() const;
    std::vector<Oot3dPicaDisplayTransferSubmission>
    TakePendingDisplayTransfers();
    std::span<const Oot3dPicaMemoryFillSubmission> PendingMemoryFills() const;
    std::vector<Oot3dPicaMemoryFillSubmission> TakePendingMemoryFills();
    Oot3dPicaSubmissionBatch TakePendingBatch();
    bool IsQuiescent() const noexcept;
    void RestoreGpuColorRenderTargetOwnership(
        std::span<const uint32_t> colorPhysicalAddresses);
    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

  private:
    struct GeometrySnapshotKey {
      Oot3dPicaResourceKind Kind = Oot3dPicaResourceKind::VertexLoader;
      uint8_t Slot = 0;
      uint32_t PhysicalAddress = 0;
      size_t Size = 0;

      bool operator==(const GeometrySnapshotKey &) const = default;
    };

    struct GeometrySnapshotKeyHash {
      size_t operator()(const GeometrySnapshotKey &key) const noexcept;
    };

    struct GeometrySnapshotCacheEntry {
      std::shared_ptr<const std::vector<uint8_t>> Bytes;
      uint64_t ContentHash = 0;
      // Lean capture: the hash of Bytes is computed when first needed
      // (savestate); a restored entry without bytes always has it.
      bool ContentHashValid = true;
      // Index range of Bytes read as 8- or 16-bit indices (lean capture):
      // 0 = not computed, 1 = 8-bit, 2 = 16-bit.
      uint8_t IndexRangeKind = 0;
      uint32_t IndexMinimum = 0;
      uint32_t IndexMaximum = 0;
      uint64_t ContentVersion = 0;
      uint64_t SourceWriteGeneration = 0;
      // config/vertex_snapshot_fast: the certificate of every guest page of
      // a multi-page range, taken after the range's and before Bytes were
      // last compared with or copied from guest memory; empty otherwise.
      std::vector<uint64_t> PageStamps;
      uint64_t LastUsedSequence = 0;
      // LRU order of the cache (oldest first, by LastUsedSequence), linked
      // through the map's own nodes, whose addresses survive rehashing: a
      // miss at the limit evicts the oldest entry in O(1) instead of scanning
      // every entry. Maintained only by the queue (LinkGeometryNewest and
      // UnlinkGeometry); a copied entry carries stale links.
      GeometrySnapshotCacheEntry* LruOlder = nullptr;
      GeometrySnapshotCacheEntry* LruNewer = nullptr;
      const GeometrySnapshotKey* LruKey = nullptr;
    };
    using GeometrySnapshotNode =
        std::pair<const GeometrySnapshotKey, GeometrySnapshotCacheEntry>;
    // config/vertex_snapshot_fast: a direct-mapped table in front of
    // mGeometrySnapshotCache, the node a key was last found at (nodes keep
    // their address until erased). A hit skips the map's bucket walk (its
    // modulo, bucket array, predecessor node and hash recomputations, each a
    // likely cache miss); a stale or other key falls back to the map. A slot
    // is cleared when its node is erased and all of them when the map is
    // replaced.
    struct GeometryFrontSlot {
      GeometrySnapshotKey Key;
      GeometrySnapshotNode *Node = nullptr;
    };
    static constexpr size_t kGeometryFrontSlots = 1024U;
    static size_t GeometryFrontIndex(const GeometrySnapshotKey &key) noexcept;
    void ClearGeometryFront() noexcept;

    struct TextureSnapshotKey {
      uint32_t PhysicalAddress = 0;
      uint16_t Width = 0;
      uint16_t Height = 0;
      uint8_t Format = 0;
      uint8_t Type = 0;
      uint8_t WrapS = 0;
      uint8_t WrapT = 0;
      bool MinLinear = false;
      bool MagLinear = false;
      bool MipLinear = false;
      int16_t LodBiasRaw = 0;
      uint8_t MinMipLevel = 0;
      uint8_t MaxMipLevel = 0;

      bool operator==(const TextureSnapshotKey &) const = default;
    };

    struct TextureSnapshotKeyHash {
        size_t operator()(const TextureSnapshotKey& key) const noexcept;
    };

    struct TextureSnapshotCacheEntry {
        std::shared_ptr<const std::vector<uint8_t>> SourceBytes;
        std::shared_ptr<const std::vector<uint8_t>> ResolvedBytes;
        uint64_t SourceHash = 0;
        uint64_t ContentHash = 0;
        uint64_t BaseLevelContentHash = 0;
        uint64_t SourceWriteGeneration = 0;
        // config/texture_snapshot_fast: as GeometrySnapshotCacheEntry's,
        // the certificate of every guest page of a multi-page texture, taken
        // after the texture's and before SourceBytes were last compared with
        // or copied from guest memory; empty otherwise.
        std::vector<uint64_t> PageStamps;
    };
    using TextureSnapshotNode =
        std::pair<const TextureSnapshotKey, TextureSnapshotCacheEntry>;
    // config/texture_snapshot_fast: the geometry front table's twin for the
    // texture cache (whose nodes are only replaced with the whole map).
    struct TextureFrontSlot {
      TextureSnapshotKey Key;
      TextureSnapshotNode *Node = nullptr;
    };
    static constexpr size_t kTextureFrontSlots = 512U;
    static size_t TextureFrontIndex(const TextureSnapshotKey &key) noexcept;
    void ClearTextureFront() noexcept;
    // The bytes of the pages of [guestAddress, + guest.size()) whose
    // certificate in `current` differs from `kept` (adjacent ones in one
    // memcmp) equal `cached`; the other pages are proven unchanged by their
    // certificates. Counts the compared bytes and the changed pages.
    static bool ComparePageRuns(const uint8_t *cached,
                                std::span<const uint8_t> guest,
                                uint32_t guestAddress,
                                std::span<const uint64_t> kept,
                                std::span<const uint64_t> current,
                                uint64_t &comparedBytes, uint64_t &dirtyPages);

    // Ordered raw-copy producer: the physical destination subranges it still
    // owns, in registration order. A newer copy truncates an older producer's
    // overlapping subranges and a guest write truncates the bytes it covers, so
    // the newest registration is always the owner of every byte it holds.
    struct RawCopySpan {
        uint32_t Address = 0;
        uint32_t ByteCount = 0;
    };

    struct RawCopyProducer {
        uint64_t CompletionId = 0;
        uint64_t AfterDrawSubmissionId = 0;
        std::vector<RawCopySpan> Owned;

        bool operator==(const RawCopyProducer&) const = default;
    };

    // Removes [address, address + byteCount) from every span, keeping the parts
    // a write or a newer copy did not touch.
    static void SubtractOwnedSpan(std::vector<RawCopySpan>& spans,
                                  uint32_t address, size_t byteCount);
    // Certify all aliases, including pointers that escaped since the last draw.
    bool ObservePhysicalWriteSpan(uint32_t address, size_t byteCount);
    std::optional<uint64_t> SnapshotWriteGeneration(uint32_t address, size_t byteCount);
    void ResetSnapshotObservation();
    bool RegisterRawCopyProducer(uint64_t completionId,
                                 uint64_t afterDrawSubmissionId,
                                 std::span<const RawCopySpan> destinations,
                                 std::string* error);
    void InvalidateRawCopyOwnership(uint32_t physicalAddress,
                                    size_t byteCount);
    void CollectCopyReferences(
        uint64_t drawSubmissionId, uint32_t physicalAddress, size_t byteCount,
        std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out) const;

    bool CaptureIndexAndVertexResources(Oot3dPicaDrawSubmission &submission,
                                        std::string *error);
    bool CaptureGeometryResource(Oot3dPicaResourceKind kind, uint8_t slot,
                                 uint32_t physicalAddress,
                                 uint32_t firstElement, size_t byteCount,
                                 Oot3dPicaResourceSnapshot &resource,
                                 std::string *error,
                                 GeometrySnapshotCacheEntry **entry = nullptr);
    // The entry of `node` becomes the newest of the LRU order (linked anew;
    // it must not be linked).
    void LinkGeometryNewest(
        std::pair<const GeometrySnapshotKey, GeometrySnapshotCacheEntry>& node);
    void UnlinkGeometry(GeometrySnapshotCacheEntry& entry);
    // Links every entry by ascending LastUsedSequence (after a restore).
    void RebuildGeometryLru();
    bool SubmitDrawLocked(Oot3dPicaDrawSubmission& submission,
                          const Oot3dPicaDrawPacketSource& source,
                          std::string* error);
    // Fills the arrays of every pending draw and empties the log.
    void MaterializePendingLocked();
    // config/texture_snapshot_fast: prefetches the next pending draw's slot
    // for writing (mPendingDraws has room for it).
    void PrefetchNextDraw() noexcept;
    // Snapshot content hashes, timed into mCounters.HashTicks.
    uint64_t HashTimed(std::span<const uint8_t> bytes);
    uint64_t ContinueHashTimed(uint64_t hash, std::span<const uint8_t> bytes);
    bool CaptureTextureResources(Oot3dPicaDrawSubmission &submission,
                                 std::string *error);

    Oot3dPicaPhysicalMemoryView mMemory;
    mutable std::mutex mQueueMutex;
    std::vector<Oot3dPicaHardwareRegisterWrite> mHardwareWrites;
    std::vector<Oot3dPicaDrawSubmission> mPendingDraws;
    std::vector<Oot3dPicaCompletionSubmission> mPendingCompletions;
    std::vector<Oot3dPicaDisplayTransferSubmission> mPendingDisplayTransfers;
    std::vector<Oot3dPicaMemoryFillSubmission> mPendingMemoryFills;
    std::unordered_set<uint32_t> mKnownColorRenderTargets;
    std::unordered_set<uint32_t> mInvalidatedColorRenderTargets;
    std::vector<Oot3dPicaDisplayTransferSubmission>
        mRestoredDisplayTransfersAwaitingTarget;
    std::vector<RawCopyProducer> mRawCopyProducers;
    GuestWriteRegistrar mGuestWriteRegistrar;
    bool mDeferGpuBackedDisplayTransfers = false;
    bool mRuntimeProfilingEnabled = false;
    bool mLeanCapture = false;
    bool mCaptureDelta = false;
    bool mVertexSnapshotFast = false;
    bool mTextureSnapshotFast = false;
    // The page certificates of the resource being captured
    // (config/vertex_snapshot_fast), kept to reuse its capacity.
    std::vector<uint64_t> mPageStamps;
    std::shared_ptr<Oot3dPicaCaptureDeltaLogPool> mDeltaPool =
        std::make_shared<Oot3dPicaCaptureDeltaLogPool>();
    // The log of mPendingDraws' entries (acquired at the first delta draw).
    std::shared_ptr<Oot3dPicaCaptureDeltaLog> mDeltaLog;
    CaptureCounters mCounters;
    TexturePayloadTransform mTexturePayloadTransform;
    TextureSnapshotTransform mTextureSnapshotTransform;
    std::unordered_map<TextureSnapshotKey, TextureSnapshotCacheEntry,
                       TextureSnapshotKeyHash>
        mTextureSnapshotCache;
    std::unordered_map<GeometrySnapshotKey, GeometrySnapshotCacheEntry,
                       GeometrySnapshotKeyHash>
        mGeometrySnapshotCache;
    Oot3dPicaSubmissionRuntimeProfile mRuntimeProfile;
    uint64_t mNextGeometryContentVersion = 1;
    uint64_t mGeometrySnapshotSequence = 0;
    GeometrySnapshotCacheEntry* mGeometryLruOldest = nullptr;
    GeometrySnapshotCacheEntry* mGeometryLruNewest = nullptr;
    std::array<GeometryFrontSlot, kGeometryFrontSlots> mGeometryFront{};
    std::array<TextureFrontSlot, kTextureFrontSlots> mTextureFront{};
    uint64_t mNextSubmissionId = 1;
    uint64_t mNextCompletionId = 1;
};

} // namespace Oot3dNativeGame
