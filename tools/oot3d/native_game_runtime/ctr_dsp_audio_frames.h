#pragma once

// Quadros de áudio do DSP que o host contou (AdvanceSystemTicks: um a cada 160
// amostras), entregues pela janela e pelo boot probe do mesmo jeito: mix HLE,
// interrupção do DSP e as threads que ela acorda. O probe não entregava
// nenhum: no MK7 a thread de som esperava o DSP para sempre e a principal
// parava na fila de comandos do som, sem nenhum draw.

#include <cstdint>
#include <functional>

namespace Oot3dNativeGame {

class NativeA32CtrHostServices;

// Entrega até frames quadros, um por vez: mixFrame (o mix HLE e o que o
// chamador mede), a interrupção do DSP e runGuest (roda o guest até a próxima
// espera; false para a entrega). Se o DSP recusa o sinal (áudio desligado),
// para depois do mix. Devolve quantos quadros sinalizou.
uint32_t DeliverPendingDspAudioFrames(NativeA32CtrHostServices& host,
                                      uint32_t frames,
                                      const std::function<void()>& mixFrame,
                                      const std::function<bool()>& runGuest);

} // namespace Oot3dNativeGame
