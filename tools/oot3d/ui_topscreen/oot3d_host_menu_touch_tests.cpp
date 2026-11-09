// Gesto de toque do menu do host (oot3d_host_menu_touch.h): toques, arrastos e rolagens.
#include "oot3d_host_menu_touch.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

using namespace Oot3dNativeGame;
using Gesture = HostMenuTouchGesture;

void Require(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool Near(float a, float b) { return std::abs(a - b) < 0.01F; }

HostMenuTouchSample Sample(bool pressed, float x, float y, bool hovered) {
  return {pressed, x, y, hovered};
}

bool NoEvents(const HostMenuTouchEvents &events) {
  return !events.MovePointer && !events.Press && !events.Release &&
         !events.ClearPointer && events.ScrollY == 0.0F;
}

// Segura o dedo parado até o hover do ImGui ser lido; retorna esse poll.
HostMenuTouchEvents Settle(Gesture &gesture, float x, float y, bool hovered) {
  for (std::uint8_t poll = 1; poll < Gesture::kHoverSettlePolls; ++poll) {
    Require(NoEvents(gesture.Update(Sample(true, x, y, false))),
            "nothing happens while ImGui's hover is pending");
  }
  return gesture.Update(Sample(true, x, y, hovered));
}

void TestQuickTap() {
  // Levanta no poll seguinte ao pouso, antes da checagem de hover: um clique onde
  // pousou, depois o ponteiro sai.
  Gesture gesture;
  auto events = gesture.Update(Sample(true, 50, 60, false));
  Require(events.MovePointer && Near(events.PointerX, 50) &&
              Near(events.PointerY, 60) && !events.Press,
          "landing only moves the pointer");
  events = gesture.Update(Sample(false, 50, 60, false));
  Require(events.MovePointer && Near(events.PointerX, 50) &&
              Near(events.PointerY, 60) && events.Press && events.Release &&
              events.ClearPointer,
          "a quick tap clicks where it landed");
  Require(gesture.Idle(), "a tap ends the gesture");
  Require(NoEvents(gesture.Update(Sample(false, 0, 0, false))),
          "no finger, no events");
}

void TestTapOnItem() {
  // Uma checkbox: pressionada assim que em hover; o dedo rola um pouco e levanta
  // fora da borda dela, mas o ponteiro ficou no ponto de pressão, então o ImGui vê
  // um clique.
  Gesture gesture;
  (void)gesture.Update(Sample(true, 100, 200, false));
  auto events = Settle(gesture, 104, 197, true);
  Require(events.Press && !events.Release && !events.MovePointer &&
              events.ScrollY == 0,
          "a hovered item is pressed where the pointer is");
  events = gesture.Update(Sample(true, 110, 206, true));
  Require(NoEvents(events), "inside the slop the pointer holds still");
  events = gesture.Update(Sample(false, 110, 206, false));
  Require(events.Release && events.ClearPointer && !events.MovePointer &&
              !events.Press,
          "the lift releases at the press point and clears the pointer");
  Require(gesture.Idle(), "the gesture ends with the finger");
}

void TestTapOnCloseButton() {
  // O X da barra de título do menu de debug em 720p: 17 px. Um dedo pousa 9 px
  // à esquerda e abaixo dele, rola no caminho, e levanta.
  const std::array<HostMenuTouchRect, 1> close{{{1191, 42, 1208, 59}}};
  const auto sample = [&](bool pressed, float x, float y, bool hovered) {
    HostMenuTouchSample s{pressed, x, y, hovered};
    s.SnapTargets = close;
    s.SnapRadius = 24;
    return s;
  };
  Gesture gesture;
  auto events = gesture.Update(sample(true, 1182, 68, false));
  Require(events.MovePointer && Near(events.PointerX, 1199.5F) &&
              Near(events.PointerY, 50.5F),
          "a finger near the close button lands on its centre");
  (void)gesture.Update(sample(true, 1185, 66, false));
  events = gesture.Update(sample(true, 1180, 72, true));
  Require(events.Press && !events.MovePointer, "the close button is pressed");
  events = gesture.Update(sample(false, 1176, 75, false));
  Require(events.Release && !events.MovePointer,
          "released on the close button: it closes the window");

  // Um toque rápido perto dele também o clica.
  events = gesture.Update(sample(true, 1212, 40, false));
  events = gesture.Update(sample(false, 1212, 40, false));
  Require(events.Press && events.Release && Near(events.PointerX, 1199.5F) &&
              Near(events.PointerY, 50.5F),
          "a quick tap near the close button clicks it");

  // Mais longe que o raio, ou um alvo longe do dedo: sem encaixe.
  events = gesture.Update(sample(true, 1150, 50, false));
  Require(Near(events.PointerX, 1150) && Near(events.PointerY, 50),
          "a finger away from the close button stays where it landed");
  (void)gesture.Update(sample(false, 1150, 50, false));

  const std::array<HostMenuTouchRect, 2> two{{{0, 0, 10, 10}, {30, 0, 40, 10}}};
  const auto point = SnapHostMenuTouchPoint(24, 5, two, 20);
  Require(Near(point.X, 35) && Near(point.Y, 5), "the nearest target wins");
  const auto inside = SnapHostMenuTouchPoint(2, 3, two, 0);
  Require(Near(inside.X, 5) && Near(inside.Y, 5),
          "a finger inside a target goes to its centre");
}

void TestScrollDrag() {
  // Espaço vazio: nada até o dedo sair da folga, depois o conteúdo o segue
  // 1:1 (incluindo o movimento feito dentro da folga), mesmo sobre itens,
  // e o levantar não clica em nada.
  Gesture gesture;
  (void)gesture.Update(Sample(true, 300, 400, false));
  auto events = Settle(gesture, 300, 392, false);
  Require(NoEvents(events), "a small motion on empty space waits");
  events = gesture.Update(Sample(true, 300, 380, false));
  Require(!events.Press && Near(events.ScrollY, -20) && events.MovePointer &&
              Near(events.PointerY, 380),
          "past the slop it scrolls by the motion since landing");
  events = gesture.Update(Sample(true, 302, 330, true));
  Require(!events.Press && Near(events.ScrollY, -50),
          "scrolling follows the finger, even over items");
  events = gesture.Update(Sample(true, 302, 330, true));
  Require(events.ScrollY == 0 && !events.MovePointer, "a still finger rests");
  events = gesture.Update(Sample(false, 302, 330, false));
  Require(!events.Press && !events.Release && events.ClearPointer,
          "a scroll ends without a click");

  // Já além da folga quando o hover é lido: rola imediatamente.
  (void)gesture.Update(Sample(true, 300, 400, false));
  events = Settle(gesture, 300, 440, false);
  Require(Near(events.ScrollY, 40), "a fast drag scrolls at once");
  (void)gesture.Update(Sample(false, 300, 440, false));

  // Um toque em espaço vazio ainda é um clique (fecha um popup).
  (void)gesture.Update(Sample(true, 500, 500, false));
  (void)Settle(gesture, 503, 498, false);
  events = gesture.Update(Sample(false, 506, 495, false));
  Require(events.MovePointer && Near(events.PointerX, 500) && events.Press &&
              events.Release && events.ClearPointer,
          "a tap on empty space clicks where it landed");
}

void TestItemDrag() {
  // Um slider: pressionado, depois o ponteiro segue assim que passa da folga.
  Gesture gesture;
  (void)gesture.Update(Sample(true, 100, 200, false));
  auto events = Settle(gesture, 100, 200, true);
  Require(events.Press, "the slider is pressed");
  events = gesture.Update(Sample(true, 160, 205, true));
  Require(events.MovePointer && Near(events.PointerX, 160) && !events.Press &&
              events.ScrollY == 0,
          "dragging a pressed item moves the pointer");
  events = gesture.Update(Sample(true, 108, 205, true));
  Require(events.MovePointer && Near(events.PointerX, 108),
          "once dragging, it follows inside the slop too");
  events = gesture.Update(Sample(false, 108, 205, false));
  Require(events.Release && events.ClearPointer, "lifting releases it");
}

} // namespace

int main() {
  TestQuickTap();
  TestTapOnItem();
  TestTapOnCloseButton();
  TestScrollDrag();
  TestItemDrag();
  std::cout << "oot3d_host_menu_touch_tests: ok\n";
  return 0;
}
