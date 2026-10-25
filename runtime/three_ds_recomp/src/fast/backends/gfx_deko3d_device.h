#pragma once
// deko3d device layer (Switch): device, queue, swapchain, per-frame slots
// (command memory + stream memory + fence), the window image acquire and the
// present. The rendering API (gfx_deko3d.cpp) records into Cmd().
//
// Conventions (docs/DEKO3D_PROGRESSO.md, "Decisões"): DepthMinusOneToOne and
// OriginUpperLeft, so offscreen images have the GL framebuffer layout (row 0 =
// y 0) and the GL 4.3 shader texts run unchanged; only drawing into the
// window flips Y.
//
// Design references (no code copied): the deko3d renderer of Aurora in
// "Alek's Ultimate NX Edition" (MIT) and switchbrew/deko3d's examples.

#if defined(__SWITCH__)
#include <deko3d.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Fast::Deko3d {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kSwapchainImages = 3;
constexpr uint32_t kCommandSliceBytes = 1U << 20;   // per slot, fed in chunks
constexpr uint32_t kCommandChunkBytes = 64U << 10;
constexpr uint32_t kStreamBlockBytes = 4U << 20;     // per slot, more blocks on demand

constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

// sdmc:/switch/oot3dre/logs/deko3d.txt (truncated by Device::Create).
void Log(const char* format, ...) __attribute__((format(printf, 1, 2)));

struct StreamSpan {
    void* Cpu = nullptr;
    DkGpuAddr Gpu = DK_GPU_ADDR_INVALID;
    uint32_t Size = 0;
};

struct DeviceStats {
    uint64_t Frames = 0;
    uint64_t Submits = 0;
    uint64_t AcquireNanoseconds = 0;
    uint64_t CommandBytes = 0;     // command memory fed, totals
    uint64_t StreamBytes = 0;      // stream memory handed out, totals
    uint64_t StreamBlocks = 0;     // live stream blocks (all slots)
    uint64_t CommandOverflows = 0; // frames that needed more than their slice
    uint64_t FenceTimeouts = 0;
};

using AcquireHook = void (*)(bool after);

class Device {
  public:
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device();

    bool Create(uint32_t width, uint32_t height, std::string* error);
    void Destroy() noexcept;
    [[nodiscard]] bool Valid() const noexcept { return mDevice != nullptr; }

    [[nodiscard]] DkDevice Handle() const noexcept { return mDevice; }
    [[nodiscard]] DkQueue Queue() const noexcept { return mQueue; }
    [[nodiscard]] DkCmdBuf Cmd() const noexcept { return mCmd; }
    [[nodiscard]] uint32_t Width() const noexcept { return mWidth; }
    [[nodiscard]] uint32_t Height() const noexcept { return mHeight; }
    [[nodiscard]] uint32_t Slot() const noexcept { return mSlot; }
    [[nodiscard]] uint64_t Frame() const noexcept { return mFrame; }
    [[nodiscard]] const DeviceStats& Stats() const noexcept { return mStats; }

    // Waits until the GPU is done with this frame's slot, then resets its
    // command and stream memory. Idempotent while a frame is open.
    void BeginFrame();
    [[nodiscard]] bool FrameOpen() const noexcept { return mFrameOpen; }

    // The window image of this frame: acquired at its first use (the
    // display may block here). The recorded work is handed to the GPU first,
    // so the scene runs while the render thread waits.
    const DkImageView& AcquireWindow(AcquireHook hook);
    [[nodiscard]] bool WindowAcquired() const noexcept { return mWindowSlot >= 0; }

    // Hands the commands recorded so far to the GPU.
    void Submit();
    // Signals the slot's fence, submits and presents (a frame that never
    // used the window acquires it here).
    void EndFrame(AcquireHook hook);
    // Waits for every submitted command (shutdown, resets).
    void WaitIdle();

    // Per-frame CPU-visible memory (valid until this slot comes round again).
    StreamSpan Stream(uint32_t size, uint32_t alignment);

    DkMemBlock MakeBlock(uint32_t size, uint32_t flags, const char* what);

    // config/deko3d_sync_check: after each recorded operation the work goes
    // to the GPU and is waited for, so the first operation that puts the
    // queue in error state is named in deko3d.txt (slow; diagnosis only).
    [[nodiscard]] bool SyncCheck() const noexcept { return mSyncCheck; }
    // True only at the checkpoint where the queue failed (the caller can log
    // what it recorded).
    bool Checkpoint(const char* what, uint64_t a = 0, uint64_t b = 0);

  private:
    static void AddCommandMemory(void* user, DkCmdBuf cmd, size_t minimumSize);
    void FeedCommandMemory(size_t minimumSize);

    struct FrameSlot {
        uint32_t CommandUsed = 0;
        std::vector<std::pair<DkMemBlock, uint32_t>> CommandOverflow;
        size_t CommandOverflowUsed = 0;
        std::vector<DkMemBlock> StreamBlocks;
        std::vector<uint32_t> StreamBlockSizes;
        size_t StreamBlock = 0;
        uint32_t StreamUsed = 0;
        DkFence Fence{};
        bool Fenced = false;
        bool Timed = false; // its two timestamps were recorded
    };
    void CollectTimestamps(uint32_t slot);

    DkDevice mDevice = nullptr;
    DkQueue mQueue = nullptr;
    DkCmdBuf mCmd = nullptr;
    DkMemBlock mCommandMemory = nullptr;
    DkMemBlock mSwapchainMemory = nullptr;
    DkMemBlock mTimestamps = nullptr; // two reports per slot
    DkSwapchain mSwapchain = nullptr;
    std::array<DkImage, kSwapchainImages> mSwapchainImages{};
    std::array<DkImageView, kSwapchainImages> mSwapchainViews{};
    std::array<FrameSlot, kFramesInFlight> mSlots{};
    uint32_t mWidth = 0;
    uint32_t mHeight = 0;
    uint32_t mSlot = 0;
    uint64_t mFrame = 0;
    int mWindowSlot = -1;
    bool mFrameOpen = false;
    bool mRecorded = false;
    bool mSyncCheck = false;
    bool mSyncFailed = false;
    uint64_t mCheckpoints = 0;
    uint64_t mFrameStartSubmits = 0;
    std::string mLastCheckpoint;
    DeviceStats mStats{};
};

} // namespace Fast::Deko3d
#endif
