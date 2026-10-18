#pragma once

// Switch thread placement (oot3d_switch_threads.cpp; política em
// oot3d_switch_thread_layout.h). Toda pthread nasce como thread de serviço
// (prioridade 0x3B, núcleos 1|2, no layout padrão); as que importam pedem o
// seu papel no início: render thread, worker de planos PICA e áudio do host
// (o áudio do SDL é reconhecido pelo pedido de TIME_CRITICAL).
//
// Off the Switch everything here is a no-op.

#include "oot3d_switch_thread_layout.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace Oot3dNativeGame {

#if defined(__SWITCH__)
// Applies the active layout's priority and core mask for `role` to the
// calling thread and names it in the per-thread perf summary.
void PlaceSwitchThread(SwitchThreadRole role);
// Records the core the calling thread runs on now (render/plan loops call it
// once per command or batch): shows where a floating thread actually ran.
void NoteSwitchThreadCore();
// The layout read from config/thread_layout (once; fixed when absent).
SwitchThreadLayoutConfig ActiveSwitchThreadLayout();
// " threads: layout=fixed game=cpu%@cores ..." for logs/perf.txt: CPU share
// of each registered thread since the previous call and, for threads that
// note their core, the cores they were seen on.
std::string SwitchThreadPerfSummary();
// Watchdog de travamento: handles e papéis das threads registradas, sem
// bloquear (try_lock; 0 se o registro estiver ocupado) e sem alocar.
size_t SnapshotSwitchThreads(uint32_t* handles, SwitchThreadRole* roles, size_t capacity) noexcept;
#else
inline void PlaceSwitchThread(SwitchThreadRole) {}
inline void NoteSwitchThreadCore() {}
inline SwitchThreadLayoutConfig ActiveSwitchThreadLayout() { return {}; }
inline std::string SwitchThreadPerfSummary() { return {}; }
#endif

} // namespace Oot3dNativeGame
