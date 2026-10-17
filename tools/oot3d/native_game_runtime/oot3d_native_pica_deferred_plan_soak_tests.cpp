// Long-run behaviour of deferred PICA planning: thousands of simulated frames
// through the same sequence the game runs (drain, placeholders, publish,
// frame close, resolution on a render thread, dependency-flush prefixes),
// checking that per-frame cost, heap traffic and every structure size stay
// constant. On the console the first version's per-frame cost grew without
// bound; this test pins the steady state.
#include "oot3d_native_pica_deferred_plan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

// Heap traffic of the whole process (every thread).
namespace {
std::atomic<uint64_t> gAllocatedBytes{0};
std::atomic<uint64_t> gAllocations{0};
} // namespace

void* operator new(std::size_t size) {
    gAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
    gAllocations.fetch_add(1U, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

using namespace Oot3dNativeGame;
using Clock = std::chrono::steady_clock;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_pica_deferred_plan_soak_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

constexpr size_t kFrames = 2400U;
constexpr size_t kDrawsPerFrame = 160U;
constexpr size_t kWindow = 200U;

// The guest-side state a PICA frontend would keep: one live packet whose
// shader program is re-uploaded every frame (a new mutation identity each
// time, like the frontend's copy-on-write storage), shared snapshot buffers
// like the submission queue's caches.
struct Frontend {
    Oot3dPicaDrawPacket Live;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Vertices;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Textures;
    uint64_t NextId = 1U;

    Frontend() {
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

    void BeginFrame(size_t frame) {
        // A shader upload: copy-on-write while older snapshots still share it.
        Live.VertexShader.Program[0] = 0x88000000U;
        Live.Registers[0x0C3U] = static_cast<uint32_t>(frame);
    }

    Oot3dPicaDrawSubmission Submit(size_t draw) {
        Live.VertexShader.FloatUniforms[3] = {static_cast<float>(draw), 0.5F, 0.25F, 1.0F};
        Live.Registers[0x0C3U + 1U] = static_cast<uint32_t>(draw % 7U);
        Oot3dPicaDrawSubmission submission(NextId++, Live);
        auto& state = submission.State;
        state.Framebuffer.ColorPhysicalAddress = draw % 9U == 8U ? 0x1F200000U : 0x1F000000U;
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
        if (draw % 2U == 0U) {
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

// The render worker: resolves each closed frame, keeps the interpolation
// history (latest and previous) and drops older frames, one frame in flight.
class RenderThread {
  public:
    RenderThread() : mThread([this] { Loop(); }) {}
    ~RenderThread() {
        {
            std::lock_guard lock(mMutex);
            mStop = true;
        }
        mWake.notify_all();
        mThread.join();
    }
    void Submit(Oot3dPicaVisualFrame frame) {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return mQueue.empty(); });
        mQueue.push_back(std::move(frame));
        mWake.notify_one();
    }
    void Drain() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return mQueue.empty() && !mBusy; });
        if (!mError.empty()) Fail(mError);
    }
    uint64_t ResolveNanoseconds() const { return mNanoseconds.load(); }

  private:
    void Loop() {
        std::unique_lock lock(mMutex);
        for (;;) {
            mWake.wait(lock, [&] { return mStop || !mQueue.empty(); });
            if (mQueue.empty()) return;
            auto frame = std::move(mQueue.front());
            mQueue.pop_front();
            mBusy = true;
            lock.unlock();
            const auto started = Clock::now();
            std::string error;
            if (!ResolveOot3dPicaDeferredVisualFrame(frame, &error)) mError = error;
            for (const auto& draw : frame.Draws) {
                if (draw.Deferred != nullptr || draw.VertexBindings.empty())
                    mError = "render thread received an unresolved plan";
            }
            mNanoseconds.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started).count()));
            mPrevious = std::move(mLatest);
            mLatest = std::move(frame);
            lock.lock();
            mBusy = false;
            mIdle.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake;
    std::condition_variable mIdle;
    std::deque<Oot3dPicaVisualFrame> mQueue;
    bool mBusy = false;
    bool mStop = false;
    std::string mError;
    std::optional<Oot3dPicaVisualFrame> mLatest;
    std::optional<Oot3dPicaVisualFrame> mPrevious;
    std::atomic<uint64_t> mNanoseconds{0};
    std::thread mThread;
};

struct FrameSample {
    double GuestMicroseconds = 0.0;
    double BuildMicroseconds = 0.0;
    uint64_t AllocatedBytes = 0;
    size_t LiveBatches = 0;
};

