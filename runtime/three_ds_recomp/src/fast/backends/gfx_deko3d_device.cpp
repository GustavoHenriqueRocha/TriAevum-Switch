// deko3d device layer (Switch). See gfx_deko3d_device.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_device.h"

#include "fast/backends/gfx_deko3d.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>
#include <sstream>

namespace Fast::Deko3d {
namespace {
std::mutex gLogMutex;
bool gLogTruncated = false;
constexpr const char* kLogPath = "sdmc:/switch/oot3dre/logs/deko3d.txt";

// dkperf: written by the render thread, taken by the perf window (game
// thread).
std::atomic<bool> gPerfLive{false};
std::atomic<uint64_t> gPerfFrames{0};
std::atomic<uint64_t> gPerfGpuNs{0};
std::atomic<uint64_t> gPerfGpuMaxNs{0};
std::atomic<uint64_t> gPerfSceneNs{0};
std::atomic<uint64_t> gPerfWaitNs{0};
std::atomic<uint64_t> gPerfTailNs{0};
std::atomic<uint64_t> gPerfBusyMaxNs{0};
std::atomic<uint64_t> gPerfSplitFrames{0};
// Render-thread (CPU) waits of the frame: the fence of the frame in flight,
// the window acquire, the present; slow = over 4 ms (p6n: the swap took
// 14-18 ms in the frames that missed a refresh).
struct CpuWait {
    std::atomic<uint64_t> Ns{0};
    std::atomic<uint64_t> MaxNs{0};
    std::atomic<uint64_t> Slow{0};
    std::atomic<uint64_t> Count{0};
};
CpuWait gFenceWait;
CpuWait gAcquireWait;
CpuWait gPresentWait;
std::atomic<uint64_t> gSwapAcquires{0};

void StoreMax(std::atomic<uint64_t>& max, uint64_t value) {
    uint64_t seen = max.load(std::memory_order_relaxed);
    while (value > seen && !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

void NoteCpuWait(CpuWait& wait, uint64_t ns) {
    wait.Ns.fetch_add(ns, std::memory_order_relaxed);
    wait.Count.fetch_add(1, std::memory_order_relaxed);
    if (ns > 4'000'000U) {
        wait.Slow.fetch_add(1, std::memory_order_relaxed);
    }
    StoreMax(wait.MaxNs, ns);
}

// Deko3dTagFrame / Deko3dTakeGpuFrame (render thread only).
FrameTag gPendingTag;
constexpr size_t kGpuFrameRing = 8U;
std::array<Fast::Deko3dGpuFrame, kGpuFrameRing> gGpuFrames{};
size_t gGpuFrameHead = 0U;
size_t gGpuFrameCount = 0U;
std::atomic<uint64_t> gPerfSubmits{0};
std::atomic<uint64_t> gPerfCommandBytes{0};
std::atomic<bool> gPerfSyncCheck{false};

// Report timestamps count GPU ticks (614.4 MHz on the Tegra X1).
constexpr uint64_t GpuTicksToNs(uint64_t ticks) {
    return ticks * 625U / 384U;
}
constexpr uint32_t kTimestampBytes = 16U; // DkCounter report: value, timestamp
// Per slot: frame top, scene end (before the acquire), after the acquire, frame end.
// The acquire makes the GPU wait for the display to release the image; that wait
// is not GPU work, so busy = scene + tail.
constexpr uint32_t kTimestampSlotBytes = 4U * kTimestampBytes;
static_assert(kFramesInFlight * kTimestampSlotBytes <= DK_MEMBLOCK_ALIGNMENT, "timestamps fit one block");

uint64_t NowNanoseconds() {
    return armTicksToNs(armGetSystemTick());
}

void DebugCallback(void*, const char* context, DkResult result, const char* message) {
    Log("deko3d debug: %s: result %d: %s", context != nullptr ? context : "?", static_cast<int>(result),
        message != nullptr ? message : "");
}
} // namespace

void Log(const char* format, ...) {
    std::lock_guard lock(gLogMutex);
    FILE* file = std::fopen(kLogPath, gLogTruncated ? "a" : "w");
    gLogTruncated = true;
    if (file == nullptr) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

Device::~Device() {
    Destroy();
}

DkMemBlock Device::MakeBlock(uint32_t size, uint32_t flags, const char* what) {
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, mDevice, AlignUp(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    DkMemBlock block = dkMemBlockCreate(&maker);
    if (block == nullptr) {
        Log("memory block for %s (%u KiB) failed", what, size >> 10);
    }
    return block;
}

bool Device::Create(uint32_t width, uint32_t height, std::string* error) {
    const auto fail = [&](const char* what) {
        Log("create failed: %s", what);
        if (error != nullptr) {
            *error = std::string("deko3d: ") + what;
        }
        Destroy();
        return false;
    };
    mWidth = width;
    mHeight = height;
    Log("deko3d: creating device for a %ux%u window", width, height);

    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    deviceMaker.cbDebug = &DebugCallback;
    deviceMaker.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginUpperLeft;
    mDevice = dkDeviceCreate(&deviceMaker);
    if (mDevice == nullptr) {
        return fail("dkDeviceCreate");
    }

    // Window images: render target + present, compressed.
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, mDevice);
    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = width;
    layoutMaker.dimensions[1] = height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layoutMaker);
    const uint32_t imageAlignment = dkImageLayoutGetAlignment(&layout);
    const uint32_t imageSize = AlignUp(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)), imageAlignment);
    mSwapchainMemory = MakeBlock(imageSize * kSwapchainImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image,
                                 "window images");
    if (mSwapchainMemory == nullptr) {
        return fail("window image memory");
    }
    std::array<const DkImage*, kSwapchainImages> images{};
    for (uint32_t index = 0; index < kSwapchainImages; ++index) {
        dkImageInitialize(&mSwapchainImages[index], &layout, mSwapchainMemory, index * imageSize);
        dkImageViewDefaults(&mSwapchainViews[index], &mSwapchainImages[index]);
        images[index] = &mSwapchainImages[index];
    }
    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, mDevice, nwindowGetDefault(), images.data(), kSwapchainImages);
    mSwapchain = dkSwapchainCreate(&swapchainMaker);
    if (mSwapchain == nullptr) {
        return fail("dkSwapchainCreate");
    }
    dkSwapchainSetSwapInterval(mSwapchain, 1);

