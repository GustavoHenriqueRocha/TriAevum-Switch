// CTR host services: the ndm:u, cfg:u, y2r:u and APT:U sessions.
#include "oot3d_native_a32_ctr_host_internal.h"
#include "oot3d_native_a32_ctr_system_archives.h"

namespace Oot3dNativeGame {

namespace {

// Blocos de configuração com valor fixo nos padrões do Azahar
// (service/cfg/cfg_defaults.cpp, acesso Global, lidos pelo cfg:u GetConfig).
// O tamanho pedido tem de ser o do bloco (o Azahar devolve InvalidSize).
std::optional<std::vector<uint8_t>> AzaharDefaultConfigBlock(uint32_t blockId, uint32_t size) {
    std::vector<uint8_t> value;
    switch (blockId) {
    case 0x000C0000U: value.assign(0xC0U, 0U); break;  // RestrictedPhotoExchange (parental)
    case 0x000C0001U: value.assign(0x14U, 0U); break;  // CoppacsRestriction
    case 0x000C0002U: value.assign(0x200U, 0U); break; // ParentalRestrictionEmail
    case 0x000D0000U: value = {0x7FU, 0x7FU, 0U, 0U}; break; // EULA aceito: 0x7F.0x7F
    case 0x000E0000U: value.assign(1U, 0U); break;
    case 0x00130000U: value.assign(4U, 0U); break;     // DebugMode
    case 0x00150002U: value = {'L', '1', 0U, 0U}; break; // ServerType
    case 0x00160000U: value.assign(4U, 0U); break;
    case 0x00170000U: value.assign(4U, 0U); break;     // MiiverseAccessKey
    default: return std::nullopt;
    }
    if (value.size() != size) return std::nullopt;
    return value;
}

} // namespace

// SendSyncRequest on a client session of ndm:u.
NativeA32HostResult NativeA32CtrHostServices::HandleNdmRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    NativeA32HostContext& context, const std::optional<std::string>& sessionType,
    uint32_t commandBuffer, NativeA32CtrSvcEvent event) {
    if (event.Detail == kNdmSuspendSchedulerRequest) {
        uint32_t performInBackground = 0;
        if (memory.Read32(commandBuffer + 4U, &performInBackground) &&
            performInBackground <= 1U &&
            memory.Write32(commandBuffer,
                           kNdmSuspendSchedulerResponse) &&
            memory.Write32(commandBuffer + 4U, kResultSuccess)) {
            mNdmSchedulerSuspended = true;
            mNdmSchedulerRunsInBackground = performInBackground != 0U;
            state.r[0] = kResultSuccess;
            event.Name += ":SuspendScheduler:background=" +
                          std::to_string(performInBackground);
            event.Detail = performInBackground;
            event.Handled = true;
            mSvcEvents.push_back(std::move(event));
            return {NativeA32HostAction::Resume};
        }
    }
    if (event.Detail == kNdmResumeSchedulerRequest &&
        memory.Write32(commandBuffer, kNdmResumeSchedulerResponse) &&
        memory.Write32(commandBuffer + 4U, kResultSuccess)) {
        mNdmSchedulerSuspended = false;
        mNdmSchedulerRunsInBackground = false;
        state.r[0] = kResultSuccess;
        event.Name += ":ResumeScheduler";
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return {NativeA32HostAction::Resume};
    }
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Wait};
}

