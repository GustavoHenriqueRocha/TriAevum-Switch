#pragma once

#include "oot3d_native_pica_visual_frame.h"
#include "oot3d_native_pica_vulkan_plan.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Oot3dNativeGame {

class Oot3dPicaDeferredPlanBuilder;

// Draw submissions of one drain pass whose Vulkan/GL plans are built off the
// guest thread. Every submission is already an immutable snapshot (register
// packet, vertex/index/texture bytes captured at the guest's submission), so
// building its plan later, on another thread, yields the same plan the guest
// thread would have built: nothing here reads guest memory.
//
// Ownership: until Publish, only the guest thread touches the batch. After
// Publish, Submissions, Plans and Status belong to the builder and are only
// touched under its mutex; Slots are immutable and may be read by anyone.
//
// Batches are recycled (Oot3dPicaDeferredPlanBuilder's pool): every vector
// keeps its capacity, so a steady game allocates no batch storage per drain
// pass. Plans and Errors may hold more elements than the current Status.
struct Oot3dPicaDeferredPlanBatch {
    enum class SlotStatus : uint8_t { Unused, Pending, Built, Failed, Taken };

    Oot3dPicaDeferredPlanBuilder* Builder = nullptr;
    std::vector<Oot3dPicaDrawSubmission> Submissions;
    std::vector<Oot3dPicaDeferredPlanSlot> Slots;
    std::vector<Oot3dPicaVulkanDrawPlan> Plans;
    std::vector<SlotStatus> Status;
    std::vector<std::string> Errors;
    bool Published = false;
    // Placeholders made by MakeDeferredPlan (guest thread only): counted
    // into the builder's stats once, at Publish, not with an atomic per draw.
    uint32_t PlaceholderCount = 0U;
};

struct Oot3dPicaDeferredPlanStats {
    // Batches alive (placeholders, frames in flight, worker queue) and kept
    // for reuse; both must stay small and constant.
    uint64_t LiveBatches = 0;
    uint64_t PooledBatches = 0;
    uint64_t QueuedBatches = 0;
    uint64_t AllocatedBatches = 0;
    uint64_t DeferredDraws = 0;
    uint64_t WorkerBuilds = 0;
    uint64_t ConsumerBuilds = 0;
    uint64_t SerialBuilds = 0;
    uint64_t WorkerBuildNanoseconds = 0;
    uint64_t ConsumerBuildNanoseconds = 0;
    uint64_t SerialBuildNanoseconds = 0;
    // Blocked on the build mutex (contention between the render thread and
    // the worker): resolves (render thread / dependency flush) and worker.
    uint64_t ConsumerLockWaits = 0;
    uint64_t ConsumerLockWaitNanoseconds = 0;
    uint64_t WorkerLockWaits = 0;
    uint64_t WorkerLockWaitNanoseconds = 0;
    // Build-mutex acquisitions by the resolves (one per run of draws).
    uint64_t ConsumerLocks = 0;
    // Worker without a batch to build.
    uint64_t WorkerIdleNanoseconds = 0;
};

// Builds PICA draw plans for the guest thread. Serial builds (Build) run on
// the caller; deferred batches are built by an optional background worker and
// finished on demand by whoever resolves a plan (the render worker, or the
// guest thread inside a synchronous render command). All builds share one
// shader-source cache and are serialized by one mutex, so the cache never sees
// two threads at once.
class Oot3dPicaDeferredPlanBuilder final {
  public:
    explicit Oot3dPicaDeferredPlanBuilder(
        Oot3dPicaVulkanShaderSourceCache& shaderCache);
    ~Oot3dPicaDeferredPlanBuilder();

    Oot3dPicaDeferredPlanBuilder(const Oot3dPicaDeferredPlanBuilder&) = delete;
    Oot3dPicaDeferredPlanBuilder& operator=(
        const Oot3dPicaDeferredPlanBuilder&) = delete;

    // Deferred planning is used only while enabled; the worker pre-builds
    // published batches (otherwise plans are built when resolved).
    void Configure(bool deferred, bool worker);
    [[nodiscard]] bool Deferred() const noexcept { return mDeferred; }
    [[nodiscard]] bool WorkerEnabled() const noexcept { return mWorker; }
    // The worker also decodes the new textures of each plan it builds into
    // Fast::Oot3d::SharedPicaPredecodeCache(), which the GL backend uploads
    // from instead of decoding on the render thread (pica_texture_predecode.h).
    void SetPredecodeTextures(bool enabled) noexcept {
        mPredecodeTextures.store(enabled, std::memory_order_relaxed);
    }
    // Joins the worker (it finishes at most the plan it is building). Plans
    // still pending are built when resolved.
    void StopWorker();

    // Builds immediately on the calling thread (the serial path).
    bool Build(Oot3dPicaDrawSubmission& submission,
               Oot3dPicaVulkanDrawPlan& plan, std::string* error = nullptr);

