// Leaving the game with frames in flight.
//
// A simulated NativeA32WindowSession: a deferred plan builder with its own
// worker and recycled batches that own the plan bytes (uniforms, vertices),
// render-side interpolation history (previous/latest frame, as fps60 keeps
// it), a fake GL buffer that every draw uploads into (the memcpy of
// glBufferData), and the NativeRenderCommandStream that runs the frames on a
// render worker, at most two frames in flight. Members are declared in the
// order of the real session; one renderer-used member is declared after the
// stream, as the real visual sample scratch is.
//
// Every exit path must stop rendering before anything it uses is released,
// and no frame (new, interpolated or repeated sample) may run once the exit
// has begun:
// - the render thread sees the window close (Quit, HOME > Close: the window's
//   events are pumped by the frame-start command on the render thread);
// - the guest thread ends the loop while frames are queued behind a frame
//   the render thread is still executing;
// - an exception unwinds the session with frames queued.
// The session releases everything right after StopRendering, so a command
// that still ran would touch freed memory or a null buffer: run under ASan
// and TSan.
#include "oot3d_native_render_command_stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Oot3dNativeGame::NativeRenderCommandStream;

void Require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "oot3d_native_render_shutdown_tests: " << message << '\n';
        std::exit(1);
    }
}

constexpr size_t kDrawsPerFrame = 24U;
constexpr size_t kUniformBytes = 256U;
constexpr size_t kVertexBytes = 1536U;

struct Plan {
    uint64_t Sequence = 0U;
    std::vector<uint8_t> Uniforms;
    std::vector<uint8_t> Vertices;
};

// One drain pass of deferred plans, recycled with its capacity (the shape of
// Oot3dPicaDeferredPlanBatch).
struct Batch {
    uint64_t Sequence = 0U;
    std::vector<Plan> Plans;
    std::vector<uint8_t> Built;
    std::vector<uint8_t> Taken;
};

class PlanBuilder {
  public:
    PlanBuilder() : mPool(std::make_shared<Pool>()) {
        mThread = std::thread([this] { WorkerLoop(); });
    }
    ~PlanBuilder() { StopWorker(); }

    std::shared_ptr<Batch> Begin(uint64_t sequence) {
        std::unique_ptr<Batch> storage;
        {
            std::lock_guard lock(mPool->Mutex);
            if (!mPool->Free.empty()) {
                storage = std::move(mPool->Free.back());
                mPool->Free.pop_back();
            }
        }
        if (storage == nullptr) storage = std::make_unique<Batch>();
        auto pool = mPool;
        std::shared_ptr<Batch> batch(storage.release(), [pool](Batch* released) {
            // Recycling releases what the batch owns, as BatchRecycler does.
            for (auto& plan : released->Plans) plan = {};
            std::unique_ptr<Batch> owned(released);
            std::lock_guard lock(pool->Mutex);
            if (pool->Free.size() < 4U) pool->Free.push_back(std::move(owned));
        });
        batch->Sequence = sequence;
        batch->Plans.resize(kDrawsPerFrame);
        batch->Built.assign(kDrawsPerFrame, 0U);
        batch->Taken.assign(kDrawsPerFrame, 0U);
        return batch;
    }

    void Publish(std::shared_ptr<Batch> batch) {
        {
            std::lock_guard lock(mQueueMutex);
            if (mStop) return;
            mQueue.push_back(std::move(batch));
        }
        mQueueReady.notify_one();
    }

    // Takes the built plan (building it now if the worker has not).
    bool Resolve(Batch& batch, size_t index, Plan& out) {
        std::lock_guard lock(mBuildMutex);
        if (batch.Taken[index] != 0U) return false;
        BuildLocked(batch, index);
        out = std::move(batch.Plans[index]);
        batch.Taken[index] = 1U;
        return true;
    }

    void StopWorker() {
        if (!mThread.joinable()) return;
        {
            std::lock_guard lock(mQueueMutex);
            mStop = true;
        }
        mQueueReady.notify_all();
        mThread.join();
        std::lock_guard lock(mQueueMutex);
        mQueue.clear();
    }

  private:
    struct Pool {
        std::mutex Mutex;
        std::vector<std::unique_ptr<Batch>> Free;
    };

    void BuildLocked(Batch& batch, size_t index) {
        if (batch.Built[index] != 0U) return;
        auto& plan = batch.Plans[index];
        plan.Sequence = batch.Sequence;
        plan.Uniforms.assign(kUniformBytes,
                             static_cast<uint8_t>(batch.Sequence + index));
        plan.Vertices.assign(kVertexBytes,
                             static_cast<uint8_t>(batch.Sequence * 3U + index));
        batch.Built[index] = 1U;
    }