// SendSyncRequest on a client session of cfg:u.
NativeA32HostResult NativeA32CtrHostServices::HandleCfgRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    NativeA32HostContext& context, const std::optional<std::string>& sessionType,
    uint32_t commandBuffer, NativeA32CtrSvcEvent event) {
    if (event.Detail == kCfgGetConfigRequest) {
        uint32_t size = 0;
        uint32_t blockId = 0;
        uint32_t descriptor = 0;
        uint32_t address = 0;
        if (memory.Read32(commandBuffer + 4U, &size) &&
            memory.Read32(commandBuffer + 8U, &blockId) &&
            memory.Read32(commandBuffer + 12U, &descriptor) &&
            memory.Read32(commandBuffer + 16U, &address) &&
            (descriptor & 0x8U) != 0U &&
            ((descriptor >> 1U) & 2U) != 0U &&
            (descriptor >> 4U) == size) {
            std::vector<uint8_t> value;
            if (blockId == kCfgSoundOutputModeBlockId && size == 1U) {
                value.push_back(mConfig.SoundOutputMode);
            } else if (blockId == kCfgLanguageBlockId && size == 1U) {
                value.push_back(mConfig.SystemLanguage);
            } else if (blockId == kCfgStereoCameraSettingsBlockId &&
                       size == sizeof(mConfig.StereoCameraSettings)) {
                value.resize(size);
                std::memcpy(value.data(),
                            mConfig.StereoCameraSettings.data(),
                            value.size());
            } else if (auto fixed = AzaharDefaultConfigBlock(blockId, size)) {
                value = std::move(*fixed);
            }
            if (value.size() == size &&
                memory.WriteBytes(address, value) &&
                memory.Write32(commandBuffer, kCfgGetConfigResponse) &&
                memory.Write32(commandBuffer + 4U, kResultSuccess) &&
                memory.Write32(commandBuffer + 8U, descriptor) &&
                memory.Write32(commandBuffer + 12U, address)) {
                state.r[0] = kResultSuccess;
                std::ostringstream configDetail;
                configDetail << ":GetConfig:block=0x" << std::hex
                             << blockId << ":size=0x" << size;
                event.Name += configDetail.str();
                event.Detail = blockId;
                event.Handled = true;
                mSvcEvents.push_back(std::move(event));
                return {NativeA32HostAction::Resume};
            }
        }
    }
    if (event.Detail == kCfgGetRegionRequest &&
        memory.Write32(commandBuffer, kCfgGetRegionResponse) &&
        memory.Write32(commandBuffer + 4U, kResultSuccess) &&
        memory.Write32(commandBuffer + 8U, mConfig.SystemRegion)) {
        state.r[0] = kResultSuccess;
        event.Name +=
            ":GetRegion:" + std::to_string(mConfig.SystemRegion);
        event.Detail = mConfig.SystemRegion;
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return {NativeA32HostAction::Resume};
    }
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Wait};
}

// SendSyncRequest on a client session of y2r:u.
NativeA32HostResult NativeA32CtrHostServices::HandleY2rRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    NativeA32HostContext& context, const std::optional<std::string>& sessionType,
    uint32_t commandBuffer, NativeA32CtrSvcEvent event) {
    const uint16_t command = static_cast<uint16_t>(event.Detail >> 16);
    std::vector<uint32_t> arguments((event.Detail >> 6) & 63U);
    bool valid = true;
    for (size_t i = 0; i < arguments.size(); ++i)
        valid &= memory.Read32(commandBuffer + 4U + static_cast<uint32_t>(i) * 4U,
                               &arguments[i]);
    if (valid) {
        auto reply = CtrServices::DispatchY2r(mY2r, command, arguments, memory);
        if (reply.Handled) {
            if (!mY2rCompletionEvent) {
                mY2rCompletionEvent = std::make_shared<KernelObject>();
                mY2rCompletionEvent->Type = "event:y2r_completion";
                mY2rCompletionEvent->ResetType = 0U;
            }
            if (reply.Clear) mY2rCompletionEvent->AvailableCount = 0;
            if (reply.Signal) mY2rCompletionEvent->AvailableCount = 1;
            const uint32_t normalWords = static_cast<uint32_t>(reply.Words.size());
            if (reply.EventHandle) {
                reply.Words.push_back(0U); // Copy one kernel handle.
                reply.Words.push_back(CreateHandle(mY2rCompletionEvent));
            }
            valid = memory.Write32(commandBuffer, uint32_t(command) << 16 |
                normalWords << 6 | (reply.EventHandle ? 2U : 0U));
            for (size_t i = 0; i < reply.Words.size(); ++i)
                valid &= memory.Write32(commandBuffer + 4U + static_cast<uint32_t>(i) * 4U,
                                        reply.Words[i]);
            if (valid) {
                state.r[0] = kResultSuccess;
                event.Handled = true;
                mSvcEvents.push_back(std::move(event));
                if (reply.Signal) WakeSynchronizationWaiters();
                return {NativeA32HostAction::Resume};
            }
        }
    }
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Wait};
}

