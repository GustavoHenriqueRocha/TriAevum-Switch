#pragma once

// The guest PCs at which the A32 window's block-entry callback
// (ApplyNativeWidescreenProjectionPolicy) must run, for one launch
// configuration. The window takes its list from here, and the census tool
// (oot3d_block_entry_hook_census) writes the union over every configuration
// to tools/oot3d/native_a32_runtime/whole_aot_hook_gates.json, the gate list
// of the generator's --sparse-block-entries (N4): sparse generated code calls
// the callback only at those PCs, so a hook added here must also be added to
// that file (the census --check, run by tests/switch/run.sh, fails until it
// is regenerated, and the runtime refuses a hook without a gate,
// Oot3dWholeAotHookGatesError).

#include <cstdint>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

// One flag per condition under which the window adds hooks. Every condition
// only adds PCs.
struct Oot3dBlockEntryHookOptions final {
  // Enhanced60 gameplay timing (the player temporal bridge is installed).
  bool PlayerTemporal = false;
  // Developer builds (not OOT3D_WHOLE_AOT_PRODUCT_MODE): the typed-gameplay
  // observable exits.
  bool TypedGameplayObservableExits = false;
  // UiProfile TopScreen.
  bool TopScreen = false;
  // --extended diagnostics.
  bool ExtendedDiagnostics = false;
  // config/actor_profile on the console (oot3d_native_actor_profile.h).
  bool ActorProfile = false;
  // HD fonts loaded (the native QBF A4 blit).
  bool HdFonts = false;
};

struct Oot3dBlockEntryHookSource final {
  uint32_t Pc = 0U;
  // Stable name of the group that adds the PC (census "sources").
  std::string_view Source;
};

// Every hook of the configuration with the group that adds it, in insertion
// order (a PC may appear under several groups).
std::vector<Oot3dBlockEntryHookSource> Oot3dNativeBlockEntryHookSources(
    const Oot3dBlockEntryHookOptions &options);

// The configuration's hook list, sorted and unique.
std::vector<uint32_t> Oot3dNativeBlockEntryHookPcs(
    const Oot3dBlockEntryHookOptions &options);

// The union over every configuration (product and developer builds): the
// gate list of sparse generated code.
std::vector<uint32_t> Oot3dNativeAllBlockEntryHookPcs();

// Every condition on (the union).
Oot3dBlockEntryHookOptions Oot3dAllBlockEntryHookOptions() noexcept;

// The gameplay options' hooks and the quick put-away's resume pc, sorted: a
// gate runs only where the whole-AOT code starts a block, which the Master
// Quest mirror reads get from whole_aot_observed_sites.json.
std::vector<uint32_t> Oot3dGameplayOptionBlockPcs();

} // namespace Oot3dNativeGame