    void WorkerLoop() {
        for (;;) {
            std::shared_ptr<Batch> batch;
            {
                std::unique_lock lock(mQueueMutex);
                mQueueReady.wait(lock, [&] { return mStop || !mQueue.empty(); });
                if (mStop) return;
                batch = std::move(mQueue.front());
                mQueue.pop_front();
            }
            for (size_t index = 0; index < kDrawsPerFrame; ++index) {
                std::lock_guard lock(mBuildMutex);
                BuildLocked(*batch, index);
            }
        }
    }

    std::shared_ptr<Pool> mPool;
    std::mutex mBuildMutex;
    std::mutex mQueueMutex;
    std::condition_variable mQueueReady;
    std::deque<std::shared_ptr<Batch>> mQueue;
    bool mStop = false;
    std::thread mThread;
};

// A closed guest frame: deferred placeholders until the render thread
// resolves them.
struct Frame {
    uint64_t Sequence = 0U;
    std::shared_ptr<Batch> Placeholders;
    std::vector<Plan> Plans;
};

enum class ExitPath { RenderThreadSeesClose, GuestThreadCloses, Exception };

struct Outcome {
    uint64_t FramesSubmitted = 0U;
    uint64_t SamplesExecuted = 0U;
    uint64_t RepeatedSamples = 0U;
    uint64_t InterpolatedSamples = 0U;
    uint64_t CommandsAfterExit = 0U;
    uint64_t SamplesSkipped = 0U;
    uint64_t LastPresentedFrame = 0U;
    size_t Discarded = 0U;
};

class Session {
  public:
    Session(ExitPath path, uint64_t exitFrame)
        : mPath(path), mExitFrame(exitFrame),
          mGpuBuffer(std::make_unique<uint8_t[]>(kVertexBytes)) {
        mStream.SetMode(NativeRenderCommandStream::Mode::Worker);
    }
    // As NativeA32WindowSession: rendering stops before any member dies.
    ~Session() { StopRendering(); }

    // The frame loop and Finish().
    void Run(Outcome& outcome) {
        for (uint64_t frame = 1U;; ++frame) {
            if (!RunFrame(frame)) break;
        }
        Finish();
        outcome = mOutcome;
        outcome.Discarded = mDiscarded;
    }

  private:
    bool RunFrame(uint64_t frame) {
        // The render thread saw the exit request: leave before simulating.
        if (mStream.ShutdownRequested() || !mWindowRunning.load()) return false;
        if (mPath == ExitPath::Exception && frame == mExitFrame)
            throw std::runtime_error("guest fault while frames are queued");
        mStream.Submit([this, frame] {
            // window.StartFrame(): the events are pumped here.
            if (mPath == ExitPath::RenderThreadSeesClose && frame == mExitFrame)
                mWindowRunning = false;
            if (!mWindowRunning.load()) {
                mStream.BeginShutdown();
                mExitSeenOnRenderThread = true;
            }
        });
        // fps60: a new guest frame every second presentation.
        std::shared_ptr<Frame> completed;
        if ((frame & 1U) == 1U) {
            completed = std::make_shared<Frame>();
            completed->Sequence = frame;
            completed->Placeholders = mBuilder.Begin(frame);
            mBuilder.Publish(completed->Placeholders);
        }
        mStream.Submit([this, frame, completed] { Present(frame, completed); });
        mStream.Submit([this] {
            if (mExitSeenOnRenderThread) ++mOutcome.CommandsAfterExit;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }, true);
        ++mOutcome.FramesSubmitted;
        // GuestThreadCloses: the exit frame's presentation waits on the
        // render thread until the exit has begun, so the rest of that frame
        // and the next one are still queued when the guest thread leaves.
        if (mPath == ExitPath::GuestThreadCloses && frame == mExitFrame + 1U) {
            while (!mHeld.load())
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            mWindowRunning = false;
            return false;
        }
        return true;
    }

