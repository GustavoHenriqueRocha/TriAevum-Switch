// CTR host services: applet de biblioteca do APT:U e o seletor de Mii em HLE.
//
// Segue o Azahar (service/apt/applet_manager.cpp, apt.cpp e
// applets/mii_selector.cpp): PreloadLibraryApplet/PrepareToStartLibraryApplet
// registram o applet no slot de biblioteca, GetAppletInfo responde
// "registered && loaded", SendParameter(Request) recebe Response com o bloco
// de captura, e StartLibraryApplet fecha o seletor na hora com WakeupByExit e
// o Mii padrão (o frontend padrão do Azahar faz o mesmo).
#include "oot3d_native_a32_ctr_host_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

namespace {

constexpr uint32_t kAptMiiSelectorEd1 = 0x401U;
constexpr uint32_t kAptMiiSelectorEd2 = 0x402U;
constexpr uint32_t kAptCaptureBufferInfoSize = 0x20U;
constexpr uint32_t kAptMiiConfigSize = 0x104U;
// AppletPos::Library: o applet foi lançado pela aplicação (GetLockHandle do
// Azahar corrige AutoLibrary para Library).
constexpr uint32_t kAptLibraryAppletAttributes = 1U;
constexpr uint32_t kAptStaticBufferMaximum = 0x1000U;

// Id de applet no nome do evento: "0x402".
std::string HexId(uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%03x", value);
    return text;
}

bool IsMiiSelector(uint32_t appletId) {
    return appletId == kAptMiiSelectorEd1 || appletId == kAptMiiSelectorEd2;
}

// Title do seletor de Mii por região do cfg (JPN, USA, EUR, AUS, CHN, KOR, TWN).
uint64_t MiiSelectorTitleId(uint32_t region) {
    constexpr std::array<uint64_t, 7> titles{
        0x000400300000C102ULL, 0x000400300000C902ULL, 0x000400300000D102ULL,
        0x000400300000C102ULL, 0x000400300000D902ULL, 0x000400300000DF02ULL,
        0x000400300000E502ULL};
    return titles[region < titles.size() ? region : 2U];
}

void PutBe16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 1U] = static_cast<uint8_t>(value);
}

void PutBe32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    PutBe16(bytes, offset, static_cast<uint16_t>(value >> 16U));
    PutBe16(bytes, offset + 2U, static_cast<uint16_t>(value));
}

void PutUtf16Le(std::vector<uint8_t>& bytes, size_t offset, std::string_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        bytes[offset + i * 2U] = static_cast<uint8_t>(text[i]);
    }
}

// CRC-16 poly 0x1021, início 0, sem reflexão (3dbrew Mii#Checksum).
uint16_t MiiChecksum(const uint8_t* data, size_t size) {
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i] << 8U);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) != 0U ? static_cast<uint16_t>((crc << 1U) ^ 0x1021U)
                                        : static_cast<uint16_t>(crc << 1U);
        }
    }
    return crc;
}

// MiiResult (0x84 bytes) com o Mii padrão do Azahar
// (MiiSelector::GetStandardMiiResult, despejo do seletor do firmware 11.8.0).
std::vector<uint8_t> StandardMiiResult() {
    std::vector<uint8_t> result(0x84U, 0U);
    PutBe32(result, 0x00U, 0U);           // return_code: Mii escolhido
    PutBe32(result, 0x04U, 0U);           // is_guest_mii_selected
    PutBe32(result, 0x08U, 0xFFFFFFFFU);  // selected_guest_mii_index
    constexpr size_t mii = 0x0CU;         // ChecksummedMiiData
    result[mii + 0x00U] = 0x03U;          // version
    result[mii + 0x01U] = 0x00U;          // mii_options
    result[mii + 0x02U] = 0x10U;          // mii_pos
    result[mii + 0x03U] = 0x30U;          // console_identity (3DS)
    PutBe32(result, mii + 0x04U, 0xD285B6B3U);  // system_id (u64 big-endian)
    PutBe32(result, mii + 0x08U, 0x00C8850AU);
    PutBe32(result, mii + 0x0CU, 0x98391EE4U);  // mii_id
    constexpr std::array<uint8_t, 6> mac{0x40U, 0xF4U, 0x07U, 0xB7U, 0x37U, 0x10U};
    std::copy(mac.begin(), mac.end(), result.begin() + mii + 0x10U);
    PutBe16(result, mii + 0x18U, 0xA600U);      // mii_details
    PutUtf16Le(result, mii + 0x1AU, "Citra");   // mii_name (10 UTF-16)
    result[mii + 0x2EU] = 0x40U;                // height
    result[mii + 0x2FU] = 0x40U;                // width
    result[mii + 0x32U] = 0x21U;                // hair_style
    result[mii + 0x33U] = 0x01U;                // hair_details
    PutBe32(result, mii + 0x34U, 0x02684418U);  // eye_details
    PutBe32(result, mii + 0x38U, 0x26344614U);  // eyebrow_details
    PutBe16(result, mii + 0x3CU, 0x8112U);      // nose_details
    PutBe16(result, mii + 0x3EU, 0x1768U);      // mouth_details
    PutBe16(result, mii + 0x40U, 0x0D00U);      // mustache_details
    PutBe16(result, mii + 0x42U, 0x0029U);      // beard_details
    PutBe16(result, mii + 0x44U, 0x0052U);      // glasses_details
    PutBe16(result, mii + 0x46U, 0x4850U);      // mole_details
    PutUtf16Le(result, mii + 0x48U, "flTobi");  // author_name
    // 0x5C bytes do MiiData + 2 de padding, CRC big-endian em +0x5E.
    PutBe16(result, mii + 0x5EU, MiiChecksum(result.data() + mii, 0x5EU));
    return result;
}

} // namespace

