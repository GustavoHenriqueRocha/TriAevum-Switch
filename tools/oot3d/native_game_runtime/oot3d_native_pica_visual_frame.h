#pragma once

#include "oot3d_native_pica_vulkan_plan.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Oot3dNativeGame {

// The draw plans of a visual frame. A plan is ~16 KiB, so a frame's vector is
// several MiB; one was allocated (and a different-sized one freed, frames
// later, on the render thread) for every guest frame, and those large blocks
// of varying size fragmented the console's heap (newlib malloc, shared with
// the GL driver). The storage of a dropped vector now goes back to a small
// process-wide pool, cleared, and the accumulator takes its next frame's
// vector from there: in steady state no frame allocates its plan array.
class Oot3dPicaPlanVector : public std::vector<Oot3dPicaVulkanDrawPlan> {
  public:
    using Base = std::vector<Oot3dPicaVulkanDrawPlan>;
    using Base::Base;

    Oot3dPicaPlanVector() = default;
    Oot3dPicaPlanVector(const Oot3dPicaPlanVector&) = default;
    Oot3dPicaPlanVector(Oot3dPicaPlanVector&&) noexcept = default;
    Oot3dPicaPlanVector(Base&& other) noexcept : Base(std::move(other)) {}
    Oot3dPicaPlanVector& operator=(const Oot3dPicaPlanVector& other) {
        Base::operator=(other);
        return *this;
    }
    Oot3dPicaPlanVector& operator=(Oot3dPicaPlanVector&& other) noexcept {
        if (this != &other) {
            Recycle();
            Base::operator=(std::move(static_cast<Base&>(other)));
        }
        return *this;
    }
    Oot3dPicaPlanVector& operator=(Base&& other) noexcept {
        Recycle();
        Base::operator=(std::move(other));
        return *this;
    }
    Oot3dPicaPlanVector& operator=(const Base& other) {
        Base::operator=(other);
        return *this;
    }
    Oot3dPicaPlanVector& operator=(
        std::initializer_list<Oot3dPicaVulkanDrawPlan> plans) {
        Base::operator=(plans);
        return *this;
    }
    ~Oot3dPicaPlanVector() { Recycle(); }

    // An empty vector with room for at least `minimum` plans, from the pool
    // when it has one.
    [[nodiscard]] static Oot3dPicaPlanVector Take(size_t minimum);

    struct PoolStats {
        uint64_t Taken = 0;     // Take() calls served from the pool
        uint64_t Missed = 0;    // Take() calls that had to allocate
        uint64_t Returned = 0;  // storages kept by the pool
        uint64_t Spares = 0;    // storages in the pool now
    };
    [[nodiscard]] static PoolStats Stats() noexcept;

  private:
    // Destroys the plans and hands the storage to the pool.
    void Recycle() noexcept;
};

struct Oot3dPicaVisualFrame {
    uint64_t Sequence = 0;
    Oot3dPicaDisplayTransferSubmission TopTransfer;
    Oot3dPicaPlanVector Draws;
    std::vector<uint64_t> StrictDrawIdentities;
    std::vector<Oot3dPicaMemoryFillSubmission> MemoryFills;
    std::vector<Oot3dPicaDisplayTransferSubmission> DisplayTransfers;
    // The leading work a raw-copy dependency flush already executed for the
    // canonical target when this frame was closed: draws and fills up to this
    // submission, and this many leading display transfers. The copy itself may
    // belong to a later frame. Only the canonical continuation of a newly
    // closed frame reads it, so a restored replay frame does not carry it.
    std::optional<uint64_t> DependencyExecutedThroughDraw;
    size_t DependencyExecutedTransfers = 0;
};

struct Oot3dPicaVisualFrameSample {
    Oot3dPicaDisplayTransferSubmission TopTransfer;
    std::vector<Oot3dPicaVulkanDrawPlan> Draws;
    std::vector<Oot3dPicaMemoryFillSubmission> MemoryFills;
    std::vector<Oot3dPicaDisplayTransferSubmission> DisplayTransfers;
    size_t MatchedDraws = 0;
    size_t StrictUniqueMatchedDraws = 0;
    size_t StrictOrdinalMatchedDraws = 0;
    size_t StructuralOrdinalMatchedDraws = 0;
    size_t PipelineOrdinalMatchedDraws = 0;
    size_t InterpolatedDraws = 0;
    size_t InterpolatedPerInstanceVertexDraws = 0;
    size_t InterpolatedPerVertexDraws = 0;
};