    // One queue; zcull off (it lost tiles of decals in Aurora's renderer).
    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, mDevice);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio | DkQueueFlags_DisableZcull;
    queueMaker.commandMemorySize = 1U << 20;
    queueMaker.flushThreshold = 128U << 10;
    // Shaders that spill need more than the default 2 KiB per warp
    // (ShaderStore refuses the ones above it).
    queueMaker.perWarpScratchMemorySize = kPerWarpScratchBytes;
    mQueue = dkQueueCreate(&queueMaker);
    if (mQueue == nullptr) {
        return fail("dkQueueCreate");
    }

    mCommandMemory = MakeBlock(kCommandSliceBytes * kFramesInFlight,
                               DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command memory");
    if (mCommandMemory == nullptr) {
        return fail("command memory");
    }
    DkCmdBufMaker cmdMaker;
    dkCmdBufMakerDefaults(&cmdMaker, mDevice);
    cmdMaker.userData = this;
    cmdMaker.cbAddMem = &Device::AddCommandMemory;
    mCmd = dkCmdBufCreate(&cmdMaker);
    if (mCmd == nullptr) {
        return fail("dkCmdBufCreate");
    }
    mTimestamps = MakeBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached,
                            "timestamps");
    if (std::FILE* file = std::fopen("sdmc:/switch/oot3dre/config/deko3d_sync_check", "rb")) {
        char word[64];
        while (std::fscanf(file, "%63s", word) == 1) {
            if (std::strcmp(word, "nowait") == 0) {
                mSyncNoWait = true;
            } else {
                mSyncPoints.emplace_back(word);
            }
        }
        std::fclose(file);
        mSyncCheck = true;
        std::string points;
        for (const std::string& point : mSyncPoints) {
            points += ' ';
            points += point;
        }
        Log("sync check on (slow; diagnosis only): %s at%s%s", mSyncNoWait ? "submit only" : "submit and wait",
            points.empty() ? " every operation" : points.c_str(), "");
    }
    gPerfSyncCheck.store(mSyncCheck, std::memory_order_relaxed);
    gPerfLive.store(true, std::memory_order_release);
    Log("deko3d: ready (window image %u KiB x %u, %u frames in flight)", imageSize >> 10, kSwapchainImages,
        kFramesInFlight);
    return true;
}

