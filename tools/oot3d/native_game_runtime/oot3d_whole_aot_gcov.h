#pragma once

// GCC profile-generate support for the whole-AOT shards
// (OOT3D_WHOLE_AOT_PROFILE_MODE=generate with GCC, see native_runtime.cmake).
//
// libgcov writes the .gcda files at normal process exit. A Switch homebrew
// closed from the HOME menu may never reach exit, so the frame loop also
// flushes periodically: __gcov_dump() merges the counters into the files
// (adding to what is there) and __gcov_reset() starts the next interval, so
// repeated flushes and the final exit-time write never double count.
//
// Without OOT3D_WHOLE_AOT_GCOV_GENERATE every function is an empty inline.

#if defined(OOT3D_WHOLE_AOT_GCOV_GENERATE)
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>

extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
#endif

#include <cstdint>

namespace Oot3dNativeGame {

#if defined(OOT3D_WHOLE_AOT_GCOV_GENERATE)

#ifndef OOT3D_WHOLE_AOT_GCOV_DIR
#define OOT3D_WHOLE_AOT_GCOV_DIR "sdmc:/switch/oot3dre/pgo"
#endif

// Host frames between flushes (3 to 6 minutes of play); each flush stalls
// the game while a few hundred files are rewritten on the SD card.
inline constexpr uint64_t kOot3dWholeAotGcovFlushFrames = 60U * 60U * 3U;

// Creates every directory of OOT3D_WHOLE_AOT_GCOV_DIR so libgcov can open
// its files directly.
inline void Oot3dWholeAotGcovInitialize() {
    const std::string directory = OOT3D_WHOLE_AOT_GCOV_DIR;
    for (size_t slash = directory.find('/', 1); ; slash = directory.find('/', slash + 1)) {
        const std::string prefix = directory.substr(0, slash);
        if (!prefix.empty() && prefix.back() != ':' &&  // "sdmc:" is the device
            mkdir(prefix.c_str(), 0777) != 0 && errno != EEXIST) {
            std::fprintf(stderr, "gcov: cannot create %s\n", prefix.c_str());
        }
        if (slash == std::string::npos) {
            break;
        }
    }
}

inline void Oot3dWholeAotGcovFlush() {
    __gcov_dump();
    __gcov_reset();
}

inline void Oot3dWholeAotGcovFrame(uint64_t hostFrame) {
    if (hostFrame != 0U && hostFrame % kOot3dWholeAotGcovFlushFrames == 0U) {
        Oot3dWholeAotGcovFlush();
    }
}

#else

inline void Oot3dWholeAotGcovInitialize() {
}

inline void Oot3dWholeAotGcovFlush() {
}

inline void Oot3dWholeAotGcovFrame(uint64_t) {
}

#endif

} // namespace Oot3dNativeGame
