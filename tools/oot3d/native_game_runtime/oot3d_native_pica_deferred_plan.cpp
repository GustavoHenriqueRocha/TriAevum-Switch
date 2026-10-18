#include "oot3d_native_pica_deferred_plan.h"
#include "oot3d_switch_threads.h"

#include <chrono>
#include <exception>
#include <utility>

namespace Oot3dNativeGame {
namespace {

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

uint64_t NanosecondsSince(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
}

} // namespace

Oot3dPicaDeferredPlanBuilder::Oot3dPicaDeferredPlanBuilder(
    Oot3dPicaVulkanShaderSourceCache& shaderCache)
    : mShaderCache(shaderCache) {}

Oot3dPicaDeferredPlanBuilder::~Oot3dPicaDeferredPlanBuilder() {
    StopWorker();
}

void Oot3dPicaDeferredPlanBuilder::Configure(bool deferred, bool worker) {
    mDeferred = deferred;
    mWorker = deferred && worker;
    if (mWorker && !mThread.joinable()) {
        {
            std::lock_guard lock(mQueueMutex);
            mStop = false;
            mStopRequested.store(false, std::memory_order_relaxed);
        }
        mThread = std::thread([this] { WorkerLoop(); });
    } else if (!mWorker) {
        StopWorker();
    }
}

void Oot3dPicaDeferredPlanBuilder::StopWorker() {
    if (!mThread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mQueueMutex);
        mStop = true;
        mStopRequested.store(true, std::memory_order_relaxed);
    }
    mQueueReady.notify_all();
    mThread.join();
    std::lock_guard lock(mQueueMutex);
    // Unbuilt slots stay pending: their plans are built when resolved.
    mQueue.clear();
    mWorker = false;
}

bool Oot3dPicaDeferredPlanBuilder::Build(Oot3dPicaDrawSubmission& submission,
                                         Oot3dPicaVulkanDrawPlan& plan,
                                         std::string* error) {
    const auto started = std::chrono::steady_clock::now();
    std::lock_guard lock(mBuildMutex);
    const bool built = BuildOot3dPicaVulkanDrawPlanAndConsumeResources(
        submission, plan, error, &mShaderCache);
    mSerialBuilds.fetch_add(1U, std::memory_order_relaxed);
    mSerialNanoseconds.fetch_add(NanosecondsSince(started),
                                 std::memory_order_relaxed);
    return built;
}

// Pooled batches: more than the few alive at once (accumulator, frames in
// flight, worker queue) would only pin memory.
constexpr size_t kPooledBatches = 4U;

void Oot3dPicaDeferredPlanBuilder::BatchRecycler::operator()(
    Oot3dPicaDeferredPlanBatch* batch) const noexcept {
    using Status = Oot3dPicaDeferredPlanBatch::SlotStatus;
    // Release what the batch still references (submission snapshots, plans
    // nobody took) but keep every vector's capacity.
    batch->Submissions.clear();
    for (size_t index = 0; index < batch->Status.size(); ++index) {
        if (batch->Status[index] == Status::Built) {
            batch->Plans[index] = {};
        }
    }
    batch->Status.clear();
    batch->Published = false;
    batch->Builder = nullptr;
    Pool->Live.fetch_sub(1U, std::memory_order_relaxed);
    std::unique_ptr<Oot3dPicaDeferredPlanBatch> owned(batch);
    std::lock_guard lock(Pool->Mutex);
    if (Pool->Free.size() < kPooledBatches) {
        Pool->Free.push_back(std::move(owned));
    }
}

