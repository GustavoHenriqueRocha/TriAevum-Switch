// Alocador com páginas de guarda (oot3d_switch_guard_alloc.h): leitura de
// config/guard_alloc, posição do objeto na página e a quarentena FIFO.
#include "oot3d_switch_guard_alloc.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using namespace Oot3dNativeGame::GuardAlloc;

void Require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "oot3d_switch_guard_alloc_tests: " << message << '\n';
        std::exit(1);
    }
}

void TestParse() {
    Config config;
    Require(ParseConfig("", config) && config.MinSize == 57U && config.MaxSize == 72U &&
                config.QuarantineBlocks == 4096U,
            "empty file must be the default 57..72 / 4096");
    Require(ParseConfig("40 96\r\n", config) && config.MinSize == 40U && config.MaxSize == 96U &&
                config.QuarantineBlocks == 4096U,
            "range only keeps the default quarantine");
    Require(ParseConfig("1 4000 0", config) == false, "max past the header must be rejected");
    Require(ParseConfig("72 57", config) == false, "min > max must be rejected");
    Require(ParseConfig("0 72", config) == false, "min 0 must be rejected");
    Require(ParseConfig("57 72 x", config) == false, "garbage must be rejected");
    Require(ParseConfig("57 72 16 9", config) == false, "a fourth field must be rejected");
    Require(ParseConfig("99999999999999999999999 72", config) == false, "overflow");
    Require(ParseConfig("8 16 0", config) && config.QuarantineBlocks == 0U, "no quarantine");
}

void TestLayout() {
    const Config config;
    // O chunk corrompido dos tours 16-20: 0x50 bytes = pedidos de 57 a 72.
    Require(!InRange(config, 56U) && InRange(config, 57U) && InRange(config, 72U) &&
                !InRange(config, 73U),
            "default range must be exactly the 0x50 chunk class");
    for (size_t size = 1; size <= 3968U; ++size) {
        const size_t offset = ObjectOffset(size);
        Require(offset % 16U == 0U, "object must stay 16-byte aligned");
        Require(offset + RoundedSize(size) == kPageSize, "object must end at the guard page");
        Require(offset >= sizeof(BlockHeader), "object must not overlap the header");
    }
    Require(sizeof(BlockHeader) % 16U == 0U, "header keeps the object alignment");
}

void TestQuarantine() {
    std::vector<QuarantineEntry> storage(3);
    QuarantineRing ring(storage.data(), storage.size());
    QuarantineEntry evicted;
    for (uint64_t serial = 1; serial <= 3; ++serial) {
        QuarantineEntry entry;
        entry.Page = 0x10000U * serial;
        entry.Serial = serial;
        Require(!ring.Push(entry, evicted), "no eviction before full");
    }
    Require(ring.Count() == 3U, "count after fill");
    QuarantineEntry fourth;
    fourth.Page = 0x40000U;
    fourth.Serial = 4;
    Require(ring.Push(fourth, evicted) && evicted.Serial == 1U, "oldest leaves first");
    Require(ring.Find(0x10000U) == nullptr, "evicted block is no longer found");
    const QuarantineEntry* hit = ring.Find(0x20000U + kPageSize + 8U);
    Require(hit != nullptr && hit->Serial == 2U, "address in the guard page finds its block");
    Require(ring.Find(0x40000U + kBlockBytes) == nullptr, "end is exclusive");
    Require(ring.Find(0x40000U + kBlockBytes - 1U) != nullptr, "last byte is inside");

    QuarantineRing none(nullptr, 0U);
    Require(none.Push(fourth, evicted) && evicted.Serial == 4U,
            "zero capacity releases the block at once");
}

} // namespace

int main() {
    TestParse();
    TestLayout();
    TestQuarantine();
    std::cout << "oot3d_switch_guard_alloc_tests: ok\n";
    return 0;
}
