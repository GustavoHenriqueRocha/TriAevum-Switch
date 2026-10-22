#pragma once

#include <atomic>

namespace Fast::Oot3d {

// Debug menu on the console (like Ship of Harkinian's): both stick buttons
// (L3+R3, unused by OoT3D) open and close a window with the application
// settings tabs (Dev Tools warp, savestates, ...). While it is open the game
// gets no input and the controller drives ImGui.
class DebugMenuCombo {
  public:
    // One sample of "both stick buttons held"; true once per press.
    bool Update(bool held) noexcept {
        const bool pressed = held && !mHeld;
        mHeld = held;
        return pressed;
    }

  private:
    bool mHeld = false;
};

inline std::atomic<bool>& DebugMenuOpenFlag() noexcept {
    static std::atomic<bool> open{false};
    return open;
}

inline bool DebugMenuOpen() noexcept {
    return DebugMenuOpenFlag().load(std::memory_order_relaxed);
}

inline void ToggleDebugMenu() noexcept {
    auto& open = DebugMenuOpenFlag();
    bool current = open.load(std::memory_order_relaxed);
    while (!open.compare_exchange_weak(current, !current, std::memory_order_relaxed)) {
    }
}

} // namespace Fast::Oot3d
