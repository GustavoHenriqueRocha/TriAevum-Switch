// Tests of the block-entry hook list (oot3d_native_block_entry_hooks.h) and
// of the hook-gate check of sparse whole-AOT code (Oot3dWholeAotHookGatesError).

#include "oot3d_native_block_entry_hooks.h"

#include "oot3d_landing_roll.h"
#include "oot3d_roll_dash.h"
#include "oot3d_native_a32_actor_visibility.h"
#include "oot3d_native_a32_timing_probe.h"
#include "oot3d_native_a32_window_guest_pcs.h"
#include "oot3d_native_actor_profile.h"
#include "oot3d_native_compiled_functions.h"
#include "oot3d_native_frame_rate.h"
#include "oot3d_native_hd_font_runtime.h"
#include "oot3d_native_pica_composition.h"
#include "oot3d_native_player_temporal_bridge.h"
#include "oot3d_native_ui_lifecycle_bridge.h"
#include "oot3d_native_whole_aot_runtime.h"
#include "oot3d_top_screen_input_cadence.h"
#include "oot3d_top_screen_item_dispatch.h"
#include "oot3d_top_screen_mod_profile.h"
#include "oot3d_top_screen_pause_transaction.h"
#include "oot3d_top_screen_wide_items_runtime.h"
#include "oot3d_typed_gameplay_bridge.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

using Oot3dNativeGame::Oot3dBlockEntryHookOptions;

