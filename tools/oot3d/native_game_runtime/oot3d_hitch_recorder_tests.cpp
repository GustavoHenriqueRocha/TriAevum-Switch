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
    // Locked 30 FPS: no hitch.
    HitchRecorder recorder;
    HitchSample sample;
    for (int frame = 0; frame < 300; ++frame)
      Require(!recorder.Observe(frame / 30.0, frame, kTarget, sample), "30 FPS without hitch");
    Require(recorder.Total() == 0U, "zero total");
  }
  {
    // A 55 ms frame with a new shader, a 70 ms one with a texture burst,
    // a 60 ms one with logic over budget and a 55 ms one with no cause in the game.
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
    Require(line && line->find("cause=textures") != std::string::npos, "textures");
    Require(line->find("tex_new=40") != std::string::npos, "tex_new");
    now += 0.060; sample.GameBusyMs = 45.0;
    line = recorder.Observe(now, 3, kTarget, sample);
    Require(line && line->find("cause=logic") != std::string::npos, "logic");
    now += 0.055; sample.GameBusyMs = 10.0;
    line = recorder.Observe(now, 4, kTarget, sample);
    Require(line && line->find("cause=render") != std::string::npos, "render");
    // 40 ms (1.2× the target) is not a hitch yet; few textures aren't a burst.
    now += 0.040; sample.TexturesCreated += 3;
    Require(!recorder.Observe(now, 5, kTarget, sample), "below threshold");
    Require(recorder.Total() == 4U, "4 hitches");
    Require(recorder.Count(HitchCause::Shader) == 1U && recorder.Count(HitchCause::Textures) == 1U &&
                recorder.Count(HitchCause::Logic) == 1U && recorder.Count(HitchCause::Render) == 1U,
            "count per cause");
  }
  {
    // Destroyed programs (ProgramsLive drops) don't count as a new shader.
    HitchRecorder recorder;
    HitchSample sample;
    sample.ProgramsLive = 50;
    recorder.Observe(0.0, 0, kTarget, sample);
    sample.ProgramsLive = 40;
    auto line = recorder.Observe(0.1, 1, kTarget, sample);
    Require(line && line->find("cause=render") != std::string::npos, "program drop");
  }
  {
    // Each line ends with the area of its frame: Kokiri Forest room 0, then
    // outside gameplay.
    HitchRecorder recorder;
    HitchSample sample;
    sample.Area = {0x55, 0};
    recorder.Observe(0.0, 0, kTarget, sample);
    auto line = recorder.Observe(0.1, 1, kTarget, sample);
    Require(line && line->find("prog_new=0 scene=0x55:spot04 room=0\n") != std::string::npos,
            "hitch area");
    sample.Area = {};
    line = recorder.Observe(0.2, 2, kTarget, sample);
    Require(line && line->find("prog_new=0 scene=- room=-\n") != std::string::npos,
            "hitch outside gameplay");
  }
  std::puts("hitch recorder: PASS");
  return 0;
}
