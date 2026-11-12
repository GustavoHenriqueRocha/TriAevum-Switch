#include "fast/oot3d/grass_late_overlays.h"

#include <iterator>

namespace Fast::Oot3d {

void GrassLateOverlays::Collect(uint64_t frame, std::span<const GrassSceneMesh> sources,
                                const OverlaysFor& overlaysFor) {
    for (const auto& source : sources) {
        mSets[source.InstanceId] = {frame, overlaysFor(source)};
        mWaiting.erase(source.InstanceId);
    }
    std::erase_if(mSets, [frame](const auto& entry) { return entry.second.Frame + kGrassLateOverlayKeepFrames < frame; });
    std::erase_if(mWaiting, [frame](const auto& entry) { return entry.second + kGrassLateOverlayKeepFrames < frame; });
}

GrassLateOverlays::Lookup GrassLateOverlays::Find(uint64_t instanceId, uint64_t frame) {
    if (const auto found = mSets.find(instanceId); found != mSets.end()) return {true, found->second.Set};
    const auto [waiting, inserted] = mWaiting.try_emplace(instanceId, frame);
    if (inserted || waiting->second == frame) return {false, nullptr};
    return {true, nullptr};
}

void GrassLateOverlays::Clear() {
    mSets.clear();
    mWaiting.clear();
}

} // namespace Fast::Oot3d