struct Oot3dPicaVisualDrawView {
    const Oot3dPicaVulkanDrawPlan* BasePlan = nullptr;
    std::optional<Oot3dPicaVertexUniformState> VertexUniforms;
    std::optional<Oot3dPicaFragmentUniformState> FragmentUniforms;
    std::optional<Oot3dPicaViewportState> Viewport;
    std::optional<std::array<float, 4>> BlendConstantColor;
    std::optional<Fast::Renderer3ds::PicaNativeLightBinding> LightBinding;
    std::vector<Oot3dPicaVulkanVertexBinding> VertexBindings;
    bool HasVertexBindingOverrides = false;

    Oot3dPicaVulkanDrawOverrides Overrides() const;
};

struct Oot3dPicaVisualFrameViewSample {
    Oot3dPicaDisplayTransferSubmission TopTransfer;
    std::vector<Oot3dPicaVisualDrawView> Draws;
    std::span<const Oot3dPicaMemoryFillSubmission> MemoryFills;
    std::span<const Oot3dPicaDisplayTransferSubmission> DisplayTransfers;
    size_t MatchedDraws = 0;
    size_t StrictUniqueMatchedDraws = 0;
    size_t StrictOrdinalMatchedDraws = 0;
    size_t StructuralOrdinalMatchedDraws = 0;
    size_t PipelineOrdinalMatchedDraws = 0;
    size_t InterpolatedDraws = 0;
    size_t InterpolatedPerInstanceVertexDraws = 0;
    size_t InterpolatedPerVertexDraws = 0;
};

struct Oot3dPicaVisualFrameAccumulatorState {
    uint64_t NextSequence = 1;
    std::vector<Oot3dPicaVulkanDrawPlan> PendingDraws;
    std::vector<Oot3dPicaMemoryFillSubmission> PendingMemoryFills;
    std::vector<Oot3dPicaDisplayTransferSubmission> PendingDisplayTransfers;
};

struct Oot3dPicaVisualContinuityTrackerState {
    std::vector<double> AcceptedDeltas;
};

struct Oot3dPicaVisualTransitionStats {
    size_t PreviousDraws = 0;
    size_t CurrentDraws = 0;
    size_t MatchedDraws = 0;
    size_t StrictUniqueMatchedDraws = 0;
    size_t StrictOrdinalMatchedDraws = 0;
    size_t StructuralOrdinalMatchedDraws = 0;
    size_t PipelineOrdinalMatchedDraws = 0;
    size_t ChangedContinuousDraws = 0;
    size_t ChangedPerInstanceVertexDraws = 0;
    size_t ChangedPerVertexDraws = 0;
    size_t AmbiguousPreviousDraws = 0;
    size_t AmbiguousCurrentDraws = 0;
    size_t UnmatchedPreviousDraws = 0;
    size_t UnmatchedCurrentDraws = 0;
    size_t TopTargetMatchedDraws = 0;
    double MatchedDrawCoverage = 0.0;
    double MedianNormalizedContinuousDelta = 0.0;
    double P90NormalizedContinuousDelta = 0.0;
    size_t CameraMatchedDraws = 0;
    size_t CameraDiscontinuousDraws = 0;
};

struct Oot3dPicaVisualInterpolationTiming {
    uint64_t MatchingNanoseconds = 0;
    uint64_t AnalysisNanoseconds = 0;
    uint64_t SamplingNanoseconds = 0;
};

uint64_t
ComputeOot3dPicaVisualDrawIdentity(const Oot3dPicaVulkanDrawPlan& plan);

bool AreOot3dPicaVisualDrawsCompatible(const Oot3dPicaVulkanDrawPlan& previous,
                                       const Oot3dPicaVulkanDrawPlan& current);