std::shared_ptr<Oot3dPicaDeferredPlanBatch>
Oot3dPicaDeferredPlanBuilder::BeginBatch(
    std::vector<Oot3dPicaDrawSubmission>& drained) {
    std::unique_ptr<Oot3dPicaDeferredPlanBatch> storage;
    {
        std::lock_guard lock(mPool->Mutex);
        if (!mPool->Free.empty()) {
            storage = std::move(mPool->Free.back());
            mPool->Free.pop_back();
        }
    }
    if (storage == nullptr) {
        storage = std::make_unique<Oot3dPicaDeferredPlanBatch>();
        mPool->Allocated.fetch_add(1U, std::memory_order_relaxed);
    }
    mPool->Live.fetch_add(1U, std::memory_order_relaxed);
    std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch(storage.release(),
                                                      BatchRecycler{mPool});
    batch->Builder = this;
    batch->Published = false;
    batch->PlaceholderCount = 0U;
    // The recycled vector is empty and keeps its capacity: the submission
    // queue fills it next, without regrowing.
    batch->Submissions.clear();
    batch->Submissions.swap(drained);
    const size_t count = batch->Submissions.size();
    batch->Slots.resize(count);
    batch->Status.assign(count, Oot3dPicaDeferredPlanBatch::SlotStatus::Unused);
    for (size_t index = 0; index < count; ++index) {
        batch->Slots[index].Batch = batch.get();
        batch->Slots[index].Index = static_cast<uint32_t>(index);
    }
    return batch;
}

Oot3dPicaVulkanDrawPlan Oot3dPicaDeferredPlanBuilder::MakeDeferredPlan(
    const std::shared_ptr<Oot3dPicaDeferredPlanBatch>& batch, size_t index) {
    Oot3dPicaVulkanDrawPlan plan;
    FillDeferredPlan(batch, index, plan);
    return plan;
}

void Oot3dPicaDeferredPlanBuilder::FillDeferredPlan(
    const std::shared_ptr<Oot3dPicaDeferredPlanBatch>& batch, size_t index,
    Oot3dPicaVulkanDrawPlan& plan) {
    const auto& submission = batch->Submissions[index];
    auto& slot = batch->Slots[index];
    // The producers the built plan's textures will carry (the plan builder
    // takes exactly these, enabled slot by slot): the guest side pins them
    // while the placeholder is pending.
    slot.CopyReferences.clear();
    for (size_t texture = 0; texture < submission.State.Textures.size();
         ++texture) {
        if (!submission.State.Textures[texture].Enabled) {
            continue;
        }
        for (const auto& resource : submission.Resources) {
            if (resource.Kind == Oot3dPicaResourceKind::Texture &&
                resource.Slot == texture) {
                slot.CopyReferences.insert(slot.CopyReferences.end(),
                                           resource.CopyReferences.begin(),
                                           resource.CopyReferences.end());
                break;
            }
        }
    }
    batch->Status[index] = Oot3dPicaDeferredPlanBatch::SlotStatus::Pending;

    plan.SubmissionId = submission.Id;
    plan.CommandListAddress = submission.Packet.CommandListAddress;
    plan.CommandListOffsetWords = submission.Packet.CommandListOffsetWords;
    plan.CompositionDomain = submission.Packet.CompositionDomain;
    plan.Composition = submission.Packet.Composition;
    plan.State = submission.State;
    // Aliasing pointer: the slot keeps its whole batch alive.
    plan.Deferred = std::shared_ptr<const Oot3dPicaDeferredPlanSlot>(
        batch, &slot);
    ++batch->PlaceholderCount;
}

void Oot3dPicaDeferredPlanBuilder::Publish(
    std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch) {
    if (batch == nullptr) {
        return;
    }
    mDeferredDraws.fetch_add(batch->PlaceholderCount, std::memory_order_relaxed);
    {
        // Ownership barrier: everything the guest thread wrote into the batch
        // happens before any build of it.
        std::lock_guard buildLock(mBuildMutex);
        batch->Published = true;
    }
    if (!mWorker) {
        return;
    }
    {
        std::lock_guard lock(mQueueMutex);
        mQueue.push_back(std::move(batch));
    }
    mQueueReady.notify_one();
}

