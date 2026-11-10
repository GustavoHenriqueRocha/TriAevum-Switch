#pragma once

// Os sensores de movimento do próprio console como o movimento de controle
// do PollNativeA32Input (oot3d_switch_motion.cpp).
//
// O driver Switch do SDL2 do devkitPro não tem suporte a sensores: no Switch
// o caminho do SDL nunca informa giroscópio. Este módulo lê o sensor de seis
// eixos da libnx do aparelho por trás do pad 0 do SDL (jogador 1 ou handheld)
// e converte para o referencial e as unidades do 3DS que o
// host.ControllerMotion carrega (a saída do ConvertSdlMotion). Tudo o que vem
// depois (calibração, aim.source, sensibilidade, inversão, o anel HID do
// guest) trata como qualquer giroscópio do SDL.
//
// Fontes, por ordem de preferência:
//   - Pro Controller do jogador 1 (FullKey);
//   - par de Joy-Cons do jogador 1: o Joy-Con direito (a mão que mira), senão
//     o esquerdo se só ele estiver conectado;
//   - Handheld (o Switch Lite, ou Joy-Cons presos no console).
// O jogador 1 vem antes para um Pro Controller pareado com um Lite ganhar do
// sensor embutido do Lite. Joy-Con sozinho na horizontal não é suportado: o
// referencial do seu estado de seis eixos processado não é documentado.
//
// A conversão e a média são puras e testadas no host
// (oot3d_switch_motion_tests.cpp); só a parte da libnx é exclusiva do Switch.

#include "three_ds_input.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

// HidSixAxisSensorState como a libnx informa, no referencial do Switch: +X à
// direita, +Y para a borda de cima, +Z para fora da tela. Aceleração em G no
// sentido da gravidade (deitado com a tela para cima e parado lê (0, 0, -1));
// velocidade angular em rotações por segundo, anti-horário positivo
// (1,0 = 360 graus/s).
struct SwitchSixAxisSample {
  std::uint64_t SamplingNumber = 0;
  std::uint64_t DeltaTimeNanoseconds = 0;
  std::array<float, 3> Acceleration{};
  std::array<float, 3> AngularVelocity{};
  bool Connected = false;
};

// Referencial do Switch -> referencial do 3DS (conferido com yuzu, Ryujinx,
// Akira e borealis; igual a Switch -> SDL -> ConvertSdlMotion):
//   acelerômetro [G]       = (-a.x, +a.z, +a.y)
//   giroscópio   [graus/s] = 360 * (-w.x, +w.z, +w.y)
// Os dois são a mesma rotação própria, então uma rotação física fica coerente
// entre os dois sensores. Os timestamps ficam em zero.
[[nodiscard]] ThreeDsRecomp::Input::MotionObservation ConvertSwitchSixAxis(
    const std::array<float, 3>& accelerationG,
    const std::array<float, 3>& angularVelocityRevolutionsPerSecond) noexcept;

// Transforma a LIFO de seis eixos (cerca de 200 Hz, 17 entradas) em uma
// observação por leitura. O host lê uma vez por quadro apresentado e o guest
// consome a entrada só em algumas leituras, enquanto o anel do giroscópio dele
// repete um valor só em todos os ticks de um refresh; a amostra mais recente
// sozinha daria aliasing, então aqui sai a média ponderada pelo tempo de todas
// as amostras desde a última leitura consumida.
class SwitchMotionAccumulator {
 public:
  // Dado do sensor mais velho que isto sai como indisponível (um sensor
  // parado deixa o último estado na LIFO).
  static constexpr std::uint64_t StaleMicroseconds = 200'000;
  // Usado quando a amostra não traz delta de tempo.
  static constexpr std::uint64_t DefaultDeltaNanoseconds = 5'000'000;

  // Esquece todas as amostras; o próximo Observe só anota a entrada mais nova.
  void Reset(std::uint64_t nowMicroseconds) noexcept;

  // `newestFirst` é a LIFO como o hidGetSixAxisSensorStates devolve. Entram
  // as entradas com sampling number acima do último visto, da mais velha para
  // a mais nova. Devolve quantas entraram.
  std::size_t Observe(std::span<const SwitchSixAxisSample> newestFirst,
                      std::uint64_t nowMicroseconds) noexcept;

  // O movimento desta leitura: a média desde a última leitura consumida, ou o
  // último valor informado quando nada chegou desde então. O timestamp só
  // avança com amostra nova, então a calibração conta cada janela uma vez.
  // Inválido quando a amostra mais nova está velha. `consume` abre outra janela.
  [[nodiscard]] ThreeDsRecomp::Input::MotionObservation Take(
      bool consume, std::uint64_t nowMicroseconds) noexcept;

  [[nodiscard]] bool Stale(std::uint64_t nowMicroseconds) const noexcept {
    return nowMicroseconds - mLastSampleMicroseconds > StaleMicroseconds;
  }

 private:
  std::array<double, 3> mAccelerationSum{};
  std::array<double, 3> mAngularVelocitySum{};
  double mSeconds = 0.0;
  std::uint64_t mLastSamplingNumber = 0;
  bool mHasSamplingNumber = false;
  std::uint64_t mLastSampleMicroseconds = 0;
  std::uint64_t mWindowMicroseconds = 0;
  ThreeDsRecomp::Input::MotionObservation mLast;
};

#if defined(__SWITCH__)
struct SwitchMotionPoll {
  ThreeDsRecomp::Input::MotionObservation Motion;
  // O sensor em uso mudou (ou reiniciou): quem chamou zera o estado que
  // supunha uma pose contínua (VirtualMotionState::ResetController).
  bool SourceChanged = false;
};

// Verdadeiro quando o jogador 1 ou o handheld tem um sensor de seis eixos
// suportado conectado. Só lê a memória compartilhada.
[[nodiscard]] bool SwitchMotionSensorConnected() noexcept;

// Só na thread do jogo. `wanted` falso para o sensor (a IMU não transmite
// enquanto nada a usa). Liga e desliga o sensor quando o aparelho muda,
// reinicia na volta do foco ou do sleep e quando os dados param, e lê a LIFO
// a cada chamada. `consume` espelha o guest consumindo a entrada desta leitura.
[[nodiscard]] SwitchMotionPoll PollSwitchMotion(bool wanted, bool consume) noexcept;
#endif

} // namespace Oot3dNativeGame