bool InterpolateOot3dPicaVisualDraw(const Oot3dPicaVulkanDrawPlan& previous,
                                    const Oot3dPicaVulkanDrawPlan& current,
                                    float alpha,
                                    Oot3dPicaVulkanDrawPlan& output);

Oot3dPicaVisualTransitionStats
AnalyzeOot3dPicaVisualTransition(const Oot3dPicaVisualFrame& previous,
                                 const Oot3dPicaVisualFrame& current);

bool SampleOot3dPicaVisualFrame(const Oot3dPicaVisualFrame& previous,
                                const Oot3dPicaVisualFrame& current,
                                float alpha,
                                Oot3dPicaVisualFrameSample& output);

bool AnalyzeAndSampleOot3dPicaVisualFrame(const Oot3dPicaVisualFrame& previous, const Oot3dPicaVisualFrame& current,
                                          float alpha, Oot3dPicaVisualTransitionStats& stats,
                                          Oot3dPicaVisualFrameSample& output);

bool AnalyzeAndSampleOot3dPicaVisualFrameView(const Oot3dPicaVisualFrame& previous, const Oot3dPicaVisualFrame& current,
                                              float alpha, Oot3dPicaVisualTransitionStats& stats,
                                              Oot3dPicaVisualFrameViewSample& output,
                                              Oot3dPicaVisualInterpolationTiming* timing = nullptr);

bool ViewOot3dPicaVisualFrame(
    const Oot3dPicaVisualFrame& frame,
    Oot3dPicaVisualFrameViewSample& output,
    bool skipCompletedDependencies = false);

// Producers a plan can still re-execute. A retained plan (accumulator pending
// work, previous or latest replay frame) keeps referencing the copy versions it
// sampled, so the backend must keep those versions alive behind a weak
// completion token until the plan is dropped. Destroying a version a retained
// plan still references would make its replay fall back to CPU bytes.
void CollectOot3dPicaPlanCopyReferences(
    const Oot3dPicaVulkanDrawPlan& plan,
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out);

void CollectOot3dPicaFrameCopyReferences(
    const Oot3dPicaVisualFrame& frame,
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference>& out);

// Draw matching and transition analysis are immutable for a pair of source
// frames. Prepare once, then sample at 1/2 for 2x or at 1/3 and 2/3 for 3x
// without repeating the matching work on every host presentation.
class Oot3dPicaPreparedVisualTransition final {
  public:
    Oot3dPicaPreparedVisualTransition();
    ~Oot3dPicaPreparedVisualTransition();
    Oot3dPicaPreparedVisualTransition(
        Oot3dPicaPreparedVisualTransition&&) noexcept;
    Oot3dPicaPreparedVisualTransition& operator=(
        Oot3dPicaPreparedVisualTransition&&) noexcept;
    Oot3dPicaPreparedVisualTransition(
        const Oot3dPicaPreparedVisualTransition&) = delete;
    Oot3dPicaPreparedVisualTransition& operator=(
        const Oot3dPicaPreparedVisualTransition&) = delete;

