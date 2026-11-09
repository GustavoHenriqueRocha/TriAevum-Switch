#pragma once

namespace Oot3dNativeGame {

// At start-up, after the game data import and before the launch arguments
// look for config/atlas_overrides.o3tu: when the pack is missing, exists
// without config/topscreen_ui_atlas_hd.png (an install from before the HD UI
// atlas was built), or Roll dash is on (config/roll_dash) without
// config/zelda_link_ext.zar (an install from before 0.2.1), offers to
// download the official TopScreen mod archive ("OoT3D: Single Screen
// Experience" 3.2.2 by M-1, GameBanana) and build the pack, the HD font
// coverage, the HD UI atlas and Link's dash animations from game/romfs.bin
// and the archive (oot3d_topscreen_import.h). The player can accept (A),
// skip this time (B) or never be asked again (X, saved in
// config/topscreen_offer; L3+R3 > Updates turns the offer back on). An
// official archive already on the SD card
// (sdmc:/switch/oot3dre/singlescreen_322.zip, or a known older release such
// as singlescreen_321.zip, e.g. copied from a PC) is used without a download
// (with a pack already there, only a release that carries what is missing).
// Returns false only when the player leaves the app.
bool RunSwitchTopScreenImport();

} // namespace Oot3dNativeGame