struct RunResult {
    std::vector<FrameSample> Frames;
    Oot3dPicaDeferredPlanStats Stats;
    double RenderResolveMicroseconds = 0.0;
};

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.empty() ? 0.0 : values[values.size() / 2U];
}

RunResult Run(bool deferred, bool worker) {
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.Configure(deferred, worker);
    Frontend frontend;
    Oot3dPicaVisualFrameAccumulator accumulator;
    std::vector<Oot3dPicaDrawSubmission> queue;    // the submission queue's vector
    std::vector<Oot3dPicaDrawSubmission> drained;  // the session's drainedDraws
    std::vector<std::weak_ptr<Oot3dPicaDeferredPlanBatch>> batches;
    RunResult result;
    std::string error;
    {
        RenderThread render;
        uint64_t completion = 1U;
        uint64_t allocatedAtFrame = gAllocatedBytes.load();
        uint64_t buildNanosecondsAtFrame = 0U;
        for (size_t frame = 0; frame < kFrames; ++frame) {
            // Whole-process heap traffic and plan-build time of the previous
            // frame (worker and render thread included).
            if (frame != 0U) {
                const uint64_t allocated = gAllocatedBytes.load();
                const auto stats = builder.Stats();
                const uint64_t build = stats.WorkerBuildNanoseconds +
                                       stats.ConsumerBuildNanoseconds +
                                       stats.SerialBuildNanoseconds;
                result.Frames.back().AllocatedBytes = allocated - allocatedAtFrame;
                result.Frames.back().BuildMicroseconds =
                    static_cast<double>(build - buildNanosecondsAtFrame) / 1000.0;
                allocatedAtFrame = allocated;
                buildNanosecondsAtFrame = build;
            }
            frontend.BeginFrame(frame);
            for (size_t draw = 0; draw < kDrawsPerFrame; ++draw) {
                queue.push_back(frontend.Submit(draw));
            }
            const auto started = Clock::now();
            // RunGuestRefresh: drain, plan (or placeholder), capture, publish.
            drained.clear();
            drained.swap(queue);
            if (deferred) {
                auto batch = builder.BeginBatch(drained);
                batches.push_back(batch);
                for (size_t index = 0; index < batch->Submissions.size(); ++index) {
                    accumulator.Append(builder.MakeDeferredPlan(batch, index));
                }
                builder.Publish(std::move(batch));
            } else {
                for (auto& submission : drained) {
                    Oot3dPicaVulkanDrawPlan plan;
                    Require(builder.Build(submission, plan, &error), error);
                    accumulator.Append(std::move(plan));
                }
            }
            // A raw-copy dependency flush every few frames: the guest waits,
            // the render worker is idle, a prefix resolves in place.
            if (frame % 5U == 4U) {
                render.Drain();
                const uint64_t through = accumulator.PendingDraws().empty()
                    ? 0U
                    : accumulator.PendingDraws()[accumulator.PendingDraws().size() / 2U].SubmissionId;
                Require(accumulator.ResolveDeferredDraws(
                            through,
                            [&](Oot3dPicaVulkanDrawPlan& plan) {
                                return ResolveOot3dPicaDeferredDrawPlan(plan, &error);
                            }),
                        error);
            }
            // PresentVisualFrame: close the frame, hand it to the renderer.
            Oot3dPicaDisplayTransferSubmission top;
            top.CompletionId = completion++;
            top.AfterDrawSubmissionId = frontend.NextId - 1U;
            top.InputPhysicalAddress = 0x1F000000U;
            top.OutputPhysicalAddress = 0x1E000000U;
            accumulator.Append(top);
            auto frames = accumulator.FinishFrames(top, {});
            Require(frames.size() == 1U && accumulator.PendingDrawCount() == 0U,
                    "frame accounting left draws behind");
            const auto guestEnd = Clock::now();
            render.Submit(std::move(frames.back()));
            FrameSample sample;
            sample.GuestMicroseconds =
                std::chrono::duration<double, std::micro>(guestEnd - started).count();
            std::erase_if(batches, [](const auto& batch) { return batch.expired(); });
            sample.LiveBatches = batches.size();
            result.Frames.push_back(sample);
        }
        render.Drain();
        result.RenderResolveMicroseconds = render.ResolveNanoseconds() / 1000.0;
    }
    builder.StopWorker();
    result.Stats = builder.Stats();
    std::erase_if(batches, [](const auto& batch) { return batch.expired(); });
    Require(deferred || batches.empty(), "serial run created batches");
    return result;
}