void Device::Destroy() noexcept {
    if (mQueue != nullptr) {
        dkQueueWaitIdle(mQueue);
    }
    if (mCmd != nullptr) {
        dkCmdBufDestroy(mCmd);
        mCmd = nullptr;
    }
    for (FrameSlot& slot : mSlots) {
        for (auto& [block, size] : slot.CommandOverflow) {
            (void)size;
            dkMemBlockDestroy(block);
        }
        for (DkMemBlock block : slot.StreamBlocks) {
            dkMemBlockDestroy(block);
        }
        slot = FrameSlot{};
    }
    if (mQueue != nullptr) {
        dkQueueDestroy(mQueue);
        mQueue = nullptr;
    }
    if (mSwapchain != nullptr) {
        dkSwapchainDestroy(mSwapchain);
        mSwapchain = nullptr;
    }
    if (mSwapchainMemory != nullptr) {
        dkMemBlockDestroy(mSwapchainMemory);
        mSwapchainMemory = nullptr;
    }
    if (mCommandMemory != nullptr) {
        dkMemBlockDestroy(mCommandMemory);
        mCommandMemory = nullptr;
    }
    if (mTimestamps != nullptr) {
        dkMemBlockDestroy(mTimestamps);
        mTimestamps = nullptr;
    }
    gPerfLive.store(false, std::memory_order_release);
    if (mDevice != nullptr) {
        dkDeviceDestroy(mDevice);
        mDevice = nullptr;
    }
    mFrameOpen = false;
    mWindowSlot = -1;
}

void Device::AddCommandMemory(void* user, DkCmdBuf, size_t minimumSize) {
    static_cast<Device*>(user)->FeedCommandMemory(minimumSize);
}

// The command buffer asks for memory while recording: the next chunk of this
// slot's slice, then the slot's overflow blocks (kept for later frames).
void Device::FeedCommandMemory(size_t minimumSize) {
    FrameSlot& slot = mSlots[mSlot];
    const uint32_t want = AlignUp(static_cast<uint32_t>(std::max<size_t>(minimumSize, kCommandChunkBytes)),
                                  DK_MEMBLOCK_ALIGNMENT);
    if (slot.CommandUsed + want + kCommandGuardBytes <= kCommandSliceBytes) {
        dkCmdBufAddMemory(mCmd, mCommandMemory, mSlot * kCommandSliceBytes + slot.CommandUsed, want);
        slot.CommandUsed += want + kCommandGuardBytes;
        mStats.CommandBytes += want;
        return;
    }
    if (slot.CommandOverflowUsed == 0) {
        ++mStats.CommandOverflows;
    }
    while (slot.CommandOverflowUsed < slot.CommandOverflow.size()) {
        auto [block, size] = slot.CommandOverflow[slot.CommandOverflowUsed++];
        if (size >= want + kCommandGuardBytes) {
            dkCmdBufAddMemory(mCmd, block, 0, size - kCommandGuardBytes);
            mStats.CommandBytes += size;
            return;
        }
    }
    const uint32_t size = std::max<uint32_t>(want + kCommandGuardBytes, 1U << 20);
    DkMemBlock block = MakeBlock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command overflow");
    if (block == nullptr) {
        Log("command memory exhausted (frame %llu)", static_cast<unsigned long long>(mFrame));
        return;
    }
    slot.CommandOverflow.emplace_back(block, size);
    slot.CommandOverflowUsed = slot.CommandOverflow.size();
    dkCmdBufAddMemory(mCmd, block, 0, size - kCommandGuardBytes);
    mStats.CommandBytes += size;
}

