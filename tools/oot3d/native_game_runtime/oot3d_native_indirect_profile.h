#pragma once

// Indirect call profile (config/indirect_profile; off by default): counts the
// whole-AOT indirect calls (BLX reg and computed ADD pc calls) per call site
// and target and prints the hottest pairs once per logs/perf.txt window as an
// "indirect:" line. It feeds a later round that guards the hottest targets
// with direct calls. Header-only; tests in
// oot3d_native_perf_diagnostics_tests.cpp.
//
// The generated dispatcher (ExecuteOot3dWholeAotIndirect, whole_aot_cpp.py)
// records through gOot3dIndirectProfile only while it is non-null, so the
// default build pays one load and one predicted branch per indirect call and
// guest state is never touched. A site is identified by the guest return
// address in LR at the call (BLX: the call pc + 4; computed calls: the resume
// pc that the call returns to).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace Oot3dNativeGame {

class Oot3dIndirectProfile final {
  public:
    static constexpr size_t kSlots = 4096U; // power of two, open addressing
    static constexpr size_t kTop = 16U;

    struct Pair {
        uint32_t Return = 0;
        uint32_t Target = 0;
        uint64_t Count = 0;
    };

    void Record(uint32_t returnPc, uint32_t target) noexcept {
        ++mCalls;
        const uint64_t key = (uint64_t{returnPc} << 32U) | target;
        size_t slot = static_cast<size_t>((key * 0x9E3779B97F4A7C15ULL) >> 52U) & (kSlots - 1U);
        for (size_t probe = 0; probe < kMaxProbes; ++probe) {
            Pair& pair = mPairs[slot];
            if (pair.Count == 0U) {
                pair = {returnPc, target, 1U};
                ++mUsed;
                return;
            }
            if (pair.Return == returnPc && pair.Target == target) {
                ++pair.Count;
                return;
            }
            slot = (slot + 1U) & (kSlots - 1U);
        }
        ++mDropped;
    }

    [[nodiscard]] uint64_t Calls() const noexcept { return mCalls; }
    [[nodiscard]] size_t Pairs() const noexcept { return mUsed; }
    [[nodiscard]] uint64_t Dropped() const noexcept { return mDropped; }

    // Most calls first; ties by return then target, so the line is stable.
    size_t Top(std::array<Pair, kTop>& out) const {
        std::array<Pair, kTop> best{};
        size_t count = 0U;
        const auto before = [](const Pair& a, const Pair& b) {
            if (a.Count != b.Count) return a.Count > b.Count;
            if (a.Return != b.Return) return a.Return < b.Return;
            return a.Target < b.Target;
        };
        for (const Pair& pair : mPairs) {
            if (pair.Count == 0U) continue;
            if (count < kTop) {
                best[count++] = pair;
                std::push_heap(best.begin(), best.begin() + count, before);
            } else if (before(pair, best.front())) {
                std::pop_heap(best.begin(), best.end(), before);
                best.back() = pair;
                std::push_heap(best.begin(), best.end(), before);
            }
        }
        std::sort_heap(best.begin(), best.begin() + count, before);
        out = best;
        return count;
    }

    // "indirect: calls=N pairs=K dropped=D top=ret>target:count,..." (calls
    // per window, guest addresses in hex), then the counts start again.
    std::string TakeLine() {
        std::array<Pair, kTop> top{};
        const size_t count = Top(top);
        std::string line;
        line.reserve(64U + count * 32U);
        char text[96];
        std::snprintf(text, sizeof(text), "indirect: calls=%llu pairs=%zu dropped=%llu top=",
                      static_cast<unsigned long long>(mCalls), mUsed,
                      static_cast<unsigned long long>(mDropped));
        line += text;
        for (size_t index = 0; index < count; ++index) {
            std::snprintf(text, sizeof(text), "%s%08X>%08X:%llu", index != 0U ? "," : "",
                          static_cast<unsigned>(top[index].Return),
                          static_cast<unsigned>(top[index].Target),
                          static_cast<unsigned long long>(top[index].Count));
            line += text;
        }
        if (count == 0U) line += '-';
        line += '\n';
        Reset();
        return line;
    }

    void Reset() noexcept {
        mPairs.fill({});
        mCalls = 0U;
        mUsed = 0U;
        mDropped = 0U;
    }

  private:
    static constexpr size_t kMaxProbes = 32U;
    std::array<Pair, kSlots> mPairs{};
    uint64_t mCalls = 0U;
    size_t mUsed = 0U;
    uint64_t mDropped = 0U;
};

// Set (by the window setup, config/indirect_profile) before guest code runs
// and read by the generated dispatcher on the same thread.
inline Oot3dIndirectProfile* gOot3dIndirectProfile = nullptr;

// Owns a profile and publishes it to the dispatcher for its lifetime.
class Oot3dIndirectProfileInstall final {
  public:
    Oot3dIndirectProfileInstall() : mProfile(std::make_unique<Oot3dIndirectProfile>()) {
        gOot3dIndirectProfile = mProfile.get();
    }
    ~Oot3dIndirectProfileInstall() {
        if (gOot3dIndirectProfile == mProfile.get()) gOot3dIndirectProfile = nullptr;
    }
    Oot3dIndirectProfileInstall(const Oot3dIndirectProfileInstall&) = delete;
    Oot3dIndirectProfileInstall& operator=(const Oot3dIndirectProfileInstall&) = delete;

    [[nodiscard]] Oot3dIndirectProfile& Profile() noexcept { return *mProfile; }

  private:
    std::unique_ptr<Oot3dIndirectProfile> mProfile;
};

} // namespace Oot3dNativeGame
