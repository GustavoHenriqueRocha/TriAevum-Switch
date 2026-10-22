#include "oot3d_fps_counter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

} // namespace

int main() {
  using Oot3dNativeGame::FpsCounter;
  {
    FpsCounter counter;
    Require(std::strcmp(counter.Text(), "-- FPS") == 0, "texto antes da 1ª janela");
    bool closed = false;
    for (int frame = 0; frame <= 30; ++frame) closed |= counter.Observe(frame / 30.0);
    Require(closed, "janela de 0,5 s fecha a 30 FPS");
    Require(Near(counter.Fps(), 30.0, 0.01), "30 FPS cravados");
    Require(Near(counter.WorstFrameMs(), 33.33, 0.01), "pior quadro 33,3 ms");
    Require(std::strncmp(counter.Text(), "30.0 FPS", 8) == 0, "texto 30.0 FPS");
  }
  {
    // 60 FPS com uma travadinha de 50 ms: a média quase não muda, o pior quadro mostra.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now);
    for (int frame = 0; frame < 40; ++frame) {
      now += frame == 20 ? 0.050 : 1.0 / 60.0;
      counter.Observe(now);
    }
    Require(counter.Fps() > 55.0 && counter.Fps() < 60.0, "média com uma travadinha");
    Require(Near(counter.WorstFrameMs(), 50.0, 0.01), "travadinha de 50 ms aparece");
  }
  {
    // A janela seguinte recomeça o pior quadro.
    FpsCounter counter;
    double now = 0.0;
    counter.Observe(now);
    now += 0.2; counter.Observe(now);
    for (int frame = 0; frame < 60; ++frame) { now += 1.0 / 60.0; counter.Observe(now); }
    Require(Near(counter.WorstFrameMs(), 16.67, 0.01), "pior quadro zera a cada janela");
  }
  std::puts("fps counter: PASS");
  return 0;
}
