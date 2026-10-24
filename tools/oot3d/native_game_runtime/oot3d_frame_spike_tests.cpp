// Medição por pico (oot3d_frame_spike_log.h), relógio de vsync
// (oot3d_vsync_clock.h) e pacer ancorado no vsync (config/pacer_vsync).
// --report imprime a tabela da simulação de deriva.
#include "oot3d_frame_spike_log.h"
#include "oot3d_native_cadence_sim.h"
#include "oot3d_native_presentation_pacer.h"
#include "oot3d_vsync_clock.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

namespace {

using namespace Oot3dNativeGame;

int gFailures = 0;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++gFailures;
  }
}

constexpr double kNominalNs = 1.0e9 / 60.0;
// Display ~0,2% mais lento que o relógio da CPU (console).
constexpr double kDisplayNs = kNominalNs * 1.002;

void TestVsyncModelPeriodAndPhase() {
  std::mt19937 rng(7U);
  std::uniform_real_distribution<double> wake(0.0, 300000.0); // até 300 us
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  VsyncClockModel model;
  const double phase0 = 3.0e9;
  for (int index = 0; index < 3000; ++index) {
    if (unit(rng) < 0.03) continue; // a thread perdeu este vsync
    const double t = phase0 + index * kDisplayNs + wake(rng);
    model.Observe(static_cast<int64_t>(t));
  }
  const auto snapshot = model.Snapshot();
  Require(snapshot.Valid, "modelo válido");
  Require(std::abs(snapshot.PeriodNs - kDisplayNs) < 20.0,
          "período medido a menos de 20 ns do real");
  // Fase: um vsync real a menos de 100 us (envoltória inferior).
  const double since = static_cast<double>(snapshot.LastVsyncNs) - phase0;
  const double error = since - std::round(since / kDisplayNs) * kDisplayNs;
  Require(error > -20000.0 && error < 100000.0, "fase perto do vsync real");
  Require(snapshot.Resets == 0U, "sem recomeço com jitter normal");
}

// tour19: sem o evento do display (rc 0x1272), a fonte reserva é o retorno
// do acquire que bloqueou: vsync + latência de 50-500 us, em 1 de cada 1-3
// vsyncs (a render thread nem sempre espera), misturado com acquires que
// não esperaram (voltam a qualquer hora) e que o filtro descarta.
void TestAcquireFallbackFindsVsync() {
  std::mt19937 rng(11U);
  std::uniform_real_distribution<double> latency(50000.0, 500000.0);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  AcquireVsyncFilter filter;
  const double phase0 = 5.0e9;
  int index = 0;
  for (int frame = 0; frame < 4000; ++frame) {
    index += unit(rng) < 0.6 ? 1 : (unit(rng) < 0.7 ? 2 : 3);
    if (unit(rng) < 0.35) {
      // Não bloqueou: volta no meio do quadro, espera curta.
      const double t = phase0 + index * kDisplayNs + unit(rng) * kDisplayNs;
      Require(!filter.Observe(static_cast<int64_t>(t), unit(rng) * 1.4),
              "acquire sem espera fica de fora");
      continue;
    }
    const double t = phase0 + index * kDisplayNs + latency(rng);
    (void)filter.Observe(static_cast<int64_t>(t), 2.0 + unit(rng) * 10.0);
  }
  const auto snapshot = filter.Snapshot();
  Require(snapshot.Valid, "fonte reserva: modelo válido");
  Require(std::abs(snapshot.PeriodNs - kDisplayNs) < 50.0,
          "fonte reserva: período a menos de 50 ns do real");
  const double since = static_cast<double>(snapshot.LastVsyncNs) - phase0;
  const double error = since - std::round(since / kDisplayNs) * kDisplayNs;
  Require(error > -20000.0 && error < 150000.0,
          "fonte reserva: fase a menos de 150 us do vsync real");
  Require(snapshot.Resets == 0U, "fonte reserva: sem recomeço");
  Require(filter.Used() > 2000U && filter.Ignored() > 1000U,
          "fonte reserva: contagem de usados e ignorados");
}

