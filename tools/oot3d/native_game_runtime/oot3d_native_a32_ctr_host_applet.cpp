// CTR host services: applet de biblioteca do APT:U, o seletor de Mii e o
// teclado de software em HLE.
//
// Segue o Azahar (service/apt/applet_manager.cpp, apt.cpp e
// applets/mii_selector.cpp): PreloadLibraryApplet/PrepareToStartLibraryApplet
// registram o applet no slot de biblioteca, GetAppletInfo responde
// "registered && loaded", SendParameter(Request) recebe Response com o bloco
// de captura, e StartLibraryApplet fecha o seletor na hora com WakeupByExit e
// o Mii padrão (o frontend padrão do Azahar faz o mesmo). O teclado de
// software (applets/swkbd.cpp) segue o mesmo ciclo e devolve na hora o texto
// do host no bloco de texto e o SoftwareKeyboardConfig com o botão de
// confirmar (o MM3D nomeia o arquivo novo assim).
#include "oot3d_native_a32_ctr_host_internal.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

namespace {

// AppletId do Azahar (service/apt/applet_manager.h): Ed1/Ed2 é o seletor de
// Mii, SoftwareKeyboard1/2 o teclado.
constexpr uint32_t kAptSoftwareKeyboard1 = 0x201U;
constexpr uint32_t kAptMiiSelectorEd1 = 0x202U;
constexpr uint32_t kAptSoftwareKeyboard2 = 0x401U;
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

bool IsSoftwareKeyboard(uint32_t appletId) {
    return appletId == kAptSoftwareKeyboard1 || appletId == kAptSoftwareKeyboard2;
}

// Applets de biblioteca com HLE aqui.
bool HasLibraryAppletHle(uint32_t appletId) {
    return IsMiiSelector(appletId) || IsSoftwareKeyboard(appletId);
}

// Title do seletor de Mii por região do cfg (JPN, USA, EUR, AUS, CHN, KOR, TWN).
uint64_t MiiSelectorTitleId(uint32_t region) {
    constexpr std::array<uint64_t, 7> titles{
        0x000400300000C102ULL, 0x000400300000C902ULL, 0x000400300000D102ULL,
        0x000400300000C102ULL, 0x000400300000D902ULL, 0x000400300000DF02ULL,
        0x000400300000E502ULL};
    return titles[region < titles.size() ? region : 2U];
}

// O teclado tem o title do seletor menos 0x100 em toda região (C002, C802,
// D002, C002, D802, DE02, E402 no Azahar).
uint64_t LibraryAppletTitleId(uint32_t appletId, uint32_t region) {
    const uint64_t mii = MiiSelectorTitleId(region);
    return IsSoftwareKeyboard(appletId) ? mii - 0x100U : mii;
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

void NativeA32CtrHostServices::SetSoftwareKeyboardText(std::u16string text) {
    mSoftwareKeyboardText = std::move(text);
}

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
            const uint64_t title = LibraryAppletTitleId(slot.AppletId, mConfig.SystemRegion);
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
        if (!HasLibraryAppletHle(appletId)) {
            return result(kAptResultAppletNotSupported, name + id + ":sem-HLE");
        }
        slot = {appletId, mAptApplicationId, true, preload};
        return result(kResultSuccess,
                      name + id + (IsMiiSelector(appletId) ? ":MiiSelector" : ":SoftwareKeyboard"));
    }
    case kAptReplySleepQueryRequest:
        // Respostas da aplicação ao aviso de sono: nada a fazer aqui.
        return result(kResultSuccess, ":ReplySleepQuery" + id);
    case kAptReplySleepNotificationCompleteRequest:
        return result(kResultSuccess, ":ReplySleepNotificationComplete" + id);
    case kAptSendCaptureBufferInfoRequest:
        // Onde a aplicação capturou as telas para o applet: nenhum applet
        // desenha neste host.
        return result(kResultSuccess, ":SendCaptureBufferInfo");
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
        if (slot.Registered && destination == slot.AppletId && HasLibraryAppletHle(destination)) {
            // MiiSelector/SoftwareKeyboard::ReceiveParameterImpl: só Request
            // com o CaptureBufferInfo; responde com o bloco de captura.
            const std::string applet = IsMiiSelector(destination) ? ":MiiSelector" : ":SoftwareKeyboard";
            if (signal != kAptSignalRequest ||
                parameter.Buffer.size() != kAptCaptureBufferInfoSize) {
                event.Name += ":SendParameter" + applet + ":unsupported:signal=" +
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
            return result(kResultSuccess, name + applet + ":Response");
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
            !slot.Registered || appletId != slot.AppletId || !HasLibraryAppletHle(appletId) ||
            size != (IsMiiSelector(appletId) ? kAptMiiConfigSize : kSoftwareKeyboardConfigSize) ||
            (bufferDescriptor & 0xFU) != 2U || (bufferDescriptor >> 14U) < size) {
            return std::nullopt;
        }
        if (parameterPending) {
            return result(kAptResultParameterPresent,
                          ":StartLibraryApplet" + id + ":ParameterPresent");
        }
        if (IsSoftwareKeyboard(appletId)) {
            // SoftwareKeyboard::Start + Update + Finalize com o frontend que
            // confirma na hora: o texto do host (com terminador) no bloco de
            // texto e o config de volta com o botão de confirmar.
            const HandleRecord* block = handle != 0U ? LookupHandle(handle) : nullptr;
            std::vector<uint8_t> config(size);
            if (block == nullptr || !block->Object->MappedAddress.has_value() ||
                !memory.ReadBytes(bufferAddress, config)) {
                event.Name += ":StartLibraryApplet" + id + ":SoftwareKeyboard:no-text-block";
                return std::nullopt;
            }
            std::u16string text = mSoftwareKeyboardText;
            const uint16_t maximum = static_cast<uint16_t>(
                config[kSoftwareKeyboardMaxTextLengthOffset] |
                (config[kSoftwareKeyboardMaxTextLengthOffset + 1U] << 8U));
            if (maximum != 0U && text.size() > maximum) text.resize(maximum);
            std::vector<uint8_t> encoded;
            for (const char16_t character : text) {
                encoded.push_back(static_cast<uint8_t>(character & 0xFFU));
                encoded.push_back(static_cast<uint8_t>(character >> 8U));
            }
            encoded.push_back(0U);
            encoded.push_back(0U);
            if (encoded.size() > block->Object->FileSize ||
                !memory.WriteBytes(*block->Object->MappedAddress, encoded)) {
                event.Name += ":StartLibraryApplet" + id + ":SoftwareKeyboard:text-write-failed";
                return std::nullopt;
            }
            // num_buttons_m1: um botão -> D0Click (0), dois -> D1Click1 (2),
            // três -> D2Click2 (5), nenhum -> None (-1): o da direita.
            uint32_t buttons = 0;
            std::memcpy(&buttons, config.data() + kSoftwareKeyboardButtonsOffset, sizeof(buttons));
            constexpr std::array<int32_t, 4> kConfirm{0, 2, 5, -1};
            const auto put32 = [&](size_t offset, uint32_t value) {
                for (size_t index = 0; index < 4U; ++index) {
                    config[offset + index] = static_cast<uint8_t>(value >> (8U * index));
                }
            };
            put32(kSoftwareKeyboardReturnCodeOffset,
                  static_cast<uint32_t>(kConfirm[std::min<uint32_t>(buttons, 3U)]));
            put32(kSoftwareKeyboardTextOffsetOffset, 0U);
            config[kSoftwareKeyboardTextLengthOffset] = static_cast<uint8_t>(text.size());
            config[kSoftwareKeyboardTextLengthOffset + 1U] = static_cast<uint8_t>(text.size() >> 8U);
            const uint32_t parent = slot.Parent;
            slot = {};
            SendAptParameter({appletId, parent, kAptSignalWakeupByExit, nullptr, std::move(config)});
            return result(kResultSuccess,
                          ":StartLibraryApplet" + id + ":SoftwareKeyboard:WakeupByExit");
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
