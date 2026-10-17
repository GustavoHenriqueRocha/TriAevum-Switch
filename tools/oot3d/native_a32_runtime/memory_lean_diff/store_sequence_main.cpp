// Diferencial de sequências inlinadas: memória atual x revisão base.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace NewSide {
uint32_t SeqCount();
uint64_t Run(uint32_t index, uint64_t seed);
uint64_t RunRaw(uint32_t index, uint64_t seed);
}
namespace OldSide {
uint32_t SeqCount();
uint64_t Run(uint32_t index, uint64_t seed);
uint64_t RunRaw(uint32_t index, uint64_t seed);
}

#if defined(SEQ_TRIALS)
// Alvo qemu (qemu_aarch64_test_runtime.cpp chama main() sem argumentos e
// não prepara TLS: tpidr_el0 aponta para um bloco estático).
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
                    std::printf("DIVERGENCIA seq %u tentativa %u: nova=%016" PRIx64
                                " antiga=%016" PRIx64 "\n", index, trial, a, b);
                }
                ++divergences;
            }
            total ^= a;
            total *= 1099511628211ULL;
        }
    }
    std::printf("execucoes=%" PRIu64 " divergencias=%" PRIu64 " resumo=%016" PRIx64 "\n",
                uint64_t{trials} * count, divergences, total);
    return divergences == 0 ? 0 : 1;
}
