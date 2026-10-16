#include "oot3d_native_guest_profile.h"

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
    return mActive;
}

void GuestBlockProfiler::Reset() noexcept {
    std::fill(mCounts.begin(), mCounts.end(), 0U);
    std::fill(mTicks.begin(), mTicks.end(), 0U);
    mTotalEntries = 0U;
    mOutside = 0U;
    mGaps = 0U;
    mGapTicks = 0U;
    mSampledFrames = 0U;
    mLastSlot = UINT32_MAX;
    mStartTicks = Ticks();
    mLastTicks = mStartTicks;
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

void GuestBlockProfiler::OnBlockEntry(uint32_t pc,
                                      oot3d::recomp::a32::GuestState& state,
                                      oot3d::recomp::a32::MemoryBus& memory,
                                      void* user) {
    auto& self = *static_cast<GuestBlockProfiler*>(user);
    const uint64_t now = Ticks();
    if (self.mLastSlot != UINT32_MAX) {
        const uint64_t delta = now - self.mLastTicks;
        if (delta <= self.mMaxGapTicks) {
            self.mTicks[self.mLastSlot] += delta;
        } else {
            ++self.mGaps;
            self.mGapTicks += delta;
        }
    }
    ++self.mTotalEntries;
    const uint32_t slot = (pc - self.mBase) >> 2U;
    if (slot < self.mSlots && pc >= self.mBase) {
        ++self.mCounts[slot];
        self.mLastSlot = slot;
    } else {
        ++self.mOutside;
        self.mLastSlot = UINT32_MAX;
    }
    if (self.mCallback != nullptr && self.IsHook(pc)) {
        self.mCallback(pc, state, memory, self.mCallbackUser);
        // O tempo do gancho (política do host) não é do bloco guest.
        self.mLastTicks = Ticks();
        return;
    }
    self.mLastTicks = Ticks();
}

bool GuestBlockProfiler::OnHostFrame(uint64_t hostFrame) {
    if (!mActive) return false;
    if (mSampling) ++mSampledFrames;
    if (!mFrameSeen) {
        mFrameSeen = true;
        mStartFrame = hostFrame;
        mLastDumpFrame = hostFrame;
    }
    if (mOptions.DumpEveryHostFrames != 0U &&
        hostFrame - mLastDumpFrame >= mOptions.DumpEveryHostFrames) {
        WriteReport(hostFrame);
        mLastDumpFrame = hostFrame;
        if (mOptions.Window) {
            Reset();
            mStartFrame = hostFrame;
        }
    }
    mSampling = (hostFrame + 1U) % mOptions.SampleEveryFrames == 0U;
    mLastSlot = UINT32_MAX;
    return mSampling;
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

bool GuestBlockProfiler::WriteReport(uint64_t hostFrame) {
    if (!mActive) return false;
    static_cast<void>(hostFrame);
    if (!mFunctionsLoaded) LoadFunctionMap();
    const double ticksPerMs = static_cast<double>(TicksPerSecond()) / 1000.0;
    const uint64_t elapsed = Ticks() - mStartTicks;
    const uint64_t frames = mSampledFrames; // quadros medidos

    struct Row {
        uint32_t Pc = 0U;
        uint64_t Count = 0U;
        uint64_t Ticks = 0U;
        uint32_t Blocks = 0U;
    };
    std::vector<Row> blocks;
    uint64_t attributed = 0U;
    for (uint32_t slot = 0; slot < mSlots; ++slot) {
        if (mCounts[slot] == 0U && mTicks[slot] == 0U) continue;
        blocks.push_back({mBase + slot * 4U, mCounts[slot], mTicks[slot], 1U});
        attributed += mTicks[slot];
    }
    // Agrega por função: maior entrada do mapa <= pc.
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
                  "# quadros_medidos=%" PRIu64 " (1 a cada %u) janela_ms=%.1f entradas_de_bloco=%" PRIu64
                  " fora_do_codigo=%" PRIu64 "\n"
                  "# tempo_atribuido_ms=%.1f (%.2f ms/quadro) intervalos_descartados=%" PRIu64
                  " (%.1f ms, guest fora: SVC/host)\n"
                  "# tempo por bloco = desde a entrada do bloco ate a proxima entrada,\n"
                  "# inclui a sobrecarga do proprio perfil (~constante por entrada).\n",
                  frames, mOptions.SampleEveryFrames, static_cast<double>(elapsed) / ticksPerMs,
                  mTotalEntries,
                  mOutside, attributedMs,
                  frames != 0U ? attributedMs / static_cast<double>(frames) : 0.0, mGaps,
                  static_cast<double>(mGapTicks) / ticksPerMs);
    out << line;
    if (!functions.empty()) {
        out << "\n# funcoes: entrada | %tempo | ms | ms/quadro | entradas | %entradas | "
               "ns/entrada | blocos | nome\n";
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
                          mTotalEntries != 0U ? 100.0 * static_cast<double>(row.Count) /
                                                    static_cast<double>(mTotalEntries) : 0.0,
                          row.Count != 0U ? ms * 1.0e6 / static_cast<double>(row.Count) : 0.0,
                          row.Blocks, row.Pc == 0U ? "(fora do mapa)" : nameOf(row.Pc).c_str());
            out << line;
        }
    }
    out << "\n# blocos: pc | %tempo | ms | entradas | ns/entrada\n";
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
    if (mAppendix) {
        out << '\n';
        mAppendix(out);
    }
    return static_cast<bool>(out);
}

} // namespace Oot3dNativeGame
