#include "oot3d_build_version.h"

// Only this file gets the definitions (game.cmake), so a new version
// recompiles one translation unit.
#if !defined(OOT3D_RELEASE_VERSION) || !defined(OOT3D_RELEASE_BUILD)
#error "OOT3D_RELEASE_VERSION and OOT3D_RELEASE_BUILD come from CMake/oot3d/game.cmake"
#endif

namespace Oot3dNativeGame {
const char *BuildVersion() noexcept { return OOT3D_RELEASE_VERSION; }

std::uint64_t BuildNumber() noexcept { return static_cast<std::uint64_t>(OOT3D_RELEASE_BUILD); }
} // namespace Oot3dNativeGame
