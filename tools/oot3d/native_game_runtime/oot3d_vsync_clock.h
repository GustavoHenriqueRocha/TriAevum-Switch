#pragma once

// Relógio do vsync real do display (config/pacer_vsync, config/frame_spikes).
//
// O pacer seguia o relógio da CPU (60,000 Hz) e o display do Switch roda um
// pouco mais devagar (~0,2%): a fase entre o swap e o vsync deriva e, a cada
// volta, um quadro cai do lado errado do vsync (refresh perdido periódico, e
// a swapchain enche até o acquire segurar a render thread).
//
// VsyncClockModel (puro, testado no host) recebe o instante de cada vsync
// visto (ns, relógio monotônico) e estima:
//  - período: mínimos quadrados de t contra o índice do vsync numa janela
//    longa (o atraso de acordar é ruído de média ~constante: vai para a
//    fase, não para a inclinação);
//  - fase: envoltória inferior (o acordar nunca é antes do vsync): um
//    resíduo negativo puxa a fase na hora, um positivo só 1/16.
// Vsyncs perdidos pela thread (acordou tarde demais) só pulam índices.
// Um resíduo maior que 1/4 de período (troca de modo de vídeo, suspensão)
// recomeça a estimativa.
//
// SwitchVsyncClock (oot3d_switch_vsync_clock.cpp): thread que espera o
// evento de vsync do display (viGetDisplayVsyncEvent) e publica o modelo.
//
// tour19 (pgo5n): o console devolveu rc 0x1272 (módulo 114 = vi, desc 9,
// não documentado no switchbrew nem nos emuladores) e o relógio nunca
// existiu (vsync_clock=0; pacer_vsync sem efeito; spikes.txt contou os
// refreshes pelo relógio de 60,000 Hz). A janela padrão do libnx
// (nwindowGetDefault, usada pelo SDL/Mesa) já abriu o display "Default" na
// mesma sessão do IApplicationDisplayService e o handle dela é estático no
// libnx (g_viDisplay, inacessível). Agora:
//  1. tenta o evento (vi -> display -> evento) e guarda a etapa que falhou
//     (StartStage: 1 viInitialize, 2 viOpenDefaultDisplay, 3
//     viGetDisplayVsyncEvent);
//  2. se falhar, fonte reserva: o retorno do acquire da janela (dequeue da
//     swapchain) que bloqueou >= kAcquireVsyncMinWaitMs. Com a fila cheia o
//     buffer só volta no vsync: o retorno é vsync + latência curta, e a
//     envoltória inferior do modelo acha a fase (AcquireVsyncFilter).
// Relógio: armGetSystemTick (cntpct_el0); nunca cntvct_el0 (Data Abort no
// Horizon).

#include <atomic>
#include <cmath>
#include <cstdint>

namespace Oot3dNativeGame {

struct VsyncClockSnapshot {
  bool Valid = false;
  int64_t LastVsyncNs = 0; // fase: um vsync (relógio do chamador)
  double PeriodNs = 0.0;
  uint64_t Vsyncs = 0U;    // vistos desde o início
  uint64_t Resets = 0U;
};

class VsyncClockModel {
public:
  static constexpr uint32_t kMinimumSamples = 30U;
  static constexpr uint32_t kWindow = 1200U; // ~20 s: recomeça a regressão

  explicit VsyncClockModel(double nominalPeriodNs = 1.0e9 / 60.0)
      : mNominalNs(nominalPeriodNs), mPeriodNs(nominalPeriodNs) {}

