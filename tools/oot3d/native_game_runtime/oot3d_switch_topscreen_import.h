#pragma once

namespace Oot3dNativeGame {

// At start-up, after the game data import and before the launch arguments
// look for config/atlas_overrides.o3tu: when the pack is missing, or exists
// without config/topscreen_ui_atlas_hd.png (an install from before the HD UI
// atlas was built), offers to download the official TopScreen mod archive
// ("OoT3D: Single Screen Experience" 3.2.1 by M-1, GameBanana) and build the
// pack, the HD font coverage and the HD UI atlas from game/romfs.bin
// (oot3d_topscreen_import.h). The player can accept (A), skip this time (B)
// or never be asked again (X, saved in config/topscreen_offer; L3+R3 >
// Updates turns the offer back on). An official archive already on the SD
// card (sdmc:/switch/oot3dre/singlescreen_321.zip, e.g. copied from a PC) is
// used without a download (with a pack already there, only a release that
// carries the HD atlas). Returns false only when the player leaves the app.
bool RunSwitchTopScreenImport();

} // namespace Oot3dNativeGame
