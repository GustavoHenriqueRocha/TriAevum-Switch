#include "oot3d_host_menu_touch.h"

#include <algorithm>
#include <cmath>

namespace Oot3dNativeGame {

HostMenuTouchPoint
SnapHostMenuTouchPoint(float x, float y,
                       std::span<const HostMenuTouchRect> targets,
                       float radius) noexcept {
  HostMenuTouchPoint point{x, y};
  float best = radius;
  bool found = false;
  for (const auto &target : targets) {
    const float dx = std::max({target.MinX - x, 0.0F, x - target.MaxX});
    const float dy = std::max({target.MinY - y, 0.0F, y - target.MaxY});
    const float distance = std::hypot(dx, dy);
    if (distance <= best && (!found || distance < best)) {
      best = distance;
      found = true;
      point = {(target.MinX + target.MaxX) * 0.5F,
               (target.MinY + target.MaxY) * 0.5F};
    }
  }
  return point;
}

HostMenuTouchEvents
HostMenuTouchGesture::Update(const HostMenuTouchSample &sample) noexcept {
  HostMenuTouchEvents events;
  const auto movePointer = [&](float x, float y) {
    events.MovePointer = true;
    events.PointerX = x;
    events.PointerY = y;
  };
  // Um toque: pressionar e soltar onde o dedo pousou.
  const auto click = [&] {
    movePointer(mPointX, mPointY);
    events.Press = true;
    events.Release = true;
  };
  const auto lift = [&] {
    events.ClearPointer = true;
    mPhase = Phase::Idle;
  };
  // Além da folga, o espaço vazio rola com o dedo (incluindo o movimento feito
  // dentro da folga) e o ponteiro o segue.
  const auto pan = [&] {
    if (!mMoved) {
      return;
    }
    events.ScrollY = sample.Y - mLastY;
    if (sample.X != mLastX || sample.Y != mLastY) {
      movePointer(sample.X, sample.Y);
      mLastX = sample.X;
      mLastY = sample.Y;
    }
  };

  if (sample.Pressed && mPhase != Phase::Idle &&
      std::max(std::abs(sample.X - mDownX), std::abs(sample.Y - mDownY)) >
          sample.TapSlop) {
    mMoved = true;
  }
  switch (mPhase) {
  case Phase::Idle:
    if (sample.Pressed) {
      mDownX = mLastX = sample.X;
      mDownY = mLastY = sample.Y;
      const auto point = SnapHostMenuTouchPoint(
          sample.X, sample.Y, sample.SnapTargets, sample.SnapRadius);
      mPointX = point.X;
      mPointY = point.Y;
      movePointer(mPointX, mPointY);
      mSettle = kHoverSettlePolls;
      mMoved = false;
      mPhase = Phase::Settling;
    }
    break;
  case Phase::Settling:
    if (!sample.Pressed) {
      // Levantou antes de o hover do ImGui ser lido: ainda é um toque.
      click();
      lift();
      break;
    }
    if (--mSettle != 0U) {
      break;
    }
    if (sample.ItemHovered) {
      // Pressionado onde o ImGui viu o item; um arrasto segue a partir do próximo poll.
      events.Press = true;
      mPhase = Phase::Pressing;
    } else {
      mPhase = Phase::Panning;
      pan();
    }
    break;
  case Phase::Pressing:
    if (!sample.Pressed) {
      // Dentro da folga o ponteiro nunca saiu do ponto de pressão: um clique.
      events.Release = true;
      lift();
    } else if (mMoved &&
               (sample.X != mLastX || sample.Y != mLastY)) {
      movePointer(sample.X, sample.Y);
      mLastX = sample.X;
      mLastY = sample.Y;
    }
    break;
  case Phase::Panning:
    if (sample.Pressed) {
      pan();
    } else {
      // Um toque em espaço vazio ainda é um clique (fecha um popup).
      if (!mMoved) {
        click();
      }
      lift();
    }
    break;
  }
  return events;
}

} // namespace Oot3dNativeGame
