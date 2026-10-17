#include "oot3d_native_pica_deferred_plan.h"
#include "oot3d_switch_threads.h"

#if defined(__SWITCH__)
#include <switch.h>
#endif

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

    Oot3dPicaVulkanDrawPlan plan;
    plan.SubmissionId = submission.Id;
    plan.CommandListAddress = submission.Packet.CommandListAddress;
    plan.CommandListOffsetWords = submission.Packet.CommandListOffsetWords;
    plan.CompositionDomain = submission.Packet.CompositionDomain;
    plan.Composition = submission.Packet.Composition;
    plan.State = submission.State;
    // Aliasing pointer: the slot keeps its whole batch alive.
    plan.Deferred = std::shared_ptr<const Oot3dPicaDeferredPlanSlot>(
        batch, &slot);
    mDeferredDraws.fetch_add(1U, std::memory_order_relaxed);
    return plan;
}

void Oot3dPicaDeferredPlanBuilder::Publish(
    std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch) {
    if (batch == nullptr) {
        return;
    }
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

void Oot3dPicaDeferredPlanBuilder::BuildSlotLocked(
    Oot3dPicaDeferredPlanBatch& batch, size_t index,
    std::atomic<uint64_t>& builds, std::atomic<uint64_t>& nanoseconds) {
    using Status = Oot3dPicaDeferredPlanBatch::SlotStatus;
    if (batch.Status[index] != Status::Pending) {
        return;
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
    builds.fetch_add(1U, std::memory_order_relaxed);
    nanoseconds.fetch_add(NanosecondsSince(started), std::memory_order_relaxed);
}

void Oot3dPicaDeferredPlanBuilder::WorkerLoop() {
#if defined(__SWITCH__)
    // One step below the render worker (0x2D), which in turn is below the
    // game and audio threads (0x2C): Horizon does not time-slice equal
    // priorities, so on a shared core the renderer always preempts this
    // prebuilding, and a plan it needs first it builds itself.
    svcSetThreadPriority(threadGetCurHandle(), 0x2E);
    // Core 1, away from the render worker on core 2: sharing its core, this
    // thread only ran while the renderer waited.
    (void)SetSwitchThreadCore(kSwitchPicaPlanThreadCore);
#endif
    for (;;) {
        std::shared_ptr<Oot3dPicaDeferredPlanBatch> batch;
        {
            std::unique_lock lock(mQueueMutex);
            mQueueReady.wait(lock, [&] { return mStop || !mQueue.empty(); });
            if (mStop) {
                return;
            }
            batch = mQueue.front();
        }
        for (size_t index = 0; index < batch->Status.size(); ++index) {
            // Nobody holds a placeholder of this batch any more (a dropped
            // frame, a reset): nothing left worth building.
            if (batch.use_count() <= 2) {
                break;
            }
            {
                std::lock_guard lock(mQueueMutex);
                if (mStop) {
                    return;
                }
            }
            std::lock_guard buildLock(mBuildMutex);
            BuildSlotLocked(*batch, index, mWorkerBuilds, mWorkerNanoseconds);
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
    return stats;
}

bool ResolveOot3dPicaDeferredDrawPlan(Oot3dPicaVulkanDrawPlan& plan,
                                      std::string* error) {
    using Status = Oot3dPicaDeferredPlanBatch::SlotStatus;
    if (plan.Deferred == nullptr) {
        return true;
    }
    // Holds the batch alive past the assignment that drops plan.Deferred.
    const auto slot = std::move(plan.Deferred);
    auto& batch = *slot->Batch;
    auto& builder = *batch.Builder;
    const size_t index = slot->Index;
    {
        std::lock_guard lock(builder.mBuildMutex);
        if (index >= batch.Status.size() || !batch.Published) {
            SetError(error, "native PICA deferred plan was never published");
            return false;
        }
        builder.BuildSlotLocked(batch, index, builder.mConsumerBuilds,
                                builder.mConsumerNanoseconds);
        switch (batch.Status[index]) {
        case Status::Built:
            // Moved straight into the caller's plan (its slot reference is
            // held above); the moved-from element is reset by its next build.
            plan = std::move(batch.Plans[index]);
            batch.Status[index] = Status::Taken;
            break;
        case Status::Failed:
            SetError(error, "native PICA Vulkan plan failed: " +
                                batch.Errors[index]);
            return false;
        default:
            SetError(error, "native PICA deferred plan resolved twice");
            return false;
        }
    }
    return true;
}

bool ResolveOot3dPicaDeferredVisualFrame(Oot3dPicaVisualFrame& frame,
                                         std::string* error) {
    bool resolved = false;
    for (auto& draw : frame.Draws) {
        if (draw.Deferred == nullptr) {
            continue;
        }
        if (!ResolveOot3dPicaDeferredDrawPlan(draw, error)) {
            return false;
        }
        resolved = true;
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
