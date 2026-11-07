#include "oot3d_native_guest_profile.h"
#include "oot3d_native_compiled_functions.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Oot3dNativeGame {
namespace {

constexpr size_t kFilterBits = 4096U;

size_t FilterBit(uint32_t pc) noexcept {
    return static_cast<size_t>(((pc >> 2U) * 2654435761U) >> 20U);
}

std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1U);
}

} // namespace

uint64_t GuestBlockProfiler::Ticks() noexcept {
#if defined(__aarch64__)
    uint64_t value = 0U;
    asm volatile("mrs %0, cntpct_el0" : "=r"(value)); // armGetSystemTick()
    return value;
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

uint64_t GuestBlockProfiler::TicksPerSecond() noexcept {
#if defined(__aarch64__)
    uint64_t value = 0U;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value != 0U ? value : 19200000U;
#else
    return 1000000000U;
#endif
}

bool GuestBlockProfiler::LoadConfig(const std::filesystem::path& path,
                                    const std::filesystem::path& defaultOutput,
                                    const std::filesystem::path& defaultFunctionMap,
                                    GuestProfileOptions* options) {
    std::error_code error;
    if (options == nullptr || path.empty() || !std::filesystem::exists(path, error)) {
        return false;
    }
    GuestProfileOptions result;
    result.OutputPath = defaultOutput;
    result.FunctionMapPath = defaultFunctionMap;
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = Trim(line.substr(equals + 1U));
        const auto number = [&]() {
            return static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
        };
        if (key == "sample_every") result.SampleEveryFrames = std::max(1U, number());
        else if (key == "dump_frames") result.DumpEveryHostFrames = number();
        else if (key == "top_functions") result.TopFunctions = number();
        else if (key == "top_blocks") result.TopBlocks = number();
        else if (key == "window") result.Window = number() != 0U;
        else if (key == "max_gap_us") result.MaxGapMicroseconds = number();
        else if (key == "output") result.OutputPath = value;
        else if (key == "functions") result.FunctionMapPath = value;
    }
    *options = std::move(result);
    return true;
}

bool GuestBlockProfiler::Start(const GuestProfileOptions& options,
                               uint32_t codeBase, uint32_t codeSize) {
    mOptions = options;
    mBase = codeBase;
    mSlots = codeSize / 4U;
    mCounts.assign(mSlots, 0U);
    mTicks.assign(mSlots, 0U);
    mMaxGapTicks = TicksPerSecond() * mOptions.MaxGapMicroseconds / 1000000U;
    Reset();
    mActive = mSlots != 0U;
    if (mActive) {
        // At boot, not at the first report: the writer and the spike
        // profile share the map read-only from here on.
        if (!mFunctionsLoaded) LoadFunctionMap();
        // The writer's zeroed set, swapped with the live counters at a dump.
        mSnapshot.Counts.assign(mSlots, 0U);
        mSnapshot.Ticks.assign(mSlots, 0U);
        // A pthread: oot3d_switch_threads.cpp makes it a service thread
        // (0x3B on cores 1-2), off the game's core.
        mWriter = std::thread([this] { WriterLoop(); });
        RegisterWithGuestOverrides();
    }
    return mActive;
}

// The frame-level fields; the counters are new (Start) or were swapped out.
void GuestBlockProfiler::Reset() noexcept {
    mTotalEntries = 0U;
    mOutside = 0U;
    mGaps = 0U;
    mGapTicks = 0U;
    mSampledFrames = 0U;
    mLastSlot = UINT32_MAX;
    mStartTicks = Ticks();
    mLastTicks = mStartTicks;
    mSpikeEntriesStart = 0U;
}

GuestBlockProfiler::~GuestBlockProfiler() {
    if (mSpikeOn) FlushSpikes();
    FinishReports();
}

