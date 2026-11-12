// The Controllers applet SDL would launch from the game loop: see
// oot3d_switch_controller_applet.h. Linked with
// -Wl,--wrap=hidLaShowControllerSupportForSystem (CMake/oot3d/game.cmake).
#include "oot3d_switch_controller_applet.h"

#include <switch.h>

#include <atomic>
#include <cstring>

namespace Oot3dNativeGame {
namespace {

std::atomic<std::uint32_t> gSkippedControllerApplets{0};

} // namespace

std::uint32_t TakeSkippedControllerApplets() noexcept {
  return gSkippedControllerApplets.exchange(0U, std::memory_order_relaxed);
}

} // namespace Oot3dNativeGame

// libnx applets/hid_la.h. Only SDL's SWITCH_UpdateControllerSupport calls it;
// SDL ignores the result and refreshes every pad after the call. No applet is
// launched: the result is what the applet would leave with nothing changed
// (the players connected now; player 1, else handheld, as the selection),
// read from HID shared memory without IPC.
extern "C" Result __wrap_hidLaShowControllerSupportForSystem(HidLaControllerSupportResultInfo *result_info,
                                                             const HidLaControllerSupportArg *arg, bool flag) {
  static_cast<void>(arg);
  static_cast<void>(flag);
  Oot3dNativeGame::gSkippedControllerApplets.fetch_add(1U, std::memory_order_relaxed);
  if (result_info != nullptr) {
    std::memset(result_info, 0, sizeof(*result_info));
    int players = 0;
    for (int id = HidNpadIdType_No1; id <= HidNpadIdType_No8; ++id)
      players += hidGetNpadStyleSet(static_cast<HidNpadIdType>(id)) != 0 ? 1 : 0;
    const bool handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0;
    if (players == 0 && handheld) players = 1;
    result_info->player_count = static_cast<s8>(players);
    result_info->selected_id = hidGetNpadStyleSet(HidNpadIdType_No1) != 0 || !handheld
                                   ? HidNpadIdType_No1
                                   : HidNpadIdType_Handheld;
  }
  return 0;
}
