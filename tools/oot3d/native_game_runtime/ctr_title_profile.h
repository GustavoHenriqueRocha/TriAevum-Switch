#pragma once

// Perfil de título do runtime CTR: o que é específico do OoT3D e só pode ser
// ligado quando o processo montado é mesmo o OoT3D.
//
// O host, o processo e o boot probe nasceram para um título só. Várias
// peças estão presas a endereços guest do code.bin do OoT3D (núcleos nativos
// escritos à mão em PCs fixos, substituições de guest_overrides.json, o
// desbloqueio do Master Quest no system.dat). Num outro code.bin esses PCs
// caem no meio de funções quaisquer e corromperiam o processo. O perfil é
// escolhido pelo program id do ExHeader (process.title_id do manifesto do
// processo); manifesto sem title_id é um manifesto antigo do OoT3D e mantém
// o comportamento de sempre.
//
// Defaults do NativeA32CtrHostConfig = perfil OoT3D: o Zelda não muda.

#include <cstdint>
#include <optional>
#include <string_view>

namespace Oot3dNativeGame {

enum class CtrTitleKind : uint8_t {
    Oot3d,
    Generic,
};

struct CtrTitleProfile {
    CtrTitleKind Kind = CtrTitleKind::Oot3d;
    std::optional<uint64_t> TitleId;
    // Prefixo de arquivos do probe (save data, relatórios).
    std::string_view Name = "oot3d";
    // Núcleos nativos manuais em PCs do OoT3D (oot3d_native_compiled_functions.cpp:
    // PicaCommandWriter 0x00307BD8, memcpy 0x00371738, Mtx3x4 0x0036C174,
    // efeitos de áudio...). Em outro título: desligado.
    bool ManualCompiledFunctions = true;
    // Substituições de guest_overrides.json (só existem se o gerador rodou com
    // --guest-overrides; o liga/desliga em tempo de execução as anula).
    bool GuestOverrides = true;
    // config/unlock_master_quest / TRIAEVUM_UNLOCK_MASTER_QUEST mexe no
    // primeiro byte de /system.dat do save: só no OoT3D.
    bool MasterQuestUnlock = true;
    // srv:GetServiceHandle de serviço desconhecido devolve uma sessão stub que
    // registra cada comando IPC no stderr e responde sucesso, em vez de parar
    // o boot. O OoT3D nunca pede serviço desconhecido; fica desligado nele.
    bool StubUnknownServices = false;
    // WaitSynchronization1/N respeitam o timeout (0 = consulta, >0 = prazo).
    // O host do OoT3D sempre tratou toda espera como infinita; mantido.
    bool KernelWaitTimeouts = false;
    // RTC fixo do probe (ms desde a época CTR "1900"; ver
    // NativeA32CtrHostConfig::FixedDateTimeMilliseconds).
    uint64_t ProbeDateTimeMilliseconds = 3517257600000ULL; // 2011-06-16
};

// Program ids conhecidos do OoT3D (JPN, USA, EUR).
constexpr bool IsOot3dTitleId(uint64_t titleId) noexcept {
    switch (titleId) {
    case 0x0004000000033400ULL:
    case 0x0004000000033500ULL:
    case 0x0004000000033600ULL:
        return true;
    default:
        return false;
    }
}

inline CtrTitleProfile Oot3dTitleProfile(
    std::optional<uint64_t> titleId = std::nullopt) {
    CtrTitleProfile profile;
    profile.TitleId = titleId;
    return profile;
}

// Qualquer outro título: nada preso a endereço do OoT3D, serviços
// desconhecidos registrados em vez de travar, timeouts do kernel reais.
inline CtrTitleProfile GenericTitleProfile(std::optional<uint64_t> titleId) {
    CtrTitleProfile profile;
    profile.Kind = CtrTitleKind::Generic;
    profile.TitleId = titleId;
    profile.Name = "ctr_title";
    profile.ManualCompiledFunctions = false;
    profile.GuestOverrides = false;
    profile.MasterQuestUnlock = false;
    profile.StubUnknownServices = true;
    profile.KernelWaitTimeouts = true;
    profile.ProbeDateTimeMilliseconds = 3530649600000ULL; // 2011-11-18
    return profile;
}

inline CtrTitleProfile SelectCtrTitleProfile(std::optional<uint64_t> titleId) {
    if (!titleId.has_value() || IsOot3dTitleId(*titleId)) {
        return Oot3dTitleProfile(titleId);
    }
    return GenericTitleProfile(titleId);
}

} // namespace Oot3dNativeGame