void Device::BeginFrame() {
    if (mFrameOpen || mDevice == nullptr) {
        return;
    }
    mSlot = static_cast<uint32_t>(mFrame % kFramesInFlight);
    FrameSlot& slot = mSlots[mSlot];
    if (slot.Fenced) {
        const uint64_t fenceBegin = NowNanoseconds();
        for (int waited = 0; waited < 5; ++waited) {
            const DkResult result = dkFenceWait(&slot.Fence, 2'000'000'000LL);
            if (result == DkResult_Success) {
                break;
            }
            ++mStats.FenceTimeouts;
            Log("frame %llu: the GPU has not finished frame %llu after %d s (result %d)%s",
                static_cast<unsigned long long>(mFrame), static_cast<unsigned long long>(mFrame - kFramesInFlight),
                (waited + 1) * 2, static_cast<int>(result),
                dkQueueIsInErrorState(mQueue) ? "; queue in error state" : "");
        }
        slot.Fenced = false;
        NoteCpuWait(gFenceWait, NowNanoseconds() - fenceBegin);
        CollectTimestamps(mSlot);
    }
    slot.StreamBlock = 0;
    slot.StreamUsed = 0;
    slot.CommandOverflowUsed = 0;
    // dkCmdBufClear rewinds to the memory fed last (the previous frame's):
    // feed this slot's first chunk explicitly.
    dkCmdBufClear(mCmd);
    dkCmdBufAddMemory(mCmd, mCommandMemory, mSlot * kCommandSliceBytes, kCommandChunkBytes);
    slot.CommandUsed = kCommandChunkBytes + kCommandGuardBytes;
    mStats.CommandBytes += kCommandChunkBytes;
    if (mTimestamps != nullptr) {
        dkCmdBufReportCounter(mCmd, DkCounter_TimestampPipelineTop,
                              dkMemBlockGetGpuAddr(mTimestamps) + mSlot * kTimestampSlotBytes);
    }
    mWindowSlot = -1;
    mRecorded = false;
    mFrameOpen = true;
    mFrameStartSubmits = mStats.Submits;
}

void Device::Submit() {
    if (mDevice == nullptr) {
        return;
    }
    const DkCmdList list = dkCmdBufFinishList(mCmd);
    dkQueueSubmitCommands(mQueue, list);
    dkQueueFlush(mQueue);
    ++mStats.Submits;
}

const DkImageView& Device::AcquireWindow(AcquireHook hook) {
    BeginFrame();
    if (mWindowSlot < 0) {
        // The scene recorded so far goes to the GPU before the wait.
        if (mTimestamps != nullptr) {
            dkCmdBufReportCounter(mCmd, DkCounter_Timestamp,
                                  dkMemBlockGetGpuAddr(mTimestamps) + mSlot * kTimestampSlotBytes + kTimestampBytes);
        }
        Submit();
        if (hook != nullptr) {
            hook(false);
        }
        const uint64_t begin = NowNanoseconds();
        mWindowSlot = dkQueueAcquireImage(mQueue, mSwapchain);
        if (mTimestamps != nullptr) {
            dkCmdBufReportCounter(mCmd, DkCounter_TimestampPipelineTop,
                                  dkMemBlockGetGpuAddr(mTimestamps) + mSlot * kTimestampSlotBytes + 2U * kTimestampBytes);
            mSlots[mSlot].Split = true;
        }
        const uint64_t acquireNs = NowNanoseconds() - begin;
        mStats.AcquireNanoseconds += acquireNs;
        NoteCpuWait(gAcquireWait, acquireNs);
        if (hook != nullptr) {
            hook(true);
        }
        if (mWindowSlot < 0 || mWindowSlot >= static_cast<int>(kSwapchainImages)) {
            Log("dkQueueAcquireImage returned %d", mWindowSlot);
            mWindowSlot = 0;
        }
    }
    return mSwapchainViews[static_cast<size_t>(mWindowSlot)];
}

void Device::EndFrame(AcquireHook hook) {
    if (mDevice == nullptr) {
        return;
    }
    if (!mFrameOpen) {
        BeginFrame();
    }
    AcquireWindow(hook);
    FrameSlot& slot = mSlots[mSlot];
    if (mTimestamps != nullptr) {
        dkCmdBufReportCounter(mCmd, DkCounter_Timestamp,
                              dkMemBlockGetGpuAddr(mTimestamps) + mSlot * kTimestampSlotBytes + 3U * kTimestampBytes);
        slot.Timed = true;
        slot.Tag = std::exchange(gPendingTag, FrameTag{});
    }
    dkCmdBufSignalFence(mCmd, &slot.Fence, true);
    Submit();
    slot.Fenced = true;
    gPerfFrames.fetch_add(1, std::memory_order_relaxed);
    gPerfSubmits.fetch_add(mStats.Submits - mFrameStartSubmits, std::memory_order_relaxed);
    gPerfCommandBytes.fetch_add(slot.CommandUsed, std::memory_order_relaxed);
    const uint64_t presentBegin = NowNanoseconds();
    dkQueuePresentImage(mQueue, mSwapchain, mWindowSlot);
    NoteCpuWait(gPresentWait, NowNanoseconds() - presentBegin);
    if (dkQueueIsInErrorState(mQueue)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Log("frame %llu: queue in error state after present", static_cast<unsigned long long>(mFrame));
        }
    }
    mWindowSlot = -1;
    mFrameOpen = false;
    ++mFrame;
    ++mStats.Frames;
    if (Deko3dFrameLogDue(mFrame)) {
        Log("frame %llu presented (submits %llu, acquire %.2f ms total, command %llu KiB, stream %llu KiB, "
            "overflows %llu)",
            static_cast<unsigned long long>(mFrame), static_cast<unsigned long long>(mStats.Submits),
            static_cast<double>(mStats.AcquireNanoseconds) / 1e6,
            static_cast<unsigned long long>(mStats.CommandBytes >> 10),
            static_cast<unsigned long long>(mStats.StreamBytes >> 10),
            static_cast<unsigned long long>(mStats.CommandOverflows));
    }
}

