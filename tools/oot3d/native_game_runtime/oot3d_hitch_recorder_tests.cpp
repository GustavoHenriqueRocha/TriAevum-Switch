#include "oot3d_hitch_recorder.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

} // namespace

int main() {
  using namespace Oot3dNativeGame;
  constexpr double kTarget = 1000.0 / 30.0;
  {
    // 30 FPS cravados: nenhum engasgo.
    HitchRecorder recorder;
    HitchSample sample;
    for (int frame = 0; frame < 300; ++frame)
      Require(!recorder.Observe(frame / 30.0, frame, kTarget, sample), "30 FPS sem engasgo");
    Require(recorder.Total() == 0U, "total zero");
  }
  {
    // Um quadro de 55 ms com shader novo, um de 70 ms com rajada de texturas,
    // um de 60 ms com a lógica estourada e um de 55 ms sem causa no jogo.
    HitchRecorder recorder;
    HitchSample sample;
    double now = 0.0;
    recorder.Observe(now, 0, kTarget, sample);
    now += 0.055; sample.ProgramsLive += 1;
    auto line = recorder.Observe(now, 1, kTarget, sample);
    Require(line && line->find("cause=shader") != std::string::npos, "shader");
    Require(line->find("prog_new=1") != std::string::npos, "prog_new");
    now += 0.070; sample.TexturesCreated += 40;
    line = recorder.Observe(now, 2, kTarget, sample);
    Require(line && line->find("cause=texturas") != std::string::npos, "texturas");
    Require(line->find("tex_new=40") != std::string::npos, "tex_new");
    now += 0.060; sample.GameBusyMs = 45.0;
    line = recorder.Observe(now, 3, kTarget, sample);
    Require(line && line->find("cause=logica") != std::string::npos, "lógica");
    now += 0.055; sample.GameBusyMs = 10.0;
    line = recorder.Observe(now, 4, kTarget, sample);
    Require(line && line->find("cause=render") != std::string::npos, "render");
    // 40 ms (1,2× o alvo) ainda não é engasgo; poucas texturas não são rajada.
    now += 0.040; sample.TexturesCreated += 3;
    Require(!recorder.Observe(now, 5, kTarget, sample), "abaixo do limiar");
    Require(recorder.Total() == 4U, "4 engasgos");
    Require(recorder.Count(HitchCause::Shader) == 1U && recorder.Count(HitchCause::Textures) == 1U &&
                recorder.Count(HitchCause::Logic) == 1U && recorder.Count(HitchCause::Render) == 1U,
            "contagem por causa");
  }
  {
    // Programas destruídos (ProgramsLive cai) não contam como shader novo.
    HitchRecorder recorder;
    HitchSample sample;
    sample.ProgramsLive = 50;
    recorder.Observe(0.0, 0, kTarget, sample);
    sample.ProgramsLive = 40;
    auto line = recorder.Observe(0.1, 1, kTarget, sample);
    Require(line && line->find("cause=render") != std::string::npos, "queda de programas");
  }
  std::puts("hitch recorder: PASS");
  return 0;
}
