#pragma once

// Guest code spike profile (config/guest_spike_ms).
//
// tour19 (pgo5n, 2026-10-23): 79% of the 428 missed refreshes had the
// game thread as the culprit (stage=guest in logs/spikes.txt); outside of
// loads there were 1-10 per window, and the Water Temple had 38 in one window
// (guest_ms 20-26 vs 8-11 on average). spikes.txt says WHEN; this profile
// says WHICH game code was heavy in those frames.
//
// On top of GuestBlockProfiler (oot3d_native_guest_profile.h, "all
// blocks" mode, cntpct_el0 clock): each interval attributed to a block
// also goes into the frame accumulator (Add, hot path: one uint32 per
// text block and a list of touched blocks, both allocated once). At the
// end of the host frame (EndFrame):
//  - frame with guest time > threshold: aggregates touched blocks per function
//    (config/guest_functions.txt map; without it, per block), keeps the N
//    most expensive in a fixed ring and adds all to the summary (fixed hash table);
//  - always: zeroes only the touched blocks.
// Formatting and writing happen outside the frame (DrainLines/FormatSummary, in the
// perf.txt window): logs/guest_spikes.txt (one block per spike) and
// logs/guest_spikes_summary.txt (top 30 by total time in frames with a
// spike, rewritten every window: valid "at the end" even if the game is closed
// via HOME).
//
// Cost: all-blocks mode calls the callback on every block entry
// (the game runs slower while enabled; times include that
// overhead, ~constant per entry: each spike's "entries" allow
// subtracting it). That's why it's behind the switch, off by default.
//
// config/guest_spike_ms: present enables it. Optional content "threshold [cutoff]":
// threshold in ms (default 14) and load cutoff in ms (default 50; frames
// above it go to the log with load=1 but not to the summary).
//
// Pure logic (no file, no clock): oot3d_guest_spike_profile_tests.cpp.

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

inline constexpr uint32_t kGuestSpikeMaxTop = 16U;

struct GuestSpikeConfig {
  double ThresholdMs = 14.0;
  double LoadMs = 50.0;
  uint32_t TopPerSpike = 10U;      // <= kGuestSpikeMaxTop
  uint32_t TouchedCapacity = 65536U;
  uint32_t RingCapacity = 128U;
  uint32_t SummaryCapacity = 16384U; // power of 2
  uint32_t SummaryTop = 30U;
};

// "14", "12,5", "14 60": threshold (1-200 ms) and load cutoff
// (> threshold, up to 5000). Empty or invalid: the defaults.
inline void ParseGuestSpikeConfig(std::string_view text,
                                  GuestSpikeConfig *config) noexcept {
  const auto number = [&](double *out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n'))
      text.remove_prefix(1U);
    double value = 0.0, divisor = 1.0;
    bool digits = false, point = false;
    while (!text.empty()) {
      const char c = text.front();
      if (c >= '0' && c <= '9') {
        digits = true;
        if (point) {
          divisor *= 10.0;
          value += (c - '0') / divisor;
        } else {
          value = value * 10.0 + (c - '0');
        }
      } else if ((c == '.' || c == ',') && !point) {
        point = true;
      } else {
        break;
      }
      text.remove_prefix(1U);
    }
    if (digits)
      *out = value;
    return digits;
  };
  double threshold = 0.0, load = 0.0;
  if (number(&threshold) && threshold >= 1.0 && threshold <= 200.0)
    config->ThresholdMs = threshold;
  if (number(&load) && load > config->ThresholdMs && load <= 5000.0)
    config->LoadMs = load;
  if (config->LoadMs <= config->ThresholdMs)
    config->LoadMs = config->ThresholdMs * 4.0;
}

struct GuestSpikeEntry {
  uint32_t Pc = 0U; // function entry (or the block, without a map)
  uint32_t Ticks = 0U;
};

struct GuestSpikeRecord {
  uint64_t HostFrame = 0U;
  uint64_t FrameTicks = 0U;
  uint64_t Entries = 0U;  // block entries in the frame
  uint32_t Blocks = 0U;   // distinct blocks
  uint32_t Functions = 0U;
  bool Overflow = false;  // more blocks than TouchedCapacity
  bool Load = false;      // above the load cutoff
  uint32_t Count = 0U;
  std::array<GuestSpikeEntry, kGuestSpikeMaxTop> Top{};
};

