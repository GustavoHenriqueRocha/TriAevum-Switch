#pragma once

// Clocks atuais do console para logs/perf.txt (oot3d_switch_clocks.cpp).
//
// Somente leitura: os MHz vêm do IPC público do sys-clk ("sys:clk",
// GetCurrentContext), o modo portátil/dock e o modo de desempenho vêm do
// applet (valores que a libnx já guarda, sem IPC). O app nunca abre clkrst
// nem pcv e nunca altera clock (SwitchWakerHD issue #33: clkrst no app
// travou o console ao acordar do sleep; quem muda clock é o sys-clk).
//
// Fora do Switch tudo aqui é vazio.

#include <string>

namespace Oot3dNativeGame {

#if defined(__SWITCH__)
// " clocks: cpu_mhz=1785 gpu_mhz=768 mem_mhz=1600 clk_src=sysclk_real
// op_mode=handheld perf_mode=normal sysclk_profile=handheld sysclk_enabled=1
// sysclk_api=4 clk_ipc_us=40" para a linha da janela. Uma chamada IPC por
// chamada desta função (uma por janela de 150 quadros); a sessão com o
// sys-clk é aberta na primeira vez e mantida. Campo que não deu para ler
// fica -1 (clk_src=none quando o sys-clk não está rodando).
std::string SwitchClockPerfSummary();
#else
inline std::string SwitchClockPerfSummary() { return {}; }
#endif

} // namespace Oot3dNativeGame