bool GuestBlockProfiler::EnableSpikes(const GuestSpikeConfig& config,
                                      const std::filesystem::path& log,
                                      const std::filesystem::path& summary,
                                      uint32_t flushFrames) {
    if (!mActive) return false;
    if (!mFunctionsLoaded) LoadFunctionMap();
    std::vector<uint32_t> entries;
    entries.reserve(mFunctions.size());
    for (const auto& function : mFunctions) entries.push_back(function.first);
    mSpike.Configure(config, mBase, mSlots,
                     static_cast<double>(TicksPerSecond()) / 1000.0, entries);
    mSpikeLog = log;
    mSpikeSummary = summary;
    mSpikeFlushFrames = std::max(1U, flushFrames);
    // A spike can land on any frame: measure all of them.
    mOptions.SampleEveryFrames = 1U;
    mSampling = true;
    mSpikeOn = mSpike.Enabled();
    if (mSpikeOn) {
        if (FILE* file = std::fopen(mSpikeLog.string().c_str(), "a")) {
            std::fprintf(file,
                         "session_start threshold_ms=%.2f load_cutoff_ms=%.2f "
                         "top=%u functions_in_map=%zu\n",
                         mSpike.Config().ThresholdMs, mSpike.Config().LoadMs,
                         mSpike.Config().TopPerSpike, entries.size());
            std::fclose(file);
        }
    }
    return mSpikeOn;
}

void GuestBlockProfiler::FlushSpikes() {
    if (!mSpikeOn) return;
    const auto nameOf = [this](uint32_t pc) -> const char* {
        const auto it = std::lower_bound(
            mFunctions.begin(), mFunctions.end(), pc,
            [](const auto& item, uint32_t value) { return item.first < value; });
        return it != mFunctions.end() && it->first == pc ? it->second.c_str() : "";
    };
    if (mSpike.Pending() != 0U) {
        mSpikeText.clear();
        (void)mSpike.DrainLines(mSpikeText, nameOf);
        if (FILE* file = std::fopen(mSpikeLog.string().c_str(), "a")) {
            std::fwrite(mSpikeText.data(), 1U, mSpikeText.size(), file);
            std::fclose(file);
        }
    }
    if (mSpike.Totals().Spikes != 0U) {
        mSpikeText.clear();
        mSpike.FormatSummary(mSpikeText, nameOf);
        if (FILE* file = std::fopen(mSpikeSummary.string().c_str(), "w")) {
            std::fwrite(mSpikeText.data(), 1U, mSpikeText.size(), file);
            std::fclose(file);
        }
    }
}

void GuestBlockProfiler::Chain(oot3d::recomp::a32::BlockEntryCallback callback,
                               void* user, std::span<const uint32_t> hooks,
                               bool forwardAll) {
    mCallback = callback;
    mCallbackUser = user;
    mForwardAll = forwardAll;
    mHooks.assign(hooks.begin(), hooks.end());
    std::sort(mHooks.begin(), mHooks.end());
    mHooks.erase(std::unique(mHooks.begin(), mHooks.end()), mHooks.end());
    mHookFilter.assign(kFilterBits / 64U, 0U);
    for (const uint32_t pc : mHooks) {
        const size_t bit = FilterBit(pc);
        mHookFilter[bit >> 6U] |= uint64_t{1} << (bit & 63U);
    }
}

bool GuestBlockProfiler::IsHook(uint32_t pc) const noexcept {
    if (mForwardAll) return true;
    const size_t bit = FilterBit(pc);
    if ((mHookFilter[bit >> 6U] & (uint64_t{1} << (bit & 63U))) == 0U) return false;
    return std::binary_search(mHooks.begin(), mHooks.end(), pc);
}

bool GuestBlockProfiler::ForwardsRange(uint32_t first, uint32_t last) const noexcept {
    if (mCallback == nullptr) return false;
    if (mForwardAll) return true;
    const auto hook = std::lower_bound(mHooks.begin(), mHooks.end(), first);
    return hook != mHooks.end() && *hook <= last;
}

void GuestBlockProfiler::Note(uint32_t pc, uint64_t now) noexcept {
    if (mLastSlot != UINT32_MAX) {
        const uint64_t delta = now - mLastTicks;
        if (delta <= mMaxGapTicks) {
            mTicks[mLastSlot] += delta;
            if (mSpikeOn) mSpike.Add(mLastSlot, delta);
        } else {
            ++mGaps;
            mGapTicks += delta;
        }
    }
    ++mTotalEntries;
    const uint32_t slot = (pc - mBase) >> 2U;
    if (slot < mSlots && pc >= mBase) {
        ++mCounts[slot];
        mLastSlot = slot;
    } else {
        ++mOutside;
        mLastSlot = UINT32_MAX;
    }
}

namespace {

// Registration for the native overrides (oot3d_native_guest_overrides.h):
// they query the forwarding filter and mark their own entry.
bool OverrideForwards(const void* user, uint32_t first, uint32_t last) noexcept {
    return static_cast<const GuestBlockProfiler*>(user)->ForwardsRange(first, last);
}

void OverrideMark(void* user, uint32_t pc) noexcept {
    static_cast<GuestBlockProfiler*>(user)->MarkNative(pc);
}

} // namespace