// O relógio do Switch no host não abre nada; a reserva liga e publica.
void TestSwitchClockFallbackOnHost() {
  SwitchVsyncClock clock;
  Require(!clock.Start(), "host: sem evento de vsync");
  Require(clock.Source() == VsyncClockSource::None, "host: sem fonte");
  clock.ObserveAcquire(1000, 5.0); // antes de ligar: ignorado
  Require(clock.AcquireSamples() == 0U, "host: reserva desligada ignora");
  Require(clock.StartAcquireFallback(), "host: reserva liga");
  Require(clock.Source() == VsyncClockSource::Acquire, "host: fonte acquire");
  for (int index = 0; index < 100; ++index)
    clock.ObserveAcquire(static_cast<int64_t>(1.0e9 + index * kDisplayNs + 1.0e5),
                         4.0);
  const auto snapshot = clock.Read();
  Require(snapshot.Valid && std::abs(snapshot.PeriodNs - kDisplayNs) < 5.0,
          "host: reserva publica o período");
  gAcquireVsyncClock.store(&clock);
}

void TestVsyncModelResetsOnModeChange() {
  VsyncClockModel model;
  for (int index = 0; index < 200; ++index)
    model.Observe(static_cast<int64_t>(1.0e9 + index * kDisplayNs));
  // Salto de fase de meio período (troca de modo, suspensão).
  const double t0 = 1.0e9 + 200.5 * kDisplayNs;
  for (int index = 0; index < 200; ++index)
    model.Observe(static_cast<int64_t>(t0 + index * kDisplayNs));
  const auto snapshot = model.Snapshot();
  Require(snapshot.Resets == 1U, "um recomeço");
  Require(snapshot.Valid && std::abs(snapshot.PeriodNs - kDisplayNs) < 5.0,
          "período de novo depois do recomeço");
}

void TestSnap() {
  const double p = kDisplayNs;
  // Prazo 3 ms depois do vsync 10: vai para o vsync 10 + 1 ms.
  const int64_t last = 1000000000;
  const int64_t deadline = last + static_cast<int64_t>(10 * p + 3.0e6);
  const int64_t snapped = SnapDeadlineToVsync(deadline, last, p, 1.0e6);
  Require(std::llabs(snapped - (last + static_cast<int64_t>(10 * p + 1.0e6))) <= 1,
          "snap no vsync mais próximo + offset");
  // Prazo antes de lastVsync (vsync novo publicado depois): ainda na grade.
  const int64_t before = SnapDeadlineToVsync(last - 2000000, last, p, 0.0);
  Require(before == last, "snap para trás");
}

void TestRefreshCounterVsync() {
  SpikeRefreshCounter counter;
  const int64_t v0 = 1000000000;
  const double p = kDisplayNs;
  const auto at = [&](double periods, double extraNs) {
    return static_cast<int64_t>(v0 + periods * p + extraNs);
  };
  Require(counter.Observe(at(0, 1.0e6), true, v0, p, kNominalNs) == 1U, "primeiro");
  Require(counter.Observe(at(1, 1.0e6), true, v0, p, kNominalNs) == 1U, "em dia");
  Require(counter.Observe(at(2, 1.5e6), true, v0, p, kNominalNs) == 1U, "jitter dentro");
  // Swap 2 ms depois do vsync 4 (pulou o 3): 2 refreshes.
  Require(counter.Observe(at(4, 2.0e6), true, v0, p, kNominalNs) == 2U, "perdeu um");
  // Dois swaps no mesmo intervalo: a fila mostra o segundo no vsync seguinte.
  Require(counter.Observe(at(4, 9.0e6), true, v0, p, kNominalNs) == 1U, "fila 1");
  Require(counter.Observe(at(5, 0.5e6), true, v0, p, kNominalNs) == 1U, "fila 2");
  // Hold de 3 refreshes.
  Require(counter.Observe(at(9, 1.0e6), true, v0, p, kNominalNs) == 3U, "três");
}

