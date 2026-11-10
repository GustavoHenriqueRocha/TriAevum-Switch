#pragma once

// How GfxRenderingAPIVulkan::EndNativePicaRenderPass closes the native PICA
// pass. Pure; host tests in tests/native_pica_pass_end_tests.cpp.

namespace Fast {

enum class NativePicaPassEnd {
    None,                 // no pass open
    EndDynamicRendering,  // PicaDynamicRenderingScope::End
    EndRenderPass,        // vkCmdEndRenderPass
    Abandon,              // forget it; record nothing
};

[[nodiscard]] inline NativePicaPassEnd ResolveNativePicaPassEnd(
    bool passActive, bool dynamicRenderingActive, bool shuttingDown) noexcept {
    if (!passActive) return NativePicaPassEnd::None;
    // The command buffer is never submitted again, and the scope may already
    // have been reset (it then no longer tells which End the pass needs).
    if (shuttingDown) return NativePicaPassEnd::Abandon;
    return dynamicRenderingActive ? NativePicaPassEnd::EndDynamicRendering
                                  : NativePicaPassEnd::EndRenderPass;
}

} // namespace Fast
