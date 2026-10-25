#pragma once

// Medição por pico (logs/spikes.txt; config/frame_spikes ou config/gpu_timing).
//
// Médias de janela escondem o que importa para a fluidez: o Switch não tem
// VRR, e um quadro que perde o vsync deixa a imagem anterior mais um refresh
// de 16,7 ms na tela. Aqui cada quadro apresentado vira uma amostra (POD, sem
// alocação); só os que perderam o vsync entram num anel SPSC de tamanho fixo
// (produtor: render thread; consumidor: a janela do perf.txt na thread do
// jogo, que formata em lote e manda ao AsyncLineWriter).
//
// Refreshes de um quadro = refreshes entre a imagem anterior e esta (1 = em
// dia; 2 = a anterior ficou um refresh a mais, este quadro perdeu o vsync).
// Com o relógio de vsync (oot3d_vsync_clock.h) usa o modelo de latch FIFO:
// o buffer entregue em t aparece no primeiro vsync depois de t e nunca antes
// do vsync seguinte ao do buffer anterior (fila da swapchain). Sem ele,
// arredonda o intervalo entre swaps pelo período nominal.
//
// Etapa culpada (missed_by_stage_*): guest = a thread do jogo ocupada passou
// do orçamento (refreshes esperados x período); render = a render thread
// ocupada (sem as esperas) passou; gpu = o último resultado da GPU passou;
// pacer = nada passou (fase/deriva do pacer, espera da swapchain, acordar
// tarde). A ordem é essa: a primeira etapa estourada leva a culpa.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Oot3dNativeGame {

enum class SpikeFrameKind : uint8_t { Step, Interp, Hold };

inline const char *SpikeFrameKindName(SpikeFrameKind kind) {
  switch (kind) {
  case SpikeFrameKind::Step: return "step";
  case SpikeFrameKind::Interp: return "interp";
  default: return "hold";
  }
}

enum class SpikeStage : uint8_t { Guest, Render, Gpu, Pacer, Count };
inline constexpr size_t kSpikeStageCount = static_cast<size_t>(SpikeStage::Count);

inline const char *SpikeStageName(SpikeStage stage) {
  switch (stage) {
  case SpikeStage::Guest: return "guest";
  case SpikeStage::Render: return "render";
  case SpikeStage::Gpu: return "gpu";
  default: return "pacer";
  }
}

// Contexto do quadro (bits): o que aconteceu desde o quadro anterior.
enum SpikeContext : uint16_t {
  kSpikeLoad = 1U << 0,      // troca de cena nos últimos ~2 s (warp/carregamento)
  kSpikeTexUpload = 1U << 1, // texturas criadas
  kSpikeShader = 1U << 2,    // compilação GLSL / tradução nvc0 (Mesa)
  kSpikeProgram = 1U << 3,   // programa GL novo (ProgramsLive subiu)
  kSpikeGlOom = 1U << 4,     // falha de alocação GL
  kSpikeEvict = 1U << 5,     // despejo de residência (texturas)
  kSpikeDrs = 1U << 6,       // resolução dinâmica trocou de escala
};

inline constexpr std::array<const char *, 7> kSpikeContextNames{
    "load", "tex", "shader", "program", "gl_oom", "evict", "drs"};

struct FrameSpikeSample {
  uint64_t HostFrame = 0U;  // quadro de host apresentado (série da render thread)
  uint64_t GuestFrame = 0U; // quadros do jogo concluídos até este
  double TimeSeconds = 0.0; // fim do swap, relógio monotônico
  SpikeFrameKind Kind = SpikeFrameKind::Hold;
  uint8_t Expected = 1U;  // refreshes esperados (2: passo do 30 travado)
  uint8_t Refreshes = 1U; // medidos
  uint8_t TopStage = 0xFFU; // GlPerf::Stage mais cara na CPU (0xFF: sem glperf)
  uint16_t Context = 0U;
  uint16_t TexturesNew = 0U;
  uint16_t ProgramsNew = 0U;
  uint16_t ShadersNew = 0U;
  float GuestMs = 0.0F;       // lógica do jogo no último quadro de host
  float GameBusyMs = 0.0F;    // thread do jogo ocupada (sem esperar o renderer)
  float RenderBusyMs = 0.0F;  // render thread trabalhando no quadro
  float RenderWaitMs = 0.0F;  // render thread esperando a swapchain (acquire)
  float PacerWaitMs = 0.0F;   // espera do pacer antes do swap
  float GpuMs = 0.0F;         // último resultado da GPU (atrasa 1-3 quadros; 0 = nenhum)
  float TopStageMs = 0.0F;
  float SwapMs = 0.0F;        // swap (eglSwapBuffers)
  // deko3d (p6o): partes do swap no render thread (Fast::Deko3dLastSwapTimes).
  float DkFinishMs = 0.0F;
  float DkFenceMs = 0.0F;
  float DkSubmitMs = 0.0F;
  float DkPresentMs = 0.0F;
};

