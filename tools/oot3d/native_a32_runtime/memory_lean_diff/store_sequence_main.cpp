// Differential of inlined sequences: current memory vs base revision.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(_NEWLIB_VERSION)
// newlib hides the POSIX setenv in strict C++ modes (qemu target).
extern "C" int setenv(const char* name, const char* value, int overwrite);
#endif

namespace NewSide {
uint32_t SeqCount();
uint64_t Run(uint32_t index, uint64_t seed);
uint64_t RunRaw(uint32_t index, uint64_t seed);
uint64_t WindowRuns();
}
namespace OldSide {
uint32_t SeqCount();
uint64_t Run(uint32_t index, uint64_t seed);
uint64_t RunRaw(uint32_t index, uint64_t seed);
}

#if defined(SEQ_TRIALS)
// qemu target (qemu_aarch64_test_runtime.cpp calls main() without arguments and
// does not set up TLS: tpidr_el0 points to a static block).
alignas(64) static unsigned char gTls[1 << 16];
int main() {
    asm volatile("msr tpidr_el0, %0" ::"r"(gTls));
    const uint32_t trials = SEQ_TRIALS;
    const uint64_t seed = SEQ_SEED;
#else
int main(int argc, char** argv) {
    const uint32_t trials = argc > 1 ? std::strtoul(argv[1], nullptr, 0) : 200U;
    const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1U;
#endif
#if defined(SEQ_WINDOW)
    // The current side's images claim the fastmem window ("1") or stay on
    // the slow path ("0"); read at the first region.
    setenv("TRIAEVUM_FASTMEM_WINDOW", SEQ_WINDOW, 1);
#endif
    const uint32_t count = NewSide::SeqCount();
    if (count != OldSide::SeqCount()) return 2;
    uint64_t total = 14695981039346656037ULL;
    uint64_t divergences = 0;
    for (uint32_t trial = 0; trial < trials; ++trial) {
        for (uint32_t index = 0; index < count; ++index) {
            const uint64_t runSeed = seed * 0x9E3779B97F4A7C15ULL + trial * 7919ULL + index;
            const uint64_t a = NewSide::Run(index, runSeed) ^
                               (NewSide::RunRaw(index, runSeed) * 3U);
            const uint64_t b = OldSide::Run(index, runSeed) ^
                               (OldSide::RunRaw(index, runSeed) * 3U);
            if (a != b) {
                if (divergences < 10) {
                    std::printf("DIVERGENCE seq %u trial %u: new=%016" PRIx64
                                " old=%016" PRIx64 "\n", index, trial, a, b);
                }
                ++divergences;
            }
            total ^= a;
            total *= 1099511628211ULL;
        }
    }
    std::printf("runs=%" PRIu64 " divergences=%" PRIu64 " digest=%016" PRIx64
                " window_runs=%" PRIu64 "\n",
                uint64_t{trials} * count, divergences, total, NewSide::WindowRuns());
#if defined(SEQ_WINDOW)
    // WINDOW=1 must have exercised the window's inline paths.
    if (SEQ_WINDOW[0] == '1' && NewSide::WindowRuns() == 0U) return 3;
#endif
    return divergences == 0 ? 0 : 1;
}
