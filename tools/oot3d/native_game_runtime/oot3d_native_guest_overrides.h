#pragma once

// Substituições nativas escritas à mão para funções guest quentes
// ("decompilação parcial").
//
// Contrato: cada GuestOverrides::Override_XXXXXXXX tem a mesma ABI da
// função traduzida Execute_<nome>_XXXXXXXX (frame, context, state) e só é
// chamada pelo prólogo que apply_guest_overrides.py insere na função gerada,
// quando a entrada é a própria entrada da função. Ela devolve:
//   - false: recusou ANTES de qualquer efeito colateral (orçamento de blocos
//     insuficiente, gancho de bloco dentro da função, FPSCR fora do modo
//     padrão, acesso que não cairia no caminho rápido da memória...). A
//     função traduzida roda então inteira, inalterada;
//   - true: executou e deixou registradores, flags (com as mesmas máscaras de
//     liveness do código gerado), VFP, FPSCR, memória, gerações de escrita,
//     BlocksRemaining/BlocksConsumed e o Oot3dWholeAotFlow exatamente como a
//     versão traduzida deixaria.
//
// Substituições de bloco (Override_<BLOCO> com resumePc): chamadas pelo
// prólogo inserido logo depois do rótulo block_<BLOCO> da função traduzida
// que contém o bloco, antes de o bloco entrar (orçamento, gancho). Devolvem
// false (recusa sem efeito: o bloco traduzido roda) ou true com resumePc = o
// PC de um bloco da mesma função onde a tradução continua (goto), deixando o
// estado exatamente como a tradução deixaria ao entrar nesse bloco; com
// resumePc = 0 a função traduzida devolve `flow` (saída por limite etc.).
// A equivalência é verificada por run_guest_override_equivalence.py contra o
// C++ gerado; uma substituição só entra em guest_overrides.json depois de
// passar sem nenhuma diferença.

#include "oot3d_native_whole_aot_runtime.h"
#include "oot3d_aot_architectural_state.h"

#include <cstddef>
#include <cstdint>

namespace Oot3dNativeGame {

// Liga/desliga em tempo de execução (padrão: ligado). Desligado, o prólogo
// gerado cai direto na tradução, o que permite A/B no console sem recompilar.
extern bool gOot3dGuestOverridesEnabled;

inline bool Oot3dGuestOverridesEnabled() noexcept {
    return gOot3dGuestOverridesEnabled;
}

void SetOot3dGuestOverridesEnabled(bool enabled) noexcept;

// Perfil guest (oot3d_native_guest_profile.cpp). Nos quadros medidos o
// callback de entrada de bloco é o do perfil, chamado em todos os blocos, mas
// ele só repassa ao callback original os PCs do filtro original. Sem este
// registro as substituições viam "gancho em todos os blocos" e recusavam em
// todo quadro medido (e as de bloco no topo de laço recusavam a cada volta),
// então o perfil media a tradução, nunca a versão nativa. Com ele, uma
// substituição só recusa se o perfil repassaria algum PC do seu intervalo;
// senão ela roda e marca no perfil a própria entrada (o tempo nativo fica no
// bloco de entrada). Preenchido pelo perfil; vazio, nada muda.
struct Oot3dGuestOverrideProfileHook {
    // Callback que o perfil instala (GuestBlockProfiler::OnBlockEntry).
    oot3d::recomp::a32::BlockEntryCallback Callback = nullptr;
    // O perfil repassaria ao callback original algum PC em [first, last]?
    bool (*Forwards)(const void* user, uint32_t first, uint32_t last) noexcept = nullptr;
    // Conta uma entrada em `pc` e passa a atribuir o tempo a ele.
    void (*Mark)(void* user, uint32_t pc) noexcept = nullptr;
};

extern Oot3dGuestOverrideProfileHook gOot3dGuestOverrideProfileHook;

struct Oot3dGuestOverrideInfo {
    uint32_t Entry = 0;
    const char* Name = "";
    uint64_t Calls = 0;     // vezes em que a versão nativa executou
    uint64_t Declined = 0;  // vezes em que recusou e a tradução executou
};

// Tabela estática das substituições compiladas, com contadores.
size_t Oot3dGuestOverrideCount() noexcept;
const Oot3dGuestOverrideInfo* Oot3dGuestOverrideTable() noexcept;

namespace GuestOverrides {

#define OOT3D_GUEST_OVERRIDE_DECLARE(entry)                                   \
    bool Override_##entry(Oot3dWholeAotFrame& frame,                          \
                          Oot3dWholeAotContext& context,                      \
                          Oot3dAotArchitecturalState& state,                  \
                          Oot3dWholeAotFlow& flow) noexcept;

OOT3D_GUEST_OVERRIDE_DECLARE(00372070) // oot3d_translate_mtx3x4_local
OOT3D_GUEST_OVERRIDE_DECLARE(002CDE90)
OOT3D_GUEST_OVERRIDE_DECLARE(003094CC)
OOT3D_GUEST_OVERRIDE_DECLARE(002F9430) // PauseRenderBuffer_SetVec2Range
OOT3D_GUEST_OVERRIDE_DECLARE(0036E168) // Math_SmoothStepToF (nome do inventário)
OOT3D_GUEST_OVERRIDE_DECLARE(003325F0) // Math3D_CylOutsideCylDist
OOT3D_GUEST_OVERRIDE_DECLARE(00307C94) // PicaCommandWriter_UploadVertexFloatUniforms
OOT3D_GUEST_OVERRIDE_DECLARE(003727F0) // Math_SinF
OOT3D_GUEST_OVERRIDE_DECLARE(004A022C) // efeito de áudio: atraso de 4 canais
OOT3D_GUEST_OVERRIDE_DECLARE(004A0338) // efeito de áudio: reverberação estéreo
OOT3D_GUEST_OVERRIDE_DECLARE(00371738) // Runtime_Memcpy
OOT3D_GUEST_OVERRIDE_DECLARE(0047AF24) // busca em 50 entradas
OOT3D_GUEST_OVERRIDE_DECLARE(00307B50) // PicaTev_ConvertSource
OOT3D_GUEST_OVERRIDE_DECLARE(00307DC8) // PicaCompare_ConvertFunction
OOT3D_GUEST_OVERRIDE_DECLARE(002BBB14) // Math3D_CirSquareVsTriSquare
OOT3D_GUEST_OVERRIDE_DECLARE(002DCEF4) // Mtx3x3_Multiply

#define OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(entry)                             \
    bool Override_##entry(Oot3dWholeAotFrame& frame,                          \
                          Oot3dWholeAotContext& context,                      \
                          Oot3dAotArchitecturalState& state,                  \
                          Oot3dWholeAotFlow& flow,                            \
                          uint32_t& resumePc) noexcept;

OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00466E68) // MeshCommandPacket_Submit: cópia
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(003247B0) // BgCheck_LineTestImpl: zera marcas
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(002FA674) // CalculateGyroscopeAxisStatus: média
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00308920) // NativeCurve_SampleFloat: busca
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00308604) // CsabCurve_SampleRotation: busca
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(00303C78) // FUN_00303b24: cópia de 64 bytes
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(0046700C) // MeshCommandPacket_PatchBoneUniforms: cópia
OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE(003563E0) // FUN_003562c0: percurso da lista

#undef OOT3D_GUEST_BLOCK_OVERRIDE_DECLARE
#undef OOT3D_GUEST_OVERRIDE_DECLARE

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