void TestRefreshCounterFallback() {
  SpikeRefreshCounter counter;
  const double p = kNominalNs;
  int64_t t = 0;
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "primeiro");
  t += static_cast<int64_t>(p * 1.1);
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "1,1 período = 1");
  t += static_cast<int64_t>(p * 2.0);
  Require(counter.Observe(t, false, 0, 0.0, p) == 2U, "2 períodos");
  t += static_cast<int64_t>(p * 0.3);
  Require(counter.Observe(t, false, 0, 0.0, p) == 1U, "curto conta 1");
}

void TestClassificationRingAndFormat() {
  FrameSpikeTotals totals;
  FrameSpikeRing<8> ring;
  const double period = 1000.0 / 60.0;
  FrameSpikeSample sample;
  sample.Kind = SpikeFrameKind::Step;
  sample.Refreshes = 1U;
  Require(!ObserveFrameSpike(sample, period, totals, ring), "em dia não registra");
  sample.Refreshes = 2U;
  sample.GameBusyMs = 22.0F;
  sample.HostFrame = 42U;
  sample.GuestFrame = 21U;
  sample.Context = kSpikeLoad | kSpikeShader;
  sample.TopStage = 2U;
  sample.TopStageMs = 9.5F;
  Require(ObserveFrameSpike(sample, period, totals, ring), "perdeu: registra");
  sample.GameBusyMs = 5.0F;
  sample.RenderBusyMs = 18.0F;
  sample.Refreshes = 3U;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  sample.RenderBusyMs = 5.0F;
  sample.GpuMs = 17.5F;
  sample.Refreshes = 2U;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  sample.GpuMs = 10.0F;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  // Passo do 30 travado: 2 refreshes esperados, 2 medidos = em dia.
  sample.Expected = 2U;
  Require(!ObserveFrameSpike(sample, period, totals, ring), "30 travado em dia");
  // ... e a 30 o orçamento é de dois períodos: 25 ms de jogo não é culpa dele.
  sample.Refreshes = 3U;
  sample.GameBusyMs = 25.0F;
  (void)ObserveFrameSpike(sample, period, totals, ring);
  const auto snapshot = totals.Take();
  Require(snapshot.Presents == 7U, "apresentações");
  Require(snapshot.MissedFrames == 5U, "quadros que perderam");
  Require(snapshot.MissedRefreshes == 6U, "refreshes perdidos");
  Require(snapshot.MissedByStage[0] == 1U && snapshot.MissedByStage[1] == 2U &&
              snapshot.MissedByStage[2] == 1U && snapshot.MissedByStage[3] == 2U,
          "por etapa (guest 1, render 2, gpu 1, pacer 2)");
  std::string text;
  const size_t drained = ring.Drain([&](const FrameSpikeRecord &record) {
    char line[512];
    const int size = FormatFrameSpike(record, line, sizeof(line));
    Require(size > 0 && static_cast<size_t>(size) < sizeof(line), "linha cabe");
    text += line;
  });
  Require(drained == 5U, "anel com 5");
  Require(text.find("host=42 guest=21 kind=step refreshes=2 expected=1 stage=guest") !=
              std::string::npos,
          "campos principais");
  Require(text.find("top=pica_top top_ms=9.50 ctx=load,shader") != std::string::npos,
          "etapa e contexto");
  Require(text.find("stage=render") != std::string::npos &&
              text.find("stage=gpu") != std::string::npos &&
              text.find("stage=pacer") != std::string::npos,
          "todas as etapas");
  // Anel cheio: descarta e conta, sem bloquear.
  for (int index = 0; index < 10; ++index) (void)ring.Push({});
  Require(ring.Dropped() == 2U, "descartes do anel cheio");
}

// Deriva: display 0,2% mais lento que o pacer, tudo cabendo no orçamento
// (jogo 14 ms por passo, render thread 9-11 ms, GPU 10 ms).
CadenceSim::Result RunDrift(bool pacerVsync, uint32_t framesInFlight) {
  CadenceSim::Segment segment;
  segment.Seconds = 60.0;
  segment.GameStepMs = 14.0;
  segment.GpuMs = 10.0;
  segment.RtStepMinMs = 9.0;
  segment.RtStepMaxMs = 11.0;
  segment.SpikeProbability = 0.0;
  CadenceSim::Params params;
  params.DisplayRefreshMs = params.RefreshMs * 1.002;
  params.SwapchainBuffers = 3U;
  params.FramesInFlight = framesInFlight;
  params.PacerVsync = pacerVsync;
  return CadenceSim::Run({segment}, params);
}

