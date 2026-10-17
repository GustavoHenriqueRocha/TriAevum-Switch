// GPU memory policy of the OpenGL PICA backend over long sessions: the
// texture cache keyed by content must stay bounded while textures keep
// changing, and the stream rings must hand out non-overlapping ranges of one
// storage generation per draw.
#include "fast/backends/gfx_opengl_pica_residency.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

namespace Fast::PicaGlResidency {
namespace {

struct FakeTexture {
    uint64_t Bytes = 0U;
    uint64_t LastUsedFrame = 0U;
};

// The backend's cache: a map keyed by content, trimmed every 30 frames or
// whenever it exceeds the budget (BeginPicaGlFrame / TrimPicaGlResidency).
struct FakeCache {
    std::map<uint64_t, FakeTexture> Textures;
    uint64_t Bytes = 0U;
    uint64_t Created = 0U;
    uint64_t Evicted = 0U;
    Policy Limits;
    std::vector<uint64_t> Scratch;

    void BeginFrame(uint64_t frame) {
        if (frame % 30U != 0U && Bytes <= Limits.BudgetBytes) return;
        CollectEvictions(Textures, frame, Bytes, Limits, Scratch);
        for (const uint64_t key : Scratch) {
            const auto found = Textures.find(key);
            ASSERT_NE(found, Textures.end());
            Bytes -= found->second.Bytes;
            Textures.erase(found);
            ++Evicted;
        }
    }

