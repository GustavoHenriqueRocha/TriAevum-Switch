#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace Oot3dNativeGame {

// Contador de FPS na tela (config/fps_counter). Conta os quadros apresentados,
// inclusive os interpolados do fps60, e fecha uma média a cada `windowSeconds`;
// o pior intervalo da janela mostra travadinhas que a média esconde.
class FpsCounter {
public:
  explicit FpsCounter(double windowSeconds = 0.5) : mWindowSeconds(windowSeconds) {}

  // Um quadro apresentado em `nowSeconds` (relógio monotônico).
  // Devolve true quando uma janela fechou e o texto mudou.
  bool Observe(double nowSeconds) {
    if (mLastSeconds < 0.0) {
      mLastSeconds = mWindowStartSeconds = nowSeconds;
      return false;
    }
    mWorstMs = std::max(mWorstMs, (nowSeconds - mLastSeconds) * 1000.0);
    mLastSeconds = nowSeconds;
    ++mFrames;
    const double elapsed = nowSeconds - mWindowStartSeconds;
    if (elapsed < mWindowSeconds) return false;
    mFps = static_cast<double>(mFrames) / elapsed;
    mShownWorstMs = mWorstMs;
    mFrames = 0U;
    mWorstMs = 0.0;
    mWindowStartSeconds = nowSeconds;
    std::snprintf(mText, sizeof(mText), "%.1f FPS  %.1f ms", mFps, mShownWorstMs);
    return true;
  }

  double Fps() const { return mFps; }
  double WorstFrameMs() const { return mShownWorstMs; }
  const char *Text() const { return mText; }

private:
  double mWindowSeconds;
  double mLastSeconds = -1.0;
  double mWindowStartSeconds = 0.0;
  double mWorstMs = 0.0;
  double mShownWorstMs = 0.0;
  double mFps = 0.0;
  uint32_t mFrames = 0U;
  char mText[48] = "-- FPS";
};

} // namespace Oot3dNativeGame
