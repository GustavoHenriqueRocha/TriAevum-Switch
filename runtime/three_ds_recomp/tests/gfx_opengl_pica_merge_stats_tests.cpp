// Contagem de draws PICA juntaveis (fast/backends/gfx_opengl_pica_merge_stats.h).

#include "fast/backends/gfx_opengl_pica_merge_stats.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using namespace Fast::PicaGlMergeStats;

TEST(PicaMergeStats, IdenticalConsecutiveDrawsMerge) {
    MergeTracker tracker;
    EXPECT_FALSE(tracker.Feed(7U));
    EXPECT_TRUE(tracker.Feed(7U));
    EXPECT_TRUE(tracker.Feed(7U));
    EXPECT_FALSE(tracker.Feed(9U));
    EXPECT_TRUE(tracker.Feed(9U));
    uint32_t draws = 0U, mergeable = 0U, runMax = 0U;
    tracker.TakeFrame(&draws, &mergeable, &runMax);
    EXPECT_EQ(draws, 5U);
    EXPECT_EQ(mergeable, 3U);
    EXPECT_EQ(runMax, 3U);
}

TEST(PicaMergeStats, AlternatingStatesNeverMerge) {
    MergeTracker tracker;
    for (uint64_t index = 0U; index < 10U; ++index) {
        EXPECT_FALSE(tracker.Feed(index % 2U));
    }
    EXPECT_EQ(tracker.Mergeable, 0U);
    EXPECT_EQ(tracker.RunMax, 1U);
}

TEST(PicaMergeStats, BreakSplitsAndFrameResets) {
    MergeTracker tracker;
    tracker.Feed(1U);
    tracker.Feed(1U);
    tracker.Break();
    EXPECT_FALSE(tracker.Feed(1U)); // limpeza no meio: nao junta
    EXPECT_TRUE(tracker.Feed(1U));
    uint32_t draws = 0U, mergeable = 0U, runMax = 0U;
    tracker.TakeFrame(&draws, &mergeable, &runMax);
    EXPECT_EQ(draws, 4U);
    EXPECT_EQ(mergeable, 2U);
    EXPECT_EQ(runMax, 2U);
    EXPECT_FALSE(tracker.Feed(1U)); // novo quadro: sem herdar o anterior
    tracker.TakeFrame(&draws, &mergeable, &runMax);
    EXPECT_EQ(mergeable, 0U);
    EXPECT_EQ(runMax, 1U);
}

TEST(PicaMergeStats, HasherSeparatesBlocksAndValues) {
    const std::array<uint8_t, 3> bytes{1, 2, 3};
    Hasher a, b, c;
    a.Block(bytes.data(), 1U);
    a.Block(bytes.data() + 1, 2U);
    b.Block(bytes.data(), 2U);
    b.Block(bytes.data() + 2, 1U);
    EXPECT_NE(a.Value, b.Value);
    c.Block(bytes.data(), 1U);
    c.Block(bytes.data() + 1, 2U);
    EXPECT_EQ(a.Value, c.Value);
    Hasher d, e;
    d.Add(1U);
    e.Add(2U);
    EXPECT_NE(d.Value, e.Value);
}

} // namespace
