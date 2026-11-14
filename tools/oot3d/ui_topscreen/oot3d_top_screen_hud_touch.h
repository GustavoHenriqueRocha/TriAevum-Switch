#pragma once

// Toques na tela sensível no HUD de jogo do TopScreen (Switch portátil).
//
// O 3DS deixa o jogador tocar nos botões de item da tela de baixo; o TopScreen
// leva esses controles para o HUD da tela de cima, que o host redesenha numa
// tela de 400x240. Um toque num dos controles desenhados produz exatamente o
// que o controle físico que ele representa produz (X, Y, Item I = ZR,
// Item II = ZL, B, ou uma direção do D-pad para os ícones de ação em volta da
// cruz), então cada consumidor existente decide o que acontece.
//
// Cada dedo ganha um dono quando pousa e o mantém até levantar: o toque
// nativo de página (título, seleção de arquivo, páginas de pausa), um controle
// do HUD, a mira (mira por arrasto, quando ativada: o primeiro dedo que pousa
// fora de todos os controles), o menu do host (L3+R3 ou outra janela ImGui: o
// primeiro dedo que pousa enquanto ele está aberto), ou nada. Um dedo que pousa
// fora de um controle nunca o pressiona, mesmo que deslize sobre ele, um dedo
// do HUD nunca mira, e um dedo do menu nunca chega ao jogo.

#include "oot3d_ui/ui_primitives.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Oot3dNativeGame {

enum class TopScreenHudTouchControl : std::uint8_t {
  None,
  X,
  Y,
  ItemI,
  ItemII,
  B,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
};

// Os bits de pad do 3DS (ThreeDsRecomp::Input::Button) do controle físico que
// um controle do HUD representa. ZR e ZL são os bits extras que o TopScreen lê
// como Item I e Item II.
[[nodiscard]] std::uint32_t
TopScreenHudTouchButtons(TopScreenHudTouchControl control) noexcept;

// Um retângulo da tela de 400x240 do TopScreen em pixels da janela, pelo
// viewport em que o renderizador de UI do N64 encaixa a tela
// (FitUiPresentationViewport).
[[nodiscard]] oot3d::ui::UiRect
MapTopScreenCanvasRectToWindow(const oot3d::ui::UiRect &canvas,
                               std::uint32_t windowWidth,
                               std::uint32_t windowHeight) noexcept;

// Áreas de acerto dos controles do HUD desenhados, em pixels da janela.
struct TopScreenHudTouchTargets {
  // X, Y, Item I, Item II e B: os círculos do touch cluster (quads de origem 0-4).
  std::array<oot3d::ui::UiRect, 5> Buttons{};
  std::array<bool, 5> ButtonVisible{};
  // A cruz junto com os ícones de ação em volta dela.
  oot3d::ui::UiRect Dpad{};
  float DpadCenterX = 0.0F;
  float DpadCenterY = 0.0F;
  // Toques mais perto do centro que isto não escolhem direção.
  float DpadDeadZone = 0.0F;
  bool DpadVisible = false;
};

// Lê os controles das primitivas do HUD de jogo que o host desenhou por último
// (lastTopScreenPrimitivesBySubsystem[GameplayHud], após escala do HUD, margens
// e recorte). Controles ocultos ou esmaecidos ficam de fora.
[[nodiscard]] TopScreenHudTouchTargets ResolveTopScreenHudTouchTargets(
    std::span<const oot3d::ui::UiPrimitive> hud, std::uint32_t windowWidth,
    std::uint32_t windowHeight) noexcept;

// O controle sob um ponto da janela, ou None.
[[nodiscard]] TopScreenHudTouchControl
HitTestTopScreenHudTouch(const TopScreenHudTouchTargets &targets, float windowX,
                         float windowY) noexcept;

enum class TopScreenTouchOwner : std::uint8_t {
  Unassigned,
  Page,
  Hud,
  Aim,
  Menu,
  Ignored,
};

