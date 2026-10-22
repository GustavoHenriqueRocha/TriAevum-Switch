#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace Oot3dNativeGame {

// Registro de engasgos (logs/hitches.txt): cada apresentação que demora mais
// que `threshold` vezes o intervalo-alvo vira uma linha com o que aconteceu
// naquele quadro, para achar a causa de cada engasgo (o perf.txt só tem médias
// de 5 s). Roda na render thread, a cada apresentação; só formata texto quando
// há engasgo.
enum class HitchCause : uint8_t { Shader, Textures, Logic, Render, Count };

inline const char *HitchCauseName(HitchCause cause) {
  switch (cause) {
  case HitchCause::Shader: return "shader";
  case HitchCause::Textures: return "texturas";
  case HitchCause::Logic: return "logica";
  case HitchCause::Render: return "render";
  default: return "?";
  }
}

struct HitchSample {
  double GuestMs = 0.0;    // lógica do jogo no último quadro do jogo
  double GameBusyMs = 0.0; // thread do jogo ocupada (sem esperar o renderer)
  uint64_t TexturesCreated = 0U; // contadores acumulados do backend
  uint64_t ProgramsLive = 0U;
};

class HitchRecorder {
public:
  // Textures created in one presentation that count as an upload burst.
  static constexpr uint64_t kTextureBurst = 8U;

  explicit HitchRecorder(double threshold = 1.5) : mThreshold(threshold) {}

  // Uma apresentação em `nowSeconds`; targetMs = intervalo esperado (33,3 a
  // 30 FPS). Devolve a linha do engasgo, se houve.
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
                      "game_ms=%.1f tex_new=%llu prog_new=%llu\n",
                      static_cast<unsigned long long>(hostFrame), nowSeconds, dtMs,
                      dtMs - targetMs, HitchCauseName(cause), sample.GuestMs,
                      sample.GameBusyMs, static_cast<unsigned long long>(textures),
                      static_cast<unsigned long long>(programs));
        line.emplace(text);
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