    // The render command of PresentVisualFrame.
    void Present(uint64_t frame, const std::shared_ptr<Frame>& completed) {
        if (mExitSeenOnRenderThread) ++mOutcome.CommandsAfterExit;
        if (mPath == ExitPath::GuestThreadCloses && frame == mExitFrame) {
            mHeld = true;
            while (!mStream.ShutdownRequested())
                std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        mOutcome.LastPresentedFrame = frame;
        if (completed != nullptr) {
            completed->Plans.resize(kDrawsPerFrame);
            for (size_t index = 0; index < kDrawsPerFrame; ++index) {
                Require(mBuilder.Resolve(*completed->Placeholders, index,
                                         completed->Plans[index]),
                        "a deferred plan resolved twice");
            }
            completed->Placeholders.reset();
            std::optional<Frame> current(std::move(*completed));
            Execute(*current, nullptr, 1.0F);
            if (mLatest.has_value()) {
                Execute(*mLatest, &*current, 0.0F);
                ++mOutcome.InterpolatedSamples;
                mPrevious = std::move(mLatest);
            }
            mLatest = std::move(current);
        } else if (mPrevious.has_value() && mLatest.has_value()) {
            // RepeatedFrame: the half step between the two last frames.
            Execute(*mPrevious, &*mLatest, 0.5F);
            ++mOutcome.RepeatedSamples;
        }
    }

    // ExecuteVisualSample + SubmitPicaDraw: every draw uploads its bytes.
    void Execute(const Frame& base, const Frame* next, float alpha) {
        if (mStream.ShutdownRequested()) {
            ++mOutcome.SamplesSkipped;
            return;
        }
        mSampleScratch.clear();
        for (const auto& plan : base.Plans) mSampleScratch.push_back(&plan);
        for (size_t index = 0; index < mSampleScratch.size(); ++index) {
            const Plan& plan = *mSampleScratch[index];
            Upload(plan.Uniforms);
            if (next == nullptr) {
                Upload(plan.Vertices);
                continue;
            }
            const Plan& to = next->Plans[index];
            mInterpolated.resize(plan.Vertices.size());
            for (size_t byte = 0; byte < mInterpolated.size(); ++byte) {
                mInterpolated[byte] = static_cast<uint8_t>(
                    plan.Vertices[byte] +
                    alpha * static_cast<float>(to.Vertices[byte] - plan.Vertices[byte]));
            }
            Upload(mInterpolated);
        }
        ++mOutcome.SamplesExecuted;
    }

    void Upload(const std::vector<uint8_t>& bytes) {
        uint8_t* const target = mGpuBuffer.get();
        Require(target != nullptr, "a draw uploaded after the GPU buffer was released");
        std::memcpy(target, bytes.data(), std::min(bytes.size(), kVertexBytes));
    }

    void StopRendering() noexcept {
        if (mRenderingStopped) return;
        mRenderingStopped = true;
        mDiscarded = mStream.Shutdown();
        mBuilder.StopWorker();
    }

    void Finish() {
        StopRendering();
        // Everything the renderer used is released right away: a command
        // still running or started after this point would fail.
        mGpuBuffer.reset();
        mPrevious.reset();
        mLatest.reset();
        mInterpolated = {};
    }

    ExitPath mPath;
    uint64_t mExitFrame;
    PlanBuilder mBuilder;
    std::unique_ptr<uint8_t[]> mGpuBuffer;
    std::optional<Frame> mPrevious;
    std::optional<Frame> mLatest;
    std::vector<uint8_t> mInterpolated;
    std::atomic<bool> mWindowRunning{true};
    std::atomic<bool> mHeld{false};
    bool mExitSeenOnRenderThread = false;  // render thread only
    Outcome mOutcome;
    size_t mDiscarded = 0U;
    bool mRenderingStopped = false;
    NativeRenderCommandStream mStream{NativeRenderCommandStream::Mode::Inline, 2U};
    // Declared after the stream, like the real sample scratch: destroyed
    // before it, so only an explicit stop in ~Session keeps it valid.
    std::vector<const Plan*> mSampleScratch;
};

void TestRenderThreadSeesClose() {
    for (uint64_t exitFrame : {2U, 3U, 9U, 40U, 41U, 120U}) {
        Outcome outcome;
        Session(ExitPath::RenderThreadSeesClose, exitFrame).Run(outcome);
        Require(outcome.CommandsAfterExit == 0U,
                "a frame command ran after the render thread saw the exit");
        Require(outcome.LastPresentedFrame < exitFrame,
                "the frame that saw the exit was still presented");
        if (exitFrame > 4U)
            Require(outcome.RepeatedSamples != 0U && outcome.InterpolatedSamples != 0U,
                    "the fps60 interpolation path was not exercised");
    }
}

void TestGuestThreadCloses() {
    for (uint64_t exitFrame : {5U, 6U, 33U, 80U}) {
        Outcome outcome;
        Session(ExitPath::GuestThreadCloses, exitFrame).Run(outcome);
        Require(outcome.LastPresentedFrame == exitFrame,
                "a frame queued behind the exit was presented");
        Require(outcome.Discarded != 0U,
                "no frame was queued behind the exit");
        Require(outcome.SamplesSkipped != 0U,
                "the frame running at the exit kept drawing samples");
    }
}

void TestExceptionUnwindsWithQueuedFrames() {
    for (uint64_t exitFrame : {4U, 17U, 60U}) {
        bool threw = false;
        try {
            Outcome outcome;
            Session(ExitPath::Exception, exitFrame).Run(outcome);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        Require(threw, "the simulated guest fault did not unwind the session");
    }
}

} // namespace

int main() {
    for (int round = 0; round < 8; ++round) {
        TestRenderThreadSeesClose();
        TestGuestThreadCloses();
        TestExceptionUnwindsWithQueuedFrames();
    }
    std::cout << "oot3d_native_render_shutdown_tests: ok\n";
    return 0;
}