void Report(std::string_view name, const RunResult& run) {
    const auto window = [&](size_t first) {
        std::vector<double> times;
        std::vector<double> builds;
        double bytes = 0.0;
        size_t maximumBatches = 0;
        for (size_t frame = first; frame < first + kWindow; ++frame) {
            times.push_back(run.Frames[frame].GuestMicroseconds);
            builds.push_back(run.Frames[frame].BuildMicroseconds);
            bytes += static_cast<double>(run.Frames[frame].AllocatedBytes);
            maximumBatches = std::max(maximumBatches, run.Frames[frame].LiveBatches);
        }
        return std::tuple{Median(times), bytes / kWindow, maximumBatches, Median(builds)};
    };
    // Frames [kWindow, 2 kWindow) after warm-up against the last window
    // (whose final frame has no complete sample).
    const auto [earlyTime, earlyBytes, earlyBatches, earlyBuild] = window(kWindow);
    const auto [lateTime, lateBytes, lateBatches, lateBuild] = window(kFrames - kWindow - 1U);
    std::printf("%-14s guest_us early=%.1f late=%.1f  build_us early=%.1f late=%.1f  heap_bytes/frame early=%.0f late=%.0f  "
                "live_batches early=%zu late=%zu  worker_builds=%llu late_builds=%llu "
                "serial_builds=%llu render_resolve_ms=%.1f batches_allocated=%llu\n",
                std::string(name).c_str(), earlyTime, lateTime, earlyBuild, lateBuild, earlyBytes, lateBytes,
                earlyBatches, lateBatches,
                static_cast<unsigned long long>(run.Stats.WorkerBuilds),
                static_cast<unsigned long long>(run.Stats.ConsumerBuilds),
                static_cast<unsigned long long>(run.Stats.SerialBuilds),
                run.RenderResolveMicroseconds / 1000.0,
                static_cast<unsigned long long>(run.Stats.AllocatedBatches));
    // Constant per-frame cost: the late window is not slower than the early
    // one beyond timing noise of a shared machine (the exact checks are the
    // heap traffic and batch counts below and in main).
    Require(lateTime <= earlyTime * 2.0 + 100.0,
            std::string(name) + ": guest-thread cost per frame grows with time");
    Require(lateBuild <= earlyBuild * 2.0 + 100.0,
            std::string(name) + ": plan-build cost per frame grows with time");
    Require(lateBytes <= earlyBytes * 1.1 + 4096.0,
            std::string(name) + ": heap traffic per frame grows with time");
    Require(lateBatches <= 4U && earlyBatches <= 4U,
            std::string(name) + ": deferred batches accumulate");
}

} // namespace

int main() {
    const auto serial = Run(false, false);
    Report("serial", serial);
    const auto withWorker = Run(true, true);
    Report("worker", withWorker);
    const auto renderOnly = Run(true, false);
    Report("render-thread", renderOnly);
    // Total heap traffic of the deferred path (every thread) stays within a
    // small per-draw overhead of the serial build: no per-draw default packet
    // (64 KiB of fresh shader storage) and no per-pass multi-megabyte vectors.
    const auto total = [](const RunResult& run) {
        double bytes = 0.0;
        for (size_t frame = kFrames - kWindow - 1U; frame < kFrames - 1U; ++frame)
            bytes += static_cast<double>(run.Frames[frame].AllocatedBytes);
        return bytes / kWindow / kDrawsPerFrame;
    };
    std::printf("heap bytes per draw (whole process): serial=%.0f worker=%.0f render=%.0f\n",
                total(serial), total(withWorker), total(renderOnly));
    for (const auto* run : {&withWorker, &renderOnly}) {
        Require(total(*run) <= total(serial) + 512.0,
                "deferred planning allocates per draw beyond the serial build");
        // Batches are recycled: a handful ever allocated, none left alive.
        Require(run->Stats.AllocatedBatches <= 8U && run->Stats.LiveBatches == 0U &&
                    run->Stats.PooledBatches <= 4U && run->Stats.QueuedBatches == 0U,
                "deferred batches are not recycled");
        Require(run->Stats.DeferredDraws == kFrames * kDrawsPerFrame &&
                    run->Stats.WorkerBuilds + run->Stats.ConsumerBuilds ==
                        kFrames * kDrawsPerFrame,
                "every deferred draw is built exactly once");
    }
    std::cout << "oot3d_native_pica_deferred_plan_soak_tests: ok\n";
    return 0;
}
