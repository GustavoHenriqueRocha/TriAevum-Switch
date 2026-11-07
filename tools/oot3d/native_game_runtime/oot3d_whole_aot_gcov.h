#pragma once

// GCC profile collection for the whole-AOT shards
// (OOT3D_WHOLE_AOT_PROFILE_MODE=generate with GCC, see native_runtime.cmake;
// implementation in oot3d_whole_aot_pgo.cpp, adapted from sa2-nx's
// app/src/sa2_pgo.cpp at commit adf3a4ee (ChanseyIsTheBest, GPL-3.0,
// https://github.com/ChanseyIsTheBest/sa2-nx), whose method comes from
// nfsmw-nx's app/src/nfsmw_pgo.cpp (StevensND, GPL-3.0)).
//
// libgcov's own exit-time .gcda writes do not fit the Switch: a game closed
// from the HOME menu never exits, and a dump cut short by that leaves a
// half-written .gcda. So the shards register their counters in a section
// (-fprofile-info-section=gcov_info) and a service thread streams the whole
// session (the counts only grow, nothing is reset) with __gcov_info_to_gcda
// into sdmc:/switch/oot3dre/pgo/oot3d_pgo_<N>.stream, one file per session:
//   - every kOot3dWholeAotPgoPeriodSeconds, when the game loses focus (HOME,
//     sleep, an applet) and at exit;
//   - each dump goes to a .tmp file that replaces the previous one only when
//     complete, so a dump interrupted by closing the game keeps the last one.
// On the PC, tools/oot3d/native_a32_runtime/whole_aot_pgo_merge.py turns the
// sessions into .gcda files (gcov-tool merge-stream adds them) for the
// OOT3D_WHOLE_AOT_PROFILE_MODE=use build. Steps in SWITCH_PORT.md.
//
// Without OOT3D_WHOLE_AOT_GCOV_GENERATE every function does nothing.

#include <cstdint>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

#ifndef OOT3D_WHOLE_AOT_GCOV_DIR
#define OOT3D_WHOLE_AOT_GCOV_DIR "sdmc:/switch/oot3dre/pgo"
#endif

inline constexpr uint32_t kOot3dWholeAotPgoPeriodSeconds = 180U;

// "<dir>/oot3d_pgo_<n>.stream".
inline std::string Oot3dWholeAotPgoStreamPath(std::string_view directory, uint32_t session) {
    std::string path(directory);
    if (!path.empty() && path.back() != '/') path += '/';
    return path + "oot3d_pgo_" + std::to_string(session) + ".stream";
}

// The first session number >= 1 whose stream and .tmp are both absent
// (`exists(path)`), so earlier sessions are never overwritten; 0 when every
// number up to `limit` is taken.
template <typename Exists>
uint32_t Oot3dWholeAotPgoNextSession(std::string_view directory, Exists&& exists,
                                     uint32_t limit = 100000U) {
    for (uint32_t session = 1U; session <= limit; ++session) {
        const std::string path = Oot3dWholeAotPgoStreamPath(directory, session);
        if (!exists(path) && !exists(path + ".tmp")) return session;
    }
    return 0U;
}

// Creates the directory, picks this session's stream, starts the dump thread,
// hooks focus changes and registers the exit dump. Call once, early in main.
void Oot3dWholeAotPgoStart();

// One line for the game log: where the counts go, or why they do not.
// Empty in a build that is not instrumented.
std::string Oot3dWholeAotPgoDescribe();

} // namespace Oot3dNativeGame