void GuestBlockProfiler::MarkNative(uint32_t pc) noexcept {
    Note(pc, Ticks());
    mLastTicks = Ticks();
}

void GuestBlockProfiler::RegisterWithGuestOverrides() noexcept {
    RegisterOot3dGuestOverrideProfileHook(&GuestBlockProfiler::OnBlockEntry,
                                          &OverrideForwards, &OverrideMark);
}

void GuestBlockProfiler::OnBlockEntry(uint32_t pc,
                                      oot3d::recomp::a32::GuestState& state,
                                      oot3d::recomp::a32::MemoryBus& memory,
                                      void* user) {
    auto& self = *static_cast<GuestBlockProfiler*>(user);
    self.Note(pc, Ticks());
    if (self.mCallback != nullptr && self.IsHook(pc)) {
        self.mCallback(pc, state, memory, self.mCallbackUser);
        // Hook time (host policy) does not belong to the guest block.
        self.mLastTicks = Ticks();
        return;
    }
    self.mLastTicks = Ticks();
}

bool GuestBlockProfiler::OnHostFrame(uint64_t hostFrame) {
    if (!mActive) return false;
    if (mSpikeOn) {
        // The frame that just ended: spike? The first includes the initial load.
        if (mSpikeFrameSeen) {
            (void)mSpike.EndFrame(hostFrame, mTotalEntries - mSpikeEntriesStart);
        } else {
            mSpikeFrameSeen = true;
            mSpikeLastFlush = hostFrame;
            (void)mSpike.EndFrame(hostFrame, 0U); // the initial load (load=1)
        }
        mSpikeEntriesStart = mTotalEntries;
        if (hostFrame - mSpikeLastFlush >= mSpikeFlushFrames) {
            mSpikeLastFlush = hostFrame;
            FlushSpikes();
        }
    }
    if (mSampling) {
        ++mSampledFrames;
        ++mSampledSinceReport;
    }
    if (!mFrameSeen) {
        mFrameSeen = true;
        mStartFrame = hostFrame;
        mLastDumpFrame = hostFrame;
    }
    if (mOptions.DumpEveryHostFrames != 0U &&
        hostFrame - mLastDumpFrame >= mOptions.DumpEveryHostFrames) {
        // The writer sorts and writes; while it is still on the previous
        // report (a slow SD write) the next frame tries again.
        if (QueueReport(false)) {
            mLastDumpFrame = hostFrame;
            if (mOptions.Window) {
                Reset();
                mStartFrame = hostFrame;
            }
        } else {
            ++mReportsPostponed;
        }
    }
    mSampling = (hostFrame + 1U) % mOptions.SampleEveryFrames == 0U;
    mLastSlot = UINT32_MAX;
    return mSampling;
}

bool GuestBlockProfiler::QueueReport(bool wait) {
    std::unique_lock lock(mWriterMutex, std::defer_lock);
    if (wait) {
        lock.lock();
        mWriterChanged.wait(lock, [this] { return mSnapshotState == SnapshotState::Spare; });
    } else if (!lock.try_lock() || mSnapshotState != SnapshotState::Spare) {
        return false;
    }
    // The writer's set is zeroed: the live counters continue from zero, the
    // writer folds them into its totals (window=0) or reports them alone.
    mCounts.swap(mSnapshot.Counts);
    mTicks.swap(mSnapshot.Ticks);
    mSnapshot.TotalEntries = mTotalEntries;
    mSnapshot.Outside = mOutside;
    mSnapshot.Gaps = mGaps;
    mSnapshot.GapTicks = mGapTicks;
    mSnapshot.SampledFrames = mSampledFrames;
    mSnapshot.ElapsedTicks = Ticks() - mStartTicks;
    mSnapshot.Window = mOptions.Window;
    mSnapshot.Appendix.clear();
    if (mAppendix) {
        // Counters the game thread owns (native overrides): read here.
        std::ostringstream appendix;
        appendix << '\n';
        mAppendix(appendix);
        mSnapshot.Appendix = appendix.str();
    }
    mSampledSinceReport = 0U;
    mSnapshotState = SnapshotState::Queued;
    lock.unlock();
    mWriterChanged.notify_all();
    return true;
}