// Um contato de uma amostra da tela sensível. X e Y são normalizados ao painel
// (0..1). `End` marca a última amostra de um contato.
struct TopScreenTouchContact {
  std::uint32_t FingerId = 0;
  float X = 0.0F;
  float Y = 0.0F;
  bool End = false;
};

struct TopScreenTouchFinger {
  bool Active = false;
  bool Held = false;
  std::uint32_t Id = 0;
  // Polls consumidores que o pressionamento ainda deve ao guest depois que o
  // dedo levantou, para que um toque mais curto que um poll ainda seja visto.
  std::uint8_t PendingPolls = 0;
  float DownX = 0.0F;
  float DownY = 0.0F;
  float X = 0.0F;
  float Y = 0.0F;
  // Dedos de mira: a posição já entregue como movimento de mira.
  float AimX = 0.0F;
  float AimY = 0.0F;
  TopScreenTouchOwner Owner = TopScreenTouchOwner::Unassigned;
  TopScreenHudTouchControl Control = TopScreenHudTouchControl::None;
};

// Dedos rastreados entre amostras da tela sensível. Recebe cada nova amostra em
// ordem, então um toque que pousa e levanta entre dois polls é retido, não perdido.
class TopScreenTouchTracker {
public:
  static constexpr std::size_t kMaxFingers = 16;

  // Uma amostra: todos os contatos no painel naquele instante. Um dedo pressionado
  // ausente da amostra levantou.
  void ObserveSample(std::span<const TopScreenTouchContact> contacts) noexcept;
  void Reset() noexcept { mFingers = {}; }
  [[nodiscard]] std::span<TopScreenTouchFinger> Fingers() noexcept {
    return mFingers;
  }

private:
  std::array<TopScreenTouchFinger, kMaxFingers> mFingers{};
};

struct TopScreenTouchPollContext {
  // Uma página de toque nativa está aberta (título, seleção de arquivo, pausa).
  bool PageTouchEnabled = false;
  // A UI do host (ImGui, o menu de debug) é dona da entrada.
  bool HostUiVisible = false;
  // Vazio fora do jogo no TopScreen.
  const TopScreenHudTouchTargets *Targets = nullptr;
  std::uint32_t WindowWidth = 0;
  std::uint32_t WindowHeight = 0;
  // Mira por arrasto (aim.touch_enabled).
  bool AimEnabled = false;
};

// O painel de toque em pixels, a unidade do movimento de mira (e de
// aim.touch_degrees_per_pixel), qualquer que seja o tamanho da janela.
inline constexpr float kTopScreenTouchPanelWidth = 1280.0F;
inline constexpr float kTopScreenTouchPanelHeight = 720.0F;

struct TopScreenTouchPollResult {
  // O ponteiro da página: o primeiro dedo da página, como antes (um ponteiro).
  bool PagePressed = false;
  float PageX = 0.0F;
  float PageY = 0.0F;
  // Bits de pad de cada controle do HUD segurado por um dedo, fora de páginas e UI do host.
  std::uint32_t HudButtons = 0;
  // Movimento do dedo de mira desde o último poll consumidor, em pixels do
  // painel; só em polls consumidores, fora de páginas e UI do host.
  float AimDeltaX = 0.0F;
  float AimDeltaY = 0.0F;
  // O ponteiro do menu do host (normalizado), enquanto a UI do host está visível.
  bool MenuPressed = false;
  float MenuX = 0.0F;
  float MenuY = 0.0F;
};

// Um pressionamento do HUD dura pelo menos esta quantidade de polls consumidores
// (atualizações do guest): o jogo lê alguns botões a 30 Hz.
inline constexpr std::uint8_t kTopScreenHudMinimumPressPolls = 2;

// Dá aos dedos novos seu dono, informa o que os dedos seguram neste poll e,
// num poll que uma atualização do guest consome, retira pressionamentos já entregues.
[[nodiscard]] TopScreenTouchPollResult
ResolveTopScreenTouchPoll(TopScreenTouchTracker &tracker,
                          const TopScreenTouchPollContext &context,
                          bool guestRefreshWillConsume) noexcept;

} // namespace Oot3dNativeGame
