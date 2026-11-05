#pragma once
#include <cstdint>
namespace Oot3dNativeGame {
// This build's release identity, from the CMake cache variables
// OOT3D_RELEASE_VERSION and OOT3D_RELEASE_BUILD (CMake/oot3d/game.cmake):
// the semver string the NACP shows and the monotonic build number the
// updater compares. Development builds are "0.1.0-dev", build 0.
const char *BuildVersion() noexcept;
std::uint64_t BuildNumber() noexcept;
// "1.0.0-test.1 (build 5)".
const char *BuildVersionLabel() noexcept;
} // namespace Oot3dNativeGame