void TestDriftDisappearsWithVsyncPacer(bool report) {
  if (report)
    std::printf("deriva 60 s (display 0,2%% mais lento, swapchain 3, cabe no orçamento)\n");
  for (const uint32_t inFlight : {1U, 2U}) {
    const auto cpu = RunDrift(false, inFlight);
    const auto vsync = RunDrift(true, inFlight);
    if (report) {
      for (const auto *run : {&cpu, &vsync}) {
        std::printf("  frames_in_flight=%u pacer %-5s: held_refresh=%llu missed_refresh=%llu "
                    "pictures=%llu acquire_wait_ms=%.0f judder_ms=%.2f\n",
                    inFlight, run == &cpu ? "CPU" : "vsync",
                    static_cast<unsigned long long>(run->HeldPictureRefreshes),
                    static_cast<unsigned long long>(run->MissedRefreshes),
                    static_cast<unsigned long long>(run->Pictures), run->AcquireWaitMs,
                    run->JudderRmsMs);
      }
    }
    // Pacer da CPU: a swapchain enche (o acquire segura a render thread
    // metade do tempo) e, a cada volta da fase, um refresh sem imagem nova.
    Require(cpu.HeldPictureRefreshes >= 3U, "pacer da CPU: refresh sem imagem nova periódico");
    Require(cpu.AcquireWaitMs > 10000.0, "pacer da CPU: render thread presa no acquire");
    // No vsync: nenhum refresh sem imagem nova, swapchain nunca cheia.
    Require(vsync.HeldPictureRefreshes == 0U, "pacer no vsync: nenhum refresh sem imagem nova");
    Require(vsync.MissedRefreshes == 0U, "pacer no vsync: nenhum vsync perdido");
    Require(vsync.AcquireWaitMs < 1.0, "pacer no vsync: acquire não espera");
    // Uma imagem por refresh do display (59,88 Hz x 60 s).
    Require(vsync.Pictures + 2U >= static_cast<uint64_t>(60000.0 / (1000.0 / 60.0 * 1.002)),
            "pacer no vsync: uma imagem nova por refresh");
  }
  // Sem deriva, o pacer no vsync dá o mesmo que o da CPU.
  CadenceSim::Segment segment;
  segment.Seconds = 20.0;
  segment.SpikeProbability = 0.0;
  segment.GameStepMs = 14.0;
  segment.GpuMs = 10.0;
  CadenceSim::Params params;
  params.SwapchainBuffers = 3U;
  const auto cpu = CadenceSim::Run({segment}, params);
  params.PacerVsync = true;
  const auto vsync = CadenceSim::Run({segment}, params);
  Require(vsync.HeldPictureRefreshes <= cpu.HeldPictureRefreshes &&
              vsync.Pictures + 1U >= cpu.Pictures,
          "sem deriva: o vsync não piora");
}