void Device::WaitIdle() {
    if (mQueue != nullptr) {
        dkQueueWaitIdle(mQueue);
    }
}

StreamSpan Device::Stream(uint32_t size, uint32_t alignment) {
    BeginFrame();
    FrameSlot& slot = mSlots[mSlot];
    alignment = std::max<uint32_t>(alignment, 4U);
    while (true) {
        if (slot.StreamBlock < slot.StreamBlocks.size()) {
            const uint32_t blockSize = slot.StreamBlockSizes[slot.StreamBlock];
            const uint32_t offset = AlignUp(slot.StreamUsed, alignment);
            if (offset + size <= blockSize) {
                DkMemBlock block = slot.StreamBlocks[slot.StreamBlock];
                slot.StreamUsed = offset + size;
                mStats.StreamBytes += size;
                return {static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + offset,
                        dkMemBlockGetGpuAddr(block) + offset, size};
            }
            ++slot.StreamBlock;
            slot.StreamUsed = 0;
            continue;
        }
        const uint32_t blockSize = std::max<uint32_t>(kStreamBlockBytes, AlignUp(size + alignment, DK_MEMBLOCK_ALIGNMENT));
        DkMemBlock block = MakeBlock(blockSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "stream");
        if (block == nullptr) {
            return {};
        }
        slot.StreamBlocks.push_back(block);
        slot.StreamBlockSizes.push_back(blockSize);
        ++mStats.StreamBlocks;
        if (mStats.StreamBlocks > kFramesInFlight) {
            Log("stream memory: slot %u grew to %zu blocks (%u KiB asked)", mSlot, slot.StreamBlocks.size(),
                size >> 10);
        }
    }
}

void Device::CountSwapAcquire() {
    gSwapAcquires.fetch_add(1, std::memory_order_relaxed);
}

