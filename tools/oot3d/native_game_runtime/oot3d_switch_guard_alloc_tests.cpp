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
                config.QuarantineBlocks == 131072U && config.MaxLive == 32768U &&
                config.GuardMode == Mode::Canary,
            "empty file must be the default 57..72 / 131072 / 32768 canario");
    Require(ParseConfig("57 72 100 2000", config) && config.QuarantineBlocks == 100U &&
                config.MaxLive == 2000U,
            "quarantine and live cap");
    Require(ParseConfig("40 96\r\n", config) && config.MinSize == 40U && config.MaxSize == 96U &&
                config.QuarantineBlocks == 131072U,
            "range only keeps the default quarantine");
    Require(ParseConfig("57 72 32768 32768 pagina", config) && config.GuardMode == Mode::Page &&
                config.QuarantineBlocks == kMaxPageQuarantine,
            "page mode must clamp the quarantine (32768 closed pages broke audio and FS)");
    Require(ParseConfig("57 72 2000 100 pagina", config) && config.QuarantineBlocks == 2000U,
            "page mode below the clamp is kept");
    Require(ParseConfig("57 72 canario", config) && config.GuardMode == Mode::Canary &&
                config.QuarantineBlocks == 131072U,
            "mode right after the range");
    Require(ParseConfig("57 72 1 1 pagin", config) == false, "unknown mode");
    Require(ParseConfig("57 72 1 1 canario 5", config) == false, "number after the mode");
    Require(ParseConfig("1 3969 0 1 pagina", config) == false, "max past the header must be rejected");
    Require(ParseConfig("1 3968 0 1 pagina", config), "max right after the header is fine");
    Require(ParseConfig("72 57", config) == false, "min > max must be rejected");
    Require(ParseConfig("0 72", config) == false, "min 0 must be rejected");
    Require(ParseConfig("57 72 x", config) == false, "garbage must be rejected");
    Require(ParseConfig("57 72 16 9 1", config) == false, "a fifth field must be rejected");
    Require(ParseConfig("99999999999999999999999 72", config) == false, "overflow");
    Require(ParseConfig("8 16 0", config) && config.QuarantineBlocks == 0U, "no quarantine");
}

void TestLayout() {
    const Config config;
    // O chunk corrompido dos tours 16-20: 0x50 bytes = pedidos de 57 a 72.
    Require(!InRange(config, 56U) && InRange(config, 57U) && InRange(config, 72U) &&
                !InRange(config, 73U),
            "default range must be exactly the 0x50 chunk class");
    for (size_t size = 1; size <= kPageSize - sizeof(BlockHeader); ++size) {
        const size_t offset = ObjectOffset(size);
        // devkitA64: malloc(n >= 32) = _memalign_r(32, n).
        Require(offset % kAlignment == 0U, "object must stay 32-byte aligned");
        Require(offset + size <= kPageSize && kPageSize - (offset + size) < kAlignment,
                "object must end less than 32 bytes before the guard page");
        Require(offset >= sizeof(BlockHeader), "object must not overlap the header");
        Require(UsableSize(size) >= size && offset + UsableSize(size) == kPageSize,
                "usable size must reach the guard page");
    }
    Require(!InRange(config, 0U), "size 0 is never guarded");
}

void TestCanary() {
    uint8_t object[72];
    uint8_t snapshot[72];
    for (size_t index = 0; index < sizeof(object); ++index) {
        object[index] = snapshot[index] = static_cast<uint8_t>(index * 7U);
    }
    ChangedRange ranges[2];
    size_t changed = 99;
    Require(FindCanaryChanges(object, snapshot, sizeof(object), ranges, 2U, changed) == 0U &&
                changed == 0U,
            "untouched object");
    // A escrita dos tours 16-20: 0x00ed5391'0000001a no byte 8.
    const uint8_t write[8] = {0x1a, 0, 0, 0, 0x91, 0x53, 0xed, 0};
    for (size_t index = 0; index < 8U; ++index) object[8U + index] = write[index];
    object[70] = static_cast<uint8_t>(snapshot[70] + 1U);
    object[0] = static_cast<uint8_t>(snapshot[0] + 1U);
    const size_t count = FindCanaryChanges(object, snapshot, sizeof(object), ranges, 2U, changed);
    Require(count == 2U && ranges[0].Begin == 0U && ranges[0].End == 1U && ranges[1].Begin == 8U &&
                ranges[1].End == 16U && changed == 10U,
            "ranges are merged and capped; every changed byte is counted");
    Require(sizeof(CanaryHeader) == 384U, "canary header is 384 bytes");
    Config config;
    Require(ParseConfig("57 129 1 1 canario", config) == false,
            "canary range must fit the snapshot");
    Require(ParseConfig("57 129 1 1 pagina", config), "page mode has no snapshot limit");

    uintptr_t storage[2];
    PointerRing ring(storage, 2U);
    uintptr_t evicted = 0;
    Require(!ring.Push(10U, evicted) && !ring.Push(20U, evicted), "fill");
    Require(ring.Push(30U, evicted) && evicted == 10U && ring.Count() == 2U, "fifo");
    Require(ring.Push(40U, evicted) && evicted == 20U, "fifo again");
    PointerRing none(nullptr, 0U);
    Require(none.Push(5U, evicted) && evicted == 5U, "zero capacity");
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
    Require(hit != nullptr && hit->Serial == 2U, "address in the slack page finds its block");
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
    TestCanary();
    std::cout << "oot3d_switch_guard_alloc_tests: ok\n";
    return 0;
}
