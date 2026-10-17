#include "oot3d_native_pica_deferred_plan.h"

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

std::shared_ptr<Oot3dPicaDeferredPlanBatch>
Oot3dPicaDeferredPlanBuilder::BeginBatch(
    std::vector<Oot3dPicaDrawSubmission>&& submissions) {
    auto batch = std::make_shared<Oot3dPicaDeferredPlanBatch>();
    batch->Builder = this;
    batch->Submissions = std::move(submissions);
    submissions.clear();
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
    if (batch.Plans.size() != batch.Status.size()) {
        // Plans are several kilobytes each: allocated by the first builder,
        // never on the guest thread.
        batch.Plans.resize(batch.Status.size());
        batch.Errors.resize(batch.Status.size());
    }
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
    // The submission (register packet and remaining snapshot references) is
    // no longer needed: release it here rather than on the guest thread.
    batch.Submissions[index] = {};
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
    Oot3dPicaVulkanDrawPlan built;
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
            built = std::move(batch.Plans[index]);
            batch.Plans[index] = {};
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
    plan = std::move(built);
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