  void Observe(int64_t vsyncNs) {
    ++mVsyncs;
    if (!mStarted) {
      Restart(vsyncNs);
      return;
    }
    const double since = static_cast<double>(vsyncNs - mPhaseNs);
    const double steps = std::round(since / mPeriodNs);
    if (steps < 1.0) {
      // Dois acordares no mesmo vsync (ou relógio voltou): só a envoltória.
      const double residual = since - steps * mPeriodNs;
      if (steps == 0.0 && residual < 0.0) mPhaseNs = vsyncNs;
      return;
    }
    const double residual = since - steps * mPeriodNs;
    if (std::abs(residual) > 0.25 * mPeriodNs || steps > 600.0) {
      ++mResets;
      Restart(vsyncNs);
      return;
    }
    mIndex += static_cast<int64_t>(steps);
    const double base = static_cast<double>(mPhaseNs) + steps * mPeriodNs;
    mPhaseNs = static_cast<int64_t>(std::llround(
        residual < 0.0 ? base + residual : base + residual / 16.0));
    // Regressão: x = índice, y = t - âncora.
    const double x = static_cast<double>(mIndex - mFitIndex0);
    const double y = static_cast<double>(vsyncNs - mFitT0);
    mSx += x; mSy += y; mSxx += x * x; mSxy += x * y; ++mN;
    if (mN >= kMinimumSamples) {
      const double n = static_cast<double>(mN);
      const double den = n * mSxx - mSx * mSx;
      if (den > 0.0) {
        const double slope = (n * mSxy - mSx * mSy) / den;
        if (slope > 0.9 * mNominalNs && slope < 1.1 * mNominalNs) {
          mPeriodNs = slope;
          mValid = true;
        }
      }
    }
    if (mN >= kWindow) {
      // Nova janela a partir daqui, mantendo o período atual.
      mFitIndex0 = mIndex;
      mFitT0 = vsyncNs;
      mSx = mSy = mSxx = mSxy = 0.0;
      mN = 0U;
    }
  }

  [[nodiscard]] VsyncClockSnapshot Snapshot() const {
    VsyncClockSnapshot snapshot;
    snapshot.Valid = mValid;
    snapshot.LastVsyncNs = mPhaseNs;
    snapshot.PeriodNs = mPeriodNs;
    snapshot.Vsyncs = mVsyncs;
    snapshot.Resets = mResets;
    return snapshot;
  }

private:
  void Restart(int64_t vsyncNs) {
    mStarted = true;
    mValid = false;
    mPhaseNs = vsyncNs;
    mIndex = 0;
    mFitIndex0 = 0;
    mFitT0 = vsyncNs;
    mSx = mSy = mSxx = mSxy = 0.0;
    mN = 0U;
    mPeriodNs = mNominalNs;
  }

  double mNominalNs;
  double mPeriodNs;
  bool mStarted = false;
  bool mValid = false;
  int64_t mPhaseNs = 0;
  int64_t mIndex = 0;
  int64_t mFitIndex0 = 0;
  int64_t mFitT0 = 0;
  double mSx = 0.0, mSy = 0.0, mSxx = 0.0, mSxy = 0.0;
  uint32_t mN = 0U;
  uint64_t mVsyncs = 0U;
  uint64_t mResets = 0U;
};

// Prazo do pacer ancorado no vsync: o instante mais próximo de `deadlineNs`
// da grade lastVsync + k*período + offset.
[[nodiscard]] inline int64_t SnapDeadlineToVsync(int64_t deadlineNs, int64_t lastVsyncNs,
                                                 double periodNs, double offsetNs) {
  if (!(periodNs > 0.0)) return deadlineNs;
  const double grid = static_cast<double>(lastVsyncNs) + offsetNs;
  const double k = std::round((static_cast<double>(deadlineNs) - grid) / periodNs);
  return static_cast<int64_t>(std::llround(grid + k * periodNs));
}

// Publicação lock-free (seqlock) do instantâneo: escrita por uma thread,
// leitura por qualquer uma.
class VsyncClockPublisher {
public:
  void Publish(const VsyncClockSnapshot &snapshot) {
    const uint32_t sequence = mSequence.load(std::memory_order_relaxed);
    mSequence.store(sequence + 1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    mValid.store(snapshot.Valid, std::memory_order_relaxed);
    mLast.store(snapshot.LastVsyncNs, std::memory_order_relaxed);
    mPeriod.store(snapshot.PeriodNs, std::memory_order_relaxed);
    mVsyncs.store(snapshot.Vsyncs, std::memory_order_relaxed);
    mResets.store(snapshot.Resets, std::memory_order_relaxed);
    mSequence.store(sequence + 2U, std::memory_order_release);
  }
  [[nodiscard]] VsyncClockSnapshot Read() const {
    VsyncClockSnapshot snapshot;
    for (int attempt = 0; attempt < 8; ++attempt) {
      const uint32_t before = mSequence.load(std::memory_order_acquire);
      if ((before & 1U) != 0U) continue;
      snapshot.Valid = mValid.load(std::memory_order_relaxed);
      snapshot.LastVsyncNs = mLast.load(std::memory_order_relaxed);
      snapshot.PeriodNs = mPeriod.load(std::memory_order_relaxed);
      snapshot.Vsyncs = mVsyncs.load(std::memory_order_relaxed);
      snapshot.Resets = mResets.load(std::memory_order_relaxed);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (mSequence.load(std::memory_order_relaxed) == before) return snapshot;
    }
    return {};
  }

private:
  std::atomic<uint32_t> mSequence{0U};
  std::atomic<bool> mValid{false};
  std::atomic<int64_t> mLast{0};
  std::atomic<double> mPeriod{0.0};
  std::atomic<uint64_t> mVsyncs{0U};
  std::atomic<uint64_t> mResets{0U};
};

// Acquire que bloqueou pelo menos isto (ms) voltou num vsync.
inline constexpr double kAcquireVsyncMinWaitMs = 1.5;

// Fonte reserva: retornos do acquire da janela -> modelo do vsync. Só os
// que bloquearam (um acquire que não esperou volta a qualquer hora).
class AcquireVsyncFilter {
public:
  explicit AcquireVsyncFilter(double nominalPeriodNs = 1.0e9 / 60.0)
      : mModel(nominalPeriodNs) {}
  // true se a amostra entrou no modelo.
  bool Observe(int64_t returnNs, double waitMs) {
    if (!(waitMs >= kAcquireVsyncMinWaitMs)) {
      ++mIgnored;
      return false;
    }
    mModel.Observe(returnNs);
    ++mUsed;
    return true;
  }
  [[nodiscard]] VsyncClockSnapshot Snapshot() const { return mModel.Snapshot(); }
  [[nodiscard]] uint64_t Used() const { return mUsed; }
  [[nodiscard]] uint64_t Ignored() const { return mIgnored; }

private:
  VsyncClockModel mModel;
  uint64_t mUsed = 0U;
  uint64_t mIgnored = 0U;
};

enum class VsyncClockSource : uint8_t { None = 0, DisplayEvent = 1, Acquire = 2 };

// Thread do vsync no Switch (oot3d_switch_vsync_clock.cpp; no host, Start()
// devolve false). Tempos no relógio de std::chrono::steady_clock (ns).
class SwitchVsyncClock {
public:
  SwitchVsyncClock() = default;
  ~SwitchVsyncClock();
  SwitchVsyncClock(const SwitchVsyncClock &) = delete;
  SwitchVsyncClock &operator=(const SwitchVsyncClock &) = delete;

