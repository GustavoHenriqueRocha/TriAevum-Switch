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
#include <string_view>

namespace Oot3dNativeGame {

// config/fps60_clock: abaixo destes clocks o 60 do config/fps60 nem é
// tentado e a cadência fica travada em 30 (NativeCadenceGovernor::
// SetClockThirty) até os clocks voltarem. Sem overclock (CPU 1020) a thread
// do jogo sozinha já gasta o par inteiro; o overclock usado é 1785/614.
// Texto: "cpu=1500 gpu=550" (MHz) ou "off". Sem o arquivo: os padrões.
struct Fps60ClockGate {
  bool Enabled = true;
  int CpuMinMhz = 1500;
  int GpuMinMhz = 550;
};

[[nodiscard]] inline Fps60ClockGate ParseFps60ClockGate(std::string_view text) noexcept {
  Fps60ClockGate gate;
  size_t position = 0U;
  while (position < text.size()) {
    while (position < text.size() && (text[position] == ' ' || text[position] == '\t' ||
                                      text[position] == '\n' || text[position] == '\r'))
      ++position;
    size_t end = position;
    while (end < text.size() && text[end] != ' ' && text[end] != '\t' && text[end] != '\n' &&
           text[end] != '\r')
      ++end;
    const std::string_view word = text.substr(position, end - position);
    position = end;
    if (word == "off" || word == "0") {
      gate.Enabled = false;
      continue;
    }
    const size_t equals = word.find('=');
    if (equals == std::string_view::npos)
      continue;
    int value = 0;
    bool digits = !word.substr(equals + 1U).empty();
    for (const char c : word.substr(equals + 1U)) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
      value = value * 10 + (c - '0');
    }
    if (!digits)
      continue;
    if (word.substr(0U, equals) == "cpu")
      gate.CpuMinMhz = value;
    else if (word.substr(0U, equals) == "gpu")
      gate.GpuMinMhz = value;
  }
  return gate;
}

// MHz desconhecido (-1: sem sys-clk, leitura falhou) nunca trava: sem como
// saber o clock, fica o comportamento de antes.
[[nodiscard]] inline bool ClocksForceThirty(const Fps60ClockGate &gate, int cpuMhz,
                                            int gpuMhz) noexcept {
  if (!gate.Enabled)
    return false;
  return (cpuMhz > 0 && cpuMhz < gate.CpuMinMhz) || (gpuMhz > 0 && gpuMhz < gate.GpuMinMhz);
}

#if defined(__SWITCH__)
// " clocks: cpu_mhz=1785 gpu_mhz=768 mem_mhz=1600 clk_src=sysclk_real
// op_mode=handheld perf_mode=normal sysclk_profile=handheld sysclk_enabled=1
// sysclk_api=4 clk_ipc_us=40 clk_busy=0" para a linha da janela. Devolve a
// leitura anterior e pede a próxima a uma thread própria (o IPC nunca roda na
// thread do jogo; clk_busy=1 se a anterior ainda não voltou); a sessão com o
// sys-clk é aberta na primeira vez e mantida. Campo que não deu para ler
// fica -1 (clk_src=none quando o sys-clk não está rodando).
std::string SwitchClockPerfSummary();
// Últimos MHz lidos (-1 se desconhecido). Pede uma leitura nova à mesma
// thread no máximo a cada 2 s; nunca espera o sys-clk.
void SwitchClocksMhz(int &cpuMhz, int &gpuMhz);
#else
inline std::string SwitchClockPerfSummary() { return {}; }
inline void SwitchClocksMhz(int &cpuMhz, int &gpuMhz) { cpuMhz = gpuMhz = -1; }
#endif

} // namespace Oot3dNativeGame
