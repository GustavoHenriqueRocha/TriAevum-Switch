// Perfil de pico do guest (oot3d_guest_spike_profile.h): limite, agregação
// por função, top N, anel, resumo, carregamentos, overflow e o parser de
// config/guest_spike_ms.
#include "oot3d_guest_spike_profile.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

int gFailures = 0;

void Check(bool condition, const char *what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    ++gFailures;
  }
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

constexpr uint32_t kBase = 0x00100000U;
constexpr double kTicksPerMs = 19200.0; // cntpct_el0 a 19,2 MHz

uint64_t Ms(double ms) { return static_cast<uint64_t>(ms * kTicksPerMs); }
uint32_t Slot(uint32_t pc) { return (pc - kBase) / 4U; }

const char *Name(uint32_t pc) {
  switch (pc) {
  case 0x00110000U: return "Actor_UpdateAll";
  case 0x00120000U: return "BgCheck_Raycast";
  case 0x00130000U: return "Skin_Draw";
  default: return "";
  }
}

GuestSpikeProfile Make(GuestSpikeConfig config = {}) {
  static const std::vector<uint32_t> functions{0x00110000U, 0x00120000U,
                                               0x00130000U};
  GuestSpikeProfile profile;
  profile.Configure(config, kBase, 0x003CA000U / 4U, kTicksPerMs, functions);
  return profile;
}

void TestParser() {
  GuestSpikeConfig c;
  ParseGuestSpikeConfig("", &c);
  CHECK(c.ThresholdMs == 14.0 && c.LoadMs == 50.0);
  ParseGuestSpikeConfig("12,5\n", &c);
  CHECK(c.ThresholdMs == 12.5 && c.LoadMs == 50.0);
  GuestSpikeConfig d;
  ParseGuestSpikeConfig(" 16 80", &d);
  CHECK(d.ThresholdMs == 16.0 && d.LoadMs == 80.0);
  GuestSpikeConfig e;
  ParseGuestSpikeConfig("abc", &e);
  CHECK(e.ThresholdMs == 14.0);
  GuestSpikeConfig f;
  ParseGuestSpikeConfig("0.2", &f); // abaixo de 1 ms: padrão
  CHECK(f.ThresholdMs == 14.0);
  GuestSpikeConfig g;
  ParseGuestSpikeConfig("60", &g); // corte padrão abaixo do limite: 4x
  CHECK(g.ThresholdMs == 60.0 && g.LoadMs == 240.0);
}

// Quadro normal (9 ms): nada registrado, e os acumuladores voltam a zero.
void TestNormalFrameNotRecorded() {
  auto p = Make();
  p.Add(Slot(0x00110010U), Ms(5.0));
  p.Add(Slot(0x00120004U), Ms(4.0));
  CHECK(!p.EndFrame(1U, 1000U));
  CHECK(p.Pending() == 0U);
  CHECK(p.Totals().Frames == 1U && p.Totals().Spikes == 0U);
  // O quadro seguinte começa do zero: 9 ms de novo não somam 18.
  p.Add(Slot(0x00110010U), Ms(5.0));
  p.Add(Slot(0x00120004U), Ms(4.0));
  CHECK(!p.EndFrame(2U, 1000U));
}

// Water Temple (tour19): quadro de 22 ms. Blocos da mesma função somam;
// o top vem em ordem de tempo, com nome e porcentagem.
void TestSpikeAggregatesByFunction() {
  auto p = Make();
  // Actor_UpdateAll: 3 blocos, 12 ms; BgCheck: 2 blocos, 7 ms; Skin: 3 ms.
  p.Add(Slot(0x00110000U), Ms(4.0));
  p.Add(Slot(0x00110040U), Ms(5.0));
  p.Add(Slot(0x00110080U), Ms(3.0));
  p.Add(Slot(0x00120010U), Ms(6.0));
  p.Add(Slot(0x00120014U), Ms(1.0));
  p.Add(Slot(0x00130000U), Ms(3.0));
  // Mesmo bloco duas vezes no quadro.
  p.Add(Slot(0x00110040U), Ms(0.5));
  CHECK(p.EndFrame(77U, 250000U));
  CHECK(p.Totals().Spikes == 1U);
  std::string text;
  CHECK(p.DrainLines(text, Name) == 1U);
  CHECK(p.Pending() == 0U);
  CHECK(text.find("spike host_frame=77 guest_ms=22.50 entries=250000 blocks=6 "
                  "functions=3 load=0 overflow=0\n") == 0U);
  const auto first = text.find("fn=0x00110000 ms=12.500 pct=55.6 name=Actor_UpdateAll");
  const auto second = text.find("fn=0x00120000 ms=7.000 pct=31.1 name=BgCheck_Raycast");
  const auto third = text.find("fn=0x00130000 ms=3.000");
  CHECK(first != std::string::npos && second != std::string::npos &&
        third != std::string::npos);
  CHECK(first < second && second < third);
}

