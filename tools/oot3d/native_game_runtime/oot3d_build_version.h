#pragma once
#include <cstdint>
namespace Oot3dNativeGame {
// This build's release identity, from the CMake cache variables
// OOT3D_RELEASE_VERSION and OOT3D_RELEASE_BUILD (CMake/oot3d/game.cmake):
// the semver string the NACP and the L3+R3 menu show, and the monotonic build
// number the updater compares (never shown to players). Development builds
// are "0.1.0-dev", build 0.
const char *BuildVersion() noexcept;
std::uint64_t BuildNumber() noexcept;
} // namespace Oot3dNativeGame
