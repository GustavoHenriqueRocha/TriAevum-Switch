#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <nlohmann/json_fwd.hpp>

namespace Oot3dNativeGame {

class NativeA32Memory;

struct NativeA32DspPhysicalRegion {
    uint32_t PhysicalAddress = 0;
    uint32_t VirtualAddress = 0;
    size_t Size = 0;
};

struct NativeDspMemoryAccess {
    // The two shared regions are updated in place: BeginUpdate returns the bytes
    // of a guest range (accounted as a read of it; null if not mapped writable)
    // and EndUpdate accounts the whole range as written back, as copying it in,
    // modifying the copy and writing it back would.
    std::function<uint8_t*(uint32_t, size_t)> BeginUpdate;
    std::function<bool(uint32_t, size_t)> EndUpdate;
    std::function<const uint8_t*(uint32_t, size_t)> ResolvePhysical;
};

// One frame's shared regions, resolved by NativeA32DspHle::BeginFrame.
struct NativeA32DspFrame {
    uint8_t* Region0 = nullptr;
    uint8_t* Region1 = nullptr;
    size_t ReadIndex = 0;  // the region the guest filled
};

class NativeA32DspHle {
  public:
    static constexpr uint32_t NativeSampleRate = 32728;
    static constexpr uint32_t SamplesPerFrame = 160;

    explicit NativeA32DspHle(
        std::vector<NativeA32DspPhysicalRegion> physicalRegions);
    ~NativeA32DspHle();

    NativeA32DspHle(const NativeA32DspHle&) = delete;
    NativeA32DspHle& operator=(const NativeA32DspHle&) = delete;

    bool ProcessFrame(NativeA32Memory& memory,
                      std::vector<int16_t>& interleavedStereo,
                      std::string* error = nullptr);
    bool ProcessFrame(const NativeDspMemoryAccess& memory,
                      std::vector<int16_t>& interleavedStereo,
                      std::string* error = nullptr);
    // ProcessFrame in three steps, for a caller that releases its lock around
    // the mix (config/kernel_retake): BeginFrame resolves both shared regions
    // (the only step that fails on a sound guest: regions not mapped writable),
    // MixFrame mixes the frame in place and appends its samples, EndFrame
    // accounts both regions as written back. The same bytes, samples, memory
    // traffic and order as ProcessFrame, which is the three in a row.
    bool BeginFrame(const NativeDspMemoryAccess& memory, NativeA32DspFrame& frame,
                    std::string* error = nullptr);
    void MixFrame(const NativeDspMemoryAccess& memory, const NativeA32DspFrame& frame,
                  std::vector<int16_t>& interleavedStereo);
    bool EndFrame(const NativeDspMemoryAccess& memory, const NativeA32DspFrame& frame,
                  std::string* error = nullptr);
    // The access ProcessFrame(NativeA32Memory&) uses: the in-place updates and
    // sample reads through NativeA32Memory's paths (each takes the kernel lock).
    NativeDspMemoryAccess MemoryAccess(NativeA32Memory& memory);

    nlohmann::json CaptureState() const;
    bool RestoreState(const nlohmann::json& state,
                      std::string* error = nullptr);

    // The emulated memory's physical-to-virtual map (also used by savestates
    // to place render targets in VRAM).
    std::span<const NativeA32DspPhysicalRegion> PhysicalRegions() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Oot3dNativeGame