    void Use(uint64_t contentHash, uint64_t bytes, uint64_t frame) {
        auto [found, inserted] = Textures.try_emplace(contentHash);
        found->second.LastUsedFrame = frame;
        if (inserted) {
            found->second.Bytes = bytes;
            Bytes += bytes;
            ++Created;
        }
    }
};

constexpr uint64_t kMb = 1024ULL * 1024ULL;

TEST(PicaGlResidency, ParsesTheBudget) {
    EXPECT_EQ(ParseBudgetBytes("128", 7U), 128U * kMb);
    EXPECT_EQ(ParseBudgetBytes(" 64\n", 7U), 64U * kMb);
    EXPECT_EQ(ParseBudgetBytes("8", 7U), 7U);
    EXPECT_EQ(ParseBudgetBytes("4096", 7U), 7U);
    EXPECT_EQ(ParseBudgetBytes("x", 7U), 7U);
    EXPECT_EQ(ParseBudgetBytes("", 7U), 7U);
}

TEST(PicaGlResidency, KeepsWhatTheLastFramesUsed) {
    std::map<uint64_t, FakeTexture> textures{
        {1U, {10U * kMb, 100U}}, {2U, {10U * kMb, 99U}}, {3U, {10U * kMb, 50U}}};
    Policy policy;
    policy.BudgetBytes = 5U * kMb;
    std::vector<uint64_t> evicted;
    CollectEvictions(textures, 100U, 30U * kMb, policy, evicted);
    // Over budget, but the textures of frames 99 and 100 stay.
    EXPECT_EQ(evicted, std::vector<uint64_t>{3U});
    // Idle past the limit goes even under the budget.
    policy.BudgetBytes = 1000U * kMb;
    CollectEvictions(textures, 100U + policy.MaxIdleFrames + 1U, 30U * kMb,
                     policy, evicted);
    std::sort(evicted.begin(), evicted.end());
    EXPECT_EQ(evicted, (std::vector<uint64_t>{1U, 2U, 3U}));
}

TEST(PicaGlResidency, EvictsLeastRecentlyUsedFirst) {
    std::map<uint64_t, FakeTexture> textures;
    for (uint64_t key = 0U; key < 10U; ++key)
        textures[key] = {kMb, 10U + key};
    Policy policy;
    policy.BudgetBytes = 7U * kMb;
    std::vector<uint64_t> evicted;
    CollectEvictions(textures, 100U, 10U * kMb, policy, evicted);
    EXPECT_EQ(evicted, (std::vector<uint64_t>{0U, 1U, 2U}));
}

// Thousands of frames of play: a scene's static textures, animated ones that
// change content every frame (water, fire, guest-written images), a scene
// change every ~40 s. Without eviction the cache grew by the animated
// textures alone (the console crash after ~8 minutes); with it the bytes and
// the count stay bounded by the budget plus the working set.
TEST(PicaGlResidency, StaysBoundedOverALongSession) {
    FakeCache cache;
    cache.Limits.BudgetBytes = 32U * kMb;
    std::mt19937_64 random(1234U);
    constexpr uint64_t kFrames = 60U * 60U * 30U;  // 30 minutes at 60 Hz
    constexpr uint64_t kSceneFrames = 2400U;
    constexpr uint64_t kStaticTextures = 120U;
    constexpr uint64_t kAnimatedTextures = 6U;
    uint64_t maxBytes = 0U;
    size_t maxCount = 0U;
    uint64_t workingSet = 0U;
    for (uint64_t frame = 1U; frame <= kFrames; ++frame) {
        cache.BeginFrame(frame);
        const uint64_t scene = frame / kSceneFrames;
        uint64_t frameBytes = 0U;
        for (uint64_t texture = 0U; texture < kStaticTextures; ++texture) {
            // About 3/4 of a scene's textures are visible in a frame.
            if ((random() & 3U) == 0U) continue;
            const uint64_t bytes = (64U << (texture % 4U)) * 64U * 4U;
            cache.Use((scene << 32U) | texture, bytes, frame);
            frameBytes += bytes;
        }
        for (uint64_t texture = 0U; texture < kAnimatedTextures; ++texture) {
            const uint64_t bytes = 128U * 128U * 4U;
            cache.Use((1ULL << 60U) | (frame << 8U) | texture, bytes, frame);
            frameBytes += bytes;
        }
        workingSet = std::max(workingSet, frameBytes);
        maxBytes = std::max(maxBytes, cache.Bytes);
        maxCount = std::max(maxCount, cache.Textures.size());
    }
    // Two frames are always kept (MinIdleFrames): the bound is the budget
    // plus those two frames' textures.
    EXPECT_LE(maxBytes, cache.Limits.BudgetBytes + 2U * workingSet);
    EXPECT_LT(maxCount, 2000U);
    EXPECT_GT(cache.Evicted, kFrames * kAnimatedTextures / 2U);
    // Steady state: the last minute is as large as the first one after the
    // budget filled up, not growing.
    EXPECT_LE(cache.Bytes, cache.Limits.BudgetBytes + 2U * workingSet);
    EXPECT_EQ(cache.Created - cache.Evicted, cache.Textures.size());
}

TEST(PicaGlStreamRing, ReservedRangesShareOneGenerationAndNeverOverlap) {
    StreamRing ring(1U << 20U);
    std::mt19937_64 random(99U);
    uint64_t orphans = 0U;
    for (int draw = 0; draw < 200000; ++draw) {
        const uint64_t vertexBlock = 1504U;
        const uint64_t fragmentBlock = (random() & 1U) != 0U ? 6112U : 1184U;
        const uint64_t total = StreamRing::Span(vertexBlock, 256U) +
                               StreamRing::Span(fragmentBlock, 256U);
        ASSERT_TRUE(ring.Fits(total));
        const uint64_t headBefore = ring.Head();
        const uint64_t generationBefore = ring.Generation();
        if (ring.Reserve(total)) {
            ++orphans;
            ASSERT_EQ(ring.Generation(), generationBefore + 1U);
            ASSERT_EQ(ring.Head(), 0U);
        }
        const uint64_t generation = ring.Generation();
        const auto vertex = ring.Allocate(vertexBlock, 256U);
        const auto fragment = ring.Allocate(fragmentBlock, 256U);
        ASSERT_TRUE(vertex.Valid && fragment.Valid);
        // One generation per draw: both blocks in the same storage.
        ASSERT_EQ(ring.Generation(), generation);
        ASSERT_EQ(vertex.Offset % 256U, 0U);
        ASSERT_EQ(fragment.Offset % 256U, 0U);
        ASSERT_GE(fragment.Offset, vertex.Offset + vertexBlock);
        ASSERT_LE(fragment.Offset + fragmentBlock, ring.Capacity());
        if (generation == generationBefore) {
            ASSERT_GE(vertex.Offset, headBefore);
        }
    }
    // One orphan per ring's worth of data, not one per draw.
    EXPECT_LT(orphans, 200000U * 8000U / (1U << 20U) + 2U);
    EXPECT_GT(orphans, 0U);
}

TEST(PicaGlStreamRing, OversizedAndUnreservedRangesAreRefused) {
    StreamRing ring(4096U);
    EXPECT_FALSE(ring.Fits(5000U));
    EXPECT_FALSE(ring.Reserve(5000U));
    // Nothing reserved yet: no storage generation to allocate from.
    EXPECT_FALSE(ring.Allocate(16U, 4U).Valid);
    EXPECT_TRUE(ring.Reserve(100U));
    EXPECT_TRUE(ring.Allocate(16U, 4U).Valid);
    EXPECT_FALSE(ring.Allocate(5000U, 4U).Valid);
}

TEST(PicaGlStreamRing, AFailedMapStartsANewGeneration) {
    StreamRing ring(4096U);
    ASSERT_TRUE(ring.Reserve(64U));
    const uint64_t generation = ring.Generation();
    ASSERT_TRUE(ring.Allocate(64U, 4U).Valid);
    ring.Reset();
    // Ranges remembered from the lost storage no longer match.
    EXPECT_FALSE(ring.Allocate(16U, 4U).Valid);
    EXPECT_TRUE(ring.Reserve(64U));
    EXPECT_GT(ring.Generation(), generation);
    EXPECT_EQ(ring.Allocate(16U, 4U).Offset, 0U);
}

} // namespace
} // namespace Fast::PicaGlResidency