struct GuestSpikeTotals {
  uint64_t Frames = 0U;
  uint64_t Spikes = 0U;
  uint64_t LoadSpikes = 0U;
  uint64_t FrameTicks = 0U;
  uint64_t SpikeTicks = 0U; // spike frames outside of loads
  uint64_t RingDropped = 0U;
  uint64_t OverflowFrames = 0U;
  uint64_t SummaryFull = 0U;
};

class GuestSpikeProfile {
public:
  // functionEntries: function entries in ascending order (may be empty).
  void Configure(const GuestSpikeConfig &config, uint32_t base, uint32_t slots,
                 double ticksPerMs, std::span<const uint32_t> functionEntries) {
    mConfig = config;
    mConfig.TopPerSpike = std::clamp<uint32_t>(mConfig.TopPerSpike, 1U,
                                               kGuestSpikeMaxTop);
    uint32_t capacity = 1U;
    while (capacity < std::max<uint32_t>(mConfig.SummaryCapacity, 64U))
      capacity <<= 1U;
    mConfig.SummaryCapacity = capacity;
    mBase = base;
    mTicksPerMs = ticksPerMs > 0.0 ? ticksPerMs : 1.0;
    mThresholdTicks = static_cast<uint64_t>(mConfig.ThresholdMs * mTicksPerMs);
    mLoadTicks = static_cast<uint64_t>(mConfig.LoadMs * mTicksPerMs);
    mFrameTicks.assign(slots, 0U);
    mTouched.assign(std::max<uint32_t>(mConfig.TouchedCapacity, 1U), 0U);
    mTouchedCount = 0U;
    mScratch.assign(mTouched.size(), GuestSpikeEntry{});
    mRing.assign(std::max<uint32_t>(mConfig.RingCapacity, 1U), GuestSpikeRecord{});
    mRingHead = mRingSize = 0U;
    mSummary.assign(mConfig.SummaryCapacity, SummarySlot{});
    mSummaryUsed = 0U;
    mFunctions.assign(functionEntries.begin(), functionEntries.end());
    mFrameTotal = 0U;
    mOverflow = false;
    mTotals = {};
    mEnabled = slots != 0U;
  }

  [[nodiscard]] bool Enabled() const noexcept { return mEnabled; }
  [[nodiscard]] const GuestSpikeConfig &Config() const noexcept { return mConfig; }
  [[nodiscard]] const GuestSpikeTotals &Totals() const noexcept { return mTotals; }
  [[nodiscard]] double TicksPerMs() const noexcept { return mTicksPerMs; }

  // Hot path: `ticks` attributed to block `slot` in this frame.
  void Add(uint32_t slot, uint64_t ticks) noexcept {
    mFrameTotal += ticks;
    if (slot >= mFrameTicks.size() || ticks == 0U)
      return;
    uint32_t &frame = mFrameTicks[slot];
    if (frame == 0U) {
      if (mTouchedCount >= mTouched.size()) {
        mOverflow = true;
        return;
      }
      mTouched[mTouchedCount++] = slot;
    }
    const uint64_t sum = static_cast<uint64_t>(frame) + ticks;
    frame = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
  }

  // End of the host frame. true: the frame was a spike (recorded).
  bool EndFrame(uint64_t hostFrame, uint64_t blockEntries) {
    if (!mEnabled)
      return false;
    ++mTotals.Frames;
    mTotals.FrameTicks += mFrameTotal;
    const bool spike = mFrameTotal > mThresholdTicks;
    if (spike)
      RecordSpike(hostFrame, blockEntries);
    for (uint32_t i = 0; i < mTouchedCount; ++i)
      mFrameTicks[mTouched[i]] = 0U;
    mTouchedCount = 0U;
    mFrameTotal = 0U;
    mOverflow = false;
    return spike;
  }

  [[nodiscard]] size_t Pending() const noexcept { return mRingSize; }