void NativeA32CtrHostServices::SendAptParameter(AptParameter parameter) {
    const bool toApplication = parameter.Destination == mAptApplicationId;
    mAptParameter = std::move(parameter);
    if (toApplication && mAptParameterEvent) {
        mAptParameterEvent->AvailableCount = 1;
        WakeSynchronizationWaiters();
    }
}

std::optional<NativeA32HostResult> NativeA32CtrHostServices::HandleAptAppletRequest(
    oot3d::recomp::a32::GuestState& state, NativeA32Memory& memory,
    uint32_t commandBuffer, NativeA32CtrSvcEvent& event) {
    const uint32_t header = event.Detail;
    const uint32_t command = header >> 16U;
    // Resposta só com o resultado (MakeBuilder(1, 0)) ou com palavras extras.
    const auto reply = [&](uint32_t responseHeader, std::vector<uint32_t> words,
                           const std::string& name) -> std::optional<NativeA32HostResult> {
        bool written = memory.Write32(commandBuffer, responseHeader);
        for (size_t i = 0; written && i < words.size(); ++i) {
            written = memory.Write32(commandBuffer + 4U + static_cast<uint32_t>(i * 4U), words[i]);
        }
        if (!written) {
            return std::nullopt;
        }
        state.r[0] = kResultSuccess;
        event.Name += name;
        event.Handled = true;
        mSvcEvents.push_back(std::move(event));
        return NativeA32HostResult{NativeA32HostAction::Resume};
    };
    const auto result = [&](uint32_t code, const std::string& name) {
        return reply((command << 16U) | 0x40U, {code}, name);
    };
    uint32_t appletId = 0;
    if (!memory.Read32(commandBuffer + 4U, &appletId)) {
        return std::nullopt;
    }
    const std::string id = ":" + HexId(appletId);
    auto& slot = mAptLibraryApplet;
    const bool parameterPending = mAptParameter.has_value() || mAptWakeupPending;

    switch (header) {
    case kAptGetAppletInfoRequest: {
        if (slot.Registered && appletId == slot.AppletId) {
            const uint64_t title = MiiSelectorTitleId(mConfig.SystemRegion);
            return reply(kAptGetAppletInfoResponse,
                         {kResultSuccess, static_cast<uint32_t>(title),
                          static_cast<uint32_t>(title >> 32U), 0U /* NAND */, 1U,
                          slot.Loaded ? 1U : 0U, kAptLibraryAppletAttributes},
                         ":GetAppletInfo" + id + (slot.Loaded ? ":loaded" : ":registered"));
        }
        if (appletId == mAptApplicationId) {
            return std::nullopt;  // a própria aplicação: ainda sem HLE
        }
        return result(kAptResultNotFound, ":GetAppletInfo" + id + ":NotFound");
    }
    case kAptIsRegisteredRequest: {
        // A aplicação e o slot de biblioteca; o resto (applets de sistema,
        // ids "Any*") não existe aqui.
        const bool registered = (mAptApplicationId != 0U && appletId == mAptApplicationId) ||
                                (slot.Registered && appletId == slot.AppletId);
        return reply(kAptIsRegisteredResponse, {kResultSuccess, registered ? 1U : 0U},
                     ":IsRegistered" + id + (registered ? ":yes" : ":no"));
    }
    case kAptPreloadLibraryAppletRequest:
    case kAptPrepareToStartLibraryAppletRequest: {
        const bool preload = header == kAptPreloadLibraryAppletRequest;
        const std::string name = preload ? ":PreloadLibraryApplet" : ":PrepareToStartLibraryApplet";
        if (!preload && parameterPending) {
            return result(kAptResultParameterPresent, name + id + ":ParameterPresent");
        }
        if (slot.Registered) {
            return result(kAptResultAlreadyExists, name + id + ":AlreadyExists");
        }
        if (!IsMiiSelector(appletId)) {
            return result(kAptResultAppletNotSupported, name + id + ":sem-HLE");
        }
        slot = {appletId, mAptApplicationId, true, preload};
        return result(kResultSuccess, name + id + ":MiiSelector");
    }
    case kAptFinishPreloadingLibraryAppletRequest:
        // O Azahar marca o slot sem conferir o estado (TODO dele).
        slot.Loaded = true;
        return result(kResultSuccess, ":FinishPreloadingLibraryApplet" + id);
    case kAptSendParameterRequest: {
        uint32_t destination = 0, signal = 0, size = 0, handleDescriptor = 0, handle = 0;
        uint32_t bufferDescriptor = 0, bufferAddress = 0;
        if (!memory.Read32(commandBuffer + 8U, &destination) ||
            !memory.Read32(commandBuffer + 12U, &signal) ||
            !memory.Read32(commandBuffer + 16U, &size) ||
            !memory.Read32(commandBuffer + 20U, &handleDescriptor) ||
            !memory.Read32(commandBuffer + 24U, &handle) ||
            !memory.Read32(commandBuffer + 28U, &bufferDescriptor) ||
            !memory.Read32(commandBuffer + 32U, &bufferAddress) ||
            (handleDescriptor & ~0x10U) != 0U || (bufferDescriptor & 0xFU) != 2U) {
            // Motivo no evento (aparece no despejo do guest e no stub).
            char detail[96];
            std::snprintf(detail, sizeof(detail),
                          ":SendParameter:invalid:handle_desc=0x%x:handle=0x%x:buffer_desc=0x%x",
                          handleDescriptor, handle, bufferDescriptor);
            event.Name += detail;
            return std::nullopt;
        }
        AptParameter parameter{appletId, destination, signal, nullptr, {}};
        parameter.Buffer.resize(std::min(size, bufferDescriptor >> 14U));
        if (!memory.ReadBytes(bufferAddress, parameter.Buffer)) {
            event.Name += ":SendParameter:unreadable-buffer";
            return std::nullopt;
        }
        if (handle != 0U) {
            const HandleRecord* record = LookupHandle(handle);
            if (record == nullptr) {
                event.Name += ":SendParameter:unknown-handle";
                return std::nullopt;
            }
            parameter.Object = record->Object;
        }
        const std::string name = ":SendParameter" + id + "->" + HexId(destination) +
                                 ":signal=" + std::to_string(signal);
        if (parameterPending) {
            return result(kAptResultParameterPresent, name + ":ParameterPresent");
        }
        if (slot.Registered && destination == slot.AppletId && IsMiiSelector(destination)) {
            // MiiSelector::ReceiveParameterImpl: só Request com o
            // CaptureBufferInfo; responde com o bloco de captura.
            if (signal != kAptSignalRequest ||
                parameter.Buffer.size() != kAptCaptureBufferInfoSize) {
                event.Name += ":SendParameter:MiiSelector:unsupported:signal=" +
                              std::to_string(signal) +
                              ":size=" + std::to_string(parameter.Buffer.size());
                return std::nullopt;
            }
            uint32_t captureSize = 0;
            std::memcpy(&captureSize, parameter.Buffer.data(), sizeof(captureSize));
            auto capture = std::make_shared<KernelObject>();
            capture->Type = "shared_memory:applet_capture";
            // Size 0 (o SM3DL pede sem captura) vale: o Azahar cria o bloco
            // vazio; o MapMemoryBlock recusa mapear um bloco de tamanho 0.
            capture->FileSize = (static_cast<uint64_t>(captureSize) + 0xFFFU) & ~0xFFFULL;
            SendAptParameter({slot.AppletId, slot.Parent, kAptSignalResponse, capture, {}});
            return result(kResultSuccess, name + ":MiiSelector:Response");
        }
        SendAptParameter(std::move(parameter));
        return result(kResultSuccess, name);
    }
    case kAptStartLibraryAppletRequest: {
        uint32_t size = 0, handleDescriptor = 0, handle = 0;
        uint32_t bufferDescriptor = 0, bufferAddress = 0;
        if (!memory.Read32(commandBuffer + 8U, &size) ||
            !memory.Read32(commandBuffer + 12U, &handleDescriptor) ||
            !memory.Read32(commandBuffer + 16U, &handle) ||
            !memory.Read32(commandBuffer + 20U, &bufferDescriptor) ||
            !memory.Read32(commandBuffer + 24U, &bufferAddress) ||
            !slot.Registered || appletId != slot.AppletId || !IsMiiSelector(appletId) ||
            size != kAptMiiConfigSize || (bufferDescriptor & 0xFU) != 2U ||
            (bufferDescriptor >> 14U) < size) {
            return std::nullopt;
        }
        if (parameterPending) {
            return result(kAptResultParameterPresent,
                          ":StartLibraryApplet" + id + ":ParameterPresent");
        }
        // Wakeup com o MiiConfig -> o seletor fecha logo (frontend padrão
        // do Azahar): WakeupByExit para quem lançou, com o MiiResult.
        const uint32_t parent = slot.Parent;
        slot = {};
        SendAptParameter({appletId, parent, kAptSignalWakeupByExit, nullptr, StandardMiiResult()});
        return result(kResultSuccess, ":StartLibraryApplet" + id + ":MiiSelector:WakeupByExit");
    }
    case kAptReceiveParameterRequest:
    case kAptGlanceParameterRequest: {
        const bool glance = header == kAptGlanceParameterRequest;
        const std::string name = glance ? ":GlanceParameter" : ":ReceiveParameter";
        if (!glance && mAptWakeupPending) {
            return std::nullopt;  // o Wakeup inicial segue no HandleAptRequest
        }
        uint32_t bufferSize = 0, staticDescriptor = 0, staticAddress = 0;
        const uint32_t staticBufferTable = state.thread_pointer + 0x180U;
        if (!memory.Read32(commandBuffer + 8U, &bufferSize) ||
            !memory.Read32(staticBufferTable, &staticDescriptor) ||
            !memory.Read32(staticBufferTable + 4U, &staticAddress) ||
            bufferSize > kAptStaticBufferMaximum || (staticDescriptor & 0xFU) != 2U ||
            (staticDescriptor >> 14U) < bufferSize) {
            return std::nullopt;
        }
        AptParameter parameter;
        if (glance && mAptWakeupPending && appletId == mAptApplicationId) {
            parameter.Signal = kAptSignalWakeup;
        } else if (!mAptParameter) {
            return result(kAptResultNoData, name + id + ":NoData");
        } else if (mAptParameter->Destination != appletId) {
            return result(kAptResultNotFound, name + id + ":NotFound");
        } else {
            parameter = *mAptParameter;
        }
        // O APT sempre devolve o buffer com o tamanho pedido (Azahar).
        std::vector<uint8_t> buffer = parameter.Buffer;
        const uint32_t size = std::min(static_cast<uint32_t>(buffer.size()), bufferSize);
        buffer.resize(bufferSize, 0U);
        if (!memory.WriteBytes(staticAddress, buffer)) {
            return std::nullopt;
        }
        const uint32_t object = parameter.Object ? CreateHandle(parameter.Object) : 0U;
        const auto response = reply(glance ? kAptGlanceParameterResponse : kAptReceiveParameterResponse,
                                    {kResultSuccess, parameter.Sender, parameter.Signal, size,
                                     kMoveHandleDescriptor, object, (bufferSize << 14U) | 2U,
                                     staticAddress},
                                    name + id + ":from=" + HexId(parameter.Sender) +
                                        ":signal=" + std::to_string(parameter.Signal));
        if (!response) {
            if (object != 0U) CloseHandle(object);
            return std::nullopt;
        }
        // Receive consome; Glance só consome DspSleep/DspWakeup (NS do Azahar).
        if (!glance || parameter.Signal == 0x8U || parameter.Signal == 0x9U) {
            if (!(glance && mAptWakeupPending)) mAptParameter.reset();
        }
        return response;
    }
    default:
        return std::nullopt;
    }
}

} // namespace Oot3dNativeGame