// Sem mapa de funções: agrega por bloco; código antes da primeira função
// conta pelo próprio PC.
void TestWithoutFunctionMap() {
  GuestSpikeProfile p;
  p.Configure({}, kBase, 0x003CA000U / 4U, kTicksPerMs, {});
  p.Add(Slot(0x00110040U), Ms(10.0));
  p.Add(Slot(0x00110080U), Ms(6.0));
  CHECK(p.EndFrame(5U, 10U));
  std::string text;
  (void)p.DrainLines(text, [](uint32_t) { return ""; });
  CHECK(text.find("fn=0x00110040 ms=10.000") != std::string::npos);
  CHECK(text.find("fn=0x00110080 ms=6.000") != std::string::npos);
  CHECK(text.find("name=-") != std::string::npos);
  auto q = Make();
  q.Add(Slot(0x00100100U), Ms(20.0)); // antes de 0x00110000
  CHECK(q.EndFrame(6U, 10U));
  std::string other;
  (void)q.DrainLines(other, Name);
  CHECK(other.find("fn=0x00100100 ms=20.000") != std::string::npos);
}

// Top N limitado; o resumo soma todas as funções dos picos fora de
// carregamento e ordena por tempo total.
void TestTopLimitAndSummary() {
  GuestSpikeConfig config;
  config.TopPerSpike = 2U;
  auto p = Make(config);
  for (int frame = 0; frame < 3; ++frame) {
    p.Add(Slot(0x00110000U), Ms(8.0));
    p.Add(Slot(0x00120000U), Ms(5.0 + frame));
    p.Add(Slot(0x00130000U), Ms(2.0));
    CHECK(p.EndFrame(10U + frame, 1U));
  }
  // Um carregamento (120 ms): no log com load=1, fora do resumo.
  p.Add(Slot(0x00130000U), Ms(120.0));
  CHECK(p.EndFrame(20U, 1U));
  std::string text;
  CHECK(p.DrainLines(text, Name) == 4U);
  size_t fnLines = 0U;
  for (size_t at = text.find("  fn="); at != std::string::npos;
       at = text.find("  fn=", at + 1U))
    ++fnLines;
  CHECK(fnLines == 3U * 2U + 1U);
  CHECK(text.find("load=1") != std::string::npos);
  std::string summary;
  p.FormatSummary(summary, Name);
  CHECK(summary.find("picos=4 picos_carregamento=1") != std::string::npos);
  const auto actor = summary.find("0x00110000     24.00");
  const auto bg = summary.find("0x00120000     18.00");
  const auto skin = summary.find("0x00130000      6.00");
  CHECK(actor != std::string::npos && bg != std::string::npos &&
        skin != std::string::npos);
  CHECK(actor < bg && bg < skin);
  CHECK(summary.find("Actor_UpdateAll") != std::string::npos);
  // guest_ms_medio_pico = (15 + 16 + 17) / 3.
  CHECK(summary.find("guest_ms_medio_pico=16.000") != std::string::npos);
}

// Anel cheio: o mais antigo sai e é contado.
void TestRingOverflow() {
  GuestSpikeConfig config;
  config.RingCapacity = 4U;
  auto p = Make(config);
  for (int frame = 0; frame < 6; ++frame) {
    p.Add(Slot(0x00110000U), Ms(20.0));
    (void)p.EndFrame(static_cast<uint64_t>(frame), 1U);
  }
  CHECK(p.Pending() == 4U);
  CHECK(p.Totals().RingDropped == 2U);
  std::string text;
  (void)p.DrainLines(text, Name);
  CHECK(text.find("host_frame=0 ") == std::string::npos);
  CHECK(text.find("host_frame=2 ") < text.find("host_frame=5 "));
}

// Mais blocos que a lista de tocados: o tempo do quadro conta inteiro, os
// blocos a mais ficam fora do top e nada vaza para o quadro seguinte.
void TestTouchedOverflow() {
  GuestSpikeConfig config;
  config.TouchedCapacity = 8U;
  auto p = Make(config);
  for (uint32_t i = 0; i < 20U; ++i)
    p.Add(Slot(0x00110000U + i * 4U), Ms(1.0));
  CHECK(p.EndFrame(1U, 20U));
  CHECK(p.Totals().OverflowFrames == 1U);
  std::string text;
  (void)p.DrainLines(text, Name);
  CHECK(text.find("guest_ms=20.00") != std::string::npos);
  CHECK(text.find("blocks=8 ") != std::string::npos);
  CHECK(text.find("overflow=1") != std::string::npos);
  // Quadro seguinte limpo.
  p.Add(Slot(0x00110000U), Ms(1.0));
  CHECK(!p.EndFrame(2U, 1U));
}

// Muitos quadros com 1 pico a cada 10: contagens certas e o anel esvazia.
void TestManyFrames() {
  auto p = Make();
  std::string text;
  for (int frame = 0; frame < 200; ++frame) {
    for (uint32_t i = 0; i < 300U; ++i)
      p.Add(Slot(0x00110000U + i * 16U), Ms(frame % 10 == 0 ? 0.1 : 0.02));
    (void)p.EndFrame(static_cast<uint64_t>(frame), 300U);
  }
  CHECK(p.Totals().Spikes == 20U);
  CHECK(p.Totals().Frames == 200U);
  (void)p.DrainLines(text, Name);
  CHECK(p.Pending() == 0U);
}

} // namespace

int main() {
  TestParser();
  TestNormalFrameNotRecorded();
  TestSpikeAggregatesByFunction();
  TestWithoutFunctionMap();
  TestTopLimitAndSummary();
  TestRingOverflow();
  TestTouchedOverflow();
  TestManyFrames();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("guest spike profile: all tests passed\n");
  return 0;
}