// The window's inline assembly of the list before it moved to
// Oot3dNativeBlockEntryHookPcs (oot3d_native_a32_window_setup.cpp at
// af991bd7), with its build/launch conditions as options. Pins the cutover:
// a hook added to the function must be added here too.
std::vector<uint32_t> PreviousInlineAssembly(const Oot3dBlockEntryHookOptions &options) {
  using namespace Oot3dA32Window;
  std::vector<uint32_t> blockEntryHooks{
      kMtx4x4BuildFrustum,
      kMtx4x4BuildOrthographicProjectionRotated,
      kGameStateUpdate,
      Oot3dNativeGame::kOot3dPlayerUpdateEntry,
      Oot3dNativeGame::kOot3dFramePacingBeginEntry,
      Oot3dNativeGame::kOot3dFramePacingDecisionEntry,
      Oot3dNativeGame::kOot3dFramePacingCommitEntry,
      Oot3dNativeGame::kOot3dMeshCommandPacketSubmitEntry,
      Oot3dNativeGame::kLandingRollHookPc,
  };
  // Roll dash (11/09): always hooked, inert while config/roll_dash is off.
  blockEntryHooks.insert(blockEntryHooks.end(), Oot3dNativeGame::kRollDashHookPcs.begin(),
                         Oot3dNativeGame::kRollDashHookPcs.end());
  const auto actorVisibilityHooks = Oot3dNativeGame::NativeActorVisibilityHookPcs();
  blockEntryHooks.insert(blockEntryHooks.end(), actorVisibilityHooks.begin(),
                         actorVisibilityHooks.end());
  const auto picaCompositionHooks = Oot3dNativeGame::Oot3dNativePicaCompositionHookPcs();
  blockEntryHooks.insert(blockEntryHooks.end(), picaCompositionHooks.begin(),
                         picaCompositionHooks.end());
  if (options.PlayerTemporal) {
    const auto temporalHooks = Oot3dNativeGame::NativeA32PlayerTemporalHookPcs();
    blockEntryHooks.insert(blockEntryHooks.end(), temporalHooks.begin(), temporalHooks.end());
  }
  const auto typedGameplayObservableExitHooks =
      Oot3dNativeGame::Oot3dTypedGameplayObservableExitPoints();
  if (options.TypedGameplayObservableExits) {
    blockEntryHooks.insert(blockEntryHooks.end(), typedGameplayObservableExitHooks.begin(),
                           typedGameplayObservableExitHooks.end());
  }
  const auto &uiLifecycleEntries =
      Oot3dNativeGame::Oot3dNativeUiLifecycleBridge::GuestEntryPoints();
  blockEntryHooks.insert(blockEntryHooks.end(), uiLifecycleEntries.begin(),
                         uiLifecycleEntries.end());
  if (options.TopScreen) {
    const std::array topScreenHooks{
        Oot3dNativeGame::kTopScreenQuestMaterializedHook,
        Oot3dNativeGame::kTopScreenQuestSubmitModelsHook,
        Oot3dNativeGame::kTopScreenQuestDrawHook,
        0x00300588U,
        0x002FEABCU,
        0x00311364U,
        kPauseProjectionPrepare,
        kPauseUiDraw,
        kPauseUiDrawReturn,
        kPauseRendererVisibilityNativeDraw,
        kPauseIconBuild};
    blockEntryHooks.push_back(kPauseUiUpdate);
    blockEntryHooks.insert(blockEntryHooks.end(), topScreenHooks.begin(), topScreenHooks.end());
    const std::array topScreenSourcePortHooks{
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
        Oot3dNativeGame::kTopScreenSystemPageBoundary,
        kPauseTouchButtonsHeldDownQuery,
        kPauseTouchCoordinateUpdateCall,
        kPauseRendererVisibilityCall,
        kPauseControllerDrawCall,
        kTopScreenCameraNormal1Scalar,
        kTopScreenCameraFovScalar,
        kTopScreenCameraUpdateEntry,
        kTopScreenCameraUpdatePatchSite,
        kTopScreenGameplayCompositionPrefix,
        Oot3dNativeGame::kTopScreenPageProjectionUploadCall,
        Oot3dNativeGame::kTopScreenItemsPageDrawCall,
        Oot3dNativeGame::kTopScreenItemsCursorDrawCallA,
        Oot3dNativeGame::kTopScreenItemsCursorDrawCallB,
    };
    blockEntryHooks.insert(blockEntryHooks.end(), topScreenSourcePortHooks.begin(),
                           topScreenSourcePortHooks.end());
    for (const auto &contract : Oot3dNativeGame::TopScreenVerifiedControlFlowContracts())
      blockEntryHooks.push_back(contract.Entry);
    for (const auto &contract : Oot3dNativeGame::TopScreenVerifiedFloatLoadContracts())
      blockEntryHooks.push_back(contract.Entry);
    for (const auto pc : Oot3dNativeGame::TopScreenDungeonMapIconCoordinateSites())
      blockEntryHooks.push_back(pc);
    const auto itemHooks = Oot3dNativeGame::TopScreenItemDispatchEntries();
    blockEntryHooks.insert(blockEntryHooks.end(), itemHooks.begin(), itemHooks.end());
    blockEntryHooks.push_back(Oot3dNativeGame::kTopScreenInputUpdateBoundary);
  }
  if (options.ExtendedDiagnostics) {
    const std::array diagnosticHooks{
        kEnMagInit, kEnMagUpdate, kEnMagDraw, kPauseUiUpdate, kPauseUiDraw,
        kFileSelectUpdate, kFileSelectInit, kFileChooseInit, kPlayTransitionUpdate,
        kFileSelectActivate, kBlockingQueuePop, kBlockingQueuePush,
        kRendererCommandQueueProcessPending, kBlockingQueueTryPush,
        kTaskQueueTakeFirstFromPriority, kTaskQueueEnqueuePriorityNode,
        kTaskQueueWorkerDrainPriorityNodes, kNngxP3dCallback,
        kNngxCommonInterruptHandler, kNngxCmdlistCompletionDispatch,
        kRendererGlobalCallbackAfterLookup,
    };
    blockEntryHooks.insert(blockEntryHooks.end(), diagnosticHooks.begin(), diagnosticHooks.end());
  }
  if (options.ActorProfile) {
    const auto profileHooks = Oot3dNativeGame::NativeActorProfile::HookPcs();
    blockEntryHooks.insert(blockEntryHooks.end(), profileHooks.begin(), profileHooks.end());
  }
  std::sort(blockEntryHooks.begin(), blockEntryHooks.end());
  if (options.HdFonts) blockEntryHooks.push_back(Oot3dNativeGame::kNativeQbfA4Blit);
  std::sort(blockEntryHooks.begin(), blockEntryHooks.end());
  blockEntryHooks.erase(std::unique(blockEntryHooks.begin(), blockEntryHooks.end()),
                        blockEntryHooks.end());
  return blockEntryHooks;
}

