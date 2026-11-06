#pragma once

#include "oot3d_native_area.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace Oot3dNativeGame {

// Hitch log (logs/hitches.txt): every presentation that takes longer
// than `threshold` times the target interval becomes a line with what happened
// in that frame and where the game was (scene, room), to find the cause of
// each hitch (perf.txt only has 5 s averages). Runs on the render thread, on
// every presentation; only formats text when there is a hitch.
enum class HitchCause : uint8_t { Shader, Textures, Logic, Render, Count };

inline const char *HitchCauseName(HitchCause cause) {
  switch (cause) {
  case HitchCause::Shader: return "shader";
  case HitchCause::Textures: return "textures";
  case HitchCause::Logic: return "logic";
  case HitchCause::Render: return "render";
  default: return "?";
  }
}

struct HitchSample {
  double GuestMs = 0.0;    // game logic in the last game frame
  double GameBusyMs = 0.0; // game thread busy (not waiting for the renderer)
  uint64_t TexturesCreated = 0U; // accumulated backend counters
  uint64_t ProgramsLive = 0U;
  NativeArea Area{}; // the game's area after the last game frame
};

class HitchRecorder {
public:
  // Textures created in one presentation that count as an upload burst.
  static constexpr uint64_t kTextureBurst = 8U;

  explicit HitchRecorder(double threshold = 1.5) : mThreshold(threshold) {}

  // One presentation at `nowSeconds`; targetMs = expected interval (33.3 at
  // 30 FPS). Returns the hitch line, if there was one.
  std::optional<std::string> Observe(double nowSeconds, uint64_t hostFrame,
                                     double targetMs, const HitchSample &sample) {
    std::optional<std::string> line;
    if (mHasPrevious && targetMs > 0.0) {
      const double dtMs = (nowSeconds - mPreviousSeconds) * 1000.0;
      const uint64_t textures = sample.TexturesCreated - mPrevious.TexturesCreated;
      const uint64_t programs = sample.ProgramsLive > mPrevious.ProgramsLive
                                    ? sample.ProgramsLive - mPrevious.ProgramsLive
                                    : 0U;
      if (dtMs > targetMs * mThreshold) {
        const HitchCause cause = Classify(targetMs, sample, textures, programs);
        ++mCounts[static_cast<size_t>(cause)];
        ++mTotal;
        char text[224];
        std::snprintf(text, sizeof(text),
                      "frame=%llu t=%.3f dt_ms=%.1f late_ms=%.1f cause=%s guest_ms=%.1f "
                      "game_ms=%.1f tex_new=%llu prog_new=%llu ",
                      static_cast<unsigned long long>(hostFrame), nowSeconds, dtMs,
                      dtMs - targetMs, HitchCauseName(cause), sample.GuestMs,
                      sample.GameBusyMs, static_cast<unsigned long long>(textures),
                      static_cast<unsigned long long>(programs));
        std::string entry(text);
        entry += FormatNativeArea(sample.Area);
        entry += '\n';
        line.emplace(std::move(entry));
      }
    }
    mHasPrevious = true;
    mPreviousSeconds = nowSeconds;
    mPrevious = sample;
    return line;
  }

  uint64_t Total() const { return mTotal; }
  uint64_t Count(HitchCause cause) const { return mCounts[static_cast<size_t>(cause)]; }

private:
  static HitchCause Classify(double targetMs, const HitchSample &sample,
                             uint64_t textures, uint64_t programs) {
    if (programs > 0U) return HitchCause::Shader;
    if (textures >= kTextureBurst) return HitchCause::Textures;
    if (sample.GameBusyMs > targetMs) return HitchCause::Logic;
    return HitchCause::Render;
  }

  double mThreshold;
  bool mHasPrevious = false;
  double mPreviousSeconds = 0.0;
  HitchSample mPrevious;
  uint64_t mTotal = 0U;
  std::array<uint64_t, static_cast<size_t>(HitchCause::Count)> mCounts{};
};

} // namespace Oot3dNativeGame
