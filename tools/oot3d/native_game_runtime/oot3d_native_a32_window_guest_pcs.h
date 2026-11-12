#pragma once

// Guest addresses (code entries, block starts and data) that the A32 window
// uses, apart from the rest of oot3d_native_a32_window_shared.h so that light
// code (the block-entry hook list, oot3d_native_block_entry_hooks.cpp, and
// its census tool) can name them without the window's dependencies.

#include "oot3d_native_frame_rate.h"

#include <cstdint>

namespace Oot3dA32Window {

inline constexpr uint32_t kMtx4x4BuildFrustum = 0x002FDE9CU;
inline constexpr uint32_t kMtx4x4BuildOrthographicProjectionRotated = 0x00300AA8U;
inline constexpr uint32_t kEnMagInit = 0x0018CBB8U;
inline constexpr uint32_t kEnMagDraw = 0x001DA4F4U;
inline constexpr uint32_t kEnMagUpdate = 0x001DA9F8U;
inline constexpr uint32_t kTopScreenTitleLogoFadeHoldPrefix = 0x001DADCCU;
inline constexpr uint32_t kPauseUiUpdate = 0x0041E968U;
inline constexpr uint32_t kPauseUiDraw = 0x0041EC50U;
inline constexpr uint32_t kPauseUiDrawReturn = 0x00419830U;
inline constexpr uint32_t kPauseAlternateViewportDrawCall = 0x00419850U;
inline constexpr uint32_t kPauseAlternateViewportNativeDraw = 0x0041D1A8U;
inline constexpr uint32_t kPauseConditionalDrawCall = 0x00419874U;
inline constexpr uint32_t kPauseConditionalNativeDraw = 0x0041C500U;
inline constexpr uint32_t kPauseRoutedCommandCallA = 0x0041952CU;
inline constexpr uint32_t kPauseRoutedCommandCallB = 0x004198B0U;
inline constexpr uint32_t kPauseRoutedCommandNative = 0x00300240U;
inline constexpr uint32_t kPauseOverlayViewportDrawCall = 0x00419838U;
inline constexpr uint32_t kPauseOverlayNativeDraw = 0x0041F308U;
inline constexpr uint32_t kPauseAlternateRendererUpdateCall = 0x00300538U;
inline constexpr uint32_t kPauseAlternateRendererNativeUpdate = 0x0042A278U;
inline constexpr uint32_t kPauseSceneSevenDrawCall = 0x00300520U;
inline constexpr uint32_t kPauseSceneSevenNativeDraw = 0x004228E4U;
inline constexpr uint32_t kPauseSceneFiveDrawCall = 0x0030052CU;
inline constexpr uint32_t kPauseSceneFiveNativeDraw = 0x0042CB54U;
inline constexpr uint32_t kPauseSceneViewportDrawCall = 0x00300550U;
inline constexpr uint32_t kPauseSceneViewportNativeDraw = 0x0042CBCCU;
inline constexpr uint32_t kPauseTouchCoordinateUpdateCall = 0x0041E984U;
inline constexpr uint32_t kPauseTouchCoordinateNativeUpdate = 0x004289DCU;
inline constexpr uint32_t kPauseRendererVisibilityCall = 0x00300544U;
inline constexpr uint32_t kPauseRendererVisibilityNativeDraw = 0x00427A3CU;
inline constexpr uint32_t kPauseControllerDrawCall = 0x0041988CU;
inline constexpr uint32_t kPauseControllerNativeDraw = 0x0041AFACU;
inline constexpr uint32_t kPauseControllerRendererDraw = 0x002F1280U;
inline constexpr uint32_t kTopScreenPauseState = 0x0050AF68U;
inline constexpr uint32_t kTopScreenPauseSceneControl = 0x005043E8U;
inline constexpr uint32_t kTopScreenDirectCallReturn = 0x60000000U;
inline constexpr uint32_t kAudioPlaySoundGeneral = 0x0037547CU;
inline constexpr uint32_t kTopScreenAimCycleSound = 0x0100048EU;
inline constexpr uint32_t kTopScreenAimCycleSoundPosition = 0x0054AC20U;
inline constexpr uint32_t kTopScreenAimCycleSoundScale = 0x0054AC24U;
inline constexpr uint32_t kTopScreenOcarinaEligibility = 0x002EFFF8U;
inline constexpr uint32_t kGlViewportEntry = 0x002FEABCU;
inline constexpr uint32_t kPauseIconBuild = 0x002E6CD4U;
inline constexpr uint32_t kPauseTouchButtonsDrawCall = 0x0042F5CCU;
inline constexpr uint32_t kPauseTouchButtonsTouchBranch = 0x0042EC2CU;
inline constexpr uint32_t kPauseTouchButtonsNoTouchExit = 0x0042F294U;
// IPS 0x0042E644: `bl PauseInput_GetHeldButtons` -> `mov r0, #0`.
inline constexpr uint32_t kPauseTouchButtonsHeldDownQuery = 0x0042E644U;
inline constexpr uint32_t kPauseTouchButtonsHeldDownTest = 0x0042E648U;
inline constexpr uint32_t kPauseLowerScreenBlock = 0x0041DDC8U;
inline constexpr uint32_t kPauseLowerScreenBlockEnd = 0x0041DEC8U;
inline constexpr uint32_t kPauseUiNativeUpdateCall = 0x0041EAE0U;
inline constexpr uint32_t kPauseProjectionPrepareCall = 0x0041EB28U;
inline constexpr uint32_t kPauseProjectionPrepare = 0x002FBC50U;
inline constexpr uint32_t kPauseUiNativeUpdate = 0x0042DDA8U;
inline constexpr uint32_t kTopScreenCameraNormal1Scalar = 0x0023A930U;
inline constexpr uint32_t kTopScreenCameraNormal1DefaultPath = 0x0023A94CU;
inline constexpr uint32_t kTopScreenCameraNormal1AlternatePath = 0x0023A974U;
inline constexpr uint32_t kTopScreenCameraFovScalar = 0x00478EB8U;
inline constexpr uint32_t kTopScreenCameraUpdateEntry = 0x002D84C4U;
inline constexpr uint32_t kTopScreenCameraUpdatePatchSite = 0x002D84C8U;
inline constexpr uint32_t kTopScreenGameplayCompositionPrefix = 0x002E278CU;
inline constexpr uint32_t kTopScreenGameplayCompositionBranch = 0x002E27C0U;
inline constexpr uint32_t kFileSelectUpdate = 0x0042F9A8U;
inline constexpr uint32_t kFileSelectInit = 0x0046B114U;
inline constexpr uint32_t kGameStateUpdate =
    Oot3dNativeGame::kOot3dGameStateUpdateEntry;
inline constexpr uint32_t kFileChooseInit = 0x00450B60U;
inline constexpr uint32_t kPlayTransitionUpdate = 0x002E2E60U;
inline constexpr uint32_t kFileSelectActivate = 0x002E7AE0U;
inline constexpr uint32_t kFileSelectState = 0x00504FA0U;
inline constexpr uint32_t kNativeGameMode = 0x00588E3CU;
inline constexpr uint32_t kBlockingQueuePop = 0x002CE040U;
inline constexpr uint32_t kBlockingQueuePush = 0x002D2EF4U;
inline constexpr uint32_t kRendererCommandQueueProcessPending = 0x002DBCBCU;
inline constexpr uint32_t kBlockingQueueTryPush = 0x00490D60U;
inline constexpr uint32_t kTaskQueueTakeFirstFromPriority = 0x002D33CCU;
inline constexpr uint32_t kTaskQueueEnqueuePriorityNode = 0x0030AB9CU;
inline constexpr uint32_t kTaskQueueWorkerDrainPriorityNodes = 0x004667B8U;
inline constexpr uint32_t kNngxP3dCallback = 0x00415AE4U;
inline constexpr uint32_t kNngxCommonInterruptHandler = 0x003025C0U;
inline constexpr uint32_t kNngxGlobalState = 0x005A6F30U;
inline constexpr uint32_t kNngxCmdlistCompletionDispatch = 0x003FD420U;
inline constexpr uint32_t kNngxCmdlistOwnerState = 0x0054CC34U;
inline constexpr uint32_t kRendererGlobalCallbackAfterLookup = 0x0016CB78U;
inline constexpr uint32_t kRendererCommandQueue = 0x005B93A8U;
inline constexpr uint32_t kInputGlobalState = 0x0056559CU;
inline constexpr uint32_t kInputHeldOffset = 0x04U;
inline constexpr uint32_t kInputPressedOffset = 0x08U;
inline constexpr uint32_t kInputReleasedOffset = 0x0CU;
inline constexpr uint32_t kInputRawCurrentOffset = 0xC4U;
inline constexpr uint32_t kInputRawPreviousOffset = 0xC8U;

} // namespace Oot3dA32Window
