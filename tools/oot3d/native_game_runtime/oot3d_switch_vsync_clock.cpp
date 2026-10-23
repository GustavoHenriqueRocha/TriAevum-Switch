// Thread do vsync do display: ver oot3d_vsync_clock.h.
#include "oot3d_vsync_clock.h"

#include <chrono>
#include <thread>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace Oot3dNativeGame {

#if defined(__SWITCH__)
namespace {

struct SwitchVsyncImpl {
  ViDisplay Display{};
  Event Vsync{};
  bool ViInitialized = false;
  bool DisplayOpen = false;
  bool EventOpen = false;
  std::thread Thread;
};

int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

bool SwitchVsyncClock::Start() {
  if (Running()) return true;
  auto *impl = new SwitchVsyncImpl();
  // vi já foi aberto pela janela padrão (nwindowGetDefault); o serviço tem
  // contagem de referências, e o display "Default" pode ser aberto de novo
  // (o evento de vsync é por handle de display).
  Result rc = viInitialize(ViServiceType_Default);
  if (R_SUCCEEDED(rc)) {
    impl->ViInitialized = true;
    rc = viOpenDefaultDisplay(&impl->Display);
  }
  if (R_SUCCEEDED(rc)) {
    impl->DisplayOpen = true;
    rc = viGetDisplayVsyncEvent(&impl->Display, &impl->Vsync);
  }
  if (R_FAILED(rc)) {
    mStartResult = rc;
    if (impl->DisplayOpen) viCloseDisplay(&impl->Display);
    if (impl->ViInitialized) viExit();
    delete impl;
    return false;
  }
  impl->EventOpen = true;
  mImpl = impl;
  mStop.store(false, std::memory_order_relaxed);
  mRunning.store(true, std::memory_order_release);
  impl->Thread = std::thread([this] { Loop(); });
  return true;
}

void SwitchVsyncClock::Loop() {
  auto *impl = static_cast<SwitchVsyncImpl *>(mImpl);
  // Acima das threads do jogo (0x2C) e da render thread: o atraso de
  // acordar é o erro da fase (a thread quase não roda: ~1 us por vsync).
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
  VsyncClockModel model;
  while (!mStop.load(std::memory_order_relaxed)) {
    // 100 ms: Stop() não espera mais que isso.
    if (R_FAILED(eventWait(&impl->Vsync, 100000000ULL))) continue;
    const uint64_t tick = armGetSystemTick(); // cntpct_el0
    // Relógio do steady_clock: a diferença medida agora (os dois andam juntos).
    const int64_t steadyNow = SteadyNowNs();
    const uint64_t tickNow = armGetSystemTick();
    const int64_t stampNs =
        steadyNow - static_cast<int64_t>(armTicksToNs(tickNow - tick));
    eventClear(&impl->Vsync);
    model.Observe(stampNs);
    mPublisher.Publish(model.Snapshot());
  }
}

void SwitchVsyncClock::Stop() {
  auto *impl = static_cast<SwitchVsyncImpl *>(mImpl);
  if (impl == nullptr) return;
  mStop.store(true, std::memory_order_relaxed);
  if (impl->Thread.joinable()) impl->Thread.join();
  if (impl->EventOpen) eventClose(&impl->Vsync);
  if (impl->DisplayOpen) viCloseDisplay(&impl->Display);
  if (impl->ViInitialized) viExit();
  delete impl;
  mImpl = nullptr;
  mRunning.store(false, std::memory_order_release);
}

#else

bool SwitchVsyncClock::Start() { return false; }
void SwitchVsyncClock::Loop() {}
void SwitchVsyncClock::Stop() {}

#endif

SwitchVsyncClock::~SwitchVsyncClock() { Stop(); }

} // namespace Oot3dNativeGame