Oot3dBlockEntryHookOptions OptionsFromBits(unsigned bits) {
  Oot3dBlockEntryHookOptions options;
  options.PlayerTemporal = (bits & 1U) != 0U;
  options.TypedGameplayObservableExits = (bits & 2U) != 0U;
  options.TopScreen = (bits & 4U) != 0U;
  options.ExtendedDiagnostics = (bits & 8U) != 0U;
  options.ActorProfile = (bits & 16U) != 0U;
  options.HdFonts = (bits & 32U) != 0U;
  return options;
}
constexpr unsigned kOptionCombinations = 64U;

void TestEveryConfigurationMatchesThePreviousInlineAssembly() {
  for (unsigned bits = 0U; bits < kOptionCombinations; ++bits) {
    const auto options = OptionsFromBits(bits);
    const auto list = Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(options);
    if (list != PreviousInlineAssembly(options)) {
      std::fprintf(stderr, "FAIL: configuration bits 0x%02X differs from the inline assembly\n",
                   bits);
      ++gFailures;
    }
    Expect(std::is_sorted(list.begin(), list.end()) &&
               std::adjacent_find(list.begin(), list.end()) == list.end(),
           "the hook list must be sorted and unique");
  }
  // Snapshots of the product (TopScreen, Enhanced60, HD fonts) and default
  // developer configurations.
  Oot3dBlockEntryHookOptions product;
  product.PlayerTemporal = true;
  product.TopScreen = true;
  product.HdFonts = true;
  const auto productList = Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(product);
  Expect(std::binary_search(productList.begin(), productList.end(),
                            Oot3dNativeGame::kLandingRollHookPc),
         "the landing roll hook is in the product list");
  for (const uint32_t pc : Oot3dNativeGame::kRollDashHookPcs)
    Expect(std::binary_search(productList.begin(), productList.end(), pc),
           "every Roll dash hook is in the product list");
  const auto minimalList = Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs({});
  for (const uint32_t pc : Oot3dNativeGame::kRollDashHookPcs)
    Expect(std::binary_search(minimalList.begin(), minimalList.end(), pc),
           "the Roll dash hooks are in every configuration (the menu turns it on at once)");
  Expect(std::binary_search(productList.begin(), productList.end(),
                            Oot3dNativeGame::kTopScreenInputUpdateBoundary),
         "the TopScreen input boundary is in the product list");
  Expect(!std::binary_search(productList.begin(), productList.end(),
                             Oot3dA32Window::kEnMagInit),
         "extended diagnostics stay out of the product list");
  Expect(!std::binary_search(productList.begin(), productList.end(), 0x00442F38U),
         "0x00442F38 (dungeon map cursor) is never a hook");
}

