#include "fast/oot3d/grass_late_overlays.h"

#include <algorithm>
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

uint64_t GrassShownPlacements::Base(const GrassAsyncPlacementRequest& request) {
    uint64_t version;
    if (request.Overlays) {
        auto base = request;
        base.Overlays.reset();
        version = GrassPlacementSourceVersion(base);
    } else {
        version = GrassPlacementSourceVersion(request);
    }
    return version ^ (uint64_t(request.Budget) + 0x9e3779b97f4a7c15ULL + (version << 6U) + (version >> 2U));
}

void GrassShownPlacements::Resolve(uint64_t world, uint64_t base, uint64_t frame, GrassAsyncPlacementResult& result) {
    if (result.State == GrassAsyncPlacementState::Ready && result.Placement) {
        mShown.insert_or_assign(world, Shown{base, result.Placement, result.BuildMilliseconds, frame});
        return;
    }
    if (result.State != GrassAsyncPlacementState::Pending) return;
    if (const auto found = mShown.find(world); found != mShown.end() && found->second.Base == base) {
        found->second.LastFrame = frame;
        result.State = GrassAsyncPlacementState::Ready;
        result.Placement = found->second.Placement;
        result.BuildMilliseconds = found->second.BuildMilliseconds;
    }
}

void GrassShownPlacements::Prune(size_t keep) {
    while (mShown.size() > std::max<size_t>(32U, keep)) {
        auto oldest = mShown.begin();
        for (auto entry = mShown.begin(); entry != mShown.end(); ++entry)
            if (entry->second.LastFrame < oldest->second.LastFrame) oldest = entry;
        mShown.erase(oldest);
    }
}

} // namespace Fast::Oot3d
