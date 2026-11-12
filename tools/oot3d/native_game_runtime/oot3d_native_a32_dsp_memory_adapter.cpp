#include "oot3d_native_a32_dsp_memory_adapter.h"

#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_process_image.h"

namespace Oot3dNativeGame {

std::vector<NativeA32DspPhysicalRegion>
BuildNativeA32DspPhysicalRegions(
    const NativeA32ProcessImageManifest& manifest) {
    std::vector<NativeA32DspPhysicalRegion> regions{
        {0x20000000U, manifest.LinearHeapBaseAddress, manifest.LinearHeapSize}};
    for (const auto& region : manifest.SystemRegions) {
        if (region.Name == "ctr_vram") {
            regions.push_back(
                {0x18000000U, region.Address, region.MappedSize});
            break;
        }
    }
    return regions;
}

NativeDspMemoryAccess NativeA32DspHle::MemoryAccess(NativeA32Memory& memory) {
    NativeDspMemoryAccess access;
    access.BeginUpdate = [&memory](uint32_t address, size_t size) {
        return memory.BeginHostUpdate(address, size);
    };
    access.EndUpdate = [&memory](uint32_t address, size_t size) {
        return memory.EndHostUpdate(address, size);
    };
    access.ResolvePhysical = [this, &memory](uint32_t physicalAddress, size_t size) {
        for (const auto& region : PhysicalRegions()) {
            if (physicalAddress < region.PhysicalAddress) continue;
            const uint64_t offset = physicalAddress - region.PhysicalAddress;
            if (offset < region.Size && size <= region.Size - offset) {
                return memory.SynchronousReadView(
                    region.VirtualAddress + static_cast<uint32_t>(offset), size).data();
            }
        }
        return static_cast<const uint8_t*>(nullptr);
    };
    return access;
}

bool NativeA32DspHle::ProcessFrame(
    NativeA32Memory& memory, std::vector<int16_t>& interleavedStereo,
    std::string* error) {
    return ProcessFrame(MemoryAccess(memory), interleavedStereo, error);
}

} // namespace Oot3dNativeGame