    bool Prepare(const Oot3dPicaVisualFrame& previous,
                 const Oot3dPicaVisualFrame& current,
                 Oot3dPicaVisualInterpolationTiming* timing = nullptr);
    bool Sample(const Oot3dPicaVisualFrame& previous,
                const Oot3dPicaVisualFrame& current, float alpha,
                Oot3dPicaVisualFrameViewSample& output,
                Oot3dPicaVisualInterpolationTiming* timing = nullptr) const;
    void Reset() noexcept;
    [[nodiscard]] bool Ready() const noexcept;
    [[nodiscard]] const Oot3dPicaVisualTransitionStats& Stats() const noexcept;
    // The previous-frame draw paired with a current draw (and how: 0 strict
    // unique, 1 strict ordinal, 2 structural ordinal, 3 pipeline ordinal),
    // or -1. For tests and diagnostics.
    [[nodiscard]] int64_t PreviousIndexOf(size_t currentIndex,
                                          uint8_t* kind = nullptr) const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

class Oot3dPicaVisualFrameAccumulator final {
  public:
    void Append(Oot3dPicaVulkanDrawPlan plan);
    // A default-constructed plan appended in place, for the caller to fill
    // (deferred placeholders): no ~16 KiB plan is moved into the vector.
    // The reference is valid until the next Append/Emplace/Finish.
    Oot3dPicaVulkanDrawPlan& EmplaceDraw();
    // config/offload_plan: FinishFrames hands the pending vector's storage
    // to a frame that takes every pending draw (one guest frame closed, the
    // usual case) instead of moving each plan. Same frames, same order;
    // false keeps the element-wise move.
    void SetMoveFreeFinish(bool enabled) noexcept { mMoveFreeFinish = enabled; }
    [[nodiscard]] bool MoveFreeFinish() const noexcept { return mMoveFreeFinish; }
    void Append(Oot3dPicaMemoryFillSubmission fill);
    void Append(Oot3dPicaDisplayTransferSubmission transfer);
    std::optional<Oot3dPicaVisualFrame>
    Finish(const Oot3dPicaDisplayTransferSubmission& topTransfer);
    // Closes the pending work as one visual frame per completed guest frame,
    // oldest first. When the host falls behind, several guest refreshes
    // accumulate: an earlier snapshot of the selected transfer's source into a
    // known top-screen display framebuffer (displayOutputs) ends one guest
    // frame. Other transfers of that source (captures into other buffers)
    // stay inside their frame. Work of the guest's next frame, drawn after the
    // selected snapshot into its source, stays pending for the next call.
    // Draws into the source on one of continuationViewports (compared by
    // corner and half extents) are not next-frame work: a guest frame that
    // draws there after its snapshot keeps them.
    std::vector<Oot3dPicaVisualFrame>
    FinishFrames(const Oot3dPicaDisplayTransferSubmission& topTransfer,
                 std::span<const uint32_t> displayOutputs,
                 std::span<const Oot3dPicaViewportState> continuationViewports = {});
    size_t PendingDrawCount() const;
    // Live plans the accumulator has not published as a frame yet. They still
    // pin the copy versions their textures reference.
    [[nodiscard]] std::span<const Oot3dPicaVulkanDrawPlan> PendingDraws()
        const noexcept {
        return mPendingDraws;
    }
    // A deferred raw TextureCopy owns one guest PPF completion. The obligation
    // is part of the ordered replay state, so it must survive save and restore
    // and be discharged by the dependency boundary, never by host-local state.
    bool HasPendingRawCopyCompletion() const;
    // Views only the unexecuted native prefix. The owning plans remain available
    // for the complete visual frame and its subsequent interpolated samples.
    bool ViewDependencyWork(Oot3dPicaVisualFrameViewSample& output);
    void CompleteDependencyWork();
    // Replaces deferred placeholders among the pending draws up to a
    // submission (oot3d_native_pica_deferred_plan.h) before they execute.
    bool ResolveDeferredDraws(
        uint64_t throughSubmissionId,
        const std::function<bool(Oot3dPicaVulkanDrawPlan&)>& resolve);
    // Plans are copied: deferred placeholders must be resolved first.
    Oot3dPicaVisualFrameAccumulatorState CaptureState() const;
    bool RestoreState(Oot3dPicaVisualFrameAccumulatorState state);

  private:
    uint64_t mNextSequence = 1;
    bool mMoveFreeFinish = true;
    std::vector<Oot3dPicaVulkanDrawPlan> mPendingDraws;
    std::vector<Oot3dPicaMemoryFillSubmission> mPendingMemoryFills;
    std::vector<Oot3dPicaDisplayTransferSubmission> mPendingDisplayTransfers;
};

class Oot3dPicaVisualContinuityTracker final {
  public:
    bool Accept(const Oot3dPicaVisualTransitionStats& transition);
    double CurrentThreshold() const;
    Oot3dPicaVisualContinuityTrackerState CaptureState() const;
    bool RestoreState(Oot3dPicaVisualContinuityTrackerState state);

  private:
    std::vector<double> mAcceptedDeltas;
};

} // namespace Oot3dNativeGame