void GuestBlockProfiler::WriterLoop() {
    std::unique_lock lock(mWriterMutex);
    for (;;) {
        mWriterChanged.wait(lock, [this] {
            return mWriterStop || mSnapshotState == SnapshotState::Queued;
        });
        if (mSnapshotState != SnapshotState::Queued) return;  // stopped
        mSnapshotState = SnapshotState::Writing;
        lock.unlock();
        const bool ok = WriteReport(mSnapshot);
        lock.lock();
        mLastWriteOk = ok;
        mReportsWritten.fetch_add(1U, std::memory_order_relaxed);
        mSnapshotState = SnapshotState::Spare;
        mWriterChanged.notify_all();
    }
}

void GuestBlockProfiler::StopWriter() {
    if (!mWriter.joinable()) return;
    {
        std::lock_guard lock(mWriterMutex);
        mWriterStop = true;
    }
    mWriterChanged.notify_all();
    mWriter.join();
}

void GuestBlockProfiler::WaitWriterIdle() {
    std::unique_lock lock(mWriterMutex);
    mWriterChanged.wait(lock, [this] { return mSnapshotState == SnapshotState::Spare; });
}

bool GuestBlockProfiler::FinishReports() {
    if (!mWriter.joinable()) return mLastWriteOk;
    // Frames measured since the last report: one more, from here.
    if (mFrameSeen && mSampledSinceReport != 0U) QueueReport(true);
    WaitWriterIdle();
    StopWriter();
    return mLastWriteOk;
}

void GuestBlockProfiler::LoadFunctionMap() {
    mFunctionsLoaded = true;
    std::ifstream file(mOptions.FunctionMapPath);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string entry, name;
        fields >> entry >> name;
        if (entry.empty()) continue;
        mFunctions.emplace_back(
            static_cast<uint32_t>(std::strtoul(entry.c_str(), nullptr, 16)), name);
    }
    std::sort(mFunctions.begin(), mFunctions.end());
}