  // Abre o display e o evento e inicia a thread; false se não deu (o
  // motivo fica em StartResult()).
  bool Start();
  void Stop();
  [[nodiscard]] bool Running() const { return mRunning.load(std::memory_order_acquire); }
  [[nodiscard]] uint32_t StartResult() const { return mStartResult; }
  // Etapa que falhou (0 nenhuma; 1 vi, 2 display, 3 evento).
  [[nodiscard]] uint32_t StartStage() const { return mStartStage; }
  [[nodiscard]] VsyncClockSnapshot Read() const { return mPublisher.Read(); }

  // Fonte reserva (tour19): sem o evento, o retorno do acquire da janela.
  // StartAcquireFallback() liga; ObserveAcquire() vem da render thread
  // (uma só escritora do publicador, já que a thread do evento não existe).
  bool StartAcquireFallback() {
    if (Running()) return false;
    mSource.store(VsyncClockSource::Acquire, std::memory_order_release);
    return true;
  }
  void ObserveAcquire(int64_t returnNs, double waitMs) {
    if (mSource.load(std::memory_order_acquire) != VsyncClockSource::Acquire)
      return;
    if (mAcquire.Observe(returnNs, waitMs))
      mPublisher.Publish(mAcquire.Snapshot());
  }
  [[nodiscard]] VsyncClockSource Source() const {
    return Running() ? VsyncClockSource::DisplayEvent
                     : mSource.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint64_t AcquireSamples() const { return mAcquire.Used(); }

private:
  void Loop();
  VsyncClockPublisher mPublisher;
  AcquireVsyncFilter mAcquire;
  std::atomic<VsyncClockSource> mSource{VsyncClockSource::None};
  uint32_t mStartStage = 0U;
  std::atomic<bool> mRunning{false};
  std::atomic<bool> mStop{false};
  uint32_t mStartResult = 0U;
  void *mImpl = nullptr;
};

// O relógio cuja fonte reserva recebe os acquires (gancho da janela na
// render thread); nulo sem ela.
inline std::atomic<SwitchVsyncClock *> gAcquireVsyncClock{nullptr};

} // namespace Oot3dNativeGame
