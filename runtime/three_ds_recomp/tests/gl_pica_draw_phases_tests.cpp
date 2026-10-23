// Acumulador de fases do draw PICA (fast/backends/gl_pica_draw_phases.h).

#include "fast/backends/gl_pica_draw_phases.h"

#include <gtest/gtest.h>

using namespace Fast::PicaDrawPhases;

namespace {
uint64_t P(const FrameTotals& t, Phase p) { return t.PhaseTicks[static_cast<size_t>(p)]; }
}

TEST(PicaDrawPhases, LapsChargeTimeSincePreviousMark) {
    Tracker tracker;
    tracker.Begin(100U);
    tracker.Lap(Phase::Validate, 110U);
    tracker.Lap(Phase::Program, 135U);
    tracker.Lap(Phase::Program, 140U);
    tracker.End(150U);
    const auto frame = tracker.TakeFrame();
    EXPECT_EQ(P(frame, Phase::Validate), 10U);
    EXPECT_EQ(P(frame, Phase::Program), 30U);
    EXPECT_EQ(P(frame, Phase::Rest), 10U);
    EXPECT_EQ(frame.SubmitTicks, 50U);
    uint64_t sum = 0U;
    for (const auto v : frame.PhaseTicks) sum += v;
    EXPECT_EQ(sum, frame.SubmitTicks);  // cada instante cai em uma fase
    EXPECT_EQ(frame.Counts[static_cast<size_t>(Count::Draws)], 1U);
}

TEST(PicaDrawPhases, LapOutsideDrawIsIgnored) {
    Tracker tracker;
    tracker.Lap(Phase::Draw, 50U);
    tracker.Begin(60U);
    tracker.End(70U);
    tracker.Lap(Phase::Draw, 90U);
    const auto frame = tracker.TakeFrame();
    EXPECT_EQ(P(frame, Phase::Draw), 0U);
    EXPECT_EQ(frame.SubmitTicks, 10U);
}

TEST(PicaDrawPhases, TakeFrameResetsAndWindowAccumulates) {
    Tracker tracker;
    tracker.Add(Count::TexHashes, 2U);
    tracker.Add(Count::TexHashBytes, 4096U);
    tracker.AddSection(Section::ResolvePlans, 7U);
    WindowTotals window;
    window.Accumulate(tracker.TakeFrame());
    window.Accumulate(tracker.TakeFrame());  // quadro vazio
    EXPECT_EQ(window.Counts[static_cast<size_t>(Count::TexHashes)], 2U);
    EXPECT_EQ(window.Counts[static_cast<size_t>(Count::TexHashBytes)], 4096U);
    EXPECT_EQ(window.SectionNs[static_cast<size_t>(Section::ResolvePlans)],
              TicksToNs(7U));
}

TEST(PicaDrawPhases, BackwardsClockIsNotCharged) {
    Tracker tracker;
    tracker.Begin(100U);
    tracker.Lap(Phase::State, 90U);
    tracker.End(95U);
    const auto frame = tracker.TakeFrame();
    EXPECT_EQ(P(frame, Phase::State), 0U);
    EXPECT_EQ(frame.SubmitTicks, 0U);
}
