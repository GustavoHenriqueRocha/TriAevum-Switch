// Game log lines written before the logger exists (oot3d_game_log.h): kept
// in order, written once the sink is attached, then passed straight through;
// a bounded buffer that says how many it dropped; safe from several threads.
#include "oot3d_game_log.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::vector<std::string> gWritten;

void Sink(const std::string &line) { gWritten.push_back(line); }

void KeptUntilAttached() {
  gWritten.clear();
  GameLogLines log;
  log.Write("kernel_fast: wake=1 svc=1 ui_lock=1 retake=1");
  log.Write("TRIAEVUM_PERF_APM gpu_high=1 mem_high=1");
  log.Write("TRIAEVUM_PERF_CPU event=choose_high action=set_high");
  Require(gWritten.empty(), "nothing reaches the sink before the logger exists");
  log.Attach(Sink);
  Require(gWritten.size() == 3U && gWritten[0].rfind("kernel_fast:", 0) == 0 &&
              gWritten[1].rfind("TRIAEVUM_PERF_APM", 0) == 0 && gWritten[2].rfind("TRIAEVUM_PERF_CPU", 0) == 0,
          "the kept lines, in order, when the logger is attached");
  log.Write("TRIAEVUM_PERF_CPU event=background action=restore");
  Require(gWritten.size() == 4U && gWritten[3] == "TRIAEVUM_PERF_CPU event=background action=restore",
          "later lines go straight to the logger");
}

void Bounded() {
  gWritten.clear();
  GameLogLines log;
  for (size_t index = 0; index < GameLogLines::kMaxPending + 5U; ++index) log.Write("line " + std::to_string(index));
  log.Attach(Sink);
  Require(gWritten.size() == GameLogLines::kMaxPending + 1U, "kMaxPending kept, then one line about the rest");
  Require(gWritten.front() == "line 0" && gWritten[GameLogLines::kMaxPending - 1U] == "line 63",
          "the first lines are the ones kept");
  Require(gWritten.back() == "game_log: 5 start-up lines dropped (more than 64 before the logger)",
          "the dropped count is logged");
}

void Threads() {
  gWritten.clear();
  GameLogLines log;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&log, thread] {
      for (int index = 0; index < 200; ++index) log.Write("t" + std::to_string(thread));
    });
    if (thread == 1) log.Attach(Sink);  // attached while others write
  }
  for (auto &thread : threads) thread.join();
  // 800 lines; those written before Attach are kept up to kMaxPending, the
  // rest of the early ones counted (one more line).
  Require(gWritten.size() >= 800U - 2U * 200U && gWritten.size() <= 801U, "no line lost after the attach");
}

void SharedInstance() {
  Require(&GameLog() == &GameLog(), "one instance per process");
}

} // namespace

int main() {
  KeptUntilAttached();
  Bounded();
  Threads();
  SharedInstance();
  std::puts("game log: PASS");
  return 0;
}
