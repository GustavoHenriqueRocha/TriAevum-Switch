#pragma once

// Hand-written native replacements for hot guest functions
// ("partial decompilation").
//
// Contract: each GuestOverrides::Override_XXXXXXXX has the same ABI as the
// translated function Execute_<name>_XXXXXXXX (frame, context, state) and is
// only called by the prologue that apply_guest_overrides.py inserts into the
// generated function, when the entry is the function's own entry. It returns:
//   - false: declined BEFORE any side effect (insufficient block budget,
//     block hook inside the function, FPSCR outside the default mode, an
//     access that would not take the fast memory path...). The translated
//     function then runs in full, unchanged;
//   - true: executed and left registers, flags (with the same liveness masks
//     as the generated code), VFP, FPSCR, memory, write generations,
//     BlocksRemaining/BlocksConsumed and the Oot3dWholeAotFlow exactly as the
//     translated version would.
//
// Block overrides (Override_<BLOCK> with resumePc): called by the prologue
// inserted right after the block_<BLOCK> label of the translated function
// containing the block, before the block is entered (budget, hook). They
// return false (decline with no effect: the translated block runs) or true
// with resumePc = the PC of a block in the same function where translation
// continues (goto), leaving the state exactly as the translation would on
// entering that block; with resumePc = 0 the translated function returns
// `flow` (limit exit etc.).
// Equivalence is verified by run_guest_override_equivalence.py against the
// generated C++; an override only enters guest_overrides.json after passing
// with zero differences.

#include "oot3d_native_whole_aot_runtime.h"
#include "oot3d_aot_architectural_state.h"

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

// Runtime on/off switch (default: on). When off, the generated prologue
// falls straight through to the translation, allowing A/B on console without
// recompiling.
extern bool gOot3dGuestOverridesEnabled;

inline bool Oot3dGuestOverridesEnabled() noexcept {
    return gOot3dGuestOverridesEnabled;
}

void SetOot3dGuestOverridesEnabled(bool enabled) noexcept;

// Guest profile (oot3d_native_guest_profile.cpp). In measured frames the
// block-entry callback is the profiler's, called on every block, but it only
// forwards to the original callback the PCs of the original filter. Without
// this hook the overrides saw "hook on every block" and declined on every
// measured frame (and loop-head block overrides declined on every iteration),
// so the profile measured the translation, never the native version. With it,
// an override only declines if the profiler would forward some PC in its
// range; otherwise it runs and marks its own entry in the profile (native
// time lands on the entry block). Filled by the profiler; empty, nothing
// changes.
struct Oot3dGuestOverrideProfileHook {
    // Callback the profiler installs (GuestBlockProfiler::OnBlockEntry).
    oot3d::recomp::a32::BlockEntryCallback Callback = nullptr;
    // Would the profiler forward some PC in [first, last] to the original callback?
    bool (*Forwards)(const void* user, uint32_t first, uint32_t last) noexcept = nullptr;
    // Counts one entry at `pc` and starts attributing time to it.
    void (*Mark)(void* user, uint32_t pc) noexcept = nullptr;
};

extern Oot3dGuestOverrideProfileHook gOot3dGuestOverrideProfileHook;

struct Oot3dGuestOverrideInfo {
    uint32_t Entry = 0;
    const char* Name = "";
    uint64_t Calls = 0;     // times the native version executed
    uint64_t Declined = 0;  // times it declined and the translation executed
};

// Static table of the compiled overrides, with counters.
size_t Oot3dGuestOverrideCount() noexcept;
const Oot3dGuestOverrideInfo* Oot3dGuestOverrideTable() noexcept;

namespace GuestOverrides {

#define OOT3D_GUEST_OVERRIDE_DECLARE(entry)                                   \
    bool Override_##entry(Oot3dWholeAotFrame& frame,                          \
                          Oot3dWholeAotContext& context,                      \
                          Oot3dAotArchitecturalState& state,                  \
                          Oot3dWholeAotFlow& flow) noexcept;

OOT3D_GUEST_OVERRIDE_DECLARE(00372070) // oot3d_translate_mtx3x4_local
OOT3D_GUEST_OVERRIDE_DECLARE(002CDE90)
OOT3D_GUEST_OVERRIDE_DECLARE(003094CC)
OOT3D_GUEST_OVERRIDE_DECLARE(002F9430) // PauseRenderBuffer_SetVec2Range
OOT3D_GUEST_OVERRIDE_DECLARE(0036E168) // Math_SmoothStepToF (inventory name)
OOT3D_GUEST_OVERRIDE_DECLARE(003325F0) // Math3D_CylOutsideCylDist
OOT3D_GUEST_OVERRIDE_DECLARE(00307C94) // PicaCommandWriter_UploadVertexFloatUniforms
OOT3D_GUEST_OVERRIDE_DECLARE(003727F0) // Math_SinF
OOT3D_GUEST_OVERRIDE_DECLARE(004A022C) // audio effect: 4-channel delay
OOT3D_GUEST_OVERRIDE_DECLARE(004A0338) // audio effect: stereo reverb
OOT3D_GUEST_OVERRIDE_DECLARE(00371738) // Runtime_Memcpy
OOT3D_GUEST_OVERRIDE_DECLARE(0047AF24) // search over 50 entries
OOT3D_GUEST_OVERRIDE_DECLARE(00307B50) // PicaTev_ConvertSource
OOT3D_GUEST_OVERRIDE_DECLARE(00307DC8) // PicaCompare_ConvertFunction
OOT3D_GUEST_OVERRIDE_DECLARE(002BBB14) // Math3D_CirSquareVsTriSquare
OOT3D_GUEST_OVERRIDE_DECLARE(002DCEF4) // Mtx3x3_Multiply

#define OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(entry)                             \
    bool Override_##entry(Oot3dWholeAotFrame& frame,                          \
                          Oot3dWholeAotContext& context,                      \
                          Oot3dAotArchitecturalState& state,                  \
                          Oot3dWholeAotFlow& flow,                            \
                          uint32_t& resumePc) noexcept;

OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00466E68) // MeshCommandPacket_Submit: copy
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(003247B0) // BgCheck_LineTestImpl: clears marks
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(002FA674) // CalculateGyroscopeAxisStatus: average
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00308920) // NativeCurve_SampleFloat: search
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00308604) // CsabCurve_SampleRotation: search
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00303C78) // FUN_00303b24: 64-byte copy
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(0046700C) // MeshCommandPacket_PatchBoneUniforms: copy
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(003563E0) // FUN_003562c0: list traversal

#undef OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE
#undef OOT3D_GUEST_OVERRIDE_DECLARE

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