// After the slot's fence: its frame's timestamps are written.
void Device::CollectTimestamps(uint32_t slotIndex) {
    FrameSlot& slot = mSlots[slotIndex];
    if (!slot.Timed || mTimestamps == nullptr) {
        return;
    }
    slot.Timed = false;
    const auto* reports = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(mTimestamps)) + slotIndex * kTimestampSlotBytes;
    uint64_t t[4] = {};
    for (uint32_t i = 0; i < 4U; ++i) {
        std::memcpy(&t[i], reports + i * kTimestampBytes + 8U, sizeof(t[i]));
    }
    const bool split = slot.Split;
    slot.Split = false;
    slot.Tag.Set = slot.Tag.Set && split;
    if (t[3] <= t[0]) {
        return;
    }
    const uint64_t ns = GpuTicksToNs(t[3] - t[0]);
    gPerfGpuNs.fetch_add(ns, std::memory_order_relaxed);
    StoreMax(gPerfGpuMaxNs, ns);
    if (!split || t[1] < t[0] || t[2] < t[1] || t[3] < t[2]) {
        return;
    }
    const uint64_t scene = GpuTicksToNs(t[1] - t[0]);
    const uint64_t tail = GpuTicksToNs(t[3] - t[2]);
    gPerfSceneNs.fetch_add(scene, std::memory_order_relaxed);
    gPerfWaitNs.fetch_add(GpuTicksToNs(t[2] - t[1]), std::memory_order_relaxed);
    gPerfTailNs.fetch_add(tail, std::memory_order_relaxed);
    gPerfSplitFrames.fetch_add(1, std::memory_order_relaxed);
    StoreMax(gPerfBusyMaxNs, scene + tail);
    // The scene starts when the previous frame's work ends, if it was queued
    // behind it (NativeGpuFrameBusyTicks on GL).
    const uint64_t previousEnd = std::exchange(mPreviousGpuEnd, t[3]);
    const uint64_t start = previousEnd > t[0] && previousEnd < t[1] ? previousEnd : t[0];
    if (slot.Tag.Set) {
        Fast::Deko3dGpuFrame frame;
        frame.Kind = slot.Tag.Kind;
        frame.Milliseconds = static_cast<double>(GpuTicksToNs(t[1] - start) + tail) / 1.0e6;
        frame.CpuMilliseconds = slot.Tag.CpuMilliseconds;
        gGpuFrames[(gGpuFrameHead + gGpuFrameCount) % kGpuFrameRing] = frame;
        if (gGpuFrameCount < kGpuFrameRing) {
            ++gGpuFrameCount;
        } else {
            gGpuFrameHead = (gGpuFrameHead + 1U) % kGpuFrameRing;
        }
    }
}

bool Device::Checkpoint(const char* what, uint64_t a, uint64_t b) {
    if (!mSyncCheck || mSyncFailed || !mFrameOpen || mQueue == nullptr) {
        return false;
    }
    if (!mSyncPoints.empty() && std::find(mSyncPoints.begin(), mSyncPoints.end(), what) == mSyncPoints.end()) {
        return false;
    }
    ++mCheckpoints;
    Submit();
    if (mSyncNoWait) {
        return false;
    }
    dkQueueWaitIdle(mQueue);
    char name[128];
    std::snprintf(name, sizeof(name), "%s(%llu, %llu)", what, static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    if (dkQueueIsInErrorState(mQueue)) {
        mSyncFailed = true;
        mSyncFailurePending = true;
        Log("sync check: the queue failed at %s, checkpoint %llu of frame %llu; the one before was %s", name,
            static_cast<unsigned long long>(mCheckpoints), static_cast<unsigned long long>(mFrame),
            mLastCheckpoint.empty() ? "(none)" : mLastCheckpoint.c_str());
        return true;
    }
    mLastCheckpoint = name;
    return false;
}

} // namespace Fast::Deko3d