  // Stored spikes -> text (outside the frame). nameOf(pc) -> const char*
  // ("" if unnamed). Empties the ring.
  template <typename NameOf>
  size_t DrainLines(std::string &out, NameOf &&nameOf) {
    size_t drained = 0U;
    char line[256];
    while (mRingSize != 0U) {
      const auto &record = mRing[(mRingHead + mRing.size() - mRingSize) % mRing.size()];
      --mRingSize;
      ++drained;
      const double frameMs = static_cast<double>(record.FrameTicks) / mTicksPerMs;
      std::snprintf(line, sizeof(line),
                    "spike host_frame=%" PRIu64 " guest_ms=%.2f entries=%" PRIu64
                    " blocks=%u functions=%u load=%d overflow=%d\n",
                    record.HostFrame, frameMs, record.Entries, record.Blocks,
                    record.Functions, record.Load ? 1 : 0, record.Overflow ? 1 : 0);
      out += line;
      for (uint32_t i = 0; i < record.Count; ++i) {
        const auto &entry = record.Top[i];
        const double ms = static_cast<double>(entry.Ticks) / mTicksPerMs;
        const char *name = nameOf(entry.Pc);
        std::snprintf(line, sizeof(line), "  fn=0x%08X ms=%.3f pct=%.1f name=%s\n",
                      entry.Pc, ms, frameMs > 0.0 ? 100.0 * ms / frameMs : 0.0,
                      name != nullptr && name[0] != '\0' ? name : "-");
        out += line;
      }
    }
    return drained;
  }

  // Aggregated summary: the SummaryTop functions with the highest total time in
  // spike frames (outside of loads).
  template <typename NameOf>
  void FormatSummary(std::string &out, NameOf &&nameOf) const {
    std::vector<SummarySlot> rows;
    rows.reserve(mSummaryUsed);
    for (const auto &slot : mSummary)
      if (slot.Pc != 0U)
        rows.push_back(slot);
    std::sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) {
      return a.Ticks != b.Ticks ? a.Ticks > b.Ticks : a.Pc < b.Pc;
    });
    const auto ms = [&](uint64_t ticks) {
      return static_cast<double>(ticks) / mTicksPerMs;
    };
    const uint64_t normalSpikes = mTotals.Spikes - mTotals.LoadSpikes;
    char line[320];
    std::snprintf(
        line, sizeof(line),
        "# triaevum guest_spikes v1\n"
        "# threshold_ms=%.2f load_cutoff_ms=%.2f frames=%" PRIu64
        " spikes=%" PRIu64 " load_spikes=%" PRIu64 "\n"
        "# guest_ms_avg=%.3f guest_ms_avg_spike=%.3f ring_dropped=%" PRIu64
        " overflow_frames=%" PRIu64 " summary_full=%" PRIu64 "\n",
        mConfig.ThresholdMs, mConfig.LoadMs, mTotals.Frames, mTotals.Spikes,
        mTotals.LoadSpikes,
        mTotals.Frames != 0U ? ms(mTotals.FrameTicks) / static_cast<double>(mTotals.Frames) : 0.0,
        normalSpikes != 0U ? ms(mTotals.SpikeTicks) / static_cast<double>(normalSpikes) : 0.0,
        mTotals.RingDropped, mTotals.OverflowFrames, mTotals.SummaryFull);
    out += line;
    out += "# top by total time in spike frames: entry | ms_total | "
           "%spike_time | frames | ms/frame | ms_max | name\n";
    const size_t limit = std::min<size_t>(rows.size(), mConfig.SummaryTop);
    for (size_t i = 0; i < limit; ++i) {
      const auto &row = rows[i];
      const char *name = nameOf(row.Pc);
      std::snprintf(line, sizeof(line), "0x%08X %9.2f %6.2f%% %6u %7.3f %7.3f %s\n",
                    row.Pc, ms(row.Ticks),
                    mTotals.SpikeTicks != 0U
                        ? 100.0 * static_cast<double>(row.Ticks) /
                              static_cast<double>(mTotals.SpikeTicks)
                        : 0.0,
                    row.Frames,
                    row.Frames != 0U ? ms(row.Ticks) / static_cast<double>(row.Frames) : 0.0,
                    ms(row.MaxTicks),
                    name != nullptr && name[0] != '\0' ? name : "-");
      out += line;
    }
  }