void Oot3dPicaDeferredPlanBuilder::LockBuild(
    std::unique_lock<std::mutex>& lock, std::atomic<uint64_t>& waits,
    std::atomic<uint64_t>& nanoseconds) {
    if (lock.try_lock()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    lock.lock();
    waits.fetch_add(1U, std::memory_order_relaxed);
    nanoseconds.fetch_add(NanosecondsSince(started), std::memory_order_relaxed);
}

uint64_t Oot3dPicaDeferredPlanBuilder::BuildSlotLocked(
    Oot3dPicaDeferredPlanBatch& batch, size_t index) {
    using Status = Oot3dPicaDeferredPlanBatch::SlotStatus;
    if (batch.Status[index] != Status::Pending) {
        return 0U;
    }
    const auto started = std::chrono::steady_clock::now();
    if (batch.Plans.size() < batch.Status.size()) {
        // Plans are several kilobytes each: grown by a builder (never on the
        // guest thread) and kept by the recycled batch.
        batch.Plans.resize(batch.Status.size());
        batch.Errors.resize(batch.Status.size());
    }
    batch.Errors[index].clear();
    bool built = false;
    try {
        built = BuildOot3dPicaVulkanDrawPlanAndConsumeResources(
            batch.Submissions[index], batch.Plans[index],
            &batch.Errors[index], &mShaderCache);
    } catch (const std::exception& exception) {
        batch.Errors[index] = exception.what();
    } catch (...) {
        batch.Errors[index] = "native PICA deferred plan build threw";
    }
    // Release the snapshot payloads the plan did not take. Never assign a
    // default submission here: a default packet allocates four fresh 16 KiB
    // shader stores (64 KiB per draw), the heap churn that made the console's
    // cost grow with play time. The packet's shared shader and LUT stores go
    // when the batch is recycled.
    batch.Submissions[index].Resources.clear();
    batch.Status[index] = built ? Status::Built : Status::Failed;
    const uint64_t spent = NanosecondsSince(started);
    return spent != 0U ? spent : 1U;
}

void Oot3dPicaDeferredPlanBuilder::WorkerLoop() {
    // Switch: 0x2E, one step below the render worker (0x2D), which in turn is
    // below the game (0x2C) and audio (0x2B): if the two ever share a core
    // the renderer preempts this prebuilding, and a plan it needs first it
    // builds itself. Core 1 (layout fixed) or 1|2 (float), away from the
    // render worker's core 2.
    PlaceSwitchThread(SwitchThreadRole::PicaPlan);
    for (;;) {
        std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch;
        {
            std::unique_lock lock(mQueueMutex);
            if (!mStop && mQueue.empty()) {
                const auto idleStart = std::chrono::steady_clock::now();
                mQueueReady.wait(lock,
                                 [&] { return mStop || !mQueue.empty(); });
                mWorkerIdleNanoseconds.fetch_add(NanosecondsSince(idleStart),
                                                 std::memory_order_relaxed);
            }
            if (mStop) {
                return;
            }
            batch = mQueue.front();
        }
        NoteSwitchThreadCore();
        // Counters published once per batch, not per draw (A57 has no LSE
        // atomics: each read-modify-write is an exclusive-monitor loop).
        uint64_t builds = 0U;
        uint64_t nanoseconds = 0U;
        bool stopping = false;
        for (size_t index = 0; index < batch->Status.size(); ++index) {
            // Nobody holds a placeholder of this batch any more (a dropped
            // frame, a reset): nothing left worth building.
            if (batch.use_count() <= 2) {
                break;
            }
            if (mStopRequested.load(std::memory_order_relaxed)) {
                stopping = true;
                break;
            }
            std::unique_lock buildLock(mBuildMutex, std::defer_lock);
            LockBuild(buildLock, mWorkerLockWaits, mWorkerLockWaitNanoseconds);
            const uint64_t spent = BuildSlotLocked(*batch, index);
            if (spent != 0U) {
                ++builds;
                nanoseconds += spent;
            }
        }
        mWorkerBuilds.fetch_add(builds, std::memory_order_relaxed);
        mWorkerNanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
        if (stopping) {
            return;
        }
        std::lock_guard lock(mQueueMutex);
        if (!mQueue.empty() && mQueue.front() == batch) {
            mQueue.pop_front();
        }
    }
}

Oot3dPicaDeferredPlanStats Oot3dPicaDeferredPlanBuilder::Stats() const noexcept {
    Oot3dPicaDeferredPlanStats stats;
    stats.LiveBatches = mPool->Live.load(std::memory_order_relaxed);
    stats.AllocatedBatches = mPool->Allocated.load(std::memory_order_relaxed);
    {
        std::lock_guard lock(mPool->Mutex);
        stats.PooledBatches = mPool->Free.size();
    }
    {
        std::lock_guard lock(mQueueMutex);
        stats.QueuedBatches = mQueue.size();
    }
    stats.DeferredDraws = mDeferredDraws.load(std::memory_order_relaxed);
    stats.WorkerBuilds = mWorkerBuilds.load(std::memory_order_relaxed);
    stats.ConsumerBuilds = mConsumerBuilds.load(std::memory_order_relaxed);
    stats.SerialBuilds = mSerialBuilds.load(std::memory_order_relaxed);
    stats.WorkerBuildNanoseconds =
        mWorkerNanoseconds.load(std::memory_order_relaxed);
    stats.ConsumerBuildNanoseconds =
        mConsumerNanoseconds.load(std::memory_order_relaxed);
    stats.SerialBuildNanoseconds =
        mSerialNanoseconds.load(std::memory_order_relaxed);
    stats.ConsumerLockWaits = mConsumerLockWaits.load(std::memory_order_relaxed);
    stats.ConsumerLockWaitNanoseconds =
        mConsumerLockWaitNanoseconds.load(std::memory_order_relaxed);
    stats.WorkerLockWaits = mWorkerLockWaits.load(std::memory_order_relaxed);
    stats.WorkerLockWaitNanoseconds =
        mWorkerLockWaitNanoseconds.load(std::memory_order_relaxed);
    stats.ConsumerLocks = mConsumerLocks.load(std::memory_order_relaxed);
    stats.WorkerIdleNanoseconds =
        mWorkerIdleNanoseconds.load(std::memory_order_relaxed);
    return stats;
}

// Resolution of placeholders under the build mutex (friend of the builder).
class DeferredPlanResolver {
  public:
    // One run of resolutions under one acquisition of a builder's build
    // mutex. It holds a reference to the run's batch, so no placeholder
    // released inside the run can recycle the batch (vector clears) while
    // the worker is locked out; the batch reference goes after the unlock.
    class Run {
      public:
        Run() = default;
        Run(const Run&) = delete;
        Run& operator=(const Run&) = delete;
        ~Run() { Close(); }

        [[nodiscard]] bool Open() const noexcept { return mLock.owns_lock(); }
        [[nodiscard]] const Oot3dPicaDeferredPlanBatch* Batch() const noexcept {
            return mBatch;
        }
        [[nodiscard]] uint32_t Resolved() const noexcept { return mResolved; }
        [[nodiscard]] bool BuiltHere() const noexcept { return mBuiltHere; }

        void Begin(const std::shared_ptr<const Oot3dPicaDeferredPlanSlot>& slot) {
            mBatch = slot->Batch;
            mBuilder = mBatch->Builder;
            mKeeper = slot;
            mLock = std::unique_lock<std::mutex>(mBuilder->mBuildMutex,
                                                 std::defer_lock);
            mBuilder->LockBuild(mLock, mBuilder->mConsumerLockWaits,
                                mBuilder->mConsumerLockWaitNanoseconds);
            mResolved = 0U;
            mBuiltHere = false;
        }

        void Close() {
            if (mLock.owns_lock()) {
                mLock.unlock();
            }
            if (mBuilder != nullptr) {
                mBuilder->mConsumerLocks.fetch_add(1U, std::memory_order_relaxed);
                if (mBuilds != 0U) {
                    mBuilder->mConsumerBuilds.fetch_add(
                        mBuilds, std::memory_order_relaxed);
                    mBuilder->mConsumerNanoseconds.fetch_add(
                        mNanoseconds, std::memory_order_relaxed);
                }
            }
            mBuilds = 0U;
            mNanoseconds = 0U;
            mBuilder = nullptr;
            mBatch = nullptr;
            mKeeper.reset();  // after the unlock: may recycle the batch
        }

        // `slot` was moved out of `plan` by the caller (who keeps it alive);
        // the run's mutex is held and the slot belongs to the run's batch.
        bool Resolve(const Oot3dPicaDeferredPlanSlot& slot,
                     Oot3dPicaVulkanDrawPlan& plan, std::string* error) {
            using Status = Oot3dPicaDeferredPlanBatch::SlotStatus;
            auto& batch = *slot.Batch;
            const size_t index = slot.Index;
            ++mResolved;
            if (index >= batch.Status.size() || !batch.Published) {
                SetError(error, "native PICA deferred plan was never published");
                return false;
            }
            const uint64_t spent = mBuilder->BuildSlotLocked(batch, index);
            if (spent != 0U) {
                ++mBuilds;
                mNanoseconds += spent;
                mBuiltHere = true;
            }
            switch (batch.Status[index]) {
            case Status::Built:
                // Moved straight into the caller's plan; the moved-from
                // element is reset by its next build.
                plan = std::move(batch.Plans[index]);
                batch.Status[index] = Status::Taken;
                return true;
            case Status::Failed:
                SetError(error, "native PICA Vulkan plan failed: " +
                                    batch.Errors[index]);
                return false;
            default:
                SetError(error, "native PICA deferred plan resolved twice");
                return false;
            }
        }

      private:
        std::unique_lock<std::mutex> mLock;
        Oot3dPicaDeferredPlanBuilder* mBuilder = nullptr;
        const Oot3dPicaDeferredPlanBatch* mBatch = nullptr;
        std::shared_ptr<const Oot3dPicaDeferredPlanSlot> mKeeper;
        uint32_t mResolved = 0U;
        uint64_t mBuilds = 0U;
        uint64_t mNanoseconds = 0U;
        // True once this run built a slot itself (the worker had not).
        bool mBuiltHere = false;
    };
};

bool ResolveOot3dPicaDeferredDrawPlan(Oot3dPicaVulkanDrawPlan& plan,
                                      std::string* error) {
    if (plan.Deferred == nullptr) {
        return true;
    }
    // Holds the batch alive past the assignment that drops plan.Deferred.
    const auto slot = std::move(plan.Deferred);
    DeferredPlanResolver::Run run;
    run.Begin(slot);
    return run.Resolve(*slot, plan, error);
}

namespace {
// Draws resolved under one acquisition of the build mutex, at most: bounds
// how long the plan worker waits for the render thread.
constexpr uint32_t kResolveRunLength = 32U;
} // namespace

bool ResolveOot3dPicaDeferredVisualFrame(Oot3dPicaVisualFrame& frame,
                                         std::string* error) {
    bool resolved = false;
    {
        // Consecutive placeholders of one batch share a lock acquisition
        // (instead of a lock, an unlock and a batch reference per draw). A
        // run ends at another batch, after kResolveRunLength draws, or right
        // after a draw the worker had not built yet (built here): the worker
        // then gets the mutex for the next slot, as with a lock per draw.
        DeferredPlanResolver::Run run;
        for (auto& draw : frame.Draws) {
            if (draw.Deferred == nullptr) {
                continue;
            }
            const auto slot = std::move(draw.Deferred);
            if (run.Open() &&
                (run.Batch() != slot->Batch ||
                 run.Resolved() >= kResolveRunLength || run.BuiltHere())) {
                run.Close();
            }
            if (!run.Open()) {
                run.Begin(slot);
            }
            if (!run.Resolve(*slot, draw, error)) {
                return false;
            }
            resolved = true;
        }
    }
    if (resolved && frame.StrictDrawIdentities.size() != frame.Draws.size()) {
        frame.StrictDrawIdentities.clear();
        frame.StrictDrawIdentities.reserve(frame.Draws.size());
        for (const auto& draw : frame.Draws) {
            frame.StrictDrawIdentities.push_back(
                ComputeOot3dPicaVisualDrawIdentity(draw));
        }
    }
    return true;
}

} // namespace Oot3dNativeGame
