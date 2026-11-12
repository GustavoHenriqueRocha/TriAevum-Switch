#pragma once

#include "oot3d_top_screen_mod_profile.h"

namespace Oot3dNativeGame {

struct TopScreenDpadPresentationState {
  bool Child = false;
  std::uint16_t OwnedEquipment = 0;
  std::uint16_t EquippedEquipment = 0;
  std::uint8_t ItemZr = 0xFF;
  std::uint8_t ItemZl = 0xFF;
  std::uint8_t Ocarina = 0xFF;
  bool Boomerang = false;
  bool Slingshot = false;
  // SaveContext items[0..23] and ammo[0..15], for the D-pad item buttons.
  std::array<std::uint8_t, 24> Inventory{};
  std::array<std::uint8_t, 16> Ammo{};
  // Per direction, from ReadTopScreenDpadItemUsability: false greys a D-pad
  // item button the rules refuse now.
  std::array<bool, 4> ItemUsable{true, true, true, true};
  TopScreenNativeItemOpacity ItemOpacity;
  // 3.0 0x005CCF6C, as for the cross: the icons without a lane alpha of
  // their own (not ZR, ZL or the Ocarina) take it (loop at 0x005DD78C).
  float GroupAlpha = 1.0F;
};

bool ReadTopScreenDpadPresentationState(NativeA32Memory &memory,
                                        TopScreenDpadPresentationState *state);

// Emits mapped action icons and their native cycle marks. The cross, face buttons and global
// HUD scale remain owned by the existing HUD compositor.
std::size_t AppendTopScreenDpadPresentation(
    const TopScreenUiConfig &config,
    const TopScreenDpadPresentationState &state,
    const TopScreenAuxiliaryTouchInputs &viewState, float alpha,
    const oot3d::ui::UiTextureIdentity &itemIcons,
    // custom_ui01 (kTopScreen30UiAtlasSemantic): the View eye, Navi and camera.
    const oot3d::ui::UiTextureIdentity &uiAtlas,
    std::vector<oot3d::ui::UiPrimitive> &output);

// The ammo of each D-pad item button holding sticks, nuts, bombs, the bow,
// the slingshot, bombchus or beans, at the bottom right of its icon.
std::size_t AppendTopScreenDpadItemAmmo(
    const TopScreenUiConfig &config,
    const TopScreenDpadPresentationState &state,
    const TopScreenAuxiliaryTouchInputs &viewState, float alpha,
    const oot3d::ui::UiTextureIdentity &numberGlyphs,
    std::vector<oot3d::ui::UiPrimitive> &output);

} // namespace Oot3dNativeGame
