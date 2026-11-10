#pragma once

// Quadros de áudio do DSP que o host já contou (AdvanceSystemTicks: um a cada
// 160 amostras), entregues como a janela faz em DeliverDspAudioFrames: mix HLE,
// interrupção do DSP e as threads que ela acorda. O boot probe não entregava
// nenhum: no MK7 a thread de som esperava o DSP para sempre e a principal
// parava na fila de comandos do som, sem nenhum draw.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Oot3dNativeGame {

class NativeA32CtrHostServices;
class NativeA32DspHle;
class NativeA32Memory;

// runGuest roda o guest até a próxima espera; false para a entrega (processo
// falhou ou terminou). Devolve quantos quadros entregou, ou nullopt se o mix
// HLE falhou (motivo em error).
std::optional<uint32_t> DeliverPendingDspAudioFrames(
    NativeA32CtrHostServices& host, NativeA32DspHle& dspHle,
    NativeA32Memory& memory, const std::function<bool()>& runGuest,
    std::string* error);

} // namespace Oot3dNativeGame