// O pacer real com um leitor de vsync falso: os prazos caem na grade
// (fase de meio período em relação ao início).
struct FakeVsync {
  int64_t LastNs = 0;
  double PeriodNs = kDisplayNs;
};
bool ReadFakeVsync(const void *user, VsyncClockSnapshot *snapshot) {
  const auto *fake = static_cast<const FakeVsync *>(user);
  snapshot->Valid = true;
  snapshot->LastVsyncNs = fake->LastNs;
  snapshot->PeriodNs = fake->PeriodNs;
  return true;
}

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void TestPacerAnchorsToVsyncGrid() {
  NativeRealtimeRefreshPacer pacer(true, 60U);
  FakeVsync fake;
  fake.LastNs = SteadyNs() + static_cast<int64_t>(kDisplayNs / 2.0);
  pacer.SetVsyncAnchor(&ReadFakeVsync, &fake, std::chrono::nanoseconds(0));
  pacer.WaitForNextRefresh(); // começa o relógio (agora, fora da grade)
  int onGrid = 0;
  constexpr int kWaits = 24;
  for (int index = 0; index < kWaits; ++index) {
    pacer.WaitForNextRefresh();
    const double since = static_cast<double>(SteadyNs() - fake.LastNs);
    const double phase = since - std::floor(since / kDisplayNs) * kDisplayNs;
    // Acordou até 3 ms depois de um vsync da grade (host: agendador).
    if (phase < 3.0e6) ++onGrid;
  }
  Require(onGrid >= kWaits - 3, "pacer ancorado acorda na grade do vsync");
  const auto tail = pacer.TailCounters();
  Require(tail.VsyncAnchoredWaits == static_cast<uint64_t>(kWaits),
          "esperas ancoradas contadas");
  // Sem leitor: volta ao relógio da CPU.
  pacer.SetVsyncAnchor(nullptr, nullptr, std::chrono::nanoseconds(0));
  pacer.WaitForNextRefresh();
  Require(pacer.TailCounters().VsyncAnchoredWaits == static_cast<uint64_t>(kWaits),
          "sem leitor não ancora");
}

// config/early_frame_slot: o pacer anuncia o prazo uma vez por espera, antes
// de dormir (o slot do quadro é devolvido ali); desligado ou atrasado, o
// anúncio é "agora" e nunca fica de fora (senão o slot só voltaria no fim).
void TestPacerAnnouncesDeadlineBeforeWait() {
  struct Seen {
    int Calls = 0;
    std::chrono::steady_clock::time_point Deadline{};
  } seen;
  const auto record = [](void *user, std::chrono::steady_clock::time_point deadline) {
    auto &s = *static_cast<Seen *>(user);
    ++s.Calls;
    s.Deadline = deadline;
  };
  NativeRealtimeRefreshPacer pacer(true, 60U);
  pacer.WaitForNextRefresh(1U, false, record, &seen); // primeiro: começa o relógio
  Require(seen.Calls == 1, "primeira espera anuncia");
  pacer.WaitForNextRefresh(1U, false, record, &seen);
  const auto woke = std::chrono::steady_clock::now();
  Require(seen.Calls == 2, "espera normal anuncia uma vez");
  Require(seen.Deadline <= woke && woke - seen.Deadline < std::chrono::milliseconds(5),
          "anúncio é o prazo em que o pacer acorda");
  std::this_thread::sleep_for(std::chrono::milliseconds(60)); // atrasado
  const auto beforeLate = std::chrono::steady_clock::now();
  pacer.WaitForNextRefresh(1U, true, record, &seen);
  Require(seen.Calls == 3 && seen.Deadline >= beforeLate, "atrasado anuncia agora");
  NativeRealtimeRefreshPacer off(false, 60U);
  off.WaitForNextRefresh(1U, false, record, &seen);
  Require(seen.Calls == 4, "pacer desligado também anuncia");
  pacer.WaitForNextRefresh(1U, false, nullptr, nullptr); // sem callback não quebra
}

} // namespace

int main(int argc, char **argv) {
  const bool report = argc > 1 && std::strcmp(argv[1], "--report") == 0;
  TestVsyncModelPeriodAndPhase();
  TestVsyncModelResetsOnModeChange();
  TestAcquireFallbackFindsVsync();
  TestSwitchClockFallbackOnHost();
  Require(gAcquireVsyncClock.load() == nullptr,
          "destrutor solta o ponteiro da reserva");
  TestSnap();
  TestRefreshCounterVsync();
  TestRefreshCounterFallback();
  TestClassificationRingAndFormat();
  TestDriftDisappearsWithVsyncPacer(report);
  TestPacerAnchorsToVsyncGrid();
  TestPacerAnnouncesDeadlineBeforeWait();
  if (gFailures != 0) {
    std::fprintf(stderr, "%d falha(s)\n", gFailures);
    return 1;
  }
  std::printf("oot3d_frame_spike_tests: ok\n");
  return 0;
}
