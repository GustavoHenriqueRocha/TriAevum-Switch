#include "oot3d_native_block_entry_hooks.h"

#include "oot3d_landing_roll.h"
#include "oot3d_roll_dash.h"
#include "oot3d_native_a32_actor_visibility.h"
#include "oot3d_native_a32_timing_probe.h"
#include "oot3d_native_a32_window_guest_pcs.h"
#include "oot3d_native_actor_profile.h"
#include "oot3d_native_frame_rate.h"
#include "oot3d_native_hd_font_runtime.h"
#include "oot3d_native_pica_composition.h"
#include "oot3d_native_player_temporal_bridge.h"
#include "oot3d_native_ui_lifecycle_bridge.h"
#include "oot3d_top_screen_input_cadence.h"
#include "oot3d_top_screen_item_dispatch.h"
#include "oot3d_top_screen_mod_profile.h"
#include "oot3d_top_screen_pause_transaction.h"
#include "oot3d_top_screen_wide_items_runtime.h"
#include "oot3d_typed_gameplay_bridge.h"

#include <algorithm>
#include <array>

namespace Oot3dNativeGame {

std::vector<Oot3dBlockEntryHookSource> Oot3dNativeBlockEntryHookSources(
    const Oot3dBlockEntryHookOptions &options) {
  using namespace Oot3dA32Window;
  std::vector<Oot3dBlockEntryHookSource> hooks;
  const auto add = [&hooks](std::string_view source, auto &&pcs) {
    for (const uint32_t pc : pcs) hooks.push_back({pc, source});
  };
  add("base", std::array{
                  kMtx4x4BuildFrustum,
                  kMtx4x4BuildOrthographicProjectionRotated,
                  kGameStateUpdate,
                  kOot3dPlayerUpdateEntry,
                  kOot3dFramePacingBeginEntry,
                  kOot3dFramePacingDecisionEntry,
                  kOot3dFramePacingCommitEntry,
                  kOot3dMeshCommandPacketSubmitEntry,
                  // Landing roll (N64): the policy diverts to the game's roll setup.
                  kLandingRollHookPc,
              });
  // Roll dash (config/roll_dash, oot3d_roll_dash.h): always hooked, so the
  // menu turns it on at once; inert while it is off.
  add("roll_dash", kRollDashHookPcs);
  add("actor_visibility", NativeActorVisibilityHookPcs());
  add("pica_composition", Oot3dNativePicaCompositionHookPcs());
  if (options.PlayerTemporal)
    add("player_temporal", NativeA32PlayerTemporalHookPcs());
  if (options.TypedGameplayObservableExits)
    add("typed_gameplay_exits", Oot3dTypedGameplayObservableExitPoints());
  add("ui_lifecycle", Oot3dNativeUiLifecycleBridge::GuestEntryPoints());
  if (options.TopScreen) {
    // Texture payloads are patched when the native pause owner has loaded
    // them, before its update can submit HUD or pause draws.
    add("top_screen", std::array{
                          kPauseUiUpdate,
                          kTopScreenQuestMaterializedHook,
                          kTopScreenQuestSubmitModelsHook,
                          kTopScreenQuestDrawHook,
                          0x00300588U,
                          0x002FEABCU,
                          0x00311364U,
                          kPauseProjectionPrepare,
                          kPauseUiDraw,
                          kPauseUiDrawReturn,
                          kPauseRendererVisibilityNativeDraw,
                          kPauseIconBuild,
                      });
    add("top_screen_source_port", std::array{
                                      kPauseTouchButtonsDrawCall,
                                      kPauseTouchButtonsTouchBranch,
                                      kPauseLowerScreenBlock,
                                      kPauseUiNativeUpdateCall,
                                      kPauseProjectionPrepareCall,
                                      kPauseAlternateViewportDrawCall,
                                      kPauseConditionalDrawCall,
                                      kPauseRoutedCommandCallA,
                                      kPauseRoutedCommandCallB,
                                      kPauseOverlayViewportDrawCall,
                                      kPauseAlternateRendererUpdateCall,
                                      kPauseSceneSevenDrawCall,
                                      kPauseSceneViewportDrawCall,
                                      kPauseSceneFiveDrawCall,
                                      kTopScreenSystemPageBoundary,
                                      kPauseTouchButtonsHeldDownQuery,
                                      kPauseTouchCoordinateUpdateCall,
                                      kPauseRendererVisibilityCall,
                                      kPauseControllerDrawCall,
                                      kTopScreenCameraNormal1Scalar,
                                      kTopScreenCameraFovScalar,
                                      kTopScreenCameraUpdateEntry,
                                      kTopScreenCameraUpdatePatchSite,
                                      kTopScreenGameplayCompositionPrefix,
                                      kTopScreenPageProjectionUploadCall,
                                      kTopScreenItemsPageDrawCall,
                                      kTopScreenItemsCursorDrawCallA,
                                      kTopScreenItemsCursorDrawCallB,
                                  });
    for (const auto &contract : TopScreenVerifiedControlFlowContracts())
      hooks.push_back({contract.Entry, "top_screen_control_flow_contracts"});
    for (const auto &contract : TopScreenVerifiedFloatLoadContracts())
      hooks.push_back({contract.Entry, "top_screen_float_load_contracts"});
    add("top_screen_map_icons", TopScreenDungeonMapIconCoordinateSites());
    add("top_screen_item_dispatch", TopScreenItemDispatchEntries());
    hooks.push_back({kTopScreenInputUpdateBoundary, "top_screen_input_boundary"});
  }
  if (options.ExtendedDiagnostics) {
    add("extended_diagnostics", std::array{
                                    kEnMagInit,
                                    kEnMagUpdate,
                                    kEnMagDraw,
                                    kPauseUiUpdate,
                                    kPauseUiDraw,
                                    kFileSelectUpdate,
                                    kFileSelectInit,
                                    kFileChooseInit,
                                    kPlayTransitionUpdate,
                                    kFileSelectActivate,
                                    kBlockingQueuePop,
                                    kBlockingQueuePush,
                                    kRendererCommandQueueProcessPending,
                                    kBlockingQueueTryPush,
                                    kTaskQueueTakeFirstFromPriority,
                                    kTaskQueueEnqueuePriorityNode,
                                    kTaskQueueWorkerDrainPriorityNodes,
                                    kNngxP3dCallback,
                                    kNngxCommonInterruptHandler,
                                    kNngxCmdlistCompletionDispatch,
                                    kRendererGlobalCallbackAfterLookup,
                                });
  }
  // Actor cost profile (config/actor_profile; off by default): its hooks
  // exist only while it is on.
  if (options.ActorProfile) add("actor_profile", NativeActorProfile::HookPcs());
  if (options.HdFonts) hooks.push_back({kNativeQbfA4Blit, "hd_fonts"});
  return hooks;
}

std::vector<uint32_t> Oot3dNativeBlockEntryHookPcs(
    const Oot3dBlockEntryHookOptions &options) {
  std::vector<uint32_t> pcs;
  for (const auto &hook : Oot3dNativeBlockEntryHookSources(options))
    pcs.push_back(hook.Pc);
  std::sort(pcs.begin(), pcs.end());
  pcs.erase(std::unique(pcs.begin(), pcs.end()), pcs.end());
  return pcs;
}

Oot3dBlockEntryHookOptions Oot3dAllBlockEntryHookOptions() noexcept {
  return {true, true, true, true, true, true};
}

std::vector<uint32_t> Oot3dNativeAllBlockEntryHookPcs() {
  return Oot3dNativeBlockEntryHookPcs(Oot3dAllBlockEntryHookOptions());
}

} // namespace Oot3dNativeGame
