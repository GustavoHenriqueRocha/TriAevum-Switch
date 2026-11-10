// Display transfer modes the GPU paths execute
// (PicaDisplayTransferModeSupported, pica_display_transfer.h).

#include "fast/oot3d/pica_display_transfer.h"

#include <cstdio>

namespace {

int gFailures = 0;

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__,    \
                         #condition);                                         \
            ++gFailures;                                                      \
        }                                                                     \
    } while (false)

using namespace Fast::Oot3d;

// MK7 copies its upper screen (256x416 RGB565) with block_32 (flag 16) on
// entering a race; the window died with "display transfer mode is
// unsupported". Azahar leaves block_32 unimplemented: its software blitter
// and its accelerated path both run the transfer as an ordinary one, and a
// GPU source is a host image with no guest tiling to reinterpret.
void TestBlock32RunsAsAnOrdinaryTransfer() {
    CHECK(PicaDisplayTransferModeSupported(0x12200U));
    const auto plan = BuildPicaDisplayTransferPlan(
        {256U, 416U, 256U, 416U, 256U, 416U, 0x12200U});
    CHECK(plan.has_value());
    if (plan.has_value()) {
        CHECK(plan->DestinationWidth == 256U && plan->DestinationHeight == 416U);
        CHECK(plan->HorizontalSamples == 1U && plan->VerticalSamples == 1U);
    }
    // block_32 does not change the scaling it comes with.
    const auto scaled = BuildPicaDisplayTransferPlan(
        {512U, 832U, 256U, 416U, 512U, 832U, 0x02012200U});
    CHECK(scaled.has_value() && scaled->HorizontalSamples == 2U &&
          scaled->VerticalSamples == 2U);
}

void TestStillUnsupported() {
    // The TextureCopy bit without a texture copy, and scaling mode 3
    // (Azahar: "Unimplemented display transfer scaling mode").
    CHECK(!PicaDisplayTransferModeSupported(0x2208U));
    CHECK(!PicaDisplayTransferModeSupported(0x03002200U));
    CHECK(!BuildPicaDisplayTransferPlan({256U, 416U, 256U, 416U, 256U, 416U, 0x2208U}).has_value());
    CHECK(!BuildPicaDisplayTransferPlan({256U, 416U, 256U, 416U, 256U, 416U, 0x03002200U}).has_value());
}

void TestOrdinaryModesUnchanged() {
    for (const uint32_t flags : {0x0U, 0x1004U, 0x2200U, 0x2204U, 0x01001000U, 0x02001000U})
        CHECK(PicaDisplayTransferModeSupported(flags));
}

} // namespace

int main() {
    TestBlock32RunsAsAnOrdinaryTransfer();
    TestStillUnsupported();
    TestOrdinaryModesUnchanged();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::puts("PICA display transfer mode tests passed");
    return 0;
}