// SendSyncRequest on a client session of APT:U.
NativeA32HostResult NativeA32CtrHostServices::HandleAptRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    NativeA32HostContext& context, const std::optional<std::string>& sessionType,
    uint32_t commandBuffer, NativeA32CtrSvcEvent event) {
    if (auto applet = HandleAptAppletRequest(state, memory, commandBuffer, event)) {
        return *applet;
    }
    if (event.Detail == kAptGetLockHandleRequest) {
        uint32_t attributes = 0;
        if (memory.Read32(commandBuffer + 4U, &attributes)) {
            if (!mAptLock) {
                mAptLock = std::make_shared<KernelObject>();
                mAptLock->Type = "mutex:apt_lock";
                mAptLock->AvailableCount = 1;
            }
            const uint32_t lock = CreateHandle(mAptLock);
            if (memory.Write32(commandBuffer,
                               kAptGetLockHandleResponse) &&
                memory.Write32(commandBuffer + 4U, kResultSuccess) &&
                memory.Write32(commandBuffer + 8U, attributes) &&
                memory.Write32(commandBuffer + 12U, 0U) &&
                memory.Write32(commandBuffer + 16U,
                               kCopyHandleDescriptor) &&
                memory.Write32(commandBuffer + 20U, lock)) {
                state.r[0] = kResultSuccess;
                event.Name += ":GetLockHandle";
                event.Handled = true;
                mSvcEvents.push_back(std::move(event));
                return {NativeA32HostAction::Resume};
            }
            CloseHandle(lock);
        }
    }
    if (event.Detail == kAptInitializeRequest) {
        uint32_t appletId = 0;
        uint32_t attributes = 0;
        if (memory.Read32(commandBuffer + 4U, &appletId) &&
            memory.Read32(commandBuffer + 8U, &attributes)) {
            if (!mAptNotificationEvent) {
                mAptNotificationEvent =
                    std::make_shared<KernelObject>();
                mAptNotificationEvent->Type = "event:apt_notification";
            }
            if (!mAptParameterEvent) {
                mAptParameterEvent = std::make_shared<KernelObject>();
                mAptParameterEvent->Type = "event:apt_parameter";
                // The first initialized application receives Wakeup.
                mAptParameterEvent->AvailableCount = 1;
            }
            const uint32_t notification =
                CreateHandle(mAptNotificationEvent);
            const uint32_t parameter = CreateHandle(mAptParameterEvent);
            if (memory.Write32(commandBuffer, kAptInitializeResponse) &&
                memory.Write32(commandBuffer + 4U, kResultSuccess) &&
                memory.Write32(commandBuffer + 8U,
                               kTwoCopyHandlesDescriptor) &&
                memory.Write32(commandBuffer + 12U, notification) &&
                memory.Write32(commandBuffer + 16U, parameter)) {
                state.r[0] = kResultSuccess;
                event.Name += ":Initialize";
                event.Handled = true;
                event.Detail = appletId;
                mAptApplicationId = appletId;
                mAptWakeupPending = true;
                mSvcEvents.push_back(std::move(event));
                return {NativeA32HostAction::Resume};
            }
            CloseHandle(notification);
            CloseHandle(parameter);
        }
    }
    if (event.Detail == kAptGetSharedFontRequest) {
        // Bloco de memória com a fonte substituta, relocada para
        // kCtrSharedFontAddress; o MapMemoryBlock o preenche. Um só objeto
        // por processo: reaproveita o de um handle ainda aberto.
        std::shared_ptr<KernelObject> font;
        for (const auto& record : mHandles) {
            if (record.Object && record.Object->Type == "shared_memory:apt_font") {
                font = record.Object;
                break;
            }
        }
        if (!font) {
            font = std::make_shared<KernelObject>();
            font->Type = "shared_memory:apt_font";
            font->FileSize = kCtrSharedFontMemorySize;
        }
        const uint32_t handle = CreateHandle(font);
        if (memory.Write32(commandBuffer, kAptGetSharedFontResponse) &&
            memory.Write32(commandBuffer + 4U, kResultSuccess) &&
            memory.Write32(commandBuffer + 8U, kCtrSharedFontAddress) &&
            memory.Write32(commandBuffer + 12U, kCopyHandleDescriptor) &&
            memory.Write32(commandBuffer + 16U, handle)) {
            state.r[0] = kResultSuccess;
            event.Name += ":GetSharedFont";
            event.Handled = true;
            mSvcEvents.push_back(std::move(event));
            return {NativeA32HostAction::Resume};
        }
        CloseHandle(handle);
    }
    if (event.Detail == kAptEnableRequest &&
        memory.Write32(commandBuffer, kAptEnableResponse) &&
        memory.Write32(commandBuffer + 4U, kResultSuccess)) {
        state.r[0] = kResultSuccess;
        event.Name += ":Enable";
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return {NativeA32HostAction::Resume};
    }
    if (event.Detail == kAptNotifyToWaitRequest &&
        memory.Write32(commandBuffer, kAptNotifyToWaitResponse) &&
        memory.Write32(commandBuffer + 4U, kResultSuccess)) {
        state.r[0] = kResultSuccess;
        event.Name += ":NotifyToWait";
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return {NativeA32HostAction::Resume};
    }
    if (event.Detail == kAptReceiveParameterRequest &&
        mAptWakeupPending) {
        uint32_t appletId = 0;
        uint32_t bufferSize = 0;
        uint32_t staticDescriptor = 0;
        uint32_t staticAddress = 0;
        const uint32_t staticBufferTable =
            state.thread_pointer + 0x180U;
        if (memory.Read32(commandBuffer + 4U, &appletId) &&
            memory.Read32(commandBuffer + 8U, &bufferSize) &&
            appletId == mAptApplicationId && bufferSize <= 0x1000U &&
            memory.Read32(staticBufferTable, &staticDescriptor) &&
            memory.Read32(staticBufferTable + 4U, &staticAddress) &&
            (staticDescriptor & 0xFU) == 2U &&
            (staticDescriptor >> 14U) >= bufferSize &&
            memory.Fill(staticAddress, bufferSize, 0) &&
            memory.Write32(commandBuffer,
                           kAptReceiveParameterResponse) &&
            memory.Write32(commandBuffer + 4U, kResultSuccess) &&
            memory.Write32(commandBuffer + 8U, 0U) &&
            memory.Write32(commandBuffer + 12U, 1U) &&
            memory.Write32(commandBuffer + 16U, 0U) &&
            memory.Write32(commandBuffer + 20U,
                           kMoveHandleDescriptor) &&
            memory.Write32(commandBuffer + 24U, 0U) &&
            memory.Write32(commandBuffer + 28U,
                           (bufferSize << 14U) | 2U) &&
            memory.Write32(commandBuffer + 32U, staticAddress)) {
            mAptWakeupPending = false;
            state.r[0] = kResultSuccess;
            event.Name += ":ReceiveParameter:Wakeup";
            event.Handled = true;
            mSvcEvents.push_back(std::move(event));
            return {NativeA32HostAction::Resume};
        }
    }
    if (event.Detail == kAptAppletUtilityRequest) {
        uint32_t utilityCommand = 0;
        uint32_t inputSize = 0;
        uint32_t outputSize = 0;
        uint32_t inputDescriptor = 0;
        uint32_t inputAddress = 0;
        uint32_t outputDescriptor = 0;
        uint32_t outputAddress = 0;
        uint32_t transition = 0;
        uint8_t shellState = 0;
        const uint32_t staticBufferTable =
            state.thread_pointer + 0x180U;
        if (memory.Read32(commandBuffer + 4U, &utilityCommand) &&
            memory.Read32(commandBuffer + 8U, &inputSize) &&
            memory.Read32(commandBuffer + 12U, &outputSize) &&
            memory.Read32(commandBuffer + 16U, &inputDescriptor) &&
            memory.Read32(commandBuffer + 20U, &inputAddress) &&
            memory.Read32(staticBufferTable, &outputDescriptor) &&
            memory.Read32(staticBufferTable + 4U, &outputAddress)) {
            event.Name += ":AppletUtility:command=" +
                          std::to_string(utilityCommand) +
                          ":input=" + std::to_string(inputSize) +
                          ":output=" + std::to_string(outputSize);
            const bool validStaticBuffers =
                (inputDescriptor & 0xFU) == 2U &&
                ((inputDescriptor >> 10U) & 0xFU) == 1U &&
                (inputDescriptor >> 14U) >= inputSize &&
                (outputDescriptor & 0xFU) == 2U &&
                ((outputDescriptor >> 10U) & 0xFU) == 0U &&
                (outputDescriptor >> 14U) >= outputSize;
            const bool unlockTransition =
                utilityCommand == kAptUtilityUnlockTransition &&
                inputSize == sizeof(uint32_t) && outputSize == 1U &&
                memory.Read32(inputAddress, &transition);
            const bool sleepIfShellClosed =
                utilityCommand == kAptUtilitySleepIfShellClosed &&
                inputSize == sizeof(uint8_t) && outputSize == 1U &&
                memory.Read8(inputAddress, &shellState);
            if (validStaticBuffers &&
                (unlockTransition || sleepIfShellClosed) &&
                memory.Fill(outputAddress, outputSize, 0U) &&
                memory.Write32(commandBuffer,
                               kAptAppletUtilityResponse) &&
                memory.Write32(commandBuffer + 4U, kResultSuccess) &&
                memory.Write32(commandBuffer + 8U, kResultSuccess) &&
                memory.Write32(commandBuffer + 12U,
                               (outputSize << 14U) | 2U) &&
                memory.Write32(commandBuffer + 16U, outputAddress)) {
                state.r[0] = kResultSuccess;
                if (unlockTransition) {
                    event.Name += ":UnlockTransition:" +
                                  std::to_string(transition);
                    event.Detail = transition;
                } else {
                    event.Name += ":SleepIfShellClosed:open";
                    event.Detail = shellState;
                }
                event.Handled = true;
                mSvcEvents.push_back(std::move(event));
                return {NativeA32HostAction::Resume};
            }
        }
    }
    mSvcEvents.push_back(std::move(event));
    return {NativeA32HostAction::Wait};
}

} // namespace Oot3dNativeGame
