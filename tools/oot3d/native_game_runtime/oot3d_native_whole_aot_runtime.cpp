#include "oot3d_native_whole_aot_runtime.h"

#include <cstdio>

namespace Oot3dNativeGame {

std::string Oot3dWholeAotHookGatesError(
    const Oot3dWholeAotHookGateSet& gates, std::span<const uint32_t> sortedHooks) {
    uint32_t missing = 0U;
    if (Oot3dWholeAotHookGatesCover(gates, sortedHooks, &missing)) return {};
    if (sortedHooks.empty()) {
        return "whole-AOT: a block-entry callback on every block (guest profile, "
               "--trace-a32-blocks, --profile-a32-blocks) needs code generated "
               "without --sparse-block-entries; this build's code calls it only at "
               "the PCs of whole_aot_hook_gates.json";
    }
    char pc[16];
    std::snprintf(pc, sizeof(pc), "0x%08X", missing);
    return std::string("whole-AOT: block-entry hook ") + pc +
           " has no gate in this build's code (--sparse-block-entries); regenerate "
           "tools/oot3d/native_a32_runtime/whole_aot_hook_gates.json with "
           "oot3d_block_entry_hook_census, then regenerate and rebuild the "
           "whole-AOT code with it";
}

} // namespace Oot3dNativeGame
