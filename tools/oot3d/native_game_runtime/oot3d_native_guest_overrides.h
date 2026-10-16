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

#undef OOT3D_GUEST_OVERRIDE_DECLARE

} // namespace GuestOverrides
} // namespace Oot3dNativeGame
