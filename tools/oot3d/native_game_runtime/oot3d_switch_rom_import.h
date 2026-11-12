#pragma once

// First-launch import of the player's decrypted dump into
// sdmc:/switch/oot3dre/game/ (oot3d_rom_import.h), with a libnx console
// progress screen. Called at the start of main(), before the launch
// arguments are built and anything reads game/.

namespace Oot3dNativeGame {

// True to continue booting; false when the app should exit (no game data,
// a failed import without previous data, or the player closed the app).
[[nodiscard]] bool RunSwitchRomImport();

} // namespace Oot3dNativeGame