bool GuestBlockProfiler::WriteReport(ReportSnapshot& snapshot) {
    const double ticksPerMs = static_cast<double>(TicksPerSecond()) / 1000.0;
    const uint64_t elapsed = snapshot.ElapsedTicks;
    const uint64_t frames = snapshot.SampledFrames; // measured frames
    const uint64_t totalEntries = snapshot.TotalEntries;

    struct Row {
        uint32_t Pc = 0U;
        uint64_t Count = 0U;
        uint64_t Ticks = 0U;
        uint32_t Blocks = 0U;
    };
    std::vector<Row> blocks;
    uint64_t attributed = 0U;
    const auto addBlock = [&](uint32_t slot, uint64_t count, uint64_t ticks) {
        blocks.push_back({mBase + slot * 4U, count, ticks, 1U});
        attributed += ticks;
    };
    if (snapshot.Window) {
        for (uint32_t slot = 0; slot < mSlots; ++slot) {
            if (snapshot.Counts[slot] == 0U && snapshot.Ticks[slot] == 0U) continue;
            addBlock(slot, snapshot.Counts[slot], snapshot.Ticks[slot]);
        }
    } else {
        // Cumulative: the counters since the previous report join the
        // totals (both by slot).
        std::vector<SlotTotal> merged;
        merged.reserve(mTotals.size() + 4096U);
        size_t next = 0U;
        for (uint32_t slot = 0; slot < mSlots; ++slot) {
            const bool live = snapshot.Counts[slot] != 0U || snapshot.Ticks[slot] != 0U;
            const bool known = next < mTotals.size() && mTotals[next].Slot == slot;
            if (!live && !known) continue;
            SlotTotal total{slot, 0U, 0U};
            if (known) total = mTotals[next++];
            total.Count += snapshot.Counts[slot];
            total.Ticks += snapshot.Ticks[slot];
            merged.push_back(total);
        }
        mTotals.swap(merged);
        for (const auto& total : mTotals) addBlock(total.Slot, total.Count, total.Ticks);
    }
    // Zeroed again for the next swap.
    std::fill(snapshot.Counts.begin(), snapshot.Counts.end(), 0U);
    std::fill(snapshot.Ticks.begin(), snapshot.Ticks.end(), 0U);
    // Aggregate by function: greatest map entry <= pc.
    std::vector<Row> functions;
    if (!mFunctions.empty()) {
        std::vector<Row> byIndex(mFunctions.size());
        Row unknown;
        for (const auto& row : blocks) {
            auto it = std::upper_bound(
                mFunctions.begin(), mFunctions.end(), row.Pc,
                [](uint32_t pc, const auto& item) { return pc < item.first; });
            Row* target = &unknown;
            if (it != mFunctions.begin()) {
                target = &byIndex[static_cast<size_t>(std::prev(it) - mFunctions.begin())];
                target->Pc = std::prev(it)->first;
            }
            target->Count += row.Count;
            target->Ticks += row.Ticks;
            ++target->Blocks;
        }
        for (const auto& row : byIndex) {
            if (row.Count != 0U || row.Ticks != 0U) functions.push_back(row);
        }
        if (unknown.Count != 0U) functions.push_back(unknown);
    }
    const auto byTime = [](const Row& a, const Row& b) {
        return a.Ticks != b.Ticks ? a.Ticks > b.Ticks : a.Pc < b.Pc;
    };
    std::sort(blocks.begin(), blocks.end(), byTime);
    std::sort(functions.begin(), functions.end(), byTime);
    const auto nameOf = [&](uint32_t pc) -> std::string {
        auto it = std::lower_bound(
            mFunctions.begin(), mFunctions.end(), pc,
            [](const auto& item, uint32_t value) { return item.first < value; });
        return it != mFunctions.end() && it->first == pc ? it->second : std::string{};
    };

    std::ofstream out(mOptions.OutputPath, std::ios::trunc);
    if (!out) return false;
    char line[512];
    const double attributedMs = static_cast<double>(attributed) / ticksPerMs;
    std::snprintf(line, sizeof(line),
                  "# triaevum guest_profile v1\n"
                  "# sampled_frames=%" PRIu64 " (1 of every %u) window_ms=%.1f block_entries=%" PRIu64
                  " outside_code=%" PRIu64 "\n"
                  "# attributed_ms=%.1f (%.2f ms/frame) dropped_gaps=%" PRIu64
                  " (%.1f ms, guest away: SVC/host)\n"
                  "# time per block = from the block's entry to the next entry,\n"
                  "# includes the profiler's own overhead (~constant per entry).\n",
                  frames, mOptions.SampleEveryFrames, static_cast<double>(elapsed) / ticksPerMs,
                  totalEntries,
                  snapshot.Outside, attributedMs,
                  frames != 0U ? attributedMs / static_cast<double>(frames) : 0.0, snapshot.Gaps,
                  static_cast<double>(snapshot.GapTicks) / ticksPerMs);
    out << line;
    if (!functions.empty()) {
        out << "\n# functions: entry | %time | ms | ms/frame | entries | %entries | "
               "ns/entry | blocks | name\n";
        const size_t limit = std::min<size_t>(functions.size(), mOptions.TopFunctions);
        for (size_t i = 0; i < limit; ++i) {
            const auto& row = functions[i];
            const double ms = static_cast<double>(row.Ticks) / ticksPerMs;
            std::snprintf(line, sizeof(line),
                          "0x%08X %6.2f%% %9.2f %7.3f %11" PRIu64 " %6.2f%% %7.1f %4u %s\n",
                          row.Pc,
                          attributed != 0U ? 100.0 * static_cast<double>(row.Ticks) /
                                                 static_cast<double>(attributed) : 0.0,
                          ms, frames != 0U ? ms / static_cast<double>(frames) : 0.0,
                          row.Count,
                          totalEntries != 0U ? 100.0 * static_cast<double>(row.Count) /
                                                   static_cast<double>(totalEntries) : 0.0,
                          row.Count != 0U ? ms * 1.0e6 / static_cast<double>(row.Count) : 0.0,
                          row.Blocks, row.Pc == 0U ? "(outside map)" : nameOf(row.Pc).c_str());
            out << line;
        }
    }
    out << "\n# blocks: pc | %time | ms | entries | ns/entry\n";
    const size_t limit = std::min<size_t>(blocks.size(), mOptions.TopBlocks);
    for (size_t i = 0; i < limit; ++i) {
        const auto& row = blocks[i];
        const double ms = static_cast<double>(row.Ticks) / ticksPerMs;
        std::snprintf(line, sizeof(line), "0x%08X %6.2f%% %9.2f %11" PRIu64 " %7.1f\n",
                      row.Pc,
                      attributed != 0U ? 100.0 * static_cast<double>(row.Ticks) /
                                             static_cast<double>(attributed) : 0.0,
                      ms, row.Count,
                      row.Count != 0U ? ms * 1.0e6 / static_cast<double>(row.Count) : 0.0);
        out << line;
    }
    out << snapshot.Appendix;
    return static_cast<bool>(out);
}

} // namespace Oot3dNativeGame