    // Takes the drained submissions by swapping vectors: `drained` comes back
    // empty with the capacity of a recycled batch, ready for the submission
    // queue to fill again. No submission is copied.
    std::shared_ptr<Oot3dPicaDeferredPlanBatch> BeginBatch(
        std::vector<Oot3dPicaDrawSubmission>& drained);
    // The placeholder plan of one submission, before Publish: identity, the
    // decoded state (framebuffer, viewport) the guest-side frame accounting
    // reads, and the copy producers its textures reference. Everything else
    // is filled when the plan is resolved.
    Oot3dPicaVulkanDrawPlan MakeDeferredPlan(
        const std::shared_ptr<Oot3dPicaDeferredPlanBatch>& batch,
        size_t index);
    // The same placeholder written into a default-constructed plan that
    // already sits where it will stay (the accumulator's pending vector),
    // so the guest thread moves no plan (config/offload_plan).
    void FillDeferredPlan(
        const std::shared_ptr<Oot3dPicaDeferredPlanBatch>& batch,
        size_t index, Oot3dPicaVulkanDrawPlan& plan);
    // Hands the batch over; the guest thread must not touch its submissions
    // afterwards.
    void Publish(std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch);

    [[nodiscard]] Oot3dPicaDeferredPlanStats Stats() const noexcept;

  private:
    friend bool ResolveOot3dPicaDeferredDrawPlan(Oot3dPicaVulkanDrawPlan&,
                                                 std::string*);
    friend class DeferredPlanResolver;

    // Outlives the builder: a batch may be released by the last frame that
    // holds one of its placeholders, on any thread.
    struct BatchPool {
        std::mutex Mutex;
        std::vector<std::unique_ptr<Oot3dPicaDeferredPlanBatch>> Free;
        std::atomic<uint64_t> Live{0};
        std::atomic<uint64_t> Allocated{0};
    };
    struct BatchRecycler {
        std::shared_ptr<BatchPool> Pool;
        void operator()(Oot3dPicaDeferredPlanBatch* batch) const noexcept;
    };

    // Builds a pending slot; returns the nanoseconds spent (0: nothing to
    // build). Callers accumulate and publish their counters in bulk.
    uint64_t BuildSlotLocked(Oot3dPicaDeferredPlanBatch& batch, size_t index);
    // Takes the build mutex, timing it only when it is contended.
    void LockBuild(std::unique_lock<std::mutex>& lock,
                   std::atomic<uint64_t>& waits,
                   std::atomic<uint64_t>& nanoseconds);
    void WorkerLoop();

    Oot3dPicaVulkanShaderSourceCache& mShaderCache;
    std::shared_ptr<BatchPool> mPool = std::make_shared<BatchPool>();
    bool mDeferred = false;
    bool mWorker = false;
    std::atomic<bool> mPredecodeTextures{false};
    std::mutex mBuildMutex;
    mutable std::mutex mQueueMutex;
    std::condition_variable mQueueReady;
    std::deque<std::shared_ptr<Oot3dPicaDeferredPlanBatch>> mQueue;
    bool mStop = false;
    // Mirror of mStop the worker polls between slots without a lock.
    std::atomic<bool> mStopRequested{false};
    std::thread mThread;
    std::atomic<uint64_t> mDeferredDraws{0};
    std::atomic<uint64_t> mWorkerBuilds{0};
    std::atomic<uint64_t> mConsumerBuilds{0};
    std::atomic<uint64_t> mSerialBuilds{0};
    std::atomic<uint64_t> mWorkerNanoseconds{0};
    std::atomic<uint64_t> mConsumerNanoseconds{0};
    std::atomic<uint64_t> mSerialNanoseconds{0};
    std::atomic<uint64_t> mConsumerLockWaits{0};
    std::atomic<uint64_t> mConsumerLockWaitNanoseconds{0};
    std::atomic<uint64_t> mWorkerLockWaits{0};
    std::atomic<uint64_t> mWorkerLockWaitNanoseconds{0};
    std::atomic<uint64_t> mConsumerLocks{0};
    std::atomic<uint64_t> mWorkerIdleNanoseconds{0};
};

// Replaces a deferred placeholder with its built plan (no-op for a built
// plan). Builds it now if the worker has not. Not for two threads on the same
// plan; each placeholder resolves once.
bool ResolveOot3dPicaDeferredDrawPlan(Oot3dPicaVulkanDrawPlan& plan,
                                      std::string* error = nullptr);

// Resolves every draw of a frame and completes the strict identities that
// FinishFrames leaves to the resolution of a frame holding placeholders.
bool ResolveOot3dPicaDeferredVisualFrame(Oot3dPicaVisualFrame& frame,
                                         std::string* error = nullptr);

} // namespace Oot3dNativeGame