namespace Fast {
namespace {
std::string CpuWaitReport(const char* name, Deko3d::CpuWait& wait) {
    const uint64_t count = wait.Count.exchange(0, std::memory_order_relaxed);
    const uint64_t ns = wait.Ns.exchange(0, std::memory_order_relaxed);
    std::ostringstream out;
    out << ' ' << name << "_cpu_ms=" << (count != 0U ? static_cast<double>(ns) / static_cast<double>(count) / 1.0e6 : 0.0)
        << ' ' << name << "_cpu_max_ms=" << static_cast<double>(wait.MaxNs.exchange(0, std::memory_order_relaxed)) / 1.0e6
        << ' ' << name << "_slow=" << wait.Slow.exchange(0, std::memory_order_relaxed);
    return out.str();
}
} // namespace

std::string Deko3dPerfReport(unsigned long long hostFrames) {
    using namespace Deko3d;
    if (!gPerfLive.load(std::memory_order_acquire)) {
        return {};
    }
    const uint64_t frames = gPerfFrames.exchange(0, std::memory_order_relaxed);
    const uint64_t gpuNs = gPerfGpuNs.exchange(0, std::memory_order_relaxed);
    const uint64_t gpuMaxNs = gPerfGpuMaxNs.exchange(0, std::memory_order_relaxed);
    const uint64_t submits = gPerfSubmits.exchange(0, std::memory_order_relaxed);
    const uint64_t commandBytes = gPerfCommandBytes.exchange(0, std::memory_order_relaxed);
    const uint64_t split = gPerfSplitFrames.exchange(0, std::memory_order_relaxed);
    const uint64_t sceneNs = gPerfSceneNs.exchange(0, std::memory_order_relaxed);
    const uint64_t waitNs = gPerfWaitNs.exchange(0, std::memory_order_relaxed);
    const uint64_t tailNs = gPerfTailNs.exchange(0, std::memory_order_relaxed);
    const uint64_t busyMaxNs = gPerfBusyMaxNs.exchange(0, std::memory_order_relaxed);
    const double count = frames != 0U ? static_cast<double>(frames) : 1.0;
    const double splitCount = split != 0U ? static_cast<double>(split) : 1.0;
    std::ostringstream out;
    out << "dkperf host_frames=" << hostFrames << " frames=" << frames
        << " gpu_ms=" << static_cast<double>(gpuNs) / count / 1.0e6
        << " gpu_max_ms=" << static_cast<double>(gpuMaxNs) / 1.0e6
        << " busy_ms=" << static_cast<double>(sceneNs + tailNs) / splitCount / 1.0e6
        << " busy_max_ms=" << static_cast<double>(busyMaxNs) / 1.0e6
        << " scene_ms=" << static_cast<double>(sceneNs) / splitCount / 1.0e6
        << " acquire_wait_ms=" << static_cast<double>(waitNs) / splitCount / 1.0e6
        << " tail_ms=" << static_cast<double>(tailNs) / splitCount / 1.0e6
        << " submits_per_frame=" << static_cast<double>(submits) / count
        << " command_kib_per_frame=" << static_cast<double>(commandBytes) / count / 1024.0
        << CpuWaitReport("fence", gFenceWait) << CpuWaitReport("acquire", gAcquireWait)
        << CpuWaitReport("present", gPresentWait)
        << " swap_acquires=" << gSwapAcquires.exchange(0, std::memory_order_relaxed)
        << " sync_check=" << (gPerfSyncCheck.load(std::memory_order_relaxed) ? 1 : 0)
        << " build=" << kDeko3dBuildTag;
    return out.str();
}
void Deko3dTagFrame(uint8_t kind, double cpuMilliseconds) {
    Deko3d::gPendingTag = {true, kind, cpuMilliseconds};
}

bool Deko3dTakeGpuFrame(Deko3dGpuFrame& frame) {
    using namespace Deko3d;
    if (gGpuFrameCount == 0U) {
        return false;
    }
    frame = gGpuFrames[gGpuFrameHead];
    gGpuFrameHead = (gGpuFrameHead + 1U) % kGpuFrameRing;
    --gGpuFrameCount;
    return true;
}
} // namespace Fast
#endif