struct FrameSpikeRecord {
  FrameSpikeSample Sample;
  SpikeStage Stage = SpikeStage::Pacer;
};

[[nodiscard]] inline SpikeStage ClassifyFrameSpike(const FrameSpikeSample &sample,
                                                   double periodMs) {
  const double budget = std::max<double>(1.0, sample.Expected) * periodMs;
  if (sample.GameBusyMs > budget) return SpikeStage::Guest;
  if (sample.RenderBusyMs > budget) return SpikeStage::Render;
  if (sample.GpuMs > budget) return SpikeStage::Gpu;
  return SpikeStage::Pacer;
}

// Refreshes de cada quadro apresentado (ver o topo). Tempos em ns no mesmo
// relógio do vsync.
class SpikeRefreshCounter {
public:
  // vsyncValid: lastVsyncNs/periodNs vêm do relógio de vsync.
  uint32_t Observe(int64_t presentNs, bool vsyncValid, int64_t lastVsyncNs,
                   double periodNs, double nominalPeriodNs) {
    double refreshes = 1.0;
    if (vsyncValid && periodNs > 0.0) {
      // Primeiro vsync depois do swap; com fila, o seguinte ao do anterior.
      const double since = static_cast<double>(presentNs - lastVsyncNs);
      double latch = static_cast<double>(lastVsyncNs) +
                     (std::floor(since / periodNs) + 1.0) * periodNs;
      if (mHasPrevious && mLatchValid) {
        if (latch < mPreviousLatchNs + 0.5 * periodNs)
          latch = mPreviousLatchNs + periodNs;
        refreshes = (latch - mPreviousLatchNs) / periodNs;
      } else if (mHasPrevious && nominalPeriodNs > 0.0) {
        refreshes = static_cast<double>(presentNs - mPreviousPresentNs) / nominalPeriodNs;
      }
      mPreviousLatchNs = latch;
      mLatchValid = true;
    } else {
      if (mHasPrevious && nominalPeriodNs > 0.0)
        refreshes = static_cast<double>(presentNs - mPreviousPresentNs) / nominalPeriodNs;
      mLatchValid = false;
    }
    mHasPrevious = true;
    mPreviousPresentNs = presentNs;
    return static_cast<uint32_t>(std::clamp<int64_t>(std::llround(refreshes), 1, 255));
  }
  void Reset() {
    mHasPrevious = false;
    mLatchValid = false;
  }

private:
  bool mHasPrevious = false;
  bool mLatchValid = false;
  int64_t mPreviousPresentNs = 0;
  double mPreviousLatchNs = 0.0;
};

// Anel SPSC de tamanho fixo: Push só na render thread, Drain só no
// consumidor. Cheio: descarta e conta.
template <size_t Capacity> class FrameSpikeRing {
  static_assert((Capacity & (Capacity - 1U)) == 0U, "potência de 2");

public:
  bool Push(const FrameSpikeRecord &record) {
    const uint64_t head = mHead.load(std::memory_order_relaxed);
    const uint64_t tail = mTail.load(std::memory_order_acquire);
    if (head - tail >= Capacity) {
      mDropped.store(mDropped.load(std::memory_order_relaxed) + 1U,
                     std::memory_order_relaxed);
      return false;
    }
    mSlots[head & (Capacity - 1U)] = record;
    mHead.store(head + 1U, std::memory_order_release);
    return true;
  }
  template <typename Fn> size_t Drain(Fn &&fn) {
    const uint64_t head = mHead.load(std::memory_order_acquire);
    uint64_t tail = mTail.load(std::memory_order_relaxed);
    size_t count = 0U;
    for (; tail != head; ++tail, ++count) fn(mSlots[tail & (Capacity - 1U)]);
    mTail.store(tail, std::memory_order_release);
    return count;
  }
  uint64_t Dropped() const { return mDropped.load(std::memory_order_relaxed); }

private:
  std::array<FrameSpikeRecord, Capacity> mSlots{};
  std::atomic<uint64_t> mHead{0U};
  std::atomic<uint64_t> mTail{0U};
  std::atomic<uint64_t> mDropped{0U};
};

// Totais acumulados (escritos pela render thread, lidos pela janela do
// perf.txt): refreshes perdidos e quadros que perderam, por etapa culpada.
struct FrameSpikeTotals {
  std::atomic<uint64_t> Presents{0U};
  std::atomic<uint64_t> MissedFrames{0U};
  std::atomic<uint64_t> MissedRefreshes{0U};
  std::array<std::atomic<uint64_t>, kSpikeStageCount> MissedByStage{};

