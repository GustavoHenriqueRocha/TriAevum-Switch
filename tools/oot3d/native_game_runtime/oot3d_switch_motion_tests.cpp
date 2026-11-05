// Seis eixos do Switch -> movimento do 3DS (oot3d_switch_motion.h): a
// conversão de eixos contra rotações conhecidas e contra o caminho do SDL que
// já existe, e a média da LIFO de seis eixos em uma observação por leitura.
#include "oot3d_switch_motion.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
using ThreeDsRecomp::Input::MotionObservation;
using Vector = std::array<float, 3>;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "oot3d_switch_motion_tests: %s\n", message);
    std::exit(1);
  }
}

bool Near(float a, float b, float tolerance = 1e-3F) { return std::abs(a - b) <= tolerance; }

bool Near(const Vector& a, const Vector& b, float tolerance = 1e-3F) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

Vector Cross(const Vector& a, const Vector& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector Scale(const Vector& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

// Referencial do Switch: +X à direita, +Y para a borda de cima, +Z para fora
// da tela. A gravidade como o sensor informa, em alguns jeitos de segurar.
constexpr Vector kFaceUp{0.0F, 0.0F, -1.0F};
constexpr Vector kUpright{0.0F, -1.0F, 0.0F};  // tela virada para o jogador
const Vector kTilted{0.0F, -std::sqrt(0.5F), -std::sqrt(0.5F)};  // no meio do caminho

void TestRestPoses() {
  const auto flat = ConvertSwitchSixAxis(kFaceUp, {});
  Require(flat.AccelerometerValid && flat.GyroscopeValid, "a conversão não marcou os dois sensores como válidos");
  // A pose neutra do fork (MappingConfig::AccelerometerNeutral) é um 3DS
  // deitado com a tela para cima.
  Require(Near(flat.Accelerometer, {0.0F, -1.0F, 0.0F}), "tela para cima não leu a pose neutra do 3DS");
  const auto upright = ConvertSwitchSixAxis(kUpright, {});
  Require(Near(upright.Accelerometer, {0.0F, 0.0F, -1.0F}), "em pé não leu a gravidade em -Z");
  const auto right = ConvertSwitchSixAxis({1.0F, 0.0F, 0.0F}, {});
  Require(Near(right.Accelerometer, {-1.0F, 0.0F, 0.0F}), "+X do Switch não virou -X do 3DS");
  Require(Near(ConvertSwitchSixAxis({}, {1.0F, 0.0F, 0.0F}).GyroscopeDegreesPerSecond, {-360.0F, 0.0F, 0.0F}),
          "uma rotação por segundo não deu 360 graus/s");
}

void TestMatchesSdlPath() {
  // libnx -> SDL (yuzu, Ryujinx): accel_sdl = g * (-a.x, -a.z, a.y),
  // gyro_sdl = 2pi * (w.x, w.z, -w.y); depois o ConvertSdlMotion do fork.
  constexpr float kGravity = 9.80665F;
  constexpr float kTwoPi = 6.28318530717958647692F;
  const std::array<std::pair<Vector, Vector>, 4> cases{{
      {{0.1F, -0.2F, -0.97F}, {0.3F, -0.05F, 0.12F}},
      {kUpright, {0.0F, 0.25F, 0.0F}},
      {{0.5F, 0.5F, -0.7F}, {-0.4F, 0.6F, -0.2F}},
      {{-0.9F, 0.1F, 0.4F}, {0.01F, -0.02F, 0.9F}},
  }};
  for (const auto& [a, w] : cases) {
    const auto direct = ConvertSwitchSixAxis(a, w);
    const auto viaSdl = ThreeDsRecomp::Input::ConvertSdlMotion(
        {-a[0] * kGravity, -a[2] * kGravity, a[1] * kGravity}, true,
        {w[0] * kTwoPi, w[2] * kTwoPi, -w[1] * kTwoPi}, true);
    Require(Near(direct.Accelerometer, viaSdl.Accelerometer), "acelerômetro difere do caminho do SDL");
    Require(Near(direct.GyroscopeDegreesPerSecond, viaSdl.GyroscopeDegreesPerSecond, 1e-2F),
            "giroscópio difere do caminho do SDL");
  }
}

void TestRotationIsCoherent() {
  // Um vetor fixo no mundo, visto de um corpo girando a w, muda como -w x v.
  // Acelerômetro e giroscópio têm de passar pela mesma rotação própria, senão
  // a fusão do guest vê a gravidade andar contra o giroscópio.
  const std::array<Vector, 3> rates{{{0.2F, 0.0F, 0.0F}, {0.0F, -0.3F, 0.1F}, {0.05F, 0.4F, -0.2F}}};
  for (const auto& gravity : {kFaceUp, kUpright, kTilted}) {
    for (const auto& w : rates) {
      const auto converted = ConvertSwitchSixAxis(gravity, w);
      const auto changeInSwitch = Cross(w, gravity);
      const auto expected = ConvertSwitchSixAxis(changeInSwitch, {}).Accelerometer;
      const auto actual = Cross(Scale(converted.GyroscopeDegreesPerSecond, 1.0F / 360.0F), converted.Accelerometer);
      Require(Near(expected, actual), "giroscópio e acelerômetro não são a mesma rotação");
    }
  }
}

ThreeDsRecomp::Input::HidState Resolve(ThreeDsRecomp::Input::MotionSource source, const MotionObservation& motion,
                                       std::int32_t mouseX = 0, std::int32_t mouseY = 0, bool invertY = false) {
  ThreeDsRecomp::Input::MappingConfig config;
  config.NativeMotionSource = source;
  config.NativeMotionInvertY = invertY;
  ThreeDsRecomp::Input::PhysicalInputState input;
  input.ControllerMotion = motion;
  input.MouseDeltaX = mouseX;
  input.MouseDeltaY = mouseY;
  return ThreeDsRecomp::Input::ResolveInput(config, input, {}).Hid;
}

void TestAimFollowsTheConsole() {
  using ThreeDsRecomp::Input::MotionSource;
  // Levantar a traseira do console (-Z) é um giro positivo em torno de +X.
  // Tem de chegar ao guest com o sinal do mouse subindo.
  const auto tiltUp = ConvertSwitchSixAxis(kUpright, {0.1F, 0.0F, 0.0F});
  const float gyroUp = Resolve(MotionSource::ControllerGyroscope, tiltUp).GyroscopeDegreesPerSecond[0];
  const float mouseUp = Resolve(MotionSource::Mouse, {}, 0, -10).GyroscopeDegreesPerSecond[0];
  Require(gyroUp < 0.0F && mouseUp < 0.0F, "inclinar o console para cima não mira como o mouse subindo");
  Require(Near(gyroUp, -36.0F), "0,1 rot/s não chegou ao guest como 36 graus/s");
  // invert_y (aim.invert_y na configuração) inverte.
  Require(Resolve(MotionSource::ControllerGyroscope, tiltUp, 0, 0, true).GyroscopeDegreesPerSecond[0] > 0.0F,
          "inverter Y não inverteu o pitch do console");

  // Virar para a esquerda deitado é um giro positivo em torno da normal da
  // tela (+Z); o mouse indo para a esquerda gira igual na pose virtual deitada.
  const auto flatLeft = ConvertSwitchSixAxis(kFaceUp, {0.0F, 0.0F, 0.1F});
  const float gyroLeft = Resolve(MotionSource::ControllerGyroscope, flatLeft).GyroscopeDegreesPerSecond[1];
  const float mouseLeft = Resolve(MotionSource::Mouse, {}, -10, 0).GyroscopeDegreesPerSecond[1];
  Require(gyroLeft > 0.0F && mouseLeft > 0.0F, "virar para a esquerda não mira como o mouse indo para a esquerda");

  // Virar para a esquerda em torno do "para cima" do mundo, segurando o
  // console de qualquer jeito, vira o 3DS para a esquerda em torno do próprio
  // "para cima" (menos a gravidade).
  for (const auto& gravity : {kFaceUp, kUpright, kTilted}) {
    const auto left = ConvertSwitchSixAxis(gravity, Scale(gravity, -0.1F));
    Require(Dot(left.GyroscopeDegreesPerSecond, Scale(left.Accelerometer, -1.0F)) > 0.0F,
            "virar para a esquerda em torno do para cima do mundo não chegou ao 3DS como giro para a esquerda");
  }
  // Em pé, esse giro é em torno do eixo longo do 3DS (Z), como o próprio
  // movimento virtual do fork compõe o yaw inclinado (ResolveMotion).
  const auto uprightLeft = ConvertSwitchSixAxis(kUpright, {0.0F, 0.1F, 0.0F});
  Require(Near(uprightLeft.GyroscopeDegreesPerSecond, {0.0F, 0.0F, 36.0F}), "o yaw em pé não é em torno do +Z do 3DS");
}

SwitchSixAxisSample Sample(std::uint64_t number, Vector angularVelocity, std::uint64_t deltaNanoseconds = 5'000'000,
                           Vector acceleration = kFaceUp, bool connected = true) {
  return {number, deltaNanoseconds, acceleration, angularVelocity, connected};
}

void TestAccumulator() {
  SwitchMotionAccumulator accumulator;
  std::uint64_t now = 1'000'000;
  accumulator.Reset(now);
  // Depois de ligar, a LIFO pode ter o estado do sensor parado: só a posição.
  std::vector<SwitchSixAxisSample> lifo{Sample(10, {1.0F, 0.0F, 0.0F})};
  Require(accumulator.Observe(lifo, now) == 0, "a primeira leitura depois de ligar entrou na média");
  Require(!accumulator.Take(false, now).GyroscopeValid, "movimento informado antes de qualquer amostra nova");

  // Mais nova primeiro; 11 com 5 ms e 12 com 15 ms: média ponderada pelo tempo.
  now += 20'000;
  lifo = {Sample(12, {0.0F, 0.0F, 0.2F}, 15'000'000), Sample(11, {0.0F, 0.0F, 0.4F}), Sample(10, {1.0F, 0.0F, 0.0F})};
  Require(accumulator.Observe(lifo, now) == 2, "amostras novas não entraram exatamente uma vez");
  auto motion = accumulator.Take(false, now);
  Require(motion.GyroscopeValid && motion.AccelerometerValid, "movimento médio não é válido");
  Require(Near(motion.GyroscopeDegreesPerSecond[1], 360.0F * 0.25F), "a média não é ponderada pelo delta de tempo");
  Require(motion.GyroscopeTimestampMicroseconds == now && motion.AccelerometerTimestampMicroseconds == now,
          "os timestamps não seguem as amostras mais novas");
  const auto firstStamp = motion.GyroscopeTimestampMicroseconds;

  // Nada novo: mesmo valor e timestamp (a calibração conta uma vez).
  now += 2'000;
  Require(accumulator.Observe(lifo, now) == 0, "amostras já vistas entraram na média de novo");
  motion = accumulator.Take(true, now);
  Require(motion.GyroscopeTimestampMicroseconds == firstStamp && Near(motion.GyroscopeDegreesPerSecond[1], 90.0F),
          "uma leitura sem amostra nova mudou a observação");
  // Consumida: a janela seguinte só tem amostras novas; entradas
  // desconectadas e o delta de tempo padrão.
  now += 10'000;
  lifo.insert(lifo.begin(), {Sample(14, {0.0F, 0.0F, 0.6F}, 0), Sample(13, {0.0F, 0.0F, 5.0F}, 5'000'000, kFaceUp, false)});
  Require(accumulator.Observe(lifo, now) == 1, "amostra desconectada entrou na média");
  motion = accumulator.Take(true, now);
  Require(Near(motion.GyroscopeDegreesPerSecond[1], 216.0F) && motion.GyroscopeTimestampMicroseconds > firstStamp,
          "a janela consumida vazou para a seguinte");

  // Um sensor parado deixa o último estado: velho depois de 200 ms.
  now += SwitchMotionAccumulator::StaleMicroseconds + 1;
  Require(accumulator.Observe(lifo, now) == 0 && accumulator.Stale(now), "sensor parado não detectado");
  Require(!accumulator.Take(false, now).GyroscopeValid, "movimento velho saiu como válido");

  // Um contador que voltou (sensor reiniciado) só zera a posição.
  lifo = {Sample(3, {0.0F, 0.0F, 1.0F})};
  Require(accumulator.Observe(lifo, now) == 0, "contador reiniciado repetiu amostras antigas");
  now += 5'000;
  lifo.insert(lifo.begin(), Sample(4, {0.0F, 0.0F, 0.1F}));
  Require(accumulator.Observe(lifo, now) == 1 && Near(accumulator.Take(false, now).GyroscopeDegreesPerSecond[1], 36.0F),
          "amostras depois do contador reiniciar se perderam");
}

void TestCalibrationSeesFreshSamples() {
  // A calibração do fork pula observações cujo timestamp não andou.
  SwitchMotionAccumulator accumulator;
  ThreeDsRecomp::Input::MotionCalibrationAccumulator calibration;
  std::uint64_t now = 5'000'000;
  accumulator.Reset(now);
  std::uint64_t number = 100;
  std::vector<SwitchSixAxisSample> lifo{Sample(number, {0.001F, 0.0F, 0.0F})};
  accumulator.Observe(lifo, now);
  bool done = false;
  for (int poll = 0; poll < 200 && !done; ++poll) {
    now += 16'000;
    if (poll % 2 == 0) {  // uma leitura sim, outra não, traz amostras novas
      lifo = {Sample(number + 3, {0.001F, 0.0F, 0.0F}), Sample(number + 2, {0.001F, 0.0F, 0.0F}),
              Sample(number + 1, {0.001F, 0.0F, 0.0F})};
      number += 3;
    }
    accumulator.Observe(lifo, now);
    done = calibration.Observe(accumulator.Take(true, now));
    Require(calibration.SamplesCollected() <= static_cast<std::uint32_t>(poll / 2 + 1),
            "a calibração contou uma observação repetida");
  }
  Require(done, "a calibração nunca terminou com o sensor do Switch");
  const auto mean = calibration.Mean();
  Require(Near(mean.GyroscopeDegreesPerSecond[0], -0.36F) && Near(mean.Accelerometer, {0.0F, -1.0F, 0.0F}),
          "a média da calibração não é o viés e a pose convertidos");
}

} // namespace

int main() {
  TestRestPoses();
  TestMatchesSdlPath();
  TestRotationIsCoherent();
  TestAimFollowsTheConsole();
  TestAccumulator();
  TestCalibrationSeesFreshSamples();
  std::puts("oot3d_switch_motion_tests: ok");
  return 0;
}