void TestUnionCoversEveryConfiguration() {
  const auto all = Oot3dNativeGame::Oot3dNativeAllBlockEntryHookPcs();
  std::vector<uint32_t> unionOfAll;
  for (unsigned bits = 0U; bits < kOptionCombinations; ++bits) {
    const auto list = Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(OptionsFromBits(bits));
    unionOfAll.insert(unionOfAll.end(), list.begin(), list.end());
  }
  std::sort(unionOfAll.begin(), unionOfAll.end());
  unionOfAll.erase(std::unique(unionOfAll.begin(), unionOfAll.end()), unionOfAll.end());
  Expect(all == unionOfAll, "the census union equals the union over every configuration");
  Expect(all == Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(
                    OptionsFromBits(kOptionCombinations - 1U)),
         "every option on gives the union");
  for (const uint32_t pc : all) Expect((pc & 3U) == 0U, "hook PCs are word aligned");
  const auto sources = Oot3dNativeGame::Oot3dNativeBlockEntryHookSources(
      Oot3dNativeGame::Oot3dAllBlockEntryHookOptions());
  for (const auto &source : sources)
    Expect(!source.Source.empty() &&
               std::binary_search(all.begin(), all.end(), source.Pc),
           "every census source names a PC of the union");
}

void TestHookGateCheck() {
  using Oot3dNativeGame::Oot3dWholeAotHookGateSet;
  using Oot3dNativeGame::Oot3dWholeAotHookGatesError;
  const auto all = Oot3dNativeGame::Oot3dNativeAllBlockEntryHookPcs();
  const Oot3dWholeAotHookGateSet everyBlock{};
  Expect(Oot3dWholeAotHookGatesError(everyBlock, {}).empty(),
         "the default lowering accepts the every-block callback");
  Expect(Oot3dWholeAotHookGatesError(everyBlock, all).empty(),
         "the default lowering accepts any hook list");
  const Oot3dWholeAotHookGateSet sparse{false, all};
  for (unsigned bits = 0U; bits < kOptionCombinations; ++bits) {
    const auto list = Oot3dNativeGame::Oot3dNativeBlockEntryHookPcs(OptionsFromBits(bits));
    Expect(Oot3dWholeAotHookGatesError(sparse, list).empty(),
           "gates from the census cover every configuration");
  }
  const std::string everyBlockError = Oot3dWholeAotHookGatesError(sparse, {});
  Expect(everyBlockError.find("every block") != std::string::npos &&
             everyBlockError.find("--trace-a32-blocks") != std::string::npos,
         "sparse gates refuse the every-block callback, naming the modes that need it");
  // A gate list without the landing roll hook: the error names it.
  std::vector<uint32_t> withoutLandingRoll;
  for (const uint32_t pc : all)
    if (pc != Oot3dNativeGame::kLandingRollHookPc) withoutLandingRoll.push_back(pc);
  const Oot3dWholeAotHookGateSet missingOne{false, withoutLandingRoll};
  const std::string missingError = Oot3dWholeAotHookGatesError(missingOne, all);
  Expect(missingError.find("0x004BC080") != std::string::npos &&
             missingError.find("whole_aot_hook_gates.json") != std::string::npos,
         "an ungated hook is refused by PC with the file to regenerate");
  uint32_t missing = 0U;
  Expect(!Oot3dNativeGame::Oot3dWholeAotHookGatesCover(missingOne, all, &missing) &&
             missing == Oot3dNativeGame::kLandingRollHookPc,
         "the cover check reports the first ungated hook");
}

void TestLinkedGatesCoverTheCensus() {
  // Whatever whole-AOT code this binary links (none, the default lowering or
  // --sparse-block-entries code) gates every hook of every configuration.
  const auto gates = Oot3dNativeGame::Oot3dLinkedWholeAotHookGates();
  Expect(Oot3dNativeGame::Oot3dWholeAotHookGatesError(
             gates, Oot3dNativeGame::Oot3dNativeAllBlockEntryHookPcs())
             .empty(),
         "the linked whole-AOT code gates every hook of the census");
}

} // namespace

int main() {
  TestEveryConfigurationMatchesThePreviousInlineAssembly();
  TestUnionCoversEveryConfiguration();
  TestHookGateCheck();
  TestLinkedGatesCoverTheCensus();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return EXIT_FAILURE;
  }
  std::printf("oot3d_native_block_entry_hooks_tests passed\n");
  return EXIT_SUCCESS;
}