  struct Snapshot {
    uint64_t Presents = 0U, MissedFrames = 0U, MissedRefreshes = 0U;
    std::array<uint64_t, kSpikeStageCount> MissedByStage{};
  };
  Snapshot Take() const {
    Snapshot snapshot;
    snapshot.Presents = Presents.load(std::memory_order_relaxed);
    snapshot.MissedFrames = MissedFrames.load(std::memory_order_relaxed);
    snapshot.MissedRefreshes = MissedRefreshes.load(std::memory_order_relaxed);
    for (size_t index = 0U; index < kSpikeStageCount; ++index)
      snapshot.MissedByStage[index] = MissedByStage[index].load(std::memory_order_relaxed);
    return snapshot;
  }
};

inline void AddOwner(std::atomic<uint64_t> &counter, uint64_t value) {
  counter.store(counter.load(std::memory_order_relaxed) + value,
                std::memory_order_relaxed);
}

// Uma amostra por quadro apresentado (render thread). Devolve true quando o
// quadro perdeu o vsync (e foi para o anel).
template <size_t Capacity>
bool ObserveFrameSpike(const FrameSpikeSample &sample, double periodMs,
                       FrameSpikeTotals &totals, FrameSpikeRing<Capacity> &ring) {
  AddOwner(totals.Presents, 1U);
  const uint32_t expected = std::max<uint32_t>(1U, sample.Expected);
  if (sample.Refreshes <= expected) return false;
  FrameSpikeRecord record;
  record.Sample = sample;
  record.Stage = ClassifyFrameSpike(sample, periodMs);
  const uint64_t lost = sample.Refreshes - expected;
  AddOwner(totals.MissedFrames, 1U);
  AddOwner(totals.MissedRefreshes, lost);
  AddOwner(totals.MissedByStage[static_cast<size_t>(record.Stage)], lost);
  (void)ring.Push(record);
  return true;
}

// Nomes das etapas do GlPerf (gl_frame_profiler.h, mesma ordem).
inline const char *SpikeTopStageName(uint8_t stage) {
  static constexpr std::array<const char *, 15> kNames{
      "begin", "clear", "pica_top", "pica_bottom", "pica_off", "pica_canon",
      "transfer", "post_aa", "post_easu", "scanout", "ui", "present",
      "other", "pacing", "idle"};
  return stage < kNames.size() ? kNames[stage] : "-";
}

// Uma linha de logs/spikes.txt (com '\n'); devolve o tamanho escrito.
inline int FormatFrameSpike(const FrameSpikeRecord &record, char *out, size_t size) {
  const auto &s = record.Sample;
  char context[64] = "-";
  size_t used = 0U;
  for (size_t bit = 0U; bit < kSpikeContextNames.size(); ++bit) {
    if ((s.Context & (1U << bit)) == 0U) continue;
    const int wrote = std::snprintf(context + used, sizeof(context) - used, "%s%s",
                                    used == 0U ? "" : ",", kSpikeContextNames[bit]);
    if (wrote <= 0 || static_cast<size_t>(wrote) >= sizeof(context) - used) break;
    used += static_cast<size_t>(wrote);
  }
  return std::snprintf(
      out, size,
      "spike t=%.3f host=%llu guest=%llu kind=%s refreshes=%u expected=%u stage=%s "
      "guest_ms=%.2f game_ms=%.2f render_ms=%.2f render_wait_ms=%.2f pacer_ms=%.2f "
      "swap_ms=%.2f gpu_ms=%.2f top=%s top_ms=%.2f ctx=%s tex_new=%u prog_new=%u "
      "shader_new=%u dk_finish_ms=%.2f dk_fence_ms=%.2f dk_submit_ms=%.2f dk_present_ms=%.2f\n",
      s.TimeSeconds, static_cast<unsigned long long>(s.HostFrame),
      static_cast<unsigned long long>(s.GuestFrame), SpikeFrameKindName(s.Kind),
      static_cast<unsigned>(s.Refreshes), static_cast<unsigned>(s.Expected),
      SpikeStageName(record.Stage), s.GuestMs, s.GameBusyMs, s.RenderBusyMs,
      s.RenderWaitMs, s.PacerWaitMs, s.SwapMs, s.GpuMs, SpikeTopStageName(s.TopStage),
      s.TopStageMs, context, static_cast<unsigned>(s.TexturesNew),
      static_cast<unsigned>(s.ProgramsNew), static_cast<unsigned>(s.ShadersNew), s.DkFinishMs, s.DkFenceMs,
      s.DkSubmitMs, s.DkPresentMs);
}

} // namespace Oot3dNativeGame
