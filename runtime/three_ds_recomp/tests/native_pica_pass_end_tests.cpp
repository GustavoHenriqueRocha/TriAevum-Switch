// How the native PICA pass is closed (ResolveNativePicaPassEnd,
// native_pica_pass_end.h).

#include "fast/backends/native_pica_pass_end.h"

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

using Fast::NativePicaPassEnd;
using Fast::ResolveNativePicaPassEnd;

// MK7: a fatal renderer error left the native pass open; Shutdown resets the
// dynamic rendering scope (it no longer reports the rendering active) and
// then destroys the PICA resources, which closed the pass with
// vkCmdEndRenderPass in the abandoned command buffer: SIGSEGV in
// libvulkan_intel (gdb: Shutdown -> DestroyNativePicaShaderResources ->
// EndNativePicaRenderPass). Shutting down records nothing.
void TestShutdownAbandonsAnOpenPass() {
    CHECK(ResolveNativePicaPassEnd(true, false, true) == NativePicaPassEnd::Abandon);
    CHECK(ResolveNativePicaPassEnd(true, true, true) == NativePicaPassEnd::Abandon);
    CHECK(ResolveNativePicaPassEnd(false, false, true) == NativePicaPassEnd::None);
}

void TestFramesCloseAsBefore() {
    CHECK(ResolveNativePicaPassEnd(false, false, false) == NativePicaPassEnd::None);
    CHECK(ResolveNativePicaPassEnd(true, true, false) ==
          NativePicaPassEnd::EndDynamicRendering);
    CHECK(ResolveNativePicaPassEnd(true, false, false) == NativePicaPassEnd::EndRenderPass);
}

} // namespace

int main() {
    TestShutdownAbandonsAnOpenPass();
    TestFramesCloseAsBefore();
    if (gFailures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", gFailures);
        return 1;
    }
    std::puts("native PICA pass end tests passed");
    return 0;
}