private:
  struct SummarySlot {
    uint32_t Pc = 0U;
    uint32_t Frames = 0U;
    uint64_t Ticks = 0U;
    uint32_t MaxTicks = 0U;
  };

  uint32_t KeyOf(uint32_t slot) const noexcept {
    const uint32_t pc = mBase + slot * 4U;
    if (mFunctions.empty())
      return pc;
    const auto it = std::upper_bound(mFunctions.begin(), mFunctions.end(), pc);
    return it == mFunctions.begin() ? pc : *std::prev(it);
  }

  void RecordSpike(uint64_t hostFrame, uint64_t blockEntries) {
    ++mTotals.Spikes;
    const bool load = mFrameTotal > mLoadTicks;
    if (load)
      ++mTotals.LoadSpikes;
    else
      mTotals.SpikeTicks += mFrameTotal;
    if (mOverflow)
      ++mTotals.OverflowFrames;
    // Blocks -> functions: sort by key and sum (preallocated scratch).
    for (uint32_t i = 0; i < mTouchedCount; ++i)
      mScratch[i] = {KeyOf(mTouched[i]), mFrameTicks[mTouched[i]]};
    auto *begin = mScratch.data();
    auto *end = begin + mTouchedCount;
    std::sort(begin, end, [](const auto &a, const auto &b) { return a.Pc < b.Pc; });
    auto *write = begin;
    for (auto *read = begin; read != end; ++read) {
      if (write != begin && std::prev(write)->Pc == read->Pc) {
        const uint64_t sum = static_cast<uint64_t>(std::prev(write)->Ticks) + read->Ticks;
        std::prev(write)->Ticks = sum > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(sum);
      } else {
        *write++ = *read;
      }
    }
    const size_t functions = static_cast<size_t>(write - begin);
    if (!load)
      for (size_t i = 0; i < functions; ++i)
        AddSummary(begin[i]);
    const size_t top = std::min<size_t>(functions, mConfig.TopPerSpike);
    std::partial_sort(begin, begin + top, write, [](const auto &a, const auto &b) {
      return a.Ticks != b.Ticks ? a.Ticks > b.Ticks : a.Pc < b.Pc;
    });
    if (mRingSize == mRing.size()) {
      ++mTotals.RingDropped; // the oldest goes out
      --mRingSize;
    }
    auto &record = mRing[mRingHead];
    mRingHead = (mRingHead + 1U) % mRing.size();
    ++mRingSize;
    record.HostFrame = hostFrame;
    record.FrameTicks = mFrameTotal;
    record.Entries = blockEntries;
    record.Blocks = mTouchedCount;
    record.Functions = static_cast<uint32_t>(functions);
    record.Overflow = mOverflow;
    record.Load = load;
    record.Count = static_cast<uint32_t>(top);
    for (size_t i = 0; i < top; ++i)
      record.Top[i] = begin[i];
  }

  void AddSummary(const GuestSpikeEntry &entry) noexcept {
    const uint32_t mask = static_cast<uint32_t>(mSummary.size()) - 1U;
    uint32_t index = ((entry.Pc >> 2U) * 2654435761U) & mask;
    for (uint32_t probe = 0; probe <= mask; ++probe) {
      auto &slot = mSummary[index];
      if (slot.Pc == entry.Pc || slot.Pc == 0U) {
        if (slot.Pc == 0U) {
          if (mSummaryUsed * 4U >= mSummary.size() * 3U) {
            ++mTotals.SummaryFull;
            return;
          }
          slot.Pc = entry.Pc;
          ++mSummaryUsed;
        }
        ++slot.Frames;
        slot.Ticks += entry.Ticks;
        slot.MaxTicks = std::max(slot.MaxTicks, entry.Ticks);
        return;
      }
      index = (index + 1U) & mask;
    }
    ++mTotals.SummaryFull;
  }

  GuestSpikeConfig mConfig{};
  bool mEnabled = false;
  uint32_t mBase = 0U;
  double mTicksPerMs = 1.0;
  uint64_t mThresholdTicks = 0U;
  uint64_t mLoadTicks = 0U;
  std::vector<uint32_t> mFrameTicks;
  std::vector<uint32_t> mTouched;
  uint32_t mTouchedCount = 0U;
  std::vector<GuestSpikeEntry> mScratch;
  std::vector<GuestSpikeRecord> mRing;
  size_t mRingHead = 0U;
  size_t mRingSize = 0U;
  std::vector<SummarySlot> mSummary;
  size_t mSummaryUsed = 0U;
  std::vector<uint32_t> mFunctions;
  uint64_t mFrameTotal = 0U;
  bool mOverflow = false;
  GuestSpikeTotals mTotals{};
};

} // namespace Oot3dNativeGame
