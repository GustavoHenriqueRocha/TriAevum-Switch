#pragma once

// O dedo do menu do host (L3+R3 ou outra janela ImGui) como entrada ImGui na
// tela sensível do Switch, que não tem backend de plataforma ImGui no deko3d.
//
// O ImGui recebe um mouse de tela sensível. O ponteiro vai para onde o dedo
// pousou e, dois polls depois (quando o ImGui já desenhou um quadro com ele,
// para que seu teste de hover esteja atual), o gesto ou pressiona ali (há um
// item sob o dedo: botões, checkboxes, abas, itens de combo, sliders,
// barras de rolagem) ou trata o dedo como um arrasto da janela sob ele.
//
// Um toque é um dedo que levanta antes de se mover mais que a folga de toque.
// Um toque sempre clica onde pousou, por mais curto que seja e o que quer que
// estivesse em hover: o ponteiro pressionado fica parado até o dedo sair da folga,
// então o dedo rolando ao levantar não consegue levar a soltura para fora de um
// botão pequeno (o que cancelaria o clique). Além da folga, um item pressionado é
// arrastado (sliders, barras de rolagem) e o espaço vazio rola com o dedo.
//
// Um dedo que pousa perto de um alvo de encaixe (os botões de fechar da barra de
// título: o ImGui os desenha com a largura de uma altura de fonte, menos de 2 mm
// num Switch Lite) pousa no centro dele. Quando o dedo levanta o ponteiro sai da
// tela, então nenhum estado de hover ou pressionado fica para a navegação por controle.

#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

// Coordenadas de exibição do ImGui.
struct HostMenuTouchRect {
  float MinX = 0.0F;
  float MinY = 0.0F;
  float MaxX = 0.0F;
  float MaxY = 0.0F;
};

struct HostMenuTouchPoint {
  float X = 0.0F;
  float Y = 0.0F;
};

// O centro do alvo mais próximo de (x, y) dentro do raio (0 dentro dele),
// senão (x, y). Empates vão para o alvo anterior.
[[nodiscard]] HostMenuTouchPoint
SnapHostMenuTouchPoint(float x, float y,
                       std::span<const HostMenuTouchRect> targets,
                       float radius) noexcept;

struct HostMenuTouchSample {
  bool Pressed = false;
  // Coordenadas de exibição do ImGui.
  float X = 0.0F;
  float Y = 0.0F;
  // ImGui::IsAnyItemHovered() para o ponteiro enviado até agora.
  bool ItemHovered = false;
  // Pixels de exibição que um dedo pode mover e ainda tocar.
  float TapSlop = 16.0F;
  // Lido quando o dedo pousa.
  std::span<const HostMenuTouchRect> SnapTargets{};
  float SnapRadius = 0.0F;
};

// Aplicado nesta ordem: ponteiro, pressionar, soltar, limpar, rolar.
struct HostMenuTouchEvents {
  bool MovePointer = false;
  float PointerX = 0.0F;
  float PointerY = 0.0F;
  bool Press = false;
  bool Release = false;
  // Ponteiro fora da tela (AddMousePosEvent(-FLT_MAX, -FLT_MAX)).
  bool ClearPointer = false;
  // Pixels de exibição que o conteúdo sob o dedo segue (para baixo positivo).
  float ScrollY = 0.0F;
};

class HostMenuTouchGesture {
public:
  // Polls entre enviar o ponteiro e ler o hover do ImGui para ele.
  static constexpr std::uint8_t kHoverSettlePolls = 2;

  [[nodiscard]] HostMenuTouchEvents
  Update(const HostMenuTouchSample &sample) noexcept;
  [[nodiscard]] bool Idle() const noexcept { return mPhase == Phase::Idle; }

private:
  enum class Phase : std::uint8_t { Idle, Settling, Pressing, Panning };
  Phase mPhase = Phase::Idle;
  std::uint8_t mSettle = 0;
  // O dedo saiu da folga de toque (permanece até levantar).
  bool mMoved = false;
  // Onde o dedo pousou, e para onde o ponteiro foi (encaixado).
  float mDownX = 0.0F;
  float mDownY = 0.0F;
  float mPointX = 0.0F;
  float mPointY = 0.0F;
  // A última posição do dedo enviada (movimentos do ponteiro, rolagem).
  float mLastX = 0.0F;
  float mLastY = 0.0F;
};

} // namespace Oot3dNativeGame
