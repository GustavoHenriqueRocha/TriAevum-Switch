#pragma once

// Lines for logs/oot3d_native_game.log from code that may run before the
// file logger exists or on any thread.
//
// On the Switch the game log is spdlog's async logger, made by
// Ship::Context::InitLogging inside InitContextForDemo (window setup), at
// warning level (the release logger drops info); its file sink writes
// logs/oot3d_native_game.log on the logger's own thread. Before that,
// SPDLOG_* goes to spdlog's default console logger, and stdout/stderr go to
// libnx's null device unless nxlink is connected: start-up lines written
// that early (kernel_fast:, the Performance mode's TRIAEVUM_PERF_* lines
// from the Video panel, created before InitContextForDemo) were lost.
//
// GameLogLines keeps the lines written before Attach (at most kMaxPending,
// in order) and writes them through the sink when the window setup attaches
// it, right after InitContextForDemo; later lines go straight to the sink.
// The sink only queues the line for the logger's thread (no SD write on the
// caller's thread), so applet hooks and other threads may write too.
// Plain code: tested on the host (oot3d_game_log_tests).

#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Oot3dNativeGame {

class GameLogLines {
public:
  static constexpr size_t kMaxPending = 64U;
  using Sink = void (*)(const std::string &line);

  void Write(std::string line) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mSink != nullptr) {
      mSink(line);
      return;
    }
    if (mPending.size() < kMaxPending) {
      mPending.push_back(std::move(line));
    } else {
      ++mDropped;
    }
  }

  // Writes the kept lines through `sink`, in order, then every later line.
  void Attach(Sink sink) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mSink = sink;
    for (const std::string &line : mPending) sink(line);
    if (mDropped != 0U)
      sink("game_log: " + std::to_string(mDropped) + " start-up lines dropped (more than " +
           std::to_string(kMaxPending) + " before the logger)");
    mPending.clear();
    mPending.shrink_to_fit();
    mDropped = 0U;
  }

private:
  std::mutex mMutex;
  Sink mSink = nullptr;
  std::vector<std::string> mPending;
  size_t mDropped = 0U;
};

// The process's game log lines (one instance; never destroyed).
inline GameLogLines &GameLog() {
  static GameLogLines *lines = new GameLogLines();
  return *lines;
}

inline void WriteGameLog(std::string line) { GameLog().Write(std::move(line)); }

} // namespace Oot3dNativeGame
