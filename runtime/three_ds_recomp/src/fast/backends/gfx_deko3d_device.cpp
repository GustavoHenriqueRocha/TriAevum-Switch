// deko3d device layer (Switch). See gfx_deko3d_device.h.
#if defined(__SWITCH__)
#include "gfx_deko3d_device.h"

#include "fast/backends/gfx_deko3d.h"

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
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
std::atomic<uint64_t> gPerfSubmits{0};
std::atomic<uint64_t> gPerfCommandBytes{0};
std::atomic<bool> gPerfSyncCheck{false};

// Report timestamps count GPU ticks (614.4 MHz on the Tegra X1).
constexpr uint64_t GpuTicksToNs(uint64_t ticks) {
    return ticks * 625U / 384U;
}
constexpr uint32_t kTimestampBytes = 16U; // DkCounter report: value, timestamp
constexpr uint32_t kTimestampSlotBytes = 2U * kTimestampBytes;

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
        std::fclose(file);
        mSyncCheck = true;
        Log("sync check on: every operation is waited for (slow; diagnosis only)");
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
    if (slot.CommandUsed + want <= kCommandSliceBytes) {
        dkCmdBufAddMemory(mCmd, mCommandMemory, mSlot * kCommandSliceBytes + slot.CommandUsed, want);
        slot.CommandUsed += want;
        mStats.CommandBytes += want;
        return;
    }
    if (slot.CommandOverflowUsed == 0) {
        ++mStats.CommandOverflows;
    }
    while (slot.CommandOverflowUsed < slot.CommandOverflow.size()) {
        auto [block, size] = slot.CommandOverflow[slot.CommandOverflowUsed++];
        if (size >= want) {
            dkCmdBufAddMemory(mCmd, block, 0, size);
            mStats.CommandBytes += size;
            return;
        }
    }
    const uint32_t size = std::max<uint32_t>(want, 1U << 20);
    DkMemBlock block = MakeBlock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "command overflow");
    if (block == nullptr) {
        Log("command memory exhausted (frame %llu)", static_cast<unsigned long long>(mFrame));
        return;
    }
    slot.CommandOverflow.emplace_back(block, size);
    slot.CommandOverflowUsed = slot.CommandOverflow.size();
    dkCmdBufAddMemory(mCmd, block, 0, size);
    mStats.CommandBytes += size;
}

void Device::BeginFrame() {
    if (mFrameOpen || mDevice == nullptr) {
        return;
    }
    mSlot = static_cast<uint32_t>(mFrame % kFramesInFlight);
    FrameSlot& slot = mSlots[mSlot];
    if (slot.Fenced) {
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
        CollectTimestamps(mSlot);
    }
    slot.StreamBlock = 0;
    slot.StreamUsed = 0;
    slot.CommandOverflowUsed = 0;
    // dkCmdBufClear rewinds to the memory fed last (the previous frame's):
    // feed this slot's first chunk explicitly.
    dkCmdBufClear(mCmd);
    dkCmdBufAddMemory(mCmd, mCommandMemory, mSlot * kCommandSliceBytes, kCommandChunkBytes);
    slot.CommandUsed = kCommandChunkBytes;
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
        Submit();
        if (hook != nullptr) {
            hook(false);
        }
        const uint64_t begin = NowNanoseconds();
        mWindowSlot = dkQueueAcquireImage(mQueue, mSwapchain);
        mStats.AcquireNanoseconds += NowNanoseconds() - begin;
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
                              dkMemBlockGetGpuAddr(mTimestamps) + mSlot * kTimestampSlotBytes + kTimestampBytes);
        slot.Timed = true;
    }
    dkCmdBufSignalFence(mCmd, &slot.Fence, true);
    Submit();
    slot.Fenced = true;
    gPerfFrames.fetch_add(1, std::memory_order_relaxed);
    gPerfSubmits.fetch_add(mStats.Submits - mFrameStartSubmits, std::memory_order_relaxed);
    gPerfCommandBytes.fetch_add(slot.CommandUsed, std::memory_order_relaxed);
    dkQueuePresentImage(mQueue, mSwapchain, mWindowSlot);
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

// After the slot's fence: its frame's two timestamps are written.
void Device::CollectTimestamps(uint32_t slotIndex) {
    FrameSlot& slot = mSlots[slotIndex];
    if (!slot.Timed || mTimestamps == nullptr) {
        return;
    }
    slot.Timed = false;
    const auto* reports = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(mTimestamps)) + slotIndex * kTimestampSlotBytes;
    uint64_t begin = 0;
    uint64_t end = 0;
    std::memcpy(&begin, reports + 8U, sizeof(begin));
    std::memcpy(&end, reports + kTimestampBytes + 8U, sizeof(end));
    if (end <= begin) {
        return;
    }
    const uint64_t ns = GpuTicksToNs(end - begin);
    gPerfGpuNs.fetch_add(ns, std::memory_order_relaxed);
    uint64_t max = gPerfGpuMaxNs.load(std::memory_order_relaxed);
    while (ns > max && !gPerfGpuMaxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
    }
}

bool Device::Checkpoint(const char* what, uint64_t a, uint64_t b) {
    if (!mSyncCheck || mSyncFailed || !mFrameOpen || mQueue == nullptr) {
        return false;
    }
    ++mCheckpoints;
    Submit();
    dkQueueWaitIdle(mQueue);
    char name[128];
    std::snprintf(name, sizeof(name), "%s(%llu, %llu)", what, static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    if (dkQueueIsInErrorState(mQueue)) {
        mSyncFailed = true;
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
    const double count = frames != 0U ? static_cast<double>(frames) : 1.0;
    std::ostringstream out;
    out << "dkperf host_frames=" << hostFrames << " frames=" << frames
        << " gpu_ms=" << static_cast<double>(gpuNs) / count / 1.0e6
        << " gpu_max_ms=" << static_cast<double>(gpuMaxNs) / 1.0e6
        << " submits_per_frame=" << static_cast<double>(submits) / count
        << " command_kib_per_frame=" << static_cast<double>(commandBytes) / count / 1024.0
        << " sync_check=" << (gPerfSyncCheck.load(std::memory_order_relaxed) ? 1 : 0)
        << " build=" << kDeko3dBuildTag;
    return out.str();
}
} // namespace Fast
#endif
